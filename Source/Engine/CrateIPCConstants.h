#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

/**
    Plugin Sandboxing / Step 5 (The Headless IPC Host Skeleton) directive:
    the ONE shared definition of where the "CrateIPC_Memory" block lives and
    how big it is — both CrateSandbox (Source/Sandbox/Main.cpp, the child
    process, which CREATES/owns the backing file) and CrateSandboxBridge
    (Source/Engine/CrateSandboxBridge.h, the parent process, which only ever
    ATTACHES to it) resolve to this same function, so the two processes can
    never disagree about the path.

    NOTE ON juce::SharedMemory: the Step 5 spec named `juce::SharedMemory` as
    the class to use, but no such class exists in this JUCE version (verified
    against modules/JUCE) — JUCE has juce::InterprocessConnection (socket/pipe
    based, no raw shared buffer) and juce::MemoryMappedFile (memory-maps a
    real file, already used for the Anticipative FX disk cache elsewhere in
    this codebase). A juce::MemoryMappedFile over a plain file in the OS temp
    directory IS the standard JUCE-only substitute for named shared memory —
    both processes mmap the SAME file, giving genuine shared read/write pages,
    not a copy — so that's what's used here instead of pulling in a new
    dependency (e.g. Boost.Interprocess) for this structural skeleton step.
*/
namespace CrateIPC
{
    // Step 11 (Absolute Muscle Memory / Continuous State Sync) directive:
    // bumped from 4MB — two new fixed-size state-chunk buffers (the push
    // channel and the initial-load channel, see ControlBlock's own doc
    // comment) need real headroom for "Kontakt-scale" instrument state,
    // on top of everything already living in this block (audio buffers,
    // parameter queue, plugin path). Still a single fixed allocation, same
    // "no dynamic growth, ever" contract as before.
    // Step 39 (A/B Testing) directive: bumped from 16MB to 24MB —
    // liveRestoreStateData adds a THIRD full maxStateChunkBytes (4MB)
    // buffer alongside stateChunkData and initialStateData (see its own
    // doc comment for why it's a deliberately separate channel, not a
    // reuse of either existing one), which no longer fits the original
    // 16MB budget. Still a single fixed allocation per instance — trivial
    // against modern machine RAM even with several Cryosleep pool slots
    // and Shared Sandbox tenants alive at once.
    constexpr int64_t sharedMemoryBytes = 24 * 1024 * 1024; // 24MB, per Step 39's own directive

    // Step 9 (The Multi-Process Scalability Stress Test) directive: THIS
    // FUNCTION USED TO RETURN A FIXED, GLOBAL FILENAME — correct for a
    // single-sandbox-instance architecture, but Step 6.5's own doc comment
    // already flagged it as a placeholder "not needed while there's still
    // only ever one" CrateSandbox process. Step 9 is the moment that stops
    // being true: 50 CONCURRENT CrateSandboxBridge/CrateSandbox pairs with a
    // shared fixed filename would all map the SAME 4MB block — 50 processes
    // stomping on one ControlBlock's heartbeat, audio buffers, and parameter
    // queue, corrupting each other continuously. instanceId (a juce::Uuid,
    // generated once per CrateSandboxBridge at construction and passed to
    // its own CrateSandbox.exe as a command-line argument — see
    // CrateSandboxBridge's own instanceId member and Source/Sandbox/Main.cpp's
    // initialise()) makes every pair's file, and only that pair's file,
    // unique.
    inline juce::File getSharedMemoryFile (const juce::String& instanceId)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("CrateIPC_Memory_" + instanceId + ".shm");
    }

    // Step 7 (The VST3 Host Engine) directive: shared between BOTH
    // processes now, not just the child — the PARENT needs to write
    // pluginPath/hostSampleRate/hostBlockSize into the file BEFORE the
    // CHILD process even exists (see CrateSandboxBridge::launchSandboxProcess()),
    // which means the PARENT is now sometimes the one creating/sizing the
    // file for the very first time, not only ever attaching to one the
    // child already made. Same fixed-size pre-allocation idiom the
    // Anticipative FX disk ring already uses elsewhere in this codebase
    // (setPosition(capacity-1) + writeByte(0)) — a juce::MemoryMappedFile
    // maps an EXISTING byte range, it doesn't grow the file itself.
    inline void ensureSharedMemoryFileIsSized (const juce::File& file)
    {
        if (file.getSize() == sharedMemoryBytes)
            return;

        file.deleteFile();

        if (std::unique_ptr<juce::FileOutputStream> out { file.createOutputStream() })
        {
            out->setPosition (sharedMemoryBytes - 1);
            out->writeByte (0);
        }
    }

    // Step 5.5 (Process Management & The Atomic Heartbeat) directive: the
    // first bytes of the mapped block are a fixed control header both
    // processes agree on, rather than raw undifferentiated bytes — right
    // now that's just the heartbeat counter, but this is where any future
    // control fields (command opcodes, ready flags) belong too, so there's
    // one canonical layout instead of each side inventing its own offsets.
    // ONLY CrateSandbox (the child) ever placement-news this into existence
    // (see Source/Sandbox/Main.cpp) — CrateSandboxBridge (the parent) only
    // ever reinterpret_casts an already-constructed block via
    // getControlBlock(), never reconstructs it. std::atomic<uint32_t>'s
    // 4-byte alignment is trivially satisfied by the mapping's own page
    // alignment, so no extra alignas() is needed.
    struct ControlBlock
    {
        // Step 38 (Cache-Line Alignment / False Sharing Fix) directive:
        // heartbeatCounter is written by an entirely independent thread
        // (HeartbeatThread, ~10ms cadence) with NO logical relationship to
        // the audio round-trip handshake below it — before this fix, it sat
        // in the same cache line as parentReady/childProcessed, meaning
        // every heartbeat tick invalidated the exact cache line the
        // highest-frequency spin-loop in the whole system (gatherFromSandbox()'s
        // _mm_pause() wait on childProcessed, potentially 1000+ times/sec
        // per instance) depends on, for zero benefit. alignas(64) forces it
        // onto its own cache line.
        alignas(64) std::atomic<uint32_t> heartbeatCounter { 0 };

        // Step 6 (The IPC Audio Arteries) directive: the parent/child
        // handshake for ONE audio block's round trip. Two SEPARATE bools,
        // not one shared flag — matching this codebase's established rule
        // that a flag flipped by one role and consumed by the OTHER needs
        // its own dedicated atomic per direction rather than one flag two
        // parties both write (see flushRequested/diskFlushRequested in
        // CrateAnticipativeWrapper for the same reasoning applied to a
        // different pair of threads). parentReady: parent has finished
        // writing audioInput, child may read it. childProcessed: child has
        // finished writing audioOutput, parent may read it back.
        //
        // Step 38 directive: this whole group — parentReady, childProcessed,
        // numChannels, numSamples — is THE hottest cache line in the entire
        // system under Shared Sandbox load. parentReady is written by the
        // PARENT's real audio thread every block; childProcessed is written
        // by the CHILD's AudioBridgeThread every block and is what the
        // PARENT spin-waits on; cross-process shared memory does NOT exempt
        // this from real MESI/MESIF cache-coherency ping-pong — the CPU's
        // cache fabric operates on physical cache lines, not process
        // boundaries. alignas(64) here groups all four onto ONE line,
        // separated from heartbeatCounter above and audioInput below —
        // each side of this handshake still shares a line with the OTHER
        // side's own field (that's the handshake itself, unavoidable and
        // fine — the fix is isolating this group from UNRELATED traffic,
        // not from itself).
        alignas(64) std::atomic<bool> parentReady    { false };
        std::atomic<bool> childProcessed { false };

        // How much of audioInput/audioOutput is actually valid THIS round
        // trip — the fixed arrays below are sized for the worst case
        // (maxChannels * maxSamplesPerBlock), not every host block size.
        std::atomic<int32_t> numChannels { 0 };
        std::atomic<int32_t> numSamples  { 0 };

        static constexpr int maxChannels        = 2;
        static constexpr int maxSamplesPerBlock = 4096; // worst-case block size this bridge supports at Step 6

        // Echo/Phase Test directive: plain float arrays, not atomics — the
        // parentReady/childProcessed handshake above is what makes touching
        // these safe (only the parent writes audioInput/reads audioOutput
        // between setting parentReady and observing childProcessed; only the
        // child does the reverse), so the SAMPLES themselves don't need
        // per-element atomicity, only the two flags guarding read/write
        // ownership do.
        //
        // Step 38 directive: alignas(64) here is defensive, not a measured
        // hotspot — audioInput/audioOutput are only ever touched by the
        // OWNING thread inside the parentReady/childProcessed-protected
        // window, never concurrently polled the way the flags above are —
        // but it's free to add and keeps numSamples' own cache line from
        // being shared with the start of a 32KB data array for no reason.
        alignas(64) float audioInput  [maxChannels * maxSamplesPerBlock] {};
        float audioOutput [maxChannels * maxSamplesPerBlock] {};

        // Step 8 (Real-Time Parameter Automation) directive: a SECOND,
        // INDEPENDENT lock-free SPSC ring buffer, alongside the audio
        // round-trip handshake above rather than folded into it — parameter
        // changes and audio blocks are two unrelated cadences (the DAW can
        // push dozens of parameter events between two audio blocks, or
        // none at all) and forcing them through the same one-slot
        // parentReady/childProcessed handshake would mean losing every
        // parameter change that arrives faster than one per audio round
        // trip. moodycamel::ReaderWriterQueue (already used in-process by
        // CrateAnticipativeWrapper) is NOT usable here — it allocates its
        // own backing storage on the constructing process's own heap, so
        // its internal pointers are meaningless to a DIFFERENT process
        // mapping the same shared file. This is a hand-rolled, fixed-size,
        // pure-POD ring buffer instead — no pointers, no allocation, so its
        // raw bytes mean the same thing in both processes' address spaces,
        // exactly like audioInput/audioOutput above.
        //
        // Classic SPSC pattern: the PARENT is the SOLE producer (writes
        // paramQueue[]/paramQueueHead, only ever READS paramQueueTail); the
        // CHILD is the SOLE consumer (writes paramQueueTail, only ever
        // READS paramQueueHead). Neither side ever writes what the other
        // side owns, so no CAS/lock is needed — this is what makes an SPSC
        // ring buffer lock-free-safe with plain atomics.
        struct ParamChange
        {
            int32_t parameterIndex = -1;
            float normalizedValue = 0.0f;
        };

        static constexpr int paramQueueCapacity = 1024; // per directive — generous headroom for dense automation bursts

        std::atomic<uint32_t> paramQueueHead { 0 }; // producer(PARENT)-owned: next slot to WRITE
        std::atomic<uint32_t> paramQueueTail { 0 }; // consumer(CHILD)-owned: next slot to READ

        ParamChange paramQueue[paramQueueCapacity] {};

        // Step 31 (Real IPC Parameter Sync) directive: the metadata half of
        // real parameter sync — CHILD -> PARENT, write-once per load, gated
        // by paramMetadataReady (same "resolved once, gated by a ready
        // flag" convention as pluginUID/vendorName, Step 13.5/22). This is
        // what lets the PARENT dynamically build one REAL
        // te::AutomatableParameter per actual VST3 parameter instead of the
        // Step 30 hardcoded stub list. maxSyncedParams is a fixed cap (not
        // every VST3's full parameter count, which can run into the tens of
        // thousands for sample libraries) — same "generous but fixed, no
        // dynamic growth" contract as every other array in this struct; a
        // plugin with more than this many parameters simply doesn't get
        // Device Chain automation UI for the overflow ones.
        static constexpr int maxSyncedParams = 128;
        static constexpr int maxParamNameLength = 64;

        struct ParamMetadata
        {
            char name[maxParamNameLength] {};
            float defaultValue = 0.0f; // normalized 0-1, matching juce::AudioProcessorParameter's own convention
        };

        std::atomic<bool> paramMetadataReady { false };
        std::atomic<int32_t> paramCount { 0 };
        ParamMetadata paramMetadata[maxSyncedParams] {};

        // Step 31 directive: the VALUE READBACK half — CHILD -> PARENT,
        // continuously live (same "latest wins, both sides poll" convention
        // as windowWidth/Height). Reflects a user twiddling a knob INSIDE
        // the reparented native UI back into the DAW's own
        // AutomatableParameter, the reverse direction from paramQueue
        // above (PARENT -> CHILD, driven by DAW-side automation/the user
        // moving a Device Chain knob). paramValueRevision is bumped by the
        // CHILD only when at least one value actually changed since its
        // last publish, so the PARENT can cheaply skip touching all 128
        // slots on ticks where nothing moved.
        std::atomic<uint32_t> paramValueRevision { 0 };
        std::atomic<float> paramCurrentValues[maxSyncedParams] {};

        // Step 7 (The VST3 Host Engine) directive: write-once-before-launch
        // CONFIG, not a live per-buffer channel — same category as each
        // other (both set by the PARENT before the child process ever
        // starts, both just read once by the child at its own startup), so
        // plain fields, not atomics, matching pluginPath's own treatment.
        // The PARENT knows its REAL sampleRate/blockSizeSamples from
        // te::PluginInitialisationInfo (received in its own initialise())
        // and writes both here so the hosted plugin's prepareToPlay() call
        // matches the ACTUAL host configuration instead of a guessed
        // default.
        static constexpr int maxPluginPathLength = 1024;
        char pluginPath[maxPluginPathLength] {};
        double hostSampleRate = 0.0;
        int32_t hostBlockSize = 0;

        // Step 10 (Cross-Process Window Reparenting) directive: a THIRD
        // small, independent command channel — not folded into the audio
        // handshake or the parameter queue, since a window-handle request
        // happens once (or rarely), is answered asynchronously at GUI
        // speed (not audio-block speed), and both sides poll it off their
        // OWN message/GUI thread, never the audio thread. windowHandleValue
        // holds the CHILD's real native HWND (Windows-only, per this
        // project's own platform scope) as a plain 64-bit integer — wide
        // enough for any real pointer value, and integers round-trip
        // through shared memory identically regardless of which process
        // reads them, unlike an actual pointer (which would be meaningless
        // across address spaces — this value is only ever reinterpreted
        // back into a HWND on the SAME machine/session, which cross-process
        // window handles are already scoped to on Windows).
        std::atomic<bool> windowHandleRequested { false }; // PARENT sets true to ask
        std::atomic<bool> windowHandleReady     { false }; // CHILD sets true once windowHandleValue/windowWidth/windowHeight are valid
        std::atomic<int64_t> windowHandleValue  { 0 };

        // Step 79 (Pre-Emptive Native Parenting) directive: the Airlock's
        // own slot HWND, published by the PARENT BEFORE windowHandleRequested
        // is ever set — the CHILD reads this exactly once, before creating
        // its editor, and passes it straight into
        // AudioProcessorEditor::addToDesktop(0, (void*) hostSlotHwndValue)
        // so JUCE's own HWNDComponentPeer calls CreateWindowEx with the
        // slot as hWndParent from the very FIRST frame — never floating as
        // its own top-level window and never needing a later SetParent()
        // call at all. 0 means "no slot published yet"; the CHILD must not
        // create an editor until this is non-zero (see
        // CrateSandboxBridge::requestEditorWindow()'s own doc comment for
        // the PARENT-side half of this ordering contract).
        std::atomic<int64_t> hostSlotHwndValue { 0 };

        // Geometry Sync directive (Step 10.1): the plugin editor's EXACT
        // native size, read directly from the CHILD's own
        // AudioProcessorEditor::getWidth()/getHeight() and sent across the
        // SAME command alongside the handle — not re-derived on the PARENT
        // side via GetWindowRect() after reparenting, which measures
        // whatever the embedded HWND's bounds happen to be at that moment
        // (potentially still whatever arbitrary placeholder size the
        // PARENT's own container window had when SetParent() ran) rather
        // than the plugin's actual authored content size.
        std::atomic<int32_t> windowWidth  { 0 };
        std::atomic<int32_t> windowHeight { 0 };

        // Step 36 (The Fixed-Size Lock) directive: CHILD -> PARENT, set
        // once alongside windowWidth/Height/windowHandleReady and never
        // changed again for THIS editor's lifetime — an IPlugView's
        // canResize() is queried once at editor-creation time (the same
        // point windowWidth/Height are first read, see the CHILD's own
        // editor-creation code) and is a fixed property of that view, not
        // something that changes tick-to-tick. Lets the PARENT
        // (CrateEditorComponent) disable/hide its own resize grip entirely
        // for a plugin that can never honour a resize request, instead of
        // letting the user drag a corner that only ever snaps back or (pre
        // Step 35) risked tripping the editor-recovery guard for no real
        // reason.
        std::atomic<bool> editorCanResize { true }; // optimistic default until the CHILD's first real query lands

        // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive:
        // PARENT -> CHILD, continuously live (same "no ready-flag, just
        // compare against last-seen value" treatment as windowWidth/Height
        // above, just reversed direction). Fixed-point (scale * 1000), not
        // a raw std::atomic<float> — matches this struct's existing
        // atomic<int32_t> convention everywhere else rather than
        // introducing a new atomic-type category for one field.
        //
        // WHY THIS EXISTS: the CHILD's own editor (a VST3PluginWindow,
        // JUCE's own wrapper — see juce_VST3PluginFormat.cpp) already runs
        // a NativeScaleFactorNotifier that auto-tracks ITS OWN peer's scale
        // — but that peer becomes a foreign-process WS_CHILD the moment the
        // PARENT reparents it (Step 10), and Windows' automatic per-monitor
        // DPI broadcast to a cross-process child window is exactly the
        // scenario Microsoft's own docs flag as unreliable. The CHILD's
        // auto-detected scale freezes at whatever it was the instant of
        // reparenting and never legitimately updates again on its own.
        //
        // The PARENT, by contrast, owns the REAL top-level window that
        // genuinely sits on a real monitor — it always knows the correct
        // scale. This field is how it tells the CHILD, explicitly, since
        // the CHILD can no longer reliably find out for itself.
        std::atomic<int32_t> displayScale1000 { 1000 }; // 1000 = 100% = scale factor 1.0f

        // Step 11 (Absolute Muscle Memory / Continuous State Sync)
        // directive: TWO separate fixed-size buffers, not one shared —
        // they have different roles/ownership, and reusing one would mean
        // genuine ambiguity about whose data currently sits in it:
        //
        //   PUSH CHANNEL (CHILD -> PARENT, ongoing, whenever the plugin's
        //   state changes): guarded by stateChunkLock, a std::atomic_flag
        //   spinlock (same idiom as CrateAnticipativeWrapper's own dspLock)
        //   — this is NOT a single-producer/single-consumer ring buffer
        //   like the audio or parameter channels, because only the LATEST
        //   state ever matters; an in-flight chunk the parent hasn't
        //   consumed yet is simply overwritten by a newer one, never
        //   queued. The lock exists purely to prevent tearing (parent
        //   mid-read while the child writes newer bytes on top).
        //
        //   INITIAL-LOAD CHANNEL (PARENT -> CHILD, write-once-before-launch,
        //   same category as pluginPath/hostSampleRate/hostBlockSize): the
        //   PARENT writes its lastKnownState here before EVERY launch
        //   (including a restart after a crash) so a freshly spawned child
        //   can call setStateInformation() and resurrect the exact state
        //   the dead process had a moment before — the actual mechanism
        //   behind the "Ghost Reload" this step exists to prove out.
        static constexpr int64_t maxStateChunkBytes = 4 * 1024 * 1024; // 4MB per buffer — generous headroom for large instrument presets

        std::atomic_flag stateChunkLock = ATOMIC_FLAG_INIT;
        std::atomic<bool> stateChunkAvailable { false }; // CHILD sets true once stateChunkData/stateChunkSize are valid for the PARENT to consume
        std::atomic<int64_t> stateChunkSize { 0 };
        char stateChunkData[maxStateChunkBytes] {};

        // Write-once-before-launch — see CrateSandboxBridge::launchSandboxProcess()
        // and CrateSandbox's own initialise() (which preserves this across
        // its per-launch reset, same dance as pluginPath).
        int64_t initialStateSize = 0;
        char initialStateData[maxStateChunkBytes] {};

        // Step 39 (A/B Testing / Smart Bypass) directive, Task 4: a
        // DELIBERATELY SEPARATE channel from initialStateData/initialStateSize
        // above, despite the superficial similarity ("push a state chunk
        // into the CHILD") — initialStateData is a write-ONCE-before-launch
        // field consumed exactly once, at process boot, as part of the
        // crash-resurrection contract (Step 11's "Ghost Reload"); this is a
        // LIVE, PARENT -> CHILD "restore this chunk into the ALREADY-RUNNING
        // plugin, right now" request, usable any number of times over a
        // session (the A/B toggle). Reusing initialStateData for both would
        // risk a live restore request landing during a crash-restart window
        // and corrupting the resurrection payload, or vice versa. Consumed
        // on the CHILD's message-thread-equivalent tick (Main.cpp's own
        // timerCallback()/serviceTenant()), NEVER the audio thread —
        // AudioProcessor::setStateInformation() can allocate and do
        // arbitrarily heavy work, guarded by the SAME pluginAccessLock
        // bounded-backoff idiom every other cross-thread plugin touch in
        // this codebase already uses (see StateExtractionThread's own,
        // Step-37-fixed, version of that exact pattern).
        std::atomic<bool> liveRestoreRequested { false }; // PARENT sets true to ask; CHILD clears once applied
        std::atomic<int64_t> liveRestoreStateSize { 0 };
        char liveRestoreStateData[maxStateChunkBytes] {};

        // Store A/B Amnesia Fix (Step 53) directive: liveRestoreStateData/
        // liveRestoreStateSize above had NO lock at all, unlike the
        // PUSH channel's stateChunkData (guarded by stateChunkLock,
        // immediately below its own doc comment) — a real, unguarded
        // cross-process race: the PARENT writes this buffer directly from
        // restoreStateFromSlot(), with no acquire of anything the CHILD
        // also respects, so a fast A->B->A switch (a second restore
        // request landing before the CHILD finishes reading the FIRST
        // one's bytes out of this SAME buffer inside
        // setStateInformation()) could tear it — the CHILD reading a
        // buffer that's half slot A's bytes, half slot B's, mid-memcpy.
        // Same std::atomic_flag spinlock idiom as stateChunkLock: the
        // PARENT acquires it (message thread, can afford a short bounded
        // spin) before writing; the CHILD acquires it (already inside its
        // own pluginAccessLock-guarded block, so this is a second, THIN
        // lock scoped only to the buffer read itself) before reading.
        std::atomic_flag liveRestoreLock = ATOMIC_FLAG_INIT;

        // Step 89 (Fresh Store Fix) directive — QA finding: "Store A/B"
        // used to just snapshot CrateSandboxBridge::lastKnownState as-is,
        // a PASSIVELY-updated cache that only refreshes via the
        // Continuous State Sync channel (StateExtractionThread's own
        // debounceMs=500 quiet-period, plus the round trip back over
        // stateChunkData) — a real user clicking Store within that window
        // of their own most recent tweak captured whatever STALE state
        // happened to be cached at that instant, not what the plugin
        // actually held at click-time. PARENT sets this true to ask the
        // CHILD for an IMMEDIATE extraction (bypassing the debounce
        // entirely — see StateExtractionThread::triggerImmediateExtraction()
        // — since a manual, one-off Store click is exactly the case the
        // debounce's OWN rapid-fire-coalescing reason for existing doesn't
        // apply to); CHILD clears it once the fresh extraction has been
        // kicked off. CrateSandboxBridge::pendingStoreSlotTarget (a
        // Host-local field, not IPC — only the PARENT needs to remember
        // which slot it was about to fill) is what actually completes the
        // store once the resulting FRESH chunk lands back over the
        // existing stateChunkData/stateChunkAvailable channel.
        std::atomic<bool> forceStateExtractionRequested { false };

        // Step 39 (Live Telemetry) directive, Task 3: CHILD -> PARENT,
        // continuously live (same "latest wins, both sides poll"
        // convention as windowWidth/Height). pluginLatencySamples is the
        // hosted plugin's own reported PDC latency (rarely changes after
        // load, republished on the same "only act on genuine change" cadence
        // as everything else in this convention family). lastProcessBlockMicros
        // is the ACTUAL wall-clock time AudioBridgeThread's own
        // hostedPlugin->processBlock() call took on its most recent
        // invocation — measured with juce::Time::getMillisecondCounterHiRes()
        // immediately around that one call, nothing else, so it reflects the
        // plugin's own real CPU cost, not IPC/spin-wait overhead layered on
        // top of it (that round-trip figure already exists separately, as
        // CrateSandboxBridge::maxRoundTripMsObserved).
        std::atomic<int32_t> pluginLatencySamples { 0 };
        std::atomic<uint32_t> lastProcessBlockMicros { 0 };

        // Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive: CHILD
        // -> PARENT, one-shot (set true exactly once, by whichever thread's
        // SEH catch is the first to see this plugin instance fault; never
        // cleared by the CHILD — a fresh instance after respawn starts with
        // a freshly placement-newed ControlBlock, this field's default
        // false). The PARENT's own timerCallback() polls this exactly like
        // the existing dspStallDetected/heartbeat-timeout checks — same
        // "declare dead, recordCrash(), restart or reroute" sequence,
        // reusing that already-built recovery machinery rather than
        // inventing a second one. See AudioBridgeThread's own isCorrupted
        // member for why the audio thread itself never waits for this to
        // be observed — the mute is immediate and local, this flag is
        // purely how the PARENT finds out afterward.
        std::atomic<bool> corruptionDetected { false };

        // Step 52 directive, Task 2 (Strict VST3-Driven Resize Limits):
        // the CHILD's own best-effort PROBE of the plugin's real minimum/
        // maximum size, via the ONLY mechanism the VST3 API actually
        // exposes for this — IPlugView has no direct getMinSize()/
        // getMaxSize() query, only canResize()/checkSizeConstraint()
        // (given a CANDIDATE rect, tells you the nearest ALLOWED one) — so
        // this is populated by probing with an extreme small and an
        // extreme large candidate once, immediately at editor-creation
        // time (before the window is ever shown/reparented, so the
        // resulting flicker is invisible), and reading back whatever the
        // constrainer actually clamped to. A heuristic against the ONLY
        // API surface available, not a guaranteed direct query — see
        // CrateSandboxBridge's own comment on how the PARENT treats these
        // (a floor/ceiling for setResizeLimits(), combined with the
        // existing editorCanResize lock for the "can't resize at all"
        // case). Zero means "not yet probed" / "probe inconclusive."
        std::atomic<int32_t> pluginMinWidth  { 0 };
        std::atomic<int32_t> pluginMinHeight { 0 };
        std::atomic<int32_t> pluginMaxWidth  { 0 };
        std::atomic<int32_t> pluginMaxHeight { 0 };

        // Step 55 (The Liar's Penalty) directive: CHILD -> PARENT,
        // one-shot (same edge-trigger convention as corruptionDetected
        // above — PARENT consumes via exchange(), CHILD never clears it).
        // Set the moment the Editor View Recovery Guard catches the
        // hosted plugin IGNORING an applied resize — claiming
        // canResize=true and accepting huge probe candidates via
        // checkSizeConstraint(), then snapping straight back to its
        // creation size when actually resized (VoxDucker, caught via
        // direct log evidence: probed max 10000x10000, real resize
        // 824x824 -> snapped back to 800x800). The PARENT records the
        // conviction into PluginHealthRegistry so all FUTURE loads of
        // this UID skip the plugin's false claims entirely (see
        // forceFixedSizeEditor below).
        std::atomic<bool> fixedSizeLiarDetected { false };

        // Step 55 directive: PARENT -> CHILD, write-once-before-launch
        // (same category as pluginPath/hostSampleRate — written into the
        // block before the CHILD ever reads it, at launch/claim/tenant-
        // spawn time). True when PluginHealthRegistry says this plugin
        // was ALREADY convicted as a fixed-size liar in a past session:
        // the CHILD's editor-creation code then skips the resize probe
        // entirely and publishes canResize=false + min==max==creation
        // size, never giving the plugin's lying canResize() a second
        // chance to reopen the resize handle.
        std::atomic<bool> forceFixedSizeEditor { false };

        // Step 12.1 (The Violent Crash Test) directive: a debug/test-only
        // "poison pill" — the PARENT sets this to deliberately trigger a
        // genuine, unhandled access violation in the CHILD (see
        // CrateSandbox's own timerCallback()), so the heartbeat/DSP-stall
        // detection and resurrection machinery can be proven against a real
        // OS-level crash, not just a clean process kill (juce::ChildProcess::
        // kill(), or the taskkill/TerminateProcess used in every earlier
        // manual test) — an unhandled SEH exception goes through Windows'
        // own crash-reporting path, which is a genuinely different (and
        // potentially slower/less deterministic) termination sequence than
        // TerminateProcess, and is the more honest test of "does the parent
        // survive whatever actually happens when this ships and someone's
        // real plugin segfaults."
        std::atomic<bool> triggerCrashRequested { false };

        // Step 13.5 (Authentic VST3 UID) directive: CHILD -> PARENT,
        // write-once per launch, as soon as the child's own scan resolves
        // a PluginDescription for pluginPath — deliberately BEFORE
        // createPluginInstance() is attempted, so the real identifier is
        // already in the PARENT's hands even if instantiation itself is
        // what crashes. This is resolved in the CHILD, never re-scanned in
        // the PARENT: the CHILD is the only process that ever safely
        // executes a third-party VST3's own factory/class-enumeration
        // code — re-scanning the file directly in the PARENT to read its
        // PluginDescription would execute that same untrusted code path
        // inside the process the whole sandbox exists to protect,
        // reintroducing exactly the crash exposure Step 5 removed.
        static constexpr int maxPluginUIDLength = 128;
        std::atomic<bool> pluginUIDReady { false };
        char pluginUID[maxPluginUIDLength] {};

        // Step 22 (The Profiling Database / The Warden) directive: resolved
        // at the EXACT same point as pluginUID above (the CHILD's own scan,
        // via juce::PluginDescription::manufacturerName — never re-scanned
        // in the PARENT, same "only the CHILD ever executes untrusted
        // factory code" reasoning as pluginUID's own comment), so it's
        // gated by the SAME pluginUIDReady flag rather than a second one —
        // both fields are written together, in the same breath, by
        // loadHostedPlugin().
        static constexpr int maxVendorNameLength = 128;
        char vendorName[maxVendorNameLength] {};

        // Step 18 (The Time-Slip Engine) directive: the mode toggle —
        // PARENT sets this, CHILD's AudioBridgeThread/LookaheadWorkerThread
        // both read it. Mutually exclusive by construction: when true,
        // AudioBridgeThread explicitly skips processBlock() for this
        // tenant (defense-in-depth — the PARENT should also simply stop
        // dispatching real-time requests once this is set, but this guard
        // means a stray/leftover parentReady signal can never race
        // LookaheadWorkerThread for pluginAccessLock), and
        // LookaheadWorkerThread becomes the sole driver of
        // hostedPlugin->processBlock() calls instead.
        std::atomic<bool> isLookaheadMode { false };

        // Step 17 (The Lookahead IPC Pipeline / "Time-Slip Plumbing")
        // directive: a SECOND, entirely independent producer/consumer
        // channel from the per-block audioInput/audioOutput handshake
        // above — that one is a strict single-slot, one-block-in-flight
        // round trip, locked to real-time cadence (parentReady/
        // childProcessed). This one is a genuine PIPELINE: the PARENT can
        // enqueue MANY requests for FUTURE timeline positions without
        // waiting for each one's result before sending the next, and the
        // CHILD's LookaheadWorkerThread drains them as fast as the CPU
        // allows, completely decoupled from the real-time block cadence.
        //
        // IMPORTANT SIZING NOTE, stated plainly so it isn't mistaken for
        // the actual 4-second Time-Slip Buffer: this ring is the IN-FLIGHT
        // PIPE between the two processes, not the reservoir. The real
        // 4-second buffer of already-rendered audio lives entirely in the
        // PARENT's own heap memory (a later step), fed by draining
        // lookaheadResultRing as results arrive — the same relationship a
        // network socket buffer has to the file being transferred through
        // it. lookaheadRingCapacity only needs to be deep enough to keep
        // the pipe from stalling, not to hold the whole window.
        //
        // Two SEPARATE SPSC rings (not one bidirectional queue), same
        // reasoning as the Master Control Channel's own separate
        // head/tail pairs: PARENT is the sole producer of requests, CHILD
        // is the sole producer of results — mixing directions into one
        // ring would need a lock neither side should ever have to take.
        struct LookaheadRequestSlot
        {
            // -1 = an empty/never-written slot; a real request always has
            // a non-negative absolute sample position. Lets a defensive
            // reader distinguish "genuinely at position 0" from "nothing
            // here yet" without a separate valid-flag field.
            int64_t timelinePositionSamples = -1;
            int32_t numChannels = 0;
            int32_t numSamples  = 0;
            float audioInput[maxChannels * maxSamplesPerBlock] {};

            // Zero-Dropout Bridge directive (Step 52): the DETERMINISTIC
            // automation-curve value for every synced parameter AT THIS
            // FUTURE timeline position — sampled on the PARENT's MESSAGE
            // THREAD (AutomationCurve::getValueAt() is
            // TRACKTION_ASSERT_MESSAGE_THREAD-guarded, confirmed via direct
            // source read of tracktion_AutomationCurve.cpp, so it can never
            // be called from LookaheadProducerThread itself) via
            // CrateSandboxBridge's own futureParamCache, then copied into
            // this slot by pumpLookaheadPipeline() (LookaheadProducerThread)
            // at the same point it fills audioInput. The CHILD's
            // LookaheadWorkerThread applies these to the hosted plugin's
            // real parameters BEFORE rendering this block, so recorded/
            // drawn automation rides along with the pre-rendered future
            // audio instead of requiring a buffer flush on every automation
            // tick. numParamValues == 0 is a valid, common state (no synced
            // parameters yet, or the message thread hasn't caught up
            // precomputing this far ahead yet) — the CHILD simply leaves
            // every parameter at whatever it last had for that one block
            // rather than treating it as an error.
            int32_t numParamValues = 0;
            float paramValues[maxSyncedParams] {};
        };

        struct LookaheadResultSlot
        {
            int64_t timelinePositionSamples = -1;
            int32_t numChannels = 0;
            int32_t numSamples  = 0;
            float audioOutput[maxChannels * maxSamplesPerBlock] {};
        };

        // 64 slots x ~93ms/slot (at the worst-case maxSamplesPerBlock,
        // 44.1kHz) ~= 5.95s of MAXIMUM representable in-flight depth —
        // comfortably covers the 4-second Time-Slip target with headroom
        // as a PIPE, at a modest ~2MB per ring (64 * ~32KB/slot). At a
        // real host block size (typically far smaller than
        // maxSamplesPerBlock), the actual in-flight depth this ring holds
        // is correspondingly smaller — which is fine, since it's not
        // supposed to hold the full window itself (see this struct's own
        // doc comment above).
        static constexpr int lookaheadRingCapacity = 64;

        // Step 38 (Cache-Line Alignment / False Sharing Fix) directive:
        // lookaheadRequestHead (written by the PARENT) and
        // lookaheadRequestTail (written by the CHILD's LookaheadWorkerThread)
        // are a different-writer pair, same false-sharing shape as
        // parentReady/childProcessed above — separated onto their own
        // cache lines. The two pairs (request vs. result) were already
        // naturally isolated from EACH OTHER by the multi-megabyte
        // lookaheadRequestRing array sitting between them; this only needed
        // to separate the writer from its own reader WITHIN each pair.
        alignas(64) std::atomic<uint32_t> lookaheadRequestHead { 0 }; // producer (PARENT)-owned: next slot to WRITE
        alignas(64) std::atomic<uint32_t> lookaheadRequestTail { 0 }; // consumer (CHILD LookaheadWorkerThread)-owned: next slot to READ
        LookaheadRequestSlot lookaheadRequestRing[lookaheadRingCapacity] {};

        // Step 17 directive: result slot N always corresponds to request
        // slot N (both rings share the same capacity and the worker
        // processes strictly one request into exactly one result at the
        // SAME index before moving on) — so lookaheadResultHead is simply
        // set to the request ring's own new tail value after each
        // request is drained, rather than tracked as an independent
        // increment that could drift out of sync with it.
        //
        // Step 38 directive: same different-writer separation as the
        // request pair above — lookaheadResultHead (CHILD) and
        // lookaheadResultTail (PARENT) each get their own line.
        alignas(64) std::atomic<uint32_t> lookaheadResultHead { 0 }; // producer (CHILD)-owned: next slot considered READY
        alignas(64) std::atomic<uint32_t> lookaheadResultTail { 0 }; // consumer (PARENT)-owned: next slot to READ
        LookaheadResultSlot lookaheadResultRing[lookaheadRingCapacity] {};
    };

    static_assert (sizeof (ControlBlock) <= sharedMemoryBytes,
                   "Control block must fit inside the shared memory block");

    inline ControlBlock* getControlBlock (void* mappedData) noexcept
    {
        return reinterpret_cast<ControlBlock*> (mappedData);
    }

    constexpr int heartbeatIntervalMs     = 10; // how often the CHILD increments the counter
    constexpr int heartbeatTimeoutMs      = 50; // how long the PARENT waits with no change before declaring DEAD, once already proven alive
    constexpr int livenessCheckIntervalMs = 20; // how often the PARENT polls — comfortably inside the timeout, so a stall is never missed by more than one extra tick

    // Step 15.3 (Self-Healing & Auto-Quarantine) hotfix directive: a Tenant
    // Bridge's FIRST connection to a (possibly freshly-launched) Shared
    // Sandbox host is NOT comparable to isolated mode's own heartbeat
    // start — isolated mode's heartbeat begins immediately, in the SAME
    // process that just launched, no cross-process hop required.
    // Establishing a tenant, by contrast, requires: the shared host's
    // CommandListenerThread to wake, a hop to ITS message thread via
    // MessageManager::callAsync(), creation/sizing of a BRAND NEW 16MB
    // per-tenant shared-memory file (real disk I/O, not just a re-map of
    // an already-sized one), and only then does that tenant's own
    // HeartbeatThread start. Measured in practice this routinely takes
    // over a second even under light load — reusing heartbeatTimeoutMs
    // (50ms) here caused a genuine, reproduced restart storm: the PARENT
    // gave up and rerouted to a FRESH instanceId roughly a second before
    // the "abandoned" one would have actually succeeded, repeatedly,
    // leaking a live-but-forgotten tenant into the shared host on every
    // single failed attempt (15 of them in one test run before this was
    // caught). Only the FIRST-connection grace period is longer — once a
    // tenant has proven itself alive even once, a subsequent STEADY-STATE
    // stall/death still uses the tight heartbeatTimeoutMs above, exactly
    // like isolated mode.
    constexpr int tenantFirstConnectTimeoutMs = 8000;

    // The Parent Push & Spin-Wait directive: a hard fail-safe cap on how long
    // the AUDIO THREAD will busy-wait for one round trip through the sandbox
    // before giving up, outputting silence, and reporting a DSP stall. This
    // is deliberately much tighter than heartbeatTimeoutMs — a process that's
    // still alive (heartbeat ticking) but too slow to finish one block's DSP
    // in time is a DIFFERENT failure mode than a dead process, and the audio
    // thread can't afford to wait anywhere near 50ms of silence before
    // reacting to it.
    constexpr int spinWaitTimeoutMs = 3;

    // Step 6.5 (The Hybrid Sync Pivot) directive: Step 6's AudioBridgeThread
    // spun on parentReady with Thread::yield() — measured at 93.6% of a
    // full CPU core, CONSTANTLY, whether or not any audio was actually
    // flowing (yield() doesn't actually sleep when nothing else contends
    // for the core, it just re-schedules immediately). A per-sandboxed-
    // plugin core tax like that doesn't scale to a session running many
    // instances at once, and JUCE has no built-in named/cross-process event
    // wrapper (juce::WaitableEvent is anonymous, single-process only) — so
    // this wraps the raw Win32 auto-reset event directly. This project
    // already targets Windows exclusively (ASIO/DirectSound/WASAPI are the
    // only backends configured in CMakeLists, no other platform), so a
    // Windows-only primitive here isn't a new platform constraint.
    //
    // Auto-reset (bManualReset=FALSE): SetEvent() wakes exactly the ONE
    // thread currently waiting and immediately re-arms itself — exactly the
    // "signal once per buffer" semantics this bridge needs, with no
    // separate ResetEvent() call required on either side.
    //
    // CreateEventW (not OpenEventW) on BOTH sides deliberately: Win32
    // guarantees CreateEventW with a name that already exists just returns
    // a handle to the SAME existing kernel object (GetLastError() reports
    // ERROR_ALREADY_EXISTS, but the handle is still valid and usable) — so
    // whichever process calls it first creates the object, and the second
    // one transparently attaches to it, with neither side needing to know
    // or care which happened first.
    //
    // NAMING: per-instance as of Step 9 — see getBufferReadyEventName()'s
    // own comment; a fixed global name (the original Step 6.5 design) would
    // mean 50 concurrent children all blocked on the SAME event object,
    // where a single SetEvent() wakes an arbitrary ONE of them, not the
    // specific child the signalling parent actually intended.
    class NamedEvent
    {
    public:
        explicit NamedEvent (const juce::String& name)
        {
           #if JUCE_WINDOWS
            handle = ::CreateEventW (nullptr, FALSE, FALSE, name.toWideCharPointer());
           #else
            juce::ignoreUnused (name);
           #endif
        }

        ~NamedEvent()
        {
           #if JUCE_WINDOWS
            if (handle != nullptr)
                ::CloseHandle (handle);
           #endif
        }

        bool isValid() const noexcept
        {
           #if JUCE_WINDOWS
            return handle != nullptr;
           #else
            return false;
           #endif
        }

        // The Parent Fast-Spin directive: called from the AUDIO thread —
        // SetEvent() is a fast, non-blocking kernel call (no allocation, no
        // suspension of the CALLING thread), safe to call from a real-time
        // context, unlike wait() below.
        void signal()
        {
           #if JUCE_WINDOWS
            if (handle != nullptr)
                ::SetEvent (handle);
           #endif
        }

        // The Child Sleep directive: blocks the CALLING thread with genuine
        // 0% CPU until signalled, or until timeoutMs elapses. NEVER call
        // this from the audio thread — this is the CHILD's own DSP thread
        // only (see CrateSandboxBridge's own doc comment on why the parent
        // must never sleep/block).
        bool wait (int timeoutMs)
        {
           #if JUCE_WINDOWS
            if (handle == nullptr)
                return false;

            return ::WaitForSingleObject (handle, (DWORD) timeoutMs) == WAIT_OBJECT_0;
           #else
            juce::ignoreUnused (timeoutMs);
            return false;
           #endif
        }

    private:
       #if JUCE_WINDOWS
        HANDLE handle = nullptr;
       #endif

        NamedEvent (const NamedEvent&) = delete;
        NamedEvent& operator= (const NamedEvent&) = delete;
    };

    // Step 9 directive: per-instance, same reasoning as getSharedMemoryFile()
    // above — the SAME instanceId a CrateSandboxBridge passes to its own
    // CrateSandbox.exe on the command line names both.
    inline juce::String getBufferReadyEventName (const juce::String& instanceId)
    {
        return "CrateIPC_BufferReady_Event_" + instanceId;
    }

    // Step 17 directive: wakes the CHILD's LookaheadWorkerThread promptly
    // when the PARENT enqueues a new request — same per-instance naming
    // reasoning as getBufferReadyEventName() above (a fixed/global name
    // would mean every tenant sharing a host waking every other tenant's
    // worker on every enqueue).
    inline juce::String getLookaheadRequestReadyEventName (const juce::String& instanceId)
    {
        return "CrateIPC_LookaheadRequestReady_Event_" + instanceId;
    }

    // Step 15.1 (Multi-Tenant IPC & The Control Channel) directive: a
    // SECOND, entirely separate shared-memory block from the per-instance
    // ControlBlock above — this one is NOT per-plugin-instance, it's the
    // single Master Control Channel the PARENT uses to command an ALREADY-
    // RUNNING Shared Sandbox host process to spin up a new plugin tenant
    // inside itself, without restarting the whole process. Fixed name, not
    // instanceId-suffixed, because there is only ever ONE shared host
    // process for now — see SandboxManager's own doc comment.
    //
    // RING BUFFER, not a single "latest wins" slot: unlike the per-instance
    // state-chunk push channel (Step 11), a spawn command that arrives
    // while a previous one hasn't been consumed yet must NEVER be
    // overwritten — a whole project load could legitimately request
    // several shared-sandbox tenants in a burst, and every one of them
    // needs its own plugin actually instantiated, not just the most recent
    // request. Same hand-rolled SPSC ring-buffer idiom as the per-instance
    // parameter queue (Step 8): PARENT is the sole producer, the SHARED
    // HOST CHILD is the sole consumer, plain POD, no pointers, no
    // allocation — the raw bytes mean the same thing in both address
    // spaces.
    struct SharedHostCommandBlock
    {
        struct SpawnCommand
        {
            // Step 15.4 (The Teardown Protocol) directive: this ring buffer
            // now carries TWO kinds of command, not just spawns — Unload is
            // how the PARENT tells the shared host "a TenantBridge for this
            // instanceID was just destroyed, free its RAM." Kept as ONE
            // struct/ONE queue rather than a second ring buffer: both kinds
            // are small, infrequent, message-thread-speed commands with the
            // exact same producer/consumer contract, and instanceId is all
            // an Unload command ever needs (pluginUID/pluginPath are simply
            // left blank for one).
            enum class Type : int32_t { Spawn = 0, Unload = 1 };

            Type type = Type::Spawn;

            static constexpr int maxPluginUIDLength  = 128;
            static constexpr int maxInstanceIdLength = 64;
            static constexpr int maxPluginPathLength = 1024; // same size as ControlBlock::maxPluginPathLength — same category of data

            char pluginUID[maxPluginUIDLength] {};
            char instanceId[maxInstanceIdLength] {};

            // Step 15.2 (The Shared Host Engine) directive: pluginUID alone
            // (the authentic identifier) is NOT enough for the shared host
            // to actually LOAD a plugin — that requires the real file path,
            // exactly like the isolated-mode ControlBlock::pluginPath the
            // CHILD already reads today. pluginUID is carried alongside for
            // logging/matching only; this field is what loadHostedPlugin()'s
            // multi-tenant equivalent actually scans/instantiates. Unused
            // for an Unload command.
            char pluginPath[maxPluginPathLength] {};
        };

        static constexpr int commandQueueCapacity = 64; // generous headroom for a burst project load

        std::atomic<uint32_t> commandQueueHead { 0 }; // producer (PARENT)-owned: next slot to WRITE
        std::atomic<uint32_t> commandQueueTail { 0 }; // consumer (SHARED HOST CHILD)-owned: next slot to READ

        SpawnCommand commandQueue[commandQueueCapacity] {};
    };

    constexpr int64_t sharedHostCommandChannelBytes = (int64_t) sizeof (SharedHostCommandBlock);

    inline juce::File getSharedHostCommandChannelFile()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("CrateIPC_SharedHostCommands.shm");
    }

    inline void ensureSharedHostCommandChannelFileIsSized (const juce::File& file)
    {
        if (file.getSize() == sharedHostCommandChannelBytes)
            return;

        file.deleteFile();

        if (std::unique_ptr<juce::FileOutputStream> out { file.createOutputStream() })
        {
            out->setPosition (sharedHostCommandChannelBytes - 1);
            out->writeByte (0);
        }
    }

    inline SharedHostCommandBlock* getSharedHostCommandBlock (void* mappedData) noexcept
    {
        return reinterpret_cast<SharedHostCommandBlock*> (mappedData);
    }

    // Fixed name, same reasoning as the file above — one shared host means
    // one wake event, no instanceId suffix needed (contrast
    // getBufferReadyEventName(), which MUST be per-instance).
    inline juce::String getSharedHostCommandReadyEventName()
    {
        return "CrateIPC_SharedHostCommand_Event";
    }

    // Step 15.1 directive: the CLI flag SandboxManager launches
    // CrateSandbox.exe with to request Shared Multi-Tenant Mode instead of
    // the normal single-instance isolated mode. CrateSandbox's own
    // initialise() checks its trimmed command line against this exact
    // string before falling back to treating it as a per-instance UUID —
    // a function, not a variable, so there's no static-initialization-
    // order question across the two .cpp files that include this header.
    inline juce::String getSharedHostModeFlag()
    {
        return "--shared-host";
    }

    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // the CLI flag SandboxManager launches a POOLED CrateSandbox.exe with,
    // alongside its instanceId (e.g. "<uuid> --cryosleep") — distinguishes
    // "map my shared memory, then BLOCK on a claim event instead of loading
    // a plugin immediately" from normal isolated mode's "load
    // ControlBlock::pluginPath right now" behaviour. See Main.cpp's own
    // initialise() for how this is parsed out from the instanceId token.
    inline juce::String getCryosleepModeFlag()
    {
        return "--cryosleep";
    }

    // Step 33 directive: per-pool-slot (per-instanceId), same reasoning as
    // getBufferReadyEventName() — a fixed/global name would mean every
    // pooled process waking on the SAME claim, not the specific one
    // SandboxManager actually intends to wake. Signalled by
    // SandboxManager::claimFromPool() the instant it has written the real
    // pluginPath/hostSampleRate/hostBlockSize into this slot's ControlBlock
    // — the CHILD's own CryosleepWaitThread wakes, and only THEN calls
    // loadHostedPlugin(), same as a normal cold-start would have, just
    // skipping the OS process-creation latency entirely.
    inline juce::String getCryosleepClaimEventName (const juce::String& instanceId)
    {
        return "CrateIPC_CryosleepClaim_Event_" + instanceId;
    }

    // Process Lifecycle Ownership directive: resolves CrateSandbox.exe
    // relative to the CURRENTLY RUNNING executable, so both processes agree
    // on where to find each other without a hardcoded absolute path.
    //
    //   - Packaged/installed layout: both binaries copied into the same
    //     output folder — the primary, general-purpose case.
    //   - This repo's own CMake dev build tree specifically: the two
    //     targets land in SIBLING "*_artefacts/<Config>/" folders under
    //     build/, not the same folder — a fallback for this project's own
    //     layout, not a general JUCE convention, so it's tried second.
    inline juce::File resolveSandboxExecutable()
    {
        auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        auto packaged = exeDir.getChildFile ("CrateSandbox.exe");
        if (packaged.existsAsFile())
            return packaged;

        return exeDir.getParentDirectory().getParentDirectory()
                      .getChildFile ("CrateSandbox_artefacts")
                      .getChildFile (exeDir.getFileName())
                      .getChildFile ("CrateSandbox.exe");
    }

   #if JUCE_WINDOWS
    // Step 37 (The Debt Sweep) directive, Task 4 — the actual fix for the
    // zombie-process gap every C++-destructor-based teardown path in this
    // codebase (SandboxManager's own destructor, CrateSandboxBridge::
    // terminateSandboxProcess(), CrateSandboxApplication::shutdown()) is
    // powerless against: none of those run at all if THIS process (the
    // Parent) is force-killed via Task Manager, crashes outside a clean C++
    // unwind, or the machine loses power — confirmed directly, repeatedly,
    // over the course of this very session (every rebuild needed a manual
    // `taskkill CrateSandbox.exe` after killing the main app). A single,
    // process-wide Win32 Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    // moves that guarantee into the OS kernel itself: assigning every
    // spawned CrateSandbox.exe to this ONE job means Windows force-
    // terminates the entire assigned process set the instant THIS
    // process's handle table is torn down, on ANY exit — clean or not —
    // with zero reliance on any of our own cleanup code ever running.
    // Lazily created on first use, function-local static (same singleton-
    // safety idiom as PluginHealthRegistry/SandboxManager themselves).
    // Deliberately never explicitly closed with CloseHandle() — the OS
    // closes it (and, per the limit flag, kills everything still assigned
    // to it) when THIS process exits, which is exactly the behaviour this
    // exists for.
    inline HANDLE getSandboxJobObject()
    {
        static HANDLE job = []() -> HANDLE
        {
            auto* handle = ::CreateJobObjectW (nullptr, nullptr);

            if (handle == nullptr)
                return nullptr;

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (! ::SetInformationJobObject (handle, JobObjectExtendedLimitInformation, &info, sizeof (info)))
            {
                ::CloseHandle (handle);
                return nullptr;
            }

            return handle;
        }();

        return job;
    }

    // Step 37 (The Debt Sweep) directive, Task 4: a minimal juce::ChildProcess
    // drop-in — same start()/isRunning()/kill() public contract every
    // existing call site in this codebase already assumes, INCLUDING
    // juce::ChildProcess's own documented "deleting this object won't
    // terminate the child process" behaviour (unchanged and, post-Job-
    // Object, no longer even the real safety net) — built directly on raw
    // CreateProcessW instead, since juce::ChildProcess deliberately never
    // exposes the native HANDLE/PID a caller would need to actually call
    // AssignProcessToJobObject() itself. Every process launched through
    // this (never juce::ChildProcess, for any process this codebase spawns
    // under Windows) is assigned to getSandboxJobObject() immediately
    // after CreateProcessW succeeds — pool, tenant/shared-host, and
    // isolated processes all end up in the exact same job.
    class JobObjectProcess
    {
    public:
        JobObjectProcess() = default;

        ~JobObjectProcess()
        {
            if (processInfo.hProcess != nullptr)
            {
                ::CloseHandle (processInfo.hProcess);
                ::CloseHandle (processInfo.hThread);
            }
        }

        JobObjectProcess (const JobObjectProcess&) = delete;
        JobObjectProcess& operator= (const JobObjectProcess&) = delete;

        bool start (const juce::StringArray& args)
        {
            if (args.isEmpty())
                return false;

            // Simple space-quoting — matches juce::ChildProcess's own
            // StringArray overload's stated purpose ("avoids any manual
            // quoting of the exe path itself") for the arguments this
            // codebase actually passes (an exe path, a UUID instanceId, a
            // plain mode-flag string) — none of which ever contain an
            // embedded quote character themselves.
            juce::String commandLine;

            for (auto& arg : args)
            {
                if (! commandLine.isEmpty())
                    commandLine << " ";

                commandLine << (arg.containsChar (' ') ? ("\"" + arg + "\"") : arg);
            }

            // CreateProcessW's lpCommandLine must be a MUTABLE buffer (the
            // API can rewrite it in place) — never pass a String's own
            // internal pointer directly.
            const wchar_t* wideSrc = commandLine.toWideCharPointer();
            std::vector<wchar_t> mutableCommandLine (wideSrc, wideSrc + wcslen (wideSrc) + 1);

            STARTUPINFOW startupInfo {};
            startupInfo.cb = sizeof (startupInfo);
            startupInfo.dwFlags = STARTF_USESHOWWINDOW;
            startupInfo.wShowWindow = SW_HIDE; // matches juce::ChildProcess's own no-visible-console behaviour for these helper processes

            PROCESS_INFORMATION newProcessInfo {};

            const bool ok = ::CreateProcessW (nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE,
                                               CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &newProcessInfo) != 0;

            if (! ok)
                return false;

            processInfo = newProcessInfo;

            if (auto* job = getSandboxJobObject())
                ::AssignProcessToJobObject (job, processInfo.hProcess);

            return true;
        }

        bool isRunning() const
        {
            if (processInfo.hProcess == nullptr)
                return false;

            DWORD exitCode = 0;
            return ::GetExitCodeProcess (processInfo.hProcess, &exitCode) != 0 && exitCode == STILL_ACTIVE;
        }

        bool kill()
        {
            if (processInfo.hProcess == nullptr)
                return false;

            return ::TerminateProcess (processInfo.hProcess, 0) != 0;
        }

    private:
        PROCESS_INFORMATION processInfo {};
    };
   #endif
}
