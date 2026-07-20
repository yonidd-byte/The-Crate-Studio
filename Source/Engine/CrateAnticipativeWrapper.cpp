#include "CrateAnticipativeWrapper.h"

const char* CrateAnticipativeWrapper::xmlTypeName = "crateAnticipativeFX";

CrateAnticipativeWrapper::CrateAnticipativeWrapper (te::PluginCreationInfo info)
    : te::Plugin (info)
{
    // No automatable parameters of this wrapper's own yet — every parameter
    // a user sees/automates lives on innerPlugin itself, unchanged.
}

CrateAnticipativeWrapper::~CrateAnticipativeWrapper()
{
    notifyListenersOfDeletion(); // same first-line-of-destructor convention every other te::Plugin uses

    // Belt-and-suspenders: deinitialise() should already have stopped these
    // (every insertPlugin/deleteFromParent path runs through it), but a
    // dangling live worker thread or timer outliving its owner is exactly
    // the kind of bug that only shows up under the stress test's 100-track
    // teardown, not a quick manual test.
    stopTimer();
    detachParameterListeners();

    // Worker MUST be fully stopped before anything below touches the disk
    // streams — it's the ONLY thing that ever reads/writes them, and
    // resetting them while it could still be mid-call would be a real
    // use-after-free, not a theoretical one.
    if (worker != nullptr)
        worker->stopThread (2000);

    // Disk-Backed Shadow Bouncing directive: same belt-and-suspenders
    // reasoning as the timer/worker above — close the streams explicitly.
    // Persistent Tour Cache directive: unlike the old juce::TemporaryFile,
    // shadowCacheFile is deliberately NOT deleted here — releasing the
    // handle is just good hygiene, not cleanup, since the whole point of
    // this step is that the cache OUTLIVES the plugin instance.
    diskReadStream.reset();
    diskWriteStream.reset();
}

void CrateAnticipativeWrapper::setInnerPlugin (te::Plugin::Ptr newPlugin)
{
    // Cache Invalidation directive: never leave a listener registered on a
    // Plugin::Ptr this wrapper is about to stop owning.
    detachParameterListeners();

    innerPlugin = std::move (newPlugin);

    attachParameterListeners();

    // See this method's own doc comment in the header: if the wrapper is
    // already live in the graph by the time an inner plugin gets assigned,
    // replay the cached initialise() info immediately rather than leaving
    // the inner plugin uninitialised until some later cycle that may never
    // come.
    if (innerPlugin != nullptr && hasBeenInitialised)
        innerPlugin->initialise (cachedInitInfo);
}

void CrateAnticipativeWrapper::attachParameterListeners()
{
    if (innerPlugin == nullptr)
        return;

    for (auto* param : innerPlugin->getAutomatableParameters())
        param->addListener (this);
}

void CrateAnticipativeWrapper::detachParameterListeners()
{
    if (innerPlugin == nullptr)
        return;

    for (auto* param : innerPlugin->getAutomatableParameters())
        param->removeListener (this);
}

void CrateAnticipativeWrapper::refreshLiveModeState()
{
    // Zero Latency Override / Live-State Infection directive: the expensive
    // check — now a full upstream ROUTING GRAPH walk, not just this track's
    // own armed/monitor state — lives HERE, on the message thread (this is
    // a Timer callback), specifically so it never has to run on the audio
    // thread or ShadowWorker's thread: EditPlaybackContext::getAllInputs()
    // and te::getAudioTracks() both return a juce::Array BY VALUE (an
    // allocation), which neither of those threads may ever do.
    bool required = false;

    if (auto* track = getOwnerTrack())
    {
        std::set<te::Track*> visited;
        required = isTrackOrUpstreamLive (track, visited);
    }

    liveModeRequired.store (required, std::memory_order_relaxed);
}

bool CrateAnticipativeWrapper::isTrackDirectlyLive (te::Track& track) const
{
    if (auto* playbackContext = edit.getCurrentPlaybackContext())
        for (auto* input : playbackContext->getAllInputs())
            if (input->isLivePlayEnabled (track) || input->isRecordingEnabled (track.itemID))
                return true;

    return false;
}

bool CrateAnticipativeWrapper::isTrackOrUpstreamLive (te::Track* track, std::set<te::Track*>& visited) const
{
    if (track == nullptr || ! visited.insert (track).second)
        return false; // null, or already visited this walk (cycle guard / de-dup)

    if (isTrackDirectlyLive (*track))
        return true;

    // Direct Serial Output directive: Track::getInputTracks() already walks
    // the WHOLE downstream chain itself (see its own doc comment: "true if
    // any downstream tracks match this one"), so recursing through the
    // tracks IT returns covers a multi-hop A->B->C serial chain, not just
    // one hop — we don't need to re-implement that traversal ourselves.
    for (auto* upstream : track->getInputTracks())
        if (isTrackOrUpstreamLive (upstream, visited))
            return true;

    // Aux Send/Return directive: a send is a SEPARATE routing mechanism
    // from a track's primary output, matched by BUS NUMBER rather than a
    // direct track reference (see SendBusUtils.h's own doc comment) —
    // getInputTracks() above never sees it. Find THIS track's own return
    // bus (if it has one), then find every OTHER track anywhere in the
    // Edit that sends to that same bus number.
    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track))
    {
        int myReturnBus = -1;

        for (auto* p : audioTrack->pluginList)
        {
            if (auto* ret = dynamic_cast<te::AuxReturnPlugin*> (p))
            {
                myReturnBus = ret->busNumber;
                break;
            }
        }

        if (myReturnBus >= 0)
        {
            for (auto* candidate : te::getAudioTracks (edit))
            {
                if (candidate == audioTrack)
                    continue;

                bool sendsToMyBus = false;

                for (auto* p : candidate->pluginList)
                {
                    if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                    {
                        if (send->getBusNumber() == myReturnBus)
                        {
                            sendsToMyBus = true;
                            break;
                        }
                    }
                }

                if (sendsToMyBus && isTrackOrUpstreamLive (candidate, visited))
                    return true;
            }
        }
    }

    return false;
}

void CrateAnticipativeWrapper::renderSynchronously (const te::PluginRenderContext& fc)
{
    if (innerPlugin == nullptr)
        return;

    // Lock-Free Spinlock directive: the ONE call site for "acquire dspLock
    // or silence" on the audio thread — Zero Latency Override, Cache
    // Invalidation, and Cache Miss all route through here so there is
    // exactly one implementation of this rule, not three that could drift.
    if (! dspLock.test_and_set (std::memory_order_acquire))
    {
        innerPlugin->applyToBuffer (fc);
        dspLock.clear (std::memory_order_release);
    }
    else if (fc.destBuffer != nullptr)
    {
        // The worker currently holds dspLock. The audio thread must NEVER
        // wait for it — a bounded one-block silent dropout is an
        // acceptable cost; blocking or racing innerPlugin's DSP state is not.
        fc.destBuffer->clear (fc.bufferStartSample, fc.bufferNumSamples);
    }
}

void CrateAnticipativeWrapper::initialise (const te::PluginInitialisationInfo& info)
{
    // Safety: if this is a re-initialise (e.g. a sample-rate change)
    // without an intervening deinitialise(), stop the OLD worker before
    // touching bufferPool/the queues it may currently be reading/writing —
    // resizing bufferPool out from under a running ShadowWorker would be a
    // genuine use-after-resize race, not a theoretical one.
    if (worker != nullptr)
    {
        worker->stopThread (2000);
        worker.reset();
    }

    // Safety (continued): close any stream left over from a previous
    // initialise() — shadowCacheFile itself (just a path descriptor, not a
    // handle) gets recomputed below regardless.
    diskReadStream.reset();
    diskWriteStream.reset();

    cachedInitInfo = info;
    hasBeenInitialised = true;

    // Pre-Allocated Memory Pool / Zero Allocation Rule directive: every
    // buffer's storage is allocated HERE, once, up front — applyToBuffer()
    // (both the audio thread's consumer path and the producer's own render
    // call) must never trigger an allocation.
    constexpr int numChannels = 2; // stereo assumption — see the class's own doc comment
    bufferPool.clear();
    bufferPool.resize ((size_t) poolSize);

    for (auto& buffer : bufferPool)
        buffer.setSize (numChannels, info.blockSizeSamples);

    diskScratchBuffer.setSize (numChannels, info.blockSizeSamples);
    bytesPerDiskBlock = (int64_t) numChannels * info.blockSizeSamples * (int64_t) sizeof (float);
    diskFlushRequested.store (false, std::memory_order_relaxed);

    // Persistent Tour Cache directive: [ProjectFolder]/ShadowCache/
    // <TrackID>.crateshadow — keyed by the track's own EditItemID, stable
    // across save/load of the SAME .crate project. An unsaved project (no
    // file yet, no meaningful project-relative location) falls back to the
    // OS temp directory — Tour Cache only applies once there's an actual
    // project to be "next to".
    const auto editFile = te::EditFileOperations (edit).getEditFile();
    const auto projectFolder = editFile.existsAsFile() ? editFile.getParentDirectory()
                                                        : juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto shadowCacheDir = projectFolder.getChildFile ("ShadowCache");
    shadowCacheDir.createDirectory();

    if (auto* track = getOwnerTrack())
        shadowCacheFile = shadowCacheDir.getChildFile (juce::String (track->itemID.getRawID()) + ".crateshadow");
    else
        shadowCacheFile = shadowCacheDir.getChildFile ("unknown.crateshadow"); // shouldn't happen — every wrapper is inserted onto a track before initialise()

    const int64_t ringCapacityBytes = bytesPerDiskBlock * (int64_t) maxDiskBlocksQueued;

    // Persistent Tour Cache directive: an existing, CORRECTLY-SIZED file is
    // a previous session's fully-bounced cache — attach to it as-is,
    // WITHOUT overwriting, and treat the whole ring as already populated
    // with valid, ready-to-consume audio. Wrong size (different session,
    // different block size/sample rate, a stray leftover) or simply
    // missing falls through to the exact same fresh-pre-allocation path
    // Step 4.9 always used.
    const bool cacheAlreadyValid = bytesPerDiskBlock > 0
                                     && shadowCacheFile.existsAsFile()
                                     && shadowCacheFile.getSize() == ringCapacityBytes;

    diskWriteStream = shadowCacheFile.createOutputStream();
    diskReadStream  = shadowCacheFile.createInputStream();

    if (cacheAlreadyValid)
    {
        // Instant load, 0% CPU overhead: the JIT Spooler can start feeding
        // the audio thread from disk immediately — no fresh rendering
        // needed until this entire backlog is consumed.
        diskReadBlockIndex = 0;
        diskWriteBlockIndex = maxDiskBlocksQueued;
        diskBlocksQueued = maxDiskBlocksQueued;
    }
    else
    {
        diskBlocksQueued = 0;
        diskWriteBlockIndex = 0;
        diskReadBlockIndex = 0;

        // Fixed-Size Ring directive (see the header's own doc comment for
        // the ~50MB/s-unbounded-growth bug this replaced): pre-allocate
        // the file to EXACTLY maxDiskBlocksQueued slots up front — writing
        // one byte at the final offset forces the OS to size the file
        // immediately, rather than letting it grow lazily one write at a
        // time.
        if (diskWriteStream != nullptr && bytesPerDiskBlock > 0)
        {
            diskWriteStream->setPosition (ringCapacityBytes - 1);
            diskWriteStream->writeByte (0);
            diskWriteStream->setPosition (0);
        }
    }

    blockDurationSeconds = info.sampleRate > 0.0 ? (double) info.blockSizeSamples / info.sampleRate : 0.0;
    producerRenderTime = info.startTime;
    expectedNextEditTime = info.startTime; // Playhead Sync directive: seed to match, so the very first call never reads as a spurious jump
    seekPending.store (false, std::memory_order_relaxed);

    // Reset both queues to a clean slate — every slot starts free, nothing
    // starts "ready" (there's nothing pre-rendered yet).
    int drained = 0;
    while (freeIndices.try_dequeue (drained)) {}
    while (readyQueue.try_dequeue (drained)) {}

    for (int i = 0; i < poolSize; ++i)
        freeIndices.try_enqueue (i);

    if (innerPlugin != nullptr)
        innerPlugin->initialise (info);

    // Zero Latency Override directive: classify this track BEFORE the
    // worker (or the audio thread) ever gets a chance to touch
    // innerPlugin, then keep re-checking at a modest rate on the message
    // thread — see refreshLiveModeState()'s own doc comment for why this
    // can never run on the audio thread or ShadowWorker's thread.
    refreshLiveModeState();
    startTimer (50); // ~20Hz — cheap, and armed/live-monitor toggles react within one tick

    worker = std::make_unique<ShadowWorker> (*this);
    worker->startThread (juce::Thread::Priority::background);
}

void CrateAnticipativeWrapper::deinitialise()
{
    hasBeenInitialised = false;
    stopTimer();

    // Stop the background worker BEFORE deinitialising innerPlugin or
    // releasing the pool — the worker calls innerPlugin->applyToBuffer() and
    // touches bufferPool, so it must be fully stopped first or this is a
    // straightforward use-after-deinitialise / use-after-free race.
    if (worker != nullptr)
    {
        worker->stopThread (2000);
        worker.reset();
    }

    if (innerPlugin != nullptr)
        innerPlugin->deinitialise();

    bufferPool.clear();

    // Disk-Backed Shadow Bouncing directive: close the streams (releasing
    // the handle promptly is good hygiene). Persistent Tour Cache
    // directive: shadowCacheFile itself is NOT deleted — deinitialise()
    // can run on a normal project Load/rebuild, and destroying the cache
    // every time that happens would defeat this entire step's purpose.
    diskReadStream.reset();
    diskWriteStream.reset();
    diskBlocksQueued = 0;
    diskWriteBlockIndex = 0;
    diskReadBlockIndex = 0;
}

bool CrateAnticipativeWrapper::renderNextShadowBlock()
{
    // Complete Cache Invalidation directive: consume the WORKER's own copy
    // of the invalidation signal first, every cycle — see the class's own
    // doc comment for why this is a separate atomic from flushRequested
    // (that one is exchange()'d by the audio thread; a second consumer
    // racing the same exchange() would only sometimes see it).
    if (diskFlushRequested.exchange (false, std::memory_order_acquire))
        resetDiskCache();

    // Zero Latency Override directive: "yield completely for this track
    // until it is disarmed" — combined with ShadowWorker::run()'s own
    // exponential backoff (this returning false repeatedly settles the
    // wait to the 50ms cap), this IS that yield: no render, no queue
    // activity, nothing but a cheap atomic check ~20 times/sec.
    if (liveModeRequired.load (std::memory_order_relaxed))
        return false;

    if (innerPlugin == nullptr || blockDurationSeconds <= 0.0)
        return false;

    // Playhead Sync directive: consume a pending seek BEFORE looking at the
    // (possibly now stale) cursor at all — producerRenderTime is exclusively
    // owned/mutated by THIS thread, so this is the only place it gets
    // written to reflect a jump the audio thread detected.
    if (seekPending.exchange (false, std::memory_order_acquire))
        producerRenderTime = tcore::TimePosition::fromSeconds (seekTargetSeconds.load (std::memory_order_relaxed));

    // JIT Spooler directive: draining an existing disk backlog into a
    // freshly-freed RAM slot takes PRIORITY over rendering a brand new
    // block — this is what keeps the backlog from growing without bound
    // once the audio thread is actually consuming again.
    if (diskBlocksQueued > 0)
    {
        int freeIndex = -1;

        if (! freeIndices.try_dequeue (freeIndex))
            return false; // RAM still full — nothing to do until the audio thread frees a slot

        auto& buffer = bufferPool[(size_t) freeIndex];

        if (! readBlockFromDisk (buffer))
        {
            // Shouldn't happen under normal operation (diskBlocksQueued is
            // our own accounting of exactly how many records are on disk)
            // — but never wedge the pipeline on a broken stream: return the
            // slot and resync the counter rather than looping forever.
            jassertfalse;
            freeIndices.try_enqueue (freeIndex);
            diskBlocksQueued = 0;
            return false;
        }

        --diskBlocksQueued;

        if (! readyQueue.try_enqueue (freeIndex))
        {
            jassertfalse;
            freeIndices.try_enqueue (freeIndex);
        }

        return true;
    }

    // Normal path: render a fresh block, into a RAM slot if one's free, or
    // into diskScratchBuffer (SSD overflow) if the pool is momentarily full
    // — "pool full" no longer means "nothing to do" (Disk-Backed Shadow
    // Bouncing directive).
    int freeIndex = -1;
    const bool haveRamSlot = freeIndices.try_dequeue (freeIndex);

    // Disk Queue Cap directive: checked BEFORE any work (no lock, no
    // render, no disk touch) — once the overflow tier is already at its
    // ceiling, do NOTHING and return false so ShadowWorker::run()'s
    // existing exponential backoff handles the yield, exactly like a full
    // RAM pool already does. No second wait/condition-variable mechanism
    // needed for "disk full" — it's the identical shape of problem Step 4
    // already solved for "RAM full".
    if (! haveRamSlot && diskBlocksQueued >= maxDiskBlocksQueued)
        return false;

    // Lock-Free Spinlock directive: test_and_set() returns the PREVIOUS
    // value — true means the audio thread's own fallback already holds the
    // lock right now. The worker CAN afford to wait (unlike the audio
    // thread), but waiting here would just be busy-work under a contended
    // lock — simpler and just as correct to hand the slot back (if we took
    // one) and let ShadowWorker::run()'s own backoff pace the retry.
    if (dspLock.test_and_set (std::memory_order_acquire))
    {
        if (haveRamSlot)
            freeIndices.try_enqueue (freeIndex);

        return false;
    }

    auto& targetBuffer = haveRamSlot ? bufferPool[(size_t) freeIndex] : diskScratchBuffer;
    targetBuffer.clear();

    renderOneBlockInto (targetBuffer); // advances producerRenderTime; handles Step 4.75 loop-boundary split internally

    dspLock.clear (std::memory_order_release);

    if (haveRamSlot)
    {
        if (! readyQueue.try_enqueue (freeIndex))
        {
            // Should be unreachable — readyQueue has the same capacity as
            // the total slot count, and only one slot is ever held outside
            // freeIndices at a time — but never silently drop a rendered
            // buffer's index without at least returning it to the free
            // list, or that slot leaks out of circulation forever.
            jassertfalse;
            freeIndices.try_enqueue (freeIndex);
        }
    }
    else
    {
        // SSD Overflow directive: RAM pool is full — append this rendered
        // block to the disk cache instead of discarding the work or
        // blocking waiting for room. Real, deliberate disk I/O — WORKER
        // THREAD ONLY, never reaches the audio thread.
        writeBlockToDisk (targetBuffer);
        ++diskBlocksQueued;
    }

    return true;
}

void CrateAnticipativeWrapper::renderOneBlockInto (juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const auto blockStart = producerRenderTime;
    const auto blockEnd   = blockStart + tcore::TimeDuration::fromSeconds (blockDurationSeconds);

    // Loop Awareness directive: TE's real graph already handles loop
    // wrapping for the audio thread's own fc (we just forward it in
    // renderSynchronously()) — this is the ONE place WE invented a time
    // range ourselves, so this is the only place that needs to simulate
    // the wrap. getLoopRange() is cheap (a cached member read), safe to
    // call from this background thread.
    const bool isLooping  = edit.getTransport().looping.get();
    const auto loopRange  = edit.getTransport().getLoopRange();

    if (isLooping && loopRange.getLength() > tcore::TimeDuration()
        && blockStart < loopRange.getEnd() && blockEnd > loopRange.getEnd())
    {
        // Boundary Wrap directive: split this ONE block into two
        // applyToBuffer() calls against two sub-ranges of the SAME buffer —
        // bufferStartSample/bufferNumSamples make this a first-class,
        // supported use of PluginRenderContext, not a hack. Segment A:
        // [blockStart, loopEnd) into the front samples. Segment B:
        // [loopStart, loopStart + remainder) into the rest, AS IF the loop
        // had already wrapped — because by the time these samples actually
        // play, it will have.
        const auto preLoopDuration = loopRange.getEnd() - blockStart;
        const int preLoopSamples = juce::jlimit (0, numSamples,
                                                   (int) std::round (preLoopDuration.inSeconds() * cachedInitInfo.sampleRate));
        const int postLoopSamples = numSamples - preLoopSamples;

        if (preLoopSamples > 0)
        {
            te::PluginRenderContext preContext (&buffer, juce::AudioChannelSet::stereo(),
                                                  0, preLoopSamples,
                                                  nullptr, 0.0,
                                                  tcore::TimeRange (blockStart, loopRange.getEnd()),
                                                  true, false, false, true);
            innerPlugin->applyToBuffer (preContext);
        }

        const auto postLoopStart = loopRange.getStart();
        auto postLoopEnd = postLoopStart;

        if (postLoopSamples > 0)
        {
            postLoopEnd = postLoopStart + tcore::TimeDuration::fromSeconds ((double) postLoopSamples / cachedInitInfo.sampleRate);

            te::PluginRenderContext postContext (&buffer, juce::AudioChannelSet::stereo(),
                                                   preLoopSamples, postLoopSamples,
                                                   nullptr, 0.0,
                                                   tcore::TimeRange (postLoopStart, postLoopEnd),
                                                   true, false, false, true);
            innerPlugin->applyToBuffer (postContext);
        }

        // Cursor continues from the WRAPPED position, never from the raw
        // incrementing blockEnd — that position is one the transport is
        // simply never going to reach next.
        producerRenderTime = postLoopEnd;
    }
    else
    {
        te::PluginRenderContext shadowContext (&buffer,
                                                juce::AudioChannelSet::stereo(),
                                                0, numSamples,
                                                nullptr, 0.0,
                                                tcore::TimeRange (blockStart, blockEnd),
                                                true, false, false, true);

        innerPlugin->applyToBuffer (shadowContext);

        producerRenderTime = blockEnd;
    }
}

void CrateAnticipativeWrapper::writeBlockToDisk (const juce::AudioBuffer<float>& buffer)
{
    if (diskWriteStream == nullptr || bytesPerDiskBlock <= 0)
        return;

    // Fixed-Size Ring directive: explicit modulo-wrapped position, NOT a
    // natural sequential append — see the header's own doc comment for the
    // unbounded-file-growth bug an append-only design caused. diskBlocksQueued
    // (checked before every write, see renderNextShadowBlock()) guarantees
    // this slot was already consumed (or never written) before we overwrite
    // it — the writer can never lap the reader in a ring this size.
    const int64_t slot = diskWriteBlockIndex % (int64_t) maxDiskBlocksQueued;
    diskWriteStream->setPosition (slot * bytesPerDiskBlock);

    // No explicit flush() — the stream's own internal buffering (default
    // 0x8000 bytes) is fine for a transient overflow cache with no
    // durability requirement; if the app crashes, this file is meaningless
    // scratch data either way.
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        diskWriteStream->write (buffer.getReadPointer (ch), (size_t) buffer.getNumSamples() * sizeof (float));

    ++diskWriteBlockIndex;
}

bool CrateAnticipativeWrapper::readBlockFromDisk (juce::AudioBuffer<float>& buffer)
{
    if (diskReadStream == nullptr || bytesPerDiskBlock <= 0)
        return false;

    // Fixed-Size Ring directive: same explicit modulo-wrapped positioning
    // as the writer, read from THIS thread's own independent cursor.
    const int64_t slot = diskReadBlockIndex % (int64_t) maxDiskBlocksQueued;
    diskReadStream->setPosition (slot * bytesPerDiskBlock);

    const int bytesPerChannel = buffer.getNumSamples() * (int) sizeof (float);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        if (diskReadStream->read (buffer.getWritePointer (ch), bytesPerChannel) != bytesPerChannel)
            return false;

    ++diskReadBlockIndex;
    return true;
}

void CrateAnticipativeWrapper::resetDiskCache()
{
    // Fixed-Size Ring directive: the file itself keeps its pre-allocated
    // size forever — a flush just rewinds both cursors back to slot 0,
    // there's no truncate()/regrow churn to repeat on every invalidation
    // the way the old append-log design needed.
    diskWriteBlockIndex = 0;
    diskReadBlockIndex = 0;
    diskBlocksQueued = 0;
}

void CrateAnticipativeWrapper::applyToBuffer (const te::PluginRenderContext& fc)
{
    // Playhead Sync directive: fc.editTime is the engine's own ground-truth
    // time for this block — compare its start against what we expected
    // (the previous call's own end) to detect a seek/scrub/stop-restart/
    // loop jump the worker's synthetic cursor doesn't know about yet. Half
    // a block's duration of tolerance absorbs ordinary floating-point noise
    // between calls without missing a real discontinuity (real seeks are
    // practically always much larger than that).
    const auto expected = expectedNextEditTime;
    expectedNextEditTime = fc.editTime.getEnd();

    if (blockDurationSeconds > 0.0
        && std::abs ((fc.editTime.getStart() - expected).inSeconds()) > blockDurationSeconds * 0.5)
    {
        // Reuse the EXACT SAME cache-flush mechanism Step 4.5 built for
        // stale parameters — a discontinuity makes every currently-queued
        // buffer just as stale as a parameter twist does. Both flags: RAM
        // (flushRequested, audio-thread-consumed) AND disk (diskFlushRequested,
        // worker-thread-consumed) — Step 4.9 extends invalidation to the disk tier.
        flushRequested.store (true, std::memory_order_relaxed);
        diskFlushRequested.store (true, std::memory_order_relaxed);

        // Tell the worker where to resync — producerRenderTime itself is
        // never written from this thread (see the header's own doc comment
        // on why: it's exclusively owned/mutated by the ShadowWorker
        // thread, and a second writer would be a torn-read/write race on a
        // non-atomic type).
        seekTargetSeconds.store (fc.editTime.getStart().inSeconds(), std::memory_order_relaxed);
        seekPending.store (true, std::memory_order_release);
    }

    // Zero Latency Override directive: checked next, cheap atomic read.
    // A record-armed or live-monitored track never touches the queue at
    // all — not even to check it — full stop.
    if (liveModeRequired.load (std::memory_order_relaxed))
    {
        renderSynchronously (fc);
        return;
    }

    // Cache Invalidation / Stale Data Flush directive: innerPlugin's own
    // parameters changed since whatever's currently in readyQueue was
    // rendered — every one of those buffers reflects OLD values and must
    // not be played back as if current. Flush them all back to the free
    // list and process THIS block synchronously; normal queue consumption
    // resumes next block once the worker has caught up with the new values.
    if (flushRequested.exchange (false, std::memory_order_acq_rel))
    {
        int stale = -1;

        while (readyQueue.try_dequeue (stale))
            freeIndices.try_enqueue (stale);

        renderSynchronously (fc);
        return;
    }

    int readyIndex = -1;

    if (readyQueue.try_dequeue (readyIndex))
    {
        // Cache Hit — copy the pre-rendered block into destBuffer.
        // innerPlugin is NOT touched on this (audio) thread at all.
        auto& source = bufferPool[(size_t) readyIndex];

        if (fc.destBuffer != nullptr)
        {
            const int numChannels = juce::jmin (fc.destBufferChannels.size(), source.getNumChannels());
            const int numSamples  = juce::jmin (fc.bufferNumSamples, source.getNumSamples());

            for (int ch = 0; ch < numChannels; ++ch)
                fc.destBuffer->copyFrom (ch, fc.bufferStartSample, source, ch, 0, numSamples);
        }

        freeIndices.try_enqueue (readyIndex); // hand the slot back for the producer to reuse
    }
    else
    {
        // Cache Miss / Synchronous Fallback directive: the worker hasn't
        // produced a block in time.
        renderSynchronously (fc);
    }
}

void CrateAnticipativeWrapper::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    // Delegate entirely to innerPlugin — this wrapper has no state of its
    // own yet to reconcile against v (see the header's own doc comment on
    // why a later step, once real nested-child persistence is designed,
    // will need to revisit this rather than a flat pass-through).
    if (innerPlugin != nullptr)
        innerPlugin->restorePluginStateFromValueTree (v);
}

void CrateAnticipativeWrapper::flushPluginStateToValueTree()
{
    te::Plugin::flushPluginStateToValueTree(); // base behaviour for this wrapper's own state (enabled/bypassed, automatable params if any land later)

    if (innerPlugin != nullptr)
        innerPlugin->flushPluginStateToValueTree();
}
