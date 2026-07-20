#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <readerwriterqueue.h>

#include <vector>
#include <memory>
#include <atomic>
#include <algorithm>
#include <set>

namespace te = tracktion::engine;
namespace tcore = tracktion::core;

/**
    Anticipative FX ("Shadow Freezing") — proprietary Crate engine extension
    (see Docs/MASTER_ARCHITECTURE.md Section 9.1: Tracktion Engine's public
    SDK has no runtime Anticipative FX API, so this is built as a wrapper on
    top of the engine, not a patch into it).

    STEP 1 (Skeleton & Dependencies) built a te::Plugin that compiles, can be
    registered as a built-in type, and transparently bypasses audio.

    STEP 2 (Synchronous Proxy Forwarding & Registration) proved this wrapper
    is a perfect, transparent SYNCHRONOUS proxy for a real inner te::Plugin —
    every lifecycle/DSP call the engine makes on the wrapper forwarded
    straight through to innerPlugin, on the SAME thread, in the SAME call.
    Verified under the 100-track stress test before moving on.

    STEP 3 (Asynchronous Background Rendering / "Shadow Freeze") — THIS step.
    applyToBuffer() no longer calls innerPlugin directly by default. A
    background ShadowWorker thread renders innerPlugin AHEAD of the audio
    thread into a fixed pool of pre-allocated buffers (Pre-Allocated Memory
    Pool directive — allocated ONLY in initialise(), never in
    applyToBuffer()), and hands finished buffer indices to the audio thread
    through a moodycamel::ReaderWriterQueue<int> (Wait-Free Consumer
    directive). Cache hit: the audio thread copies a pre-rendered buffer,
    touching innerPlugin not at all. Cache miss (worker hasn't caught up
    yet): Synchronous Fallback directive — call innerPlugin->applyToBuffer()
    directly, exactly like Step 2, so audio can never drop.

    STEP 4 (Thread Safety & Smart Yielding) closed the two real gaps Step 3
    knowingly shipped with:

    1. THE DATA RACE — the Synchronous Fallback path (audio thread) and the
       ShadowWorker's own render call (background thread) both used to call
       innerPlugin->applyToBuffer() with no mutual exclusion at all.
       Resolved with dspLock, a std::atomic_flag spinlock — NOT a
       std::mutex, which can suspend a thread via the OS scheduler and has
       no place anywhere near the audio thread. The worker (which CAN
       afford to wait) simply skips its cycle and hands the pool slot back
       if the lock is already held. The audio thread (which CANNOT wait,
       ever) never spins on a failed test_and_set — it drops straight to
       destBuffer->clear() instead: a micro-dropout (one block of silence)
       is an acceptable, bounded cost; blocking or racing the DSP state is
       not.

    2. THE IDLE SPIN — a fixed wait(1) meant every idle worker thread woke
       ~1000 times/sec forever, regardless of whether anything was even
       playing (measured: 100 idle tracks cost ~12.6% of a 12-core machine
       for zero useful work). Resolved with exponential backoff in
       ShadowWorker::run() itself: the wait duration doubles (capped at
       50ms) each consecutive cycle that produces nothing, and resets to
       1ms the instant real work resumes — so idle tracks settle to ~20
       wakeups/sec each instead of ~1000, while a track that starts
       producing again reacts within one wait cycle, not up to 50ms of dead
       air.

    STEP 4.5 (The Hermetic Seal — Live Mode & Cache Invalidation):

    1. ZERO LATENCY OVERRIDE — a record-armed or live-monitored track MUST
       NEVER go through the anticipative queue, full stop: any queued
       latency at all is unacceptable when a performer is listening to
       their own input. liveModeRequired is a std::atomic<bool> applyToBuffer()
       reads with a cheap, lock-free load. Critically, the ACTUAL check
       (walking edit.getCurrentPlaybackContext()->getAllInputs(), which
       returns a juce::Array BY VALUE and therefore ALLOCATES) does NOT run
       on the audio thread, or even on ShadowWorker's thread — it runs on a
       juce::Timer ticking on the MESSAGE thread (refreshLiveModeState(),
       ~20Hz), which is the only place in this class that check could ever
       run without violating the same Zero Allocation Rule Step 3 built
       around. The audio thread only ever sees the cached bool.
       renderNextShadowBlock() checks the same flag and yields (returns
       false, doing nothing) the instant it's set — combined with Step 4's
       existing exponential backoff, an armed track's worker settles to
       ~20 wakeups/sec doing nothing at all, which IS "yield completely
       until disarmed" in practice.

    2. STRICT PDC REPORTING — getLatencySeconds() below returns ONLY
       innerPlugin->getLatencySeconds(); the 32-buffer queue is a scheduling
       mechanism, not a real signal-path delay (a cache hit plays back a
       block that innerPlugin rendered in the exact same conceptual
       position an in-place call would have), so it must contribute
       NOTHING to the reported figure. Unchanged since Step 2/3 — this note
       exists because "verify it stayed correct" is itself the deliverable,
       not just "it happens to be right."

    3. CACHE INVALIDATION — CrateAnticipativeWrapper is itself a
       te::AutomatableParameter::Listener, attached to EVERY one of
       innerPlugin's parameters (attachParameterListeners(), called from
       setInnerPlugin()). Any value or curve change sets flushRequested
       (std::atomic<bool>, message thread writes true, audio thread does
       the one read-and-clear via exchange()) — applyToBuffer() checks this
       FIRST, drains every now-stale buffer out of readyQueue back to
       freeIndices, and processes that one block synchronously so the very
       next output sample already reflects the new parameter value instead
       of up to poolSize blocks of stale, pre-Twist audio.

    STEP 4.75 (Transport Reconciliation & Loop Awareness) closed the
    remaining gap explicitly flagged at the end of Step 4.5: producerRenderTime
    was a synthetic cursor that only ever counted forward, oblivious to a
    real seek/scrub or a loop boundary — meaning the worker could render
    audio for a time position the transport was never actually going to
    reach next, and the audio thread would eventually play it back as if it
    had.

    1. PLAYHEAD SYNC (Seek/Scrub) — detected on the AUDIO thread, for free,
       using data it already receives every call: fc.editTime is the
       engine's own ground-truth time range for this exact block. This
       class tracks expectedNextEditTime (audio-thread-owned) and compares
       it against fc.editTime.getStart() on every call; a gap bigger than
       half a block's duration can only mean a discontinuity — a seek,
       scrub, stop/restart, or a loop jump the worker didn't yet know
       about. On detection: reuse the EXACT SAME flush path Step 4.5 built
       for stale parameters (flushRequested), so the current block still
       processes correctly and the stale queue empties immediately: and
       publish the new position through seekPending/seekTargetSeconds — two
       more atomics, NOT a direct write to producerRenderTime, because that
       field is exclusively owned and mutated by the ShadowWorker thread;
       the audio thread writing it directly would be a torn-read/write race
       on a non-atomic type. The worker consumes the pending seek at the
       very top of its own render function, before it ever looks at the
       (now-stale) old cursor value.

    2. LOOP AWARENESS (Boundary Wrap) — checked on the WORKER thread only
       (the audio thread's own fallback just forwards the engine's already-
       correct fc; TE's real graph already handles loop wrapping there, we
       only have to simulate it for OUR OWN invented look-ahead cursor).
       Before rendering, renderNextShadowBlock() checks whether
       [producerRenderTime, producerRenderTime + blockDuration) crosses
       edit.getTransport().getLoopRange()'s end point. If it does, the
       block is SPLIT into two innerPlugin->applyToBuffer() calls against
       two sub-ranges of the SAME pooled buffer (te::PluginRenderContext's
       own bufferStartSample/bufferNumSamples make this a first-class,
       supported thing to do, not a hack) — the front samples render
       [cursor, loopEnd) with real time advancing normally, the remaining
       samples render [loopStart, loopStart + remainder) as if the loop had
       already wrapped, and the cursor itself is set to loopStart +
       remainder afterward, NOT to the raw incrementing blockEnd. A loop
       shorter than one block's duration (so short the wrapped remainder
       ALSO overruns the loop end) is a known, deliberately unhandled edge
       case — flagged, not silently ignored — this splits exactly once per
       block, matching every realistic musical loop length.

    STEP 4.8 (Live-State Infection / Graph Routing Awareness) — "Infinite
    Shadow Bouncing" (pre-rendering massive sessions while hosting
    zero-latency live inputs elsewhere in the SAME session) requires
    liveModeRequired to mean more than "is THIS track itself armed" — a
    live vocal track feeding a reverb bus makes the REVERB unsafe to
    anticipate too (its wet return would otherwise be poolSize blocks
    stale relative to the live dry signal reaching the listener). Domino
    Effect: Vocal (live) -> Reverb bus -> Master must ALL evaluate live.

    refreshLiveModeState() (still the ONLY place this runs — the message
    thread, ~20Hz, never the audio thread) now walks the routing graph
    BACKWARDS from this wrapper's own track via isTrackOrUpstreamLive(), a
    recursive DFS covering BOTH routing mechanisms TE exposes, since
    neither alone is the whole graph:

      - DIRECT SERIAL OUTPUT: Track::getInputTracks() — already walks the
        FULL downstream chain itself per its own doc comment ("true if any
        downstream tracks match this one"), so recursing through it alone
        already covers a multi-hop A->B->C serial chain, not just one hop.

      - AUX SEND/RETURN: a send is a SEPARATE mechanism, matched by BUS
        NUMBER rather than a direct track reference (see SendBusUtils.h's
        own doc comment) — getInputTracks() never sees it. This wrapper's
        own track's AuxReturnPlugin::busNumber (if it has one) is matched
        against every OTHER track's AuxSendPlugin::getBusNumber() in the
        Edit to find direct senders into this bus.

    A std::set<Track*> visited guards against a cycle in a misconfigured
    routing graph turning this into infinite recursion.

    KNOWN SCALING CAVEAT (flagged, not solved here): this is a per-instance,
    independent recursive walk — EVERY track's own wrapper re-derives the
    WHOLE upstream graph on its OWN 20Hz timer, so total message-thread work
    scales roughly O(N^2) in track count in the worst case (a fully-chained
    or fully-bussed session). For the sessions this step was validated
    against that's still trivially cheap in absolute terms, but a genuinely
    massive "50GB session" is exactly where a single shared, per-Edit
    computation (cached once per tick, looked up by every wrapper) becomes
    the right answer instead of N independent walks — that shared/"central
    manager" evolution is real future work, not done here.

    STEP 4.9 (Disk-Backed Shadow Bouncing) extends the RAM ring buffer with
    an SSD overflow so a track can be rendered arbitrarily far ahead — not
    just poolSize (32) blocks — without growing the RAM pool. The audio
    thread's own contract is UNCHANGED and absolute: it still only ever
    touches the moodycamel queues, never the disk, never a file handle.

    THE OVERFLOW PATH: when renderNextShadowBlock() can't get a free RAM
    slot (freeIndices exhausted) AND there's no disk backlog already queued,
    it renders into diskScratchBuffer instead — a buffer dedicated to this
    path so overflow rendering never needs (or waits for) a RAM slot — then
    appends those raw PCM samples to shadowCacheFile via diskWriteStream and
    increments diskBlocksQueued. This is real, deliberate, BLOCKING disk
    I/O — on the WORKER thread ONLY, which has been allowed to wait since
    Step 3; the point of this whole class is keeping that away from the
    audio thread, not eliminating it.

    THE JIT SPOOLER: whenever diskBlocksQueued > 0, the NEXT free RAM slot
    (the instant the audio thread's own cache-hit path returns one to
    freeIndices) is filled by reading the next sequential block back off
    disk via diskReadStream, NOT by rendering a fresh one — draining the
    backlog takes priority over generating more of it. producerRenderTime
    and loop-boundary splitting (Step 4.75) are shared, unchanged logic
    between both paths (factored into renderOneBlockInto()) — the cursor
    doesn't know or care whether its output is about to land in RAM or on
    disk.

    COMPLETE CACHE INVALIDATION: flushRequested (Step 4.5/4.75) is consumed
    via exchange() by the AUDIO thread — a second consumer would race it.
    So diskFlushRequested is a SEPARATE atomic<bool>, set at the exact same
    call sites as flushRequested (the parameter listener; the seek/scrub
    discontinuity detector) but consumed independently by the WORKER thread,
    which rewinds the ring cursors back to slot 0 and resets diskBlocksQueued
    to 0 — the worker starts bouncing from scratch at the new position,
    exactly like the RAM-only path already did, just extended to the disk
    tier too.

    THE DISK QUEUE CAP: "infinite" look-ahead still has to stop somewhere —
    verified empirically that it does NOT stop on its own: 100 stress-test
    tracks (created but never played back, so nothing ever drains the RAM
    queue) wrote ~1.9GB to temp files in well under a minute with no cap in
    place. maxDiskBlocksQueued (4096 — ~43.7s of look-ahead at a common
    48kHz/512-sample config) is a hard ceiling checked BEFORE the overflow
    path does any work: once hit, renderNextShadowBlock() returns false
    without touching dspLock, the disk, or diskScratchBuffer at all — the
    EXACT SAME exponential backoff Step 4 built for "RAM pool full" handles
    "disk pool full" too, for free, with no second wait/condition-variable
    mechanism to get right. A capped-out track settles to the same ~20
    wakeups/sec idle cost every other idle track already has, and notices
    room again (worst case) within one backoff cycle once the JIT Spooler
    above drains the backlog back under the cap.

    STEP 4.95 (Persistent Tour Cache) — the "5-minute stage changeover"
    requirement: re-rendering a full 50GB session's worth of Anticipative
    FX from scratch on every load burns CPU a live show cannot spare, so the
    ring is no longer a juce::TemporaryFile (auto-deleted on destruction) —
    it's a plain, persistent juce::File under `[ProjectFolder]/ShadowCache/
    <TrackID>.crateshadow`, keyed by the track's own EditItemID (stable
    across save/load of the same .crate project, so the same physical track
    maps to the same cache file every time the project reopens). Unsaved
    projects (no .crate file yet, no meaningful "next to the project"
    location) fall back to the OS temp directory exactly as before — Tour
    Cache only applies to a project that's actually been saved somewhere.

    On initialise(), IF that file already exists AND is exactly
    bytesPerDiskBlock * maxDiskBlocksQueued bytes (the expected fixed-ring
    size for the CURRENT sample rate/block size/channel count — not a
    hardcoded "16MB" literal, since that number only holds at a common
    48kHz/512-sample config; a different config gets a different expected
    size, computed the same way both times), it is treated as a FULLY
    POPULATED ring from a previous session: diskReadBlockIndex resets to 0,
    diskWriteBlockIndex is set to maxDiskBlocksQueued, and diskBlocksQueued
    is set to maxDiskBlocksQueued — the entire cache is "ready to consume"
    the instant the plugin loads, so the JIT Spooler can start feeding the
    audio thread from disk immediately, with ZERO fresh rendering needed
    until that whole backlog is exhausted. That's the "instant load, 0% CPU
    overhead" behavior this step exists for. If the file is missing, wrong
    size (a different session, a different block size, a corrupted
    leftover), or a parameter change/seek fires flushRequested later, it's
    treated exactly like Step 4.9's original "no usable cache" case: freshly
    pre-allocated, empty, and the worker starts bouncing from scratch.
*/
class CrateAnticipativeWrapper : public te::Plugin,
                                  private juce::Timer,
                                  private te::AutomatableParameter::Listener
{
public:
    explicit CrateAnticipativeWrapper (te::PluginCreationInfo);
    ~CrateAnticipativeWrapper() override;

    //==============================================================================
    static const char* getPluginName()   { return NEEDS_TRANS ("Anticipative FX (Shadow Freeze)"); }
    static const char* xmlTypeName;

    juce::String getName() const override              { return TRANS ("Anticipative FX"); }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getSelectableDescription() override    { return TRANS ("Anticipative FX (Shadow Freeze) Plugin"); }

    // Inner Plugin Ownership directive: the real plugin this wrapper proxies
    // for (a built-in EQ/Compressor today for verification purposes, a
    // hosted VST3 once that lands). Safe to call before OR after this
    // wrapper's own initialise()/deinitialise() — see setInnerPlugin()'s own
    // doc comment for why the ordering is handled explicitly rather than
    // assumed.
    void setInnerPlugin (te::Plugin::Ptr newPlugin);
    te::Plugin::Ptr getInnerPlugin() const noexcept { return innerPlugin; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;

    // Checked in this exact order, every call: (0) Playhead Sync — compares
    // fc.editTime against expectedNextEditTime; a discontinuity publishes a
    // seek to the worker AND falls through into the SAME flush handling as
    // (2); (1) Zero Latency Override — live/armed tracks go straight to
    // renderSynchronously(), queue untouched; (2) Cache Invalidation — a
    // pending parameter change (or the seek just detected) flushes
    // readyQueue and processes this one block synchronously; (3) Wait-Free
    // Consumer — try_dequeue a pre-rendered buffer index (Cache Hit, copy
    // into destBuffer, innerPlugin untouched on this thread) or fall back
    // to renderSynchronously() (Cache Miss). Never allocates, never blocks.
    void applyToBuffer (const te::PluginRenderContext&) override;

    // Strict PDC Reporting directive: ONLY innerPlugin's own reported
    // latency — the 32-buffer queue is a scheduling mechanism, not a
    // signal-path delay, and must never leak a phantom latency figure into
    // Tracktion's plugin-delay-compensation math. Unchanged since Step 2/3;
    // see the class's own doc comment for why that's the correct answer,
    // not an oversight.
    double getLatencySeconds() override { return innerPlugin != nullptr ? innerPlugin->getLatencySeconds() : 0.0; }

    // XML State Forwarding directive: Tracktion recreates plugins from a
    // ValueTree on load, and flushes CURRENT values back into one on save —
    // delegate both to innerPlugin so ITS state (an EQ band, a compressor
    // threshold, eventually a hosted VST3's own chunk data) isn't silently
    // dropped just because it's sitting behind this proxy instead of being
    // inserted directly.
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;
    void flushPluginStateToValueTree() override;

private:
    // The Background Worker (The Producer) directive: a thin juce::Thread
    // that just drives CrateAnticipativeWrapper::renderNextShadowBlock() in
    // a loop, throttling itself via wait() whenever the pool is momentarily
    // exhausted rather than busy-spinning a core to 100%. All the actual
    // pool/queue logic lives on the owner (renderNextShadowBlock()) so this
    // class stays a pure driver, not a second copy of the state.
    class ShadowWorker : public juce::Thread
    {
    public:
        explicit ShadowWorker (CrateAnticipativeWrapper& ownerToUse)
            : juce::Thread ("Crate Shadow Worker"), owner (ownerToUse) {}

        void run() override
        {
            // Exponential Backoff directive: resets to 1ms the instant real
            // work resumes, doubles (capped at 50ms) each consecutive cycle
            // that produces nothing — an idle track settles to ~20
            // wakeups/sec instead of polling at ~1000/sec forever.
            int sleepTimeMs = 1;

            while (! threadShouldExit())
            {
                if (owner.renderNextShadowBlock())
                {
                    sleepTimeMs = 1;
                }
                else
                {
                    wait (sleepTimeMs);
                    sleepTimeMs = std::min (sleepTimeMs * 2, 50);
                }
            }
        }

    private:
        CrateAnticipativeWrapper& owner;
    };

    // Renders exactly one shadow block if a free pool slot is available AND
    // dspLock isn't currently held by the audio thread's own fallback.
    // Returns false or (nothing rendered — pool full and disk overflow
    // unavailable, or lock contended) which is ShadowWorker::run()'s cue to
    // back off rather than spin. Also yields immediately (Zero Latency
    // Override) if liveModeRequired. Disk-Backed Shadow Bouncing directive:
    // "pool full" no longer means "nothing to do" — see the class's own
    // doc comment for the JIT-spool-drain-first, overflow-to-disk-second
    // priority order this orchestrates.
    bool renderNextShadowBlock();

    // Disk-Backed Shadow Bouncing directive: the actual DSP render for ONE
    // block — advances producerRenderTime and handles the Step 4.75
    // loop-boundary split — factored out of renderNextShadowBlock() so it's
    // one shared implementation regardless of whether `buffer` is a RAM
    // pool slot or diskScratchBuffer; producerRenderTime doesn't know or
    // care which.
    void renderOneBlockInto (juce::AudioBuffer<float>& buffer);

    // Disk-Backed Shadow Bouncing directive: raw, fixed-size (numChannels *
    // blockSizeSamples * sizeof(float)) sequential PCM records — no header,
    // since channel count/block size are already known from cachedInitInfo.
    // Both are WORKER-THREAD-ONLY; real, deliberate blocking I/O is fine
    // here (see the class's own doc comment) and must never reach
    // applyToBuffer(). writeBlockToDisk() appends to diskWriteStream's
    // current (end-of-file) position; readBlockFromDisk() reads
    // sequentially from diskReadStream's own independent position — the two
    // streams' positions are never the same value except right after
    // resetDiskCache().
    void writeBlockToDisk (const juce::AudioBuffer<float>& buffer);
    bool readBlockFromDisk (juce::AudioBuffer<float>& buffer);

    // Complete Cache Invalidation directive: rewinds both ring cursors
    // (diskWriteBlockIndex/diskReadBlockIndex) and diskBlocksQueued back to
    // 0 — the persistent file's own on-disk bytes are simply overwritten in
    // place by subsequent writes, never explicitly erased. Called ONLY from
    // the worker thread (via diskFlushRequested, a SEPARATE atomic from
    // flushRequested; see the class's own doc comment for why one flag
    // can't safely have two independent consumers).
    void resetDiskCache();

    // Lock-Free Spinlock directive: the ONE place that calls
    // innerPlugin->applyToBuffer() from the audio thread — used by the Zero
    // Latency Override, Cache Invalidation, and Cache Miss paths alike, so
    // there is exactly one implementation of "acquire dspLock or silence"
    // to get right instead of three copies drifting apart.
    void renderSynchronously (const te::PluginRenderContext&);

    // Zero Latency Override directive: the EXPENSIVE, allocating check
    // (walks the routing graph, edit.getCurrentPlaybackContext()->
    // getAllInputs(), te::getAudioTracks()) — runs ONLY from
    // timerCallback() on the message thread, never the audio thread or
    // ShadowWorker. Publishes its result to liveModeRequired.
    void refreshLiveModeState();
    void timerCallback() override { refreshLiveModeState(); }

    // Live-State Infection / Graph Routing Awareness directive: does
    // ONLY the local check — is `track` itself record-armed or
    // live-monitored — no graph walk. The base case for
    // isTrackOrUpstreamLive()'s recursion below.
    bool isTrackDirectlyLive (te::Track& track) const;

    // The Domino Effect directive: true if `track` is itself live, OR if
    // ANY upstream track feeding it (via direct serial output OR an Aux
    // Send targeting `track`'s own return bus) is live, recursively — a
    // live vocal feeding a reverb bus feeding Master makes all three
    // evaluate true. `visited` is a cycle guard (and de-dup) shared across
    // the whole recursive walk from one refreshLiveModeState() tick; see
    // the class's own doc comment for the two routing mechanisms traced
    // and the known O(N^2)-across-all-wrappers scaling caveat.
    bool isTrackOrUpstreamLive (te::Track* track, std::set<te::Track*>& visited) const;

    // Cache Invalidation directive: attaches/detaches this wrapper as a
    // te::AutomatableParameter::Listener on every one of innerPlugin's
    // parameters — called from setInnerPlugin() so a replaced innerPlugin
    // never leaves a listener registered on a Ptr we no longer own.
    void attachParameterListeners();
    void detachParameterListeners();
    // Complete Cache Invalidation directive: both flags set together, every
    // time — flushRequested for the audio thread's own RAM-queue drain,
    // diskFlushRequested for the worker thread's disk-cache truncate. Two
    // atomics because they're consumed by two different threads via two
    // independent exchange() calls (see each flag's own doc comment).
    void currentValueChanged (te::AutomatableParameter&) override
    {
        flushRequested.store (true, std::memory_order_release);
        diskFlushRequested.store (true, std::memory_order_release);
    }

    void curveHasChanged (te::AutomatableParameter&) override
    {
        flushRequested.store (true, std::memory_order_release);
        diskFlushRequested.store (true, std::memory_order_release);
    }

    te::Plugin::Ptr innerPlugin;

    // Zero Latency Override / Cache Invalidation directives — both are
    // plain std::atomic<bool>: written from the message thread (a Timer
    // tick; a parameter-change callback), read from the audio thread with
    // the cheapest possible lock-free load/exchange. Never a mutex, never
    // anything that could suspend the audio thread waiting for the message
    // thread to get around to it.
    std::atomic<bool> liveModeRequired { false };
    std::atomic<bool> flushRequested   { false };

    // Playhead Sync directive: audio-thread-owned ground-truth cursor,
    // updated to fc.editTime.getEnd() at the end of every applyToBuffer()
    // call — compared against the START of the NEXT call's fc.editTime to
    // detect a discontinuity (seek/scrub/loop jump the real engine just
    // made that the worker's own synthetic cursor doesn't know about yet).
    tcore::TimePosition expectedNextEditTime;

    // Playhead Sync directive: the handoff from "audio thread detected a
    // jump" to "worker thread should re-sync its cursor" — two more plain
    // atomics, deliberately NOT a direct write to producerRenderTime (that
    // field is exclusively owned/mutated by the ShadowWorker thread; a
    // second writer would be a torn-read/write race on a non-atomic type).
    // The worker consumes this at the very top of renderNextShadowBlock().
    std::atomic<bool> seekPending { false };
    std::atomic<double> seekTargetSeconds { 0.0 };

    // Lock-Free Spinlock directive: guards every call into innerPlugin-
    // >applyToBuffer() so the worker thread and the audio thread's own
    // Synchronous Fallback can never execute inside it at the same time.
    // std::atomic_flag specifically (NOT std::mutex) — the audio thread
    // must NEVER be able to block on this, only test_and_set() and bail to
    // silence if it's contended.
    std::atomic_flag dspLock = ATOMIC_FLAG_INIT;

    // setInnerPlugin() ordering directive: caches the info the ENGINE handed
    // this wrapper's own initialise() so a newly-assigned innerPlugin that
    // arrives AFTER the wrapper is already live in the graph still gets its
    // own initialise() call immediately — nothing else would ever trigger
    // it otherwise, and a never-initialised inner plugin would silently
    // process at sampleRate/blockSize 0.
    bool hasBeenInitialised = false;
    te::PluginInitialisationInfo cachedInitInfo {};

    // Pre-Allocated Memory Pool directive: fixed-size, allocated ONLY in
    // initialise() — poolSize is arbitrary headroom (32 in-flight blocks is
    // generously more than a background thread should ever need to get
    // ahead of real-time by), not a tuned value.
    static constexpr int poolSize = 32;
    std::vector<juce::AudioBuffer<float>> bufferPool;

    // Two queues, not the one the directive names literally — see the
    // class's own doc comment: readyQueue alone (producer picks the next
    // pool slot via a bare incrementing counter) can only be race-free if
    // the consumer is GUARANTEED to have already read a slot before the
    // producer wraps back around to reuse it, which is an assumption, not a
    // guarantee. freeIndices closes that gap: a slot is only ever available
    // to the producer once the consumer has explicitly handed it back,
    // making slot reuse provably safe instead of merely probably safe.
    // Both are sized to poolSize up front and accessed ONLY via try_enqueue/
    // try_dequeue (never the plain enqueue()/dequeue() that CAN allocate on
    // growth) — Zero Allocation Rule, enforced by which API is called, not
    // just by convention.
    moodycamel::ReaderWriterQueue<int> freeIndices { (size_t) poolSize };
    moodycamel::ReaderWriterQueue<int> readyQueue   { (size_t) poolSize };

    // Producer-side synthetic transport cursor — see the class's own doc
    // comment on why this doesn't yet react to the real transport.
    tcore::TimePosition producerRenderTime;
    double blockDurationSeconds = 0.0;

    // Disk-Backed Shadow Bouncing directive: the SSD overflow tier, WORKER-
    // THREAD-ONLY (see the class's own doc comment).
    //
    // Persistent Tour Cache directive (Step 4.95): shadowCacheFile is a
    // PLAIN juce::File, not a juce::TemporaryFile — it deliberately does
    // NOT auto-delete on destruction any more. It lives at
    // [ProjectFolder]/ShadowCache/<TrackID>.crateshadow (or the OS temp
    // directory, for an unsaved project with no meaningful project-relative
    // location yet) and survives across app restarts on purpose, so a
    // previously-bounced session can resume "instantly, 0% CPU" instead of
    // re-rendering from scratch. The streams are still closed explicitly in
    // deinitialise() (good hygiene, releases the handle promptly), but that
    // no longer triggers any deletion.
    //
    // FIXED-SIZE RING, NOT AN APPEND LOG: an earlier version of this cache
    // used plain sequential append/read positions, capped only by a LOGICAL
    // block-count check before each write. That bounded how many blocks
    // were queued at once but NOT the file's physical size — every write
    // still advanced further into an ever-growing file regardless of how
    // much earlier data had already been read back, so as long as ANY
    // reads happened (letting the logical count dip below the cap), writes
    // resumed past where they left off instead of reusing already-consumed
    // space. Measured: 100 stress-test tracks grew ~50MB/s combined with NO
    // sign of slowing over 100+ seconds — the logical cap was doing nothing
    // to the file on disk. Fixed by pre-allocating the file to EXACTLY
    // maxDiskBlocksQueued slots and wrapping both diskWriteBlockIndex and
    // diskReadBlockIndex modulo that count — the file's size is now a hard,
    // physical ceiling, not just a logical one. diskBlocksQueued (the
    // in-flight count, still checked before every write) guarantees the
    // writer can never lap the reader in this now-genuinely-circular file.
    juce::File shadowCacheFile;
    std::unique_ptr<juce::FileOutputStream> diskWriteStream;
    std::unique_ptr<juce::FileInputStream> diskReadStream;

    // A render target dedicated to the disk-overflow path — deliberately
    // NOT one of the poolSize RAM buffers, so overflow rendering never
    // needs (or waits for) a free RAM slot.
    juce::AudioBuffer<float> diskScratchBuffer;

    // Worker-thread-owned bookkeeping — plain ints/int64s, no atomics
    // needed: nothing but renderNextShadowBlock() (always the same thread)
    // ever touches these. diskBlocksQueued is the LOGICAL in-flight count
    // (write index minus read index); diskWriteBlockIndex/diskReadBlockIndex
    // are ever-increasing counters, wrapped modulo maxDiskBlocksQueued ONLY
    // at the point of computing a byte offset — the counters themselves
    // never wrap, so "how many total blocks has this track ever bounced"
    // stays unambiguous even across many trips around the ring.
    int diskBlocksQueued = 0;
    int64_t diskWriteBlockIndex = 0;
    int64_t diskReadBlockIndex = 0;
    int64_t bytesPerDiskBlock = 0;

    // "Generous but Safe" Disk Queue Cap directive: "infinite" lookahead
    // still has to stop somewhere, or a paused session left open over
    // lunch fills the drive. 4096 blocks — at a common 48kHz/512-sample
    // configuration, ~43.7 seconds of look-ahead per track, ~16MB per
    // track's temp file — is a huge runway for zero-CPU SSD streaming
    // while still being a hard, finite ceiling: this is now the file's
    // actual, physical, pre-allocated size, not just a counter someone has
    // to remember to check.
    static constexpr int maxDiskBlocksQueued = 4096;

    // Complete Cache Invalidation directive: a SEPARATE consumer flag from
    // flushRequested — that one is exchange()'d by the AUDIO thread, and a
    // second reader racing the same exchange() would only sometimes see
    // it. This one is exchange()'d exclusively by the WORKER thread, set at
    // the same call sites flushRequested is.
    std::atomic<bool> diskFlushRequested { false };

    std::unique_ptr<ShadowWorker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateAnticipativeWrapper)
};
