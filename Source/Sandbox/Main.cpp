#include <JuceHeader.h>
#include "../Engine/CrateIPCConstants.h"
#include "../Engine/FlightRecorder.h"

#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_map>

// Step 51 (Silent Crash Hardening) directive: SEH-wrapped calls into the
// two third-party VST3 entry points most likely to touch a plugin's own
// internal state serialization — getStateInformation() (Continuous State
// Sync / "Store A/B") and setStateInformation() (crash resurrection AND
// the A/B live-restore channel). Kept as small, standalone free functions
// with no local C++ objects requiring unwinding, deliberately — mixing a
// __try/__except block with C++ destructor unwinding in the SAME function
// is exactly the kind of thing MSVC's exception model can get wrong if
// they're tangled together; isolating the SEH boundary into its own
// minimal function sidesteps that entirely, regardless of exception model.
// Requires /EHa (see CMakeLists.txt's own comment on why it's scoped to
// ONLY this target) — under the default /EHsc, __except(EXCEPTION_EXECUTE_HANDLER)
// does NOT catch a genuine hardware exception (an access violation) at
// all; it would silently do nothing, which is worse than not having this
// at all (a false sense of safety). A caught exception here means this
// ONE call into third-party code faulted — it does NOT mean the plugin
// instance's own internal state is trustworthy afterward; the caller is
// expected to treat a false return as "this plugin may be poisoned," not
// as "handled, business as usual."
#if JUCE_WINDOWS
static bool sehGetStateInformation (juce::AudioPluginInstance* plugin, juce::MemoryBlock& chunk)
{
    __try
    {
        plugin->getStateInformation (chunk);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool sehSetStateInformation (juce::AudioPluginInstance* plugin, const void* data, int sizeInBytes)
{
    __try
    {
        plugin->setStateInformation (data, sizeInBytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#else
static bool sehGetStateInformation (juce::AudioPluginInstance* plugin, juce::MemoryBlock& chunk)
{
    plugin->getStateInformation (chunk);
    return true;
}

static bool sehSetStateInformation (juce::AudioPluginInstance* plugin, const void* data, int sizeInBytes)
{
    plugin->setStateInformation (data, sizeInBytes);
    return true;
}
#endif

// Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive, Task 1: the
// SAME SEH isolation idiom, now around the REAL real-time entry point —
// processBlock() — which is what AudioBridgeThread and LookaheadWorkerThread
// actually call every block. This is the call site the user's own concern
// (a corrupted DSP thread outputting maximum-level noise into a live PA)
// is actually about; the getState/setStateInformation wrapping from Step 51
// covers a real but different risk (state serialization faulting), not the
// audio path itself. A caught fault here does NOT attempt to salvage
// `buffer`'s contents — the caller is expected to overwrite it with silence
// itself (see AudioBridgeThread::run()'s own comment), since whatever
// partial/garbage samples the plugin wrote before faulting are exactly the
// "maximum-level noise" scenario this whole protocol exists to prevent.
#if JUCE_WINDOWS
static bool sehProcessBlock (juce::AudioPluginInstance* plugin, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    __try
    {
        plugin->processBlock (buffer, midi);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#else
static bool sehProcessBlock (juce::AudioPluginInstance* plugin, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    plugin->processBlock (buffer, midi);
    return true;
}
#endif

#if JUCE_WINDOWS
// Step 84 (White Box Diagnostics) directive, Round 4 — QA finding: a
// screenshot showed BOTH ends of the same window at once — Melda's own
// content clipped at its top-left AND a blank white gap at the
// bottom-right of the SAME editor. That combination doesn't fit a pure
// "outer window too big" theory alone (already checked — the Host's
// setContentComponentSize() genuinely reaches the real native peer, see
// PluginWindow.cpp's own matching diagnostic) — it fits a mismatch
// between what THIS process's own JUCE peer reports as its size
// (activeEditor->getWidth()/getHeight(), what gets published over IPC)
// and where Melda's OWN actual native sub-view (created by its VST3
// IPlugView, a child HWND JUCE creates one level under the peer) is
// really positioned/sized. Enumerating the peer's real Win32 child
// window tree — bypassing JUCE's own Component-level bookkeeping
// entirely, same idea as PluginWindow.cpp's GetWindowRect check on the
// Host side — is the one measurement that can show this directly instead
// of guessing at VST3/JUCE internals from the outside.
struct CrateChildRectLoggerContext
{
    HWND peerHwnd = nullptr;
    juce::StringArray* lines = nullptr;
};

static BOOL CALLBACK crateEnumChildRectLogger (HWND hwnd, LPARAM lParam)
{
    auto* context = reinterpret_cast<CrateChildRectLoggerContext*> (lParam);

    RECT screenRect {};
    GetWindowRect (hwnd, &screenRect);

    POINT topLeft { screenRect.left, screenRect.top };
    ScreenToClient (context->peerHwnd, &topLeft); // relative to the PEER's own client area, not the screen

    wchar_t className[128] {};
    GetClassNameW (hwnd, className, 128);

    context->lines->add ("child hwnd=0x" + juce::String::toHexString ((juce::pointer_sized_int) hwnd)
                              + " class=" + juce::String (className)
                              + " relativeToPeer=(" + juce::String ((int) topLeft.x) + "," + juce::String ((int) topLeft.y) + ")"
                              + " size=" + juce::String (screenRect.right - screenRect.left) + "x" + juce::String (screenRect.bottom - screenRect.top));
    return TRUE;
}

static void crateLogPeerNativeTree (HWND peerHwnd, void (*logFn) (const juce::String&))
{
    RECT windowRect {}, clientRect {};
    GetWindowRect (peerHwnd, &windowRect);
    GetClientRect (peerHwnd, &clientRect);

    logFn ("CrateSandbox: DIAG WHITE BOX peer windowRect=" + juce::String (windowRect.right - windowRect.left) + "x" + juce::String (windowRect.bottom - windowRect.top)
               + " clientRect=" + juce::String (clientRect.right - clientRect.left) + "x" + juce::String (clientRect.bottom - clientRect.top));

    juce::StringArray childLines;
    CrateChildRectLoggerContext context { peerHwnd, &childLines };
    EnumChildWindows (peerHwnd, crateEnumChildRectLogger, (LPARAM) &context);

    if (childLines.isEmpty())
        logFn ("CrateSandbox: DIAG WHITE BOX peer has NO native child windows at all — Melda's own view isn't a separate HWND under this peer.");

    for (auto& line : childLines)
        logFn ("CrateSandbox: DIAG WHITE BOX " + line);
}
#endif

/**
    Plugin Sandboxing Step 5 (The Headless IPC Host Skeleton) — entry point
    for CrateSandbox.exe, the out-of-process plugin host. Console subsystem
    (juce_add_console_app in CMakeLists.txt), no GUI modules — this process
    exists to eventually host one third-party plugin instance in isolation so
    a crash there can never take down the main DAW.

    Step 5 did only the structural minimum: create/own the CrateIPC_Memory
    shared block and sit in the normal JUCE event loop until told to quit.

    STEP 5.5 (Process Management & The Atomic Heartbeat) adds the health
    signal the PARENT (CrateSandboxBridge) needs to tell "sandbox alive" from
    "sandbox silently died" — a dead child that never signals anything looks
    identical to a live-but-idle one otherwise, which is exactly the ghost
    the parent must never trust. HeartbeatThread is a thin driver (same shape
    as CrateAnticipativeWrapper::ShadowWorker) that does nothing but
    increment the shared ControlBlock's counter every
    CrateIPC::heartbeatIntervalMs — the parent's own timer polls that counter
    independently and declares this process dead the instant it stalls for
    longer than CrateIPC::heartbeatTimeoutMs.

    Placement-new the ControlBlock EVERY launch, unconditionally — this
    process is always the sole owner/writer of that struct, and a fresh
    launch (whether the very first one or a restart after the parent killed
    a stalled instance) always means "start the heartbeat sequence over from
    0," never "resume whatever a previous process left behind."

    STEP 6 (The IPC Audio Arteries / Echo-Phase Test) added the actual audio
    round trip: the parent writes a block into audioInput, AudioBridgeThread
    inverts every sample's phase (multiply by -1.0f) into audioOutput before
    signalling childProcessed. Phase inversion — not a passthrough — is the
    deliberate proof: a passthrough could be an accidental no-op hiding
    behind a green checkmark; a bit-exact inverted waveform can only be
    produced by code that genuinely executed on THIS side of the process
    boundary. No real plugin hosting yet — that's a later step, once this
    round trip itself is proven.

    STEP 6.5 (The Hybrid Sync Pivot) replaced Step 6's parentReady-polling
    loop (Thread::yield() in a tight while loop — measured at 93.6% of a
    full CPU core, CONSTANTLY, whether or not any audio was flowing) with a
    genuine OS-native block: AudioBridgeThread now sleeps on
    CrateIPC::NamedEvent::wait(), a raw Win32 auto-reset event, using real
    kernel-level 0% CPU while idle. The PARENT still fires
    CrateIPC::NamedEvent::signal() (a fast, non-blocking kernel call — safe
    on the audio thread) right after publishing parentReady, and still
    spin-waits on childProcessed itself (see CrateSandboxBridge's own doc
    comment for why the PARENT specifically can never sleep/block). This is
    a genuinely asymmetric design on purpose: the CHILD can afford to block
    (it has nothing else to do while idle), the PARENT never can (it IS the
    real-time audio thread).

    STEP 7 (The VST3 Host Engine) replaces Step 6's phase-inversion proof
    with real third-party DSP — the round trip mechanics above are entirely
    UNCHANGED, only what AudioBridgeThread does with the samples differs.
    loadHostedPlugin(), called from initialise() BEFORE either thread
    starts, reads pluginPath/hostSampleRate/hostBlockSize out of the
    ControlBlock (written by the PARENT before this process even existed —
    see CrateSandboxBridge's own doc comment) and synchronously loads a VST3
    via juce::AudioPluginFormatManager + juce::addHeadlessDefaultFormatsToManager()
    (the headless variant — this process has no GUI modules at all, so a
    plugin's own editor UI, if it has one, is never created; matches the
    step's own directive to skip the GUI bridge for now). AudioBridgeThread
    then calls hostedPlugin->processBlock() on a PRE-ALLOCATED scratch
    buffer (sized once, in the constructor, to the worst case this bridge
    supports) instead of a fresh juce::AudioBuffer per block — allocating on
    every round trip would be exactly the kind of avoidable real-time cost
    the Zero Allocation Rule elsewhere in this codebase (CrateAnticipativeWrapper's
    pre-allocated buffer pool) exists to prevent, and this thread's own
    latency budget matters just as much (it eats directly into the PARENT's
    spinWaitTimeoutMs). No plugin loaded (empty path, or load/instantiate
    failure) falls back to a plain passthrough rather than silently
    reintroducing Step 6's phase-inversion behavior.

    STEP 9 (The Multi-Process Scalability Stress Test) directive — REQUIRED
    ARCHITECTURE FIX: this process used to resolve a FIXED, GLOBAL shared
    file/event name, correct only because exactly one CrateSandbox instance
    ever ran at a time. Running 50 concurrently means 50 processes would
    otherwise all map the SAME 4MB block and block on the SAME wake event —
    see CrateSandboxBridge's own doc comment for the full reasoning. This
    process now reads its instanceId out of its OWN command line (the exact
    juce::Uuid string its PARENT generated and passed to sandboxProcess.start())
    and uses it for every getSharedMemoryFile()/getBufferReadyEventName() call
    — the one piece of information that MUST be known before this process can
    create or map anything, so it's a launch-time argument, not something
    read out of the (not-yet-mapped) shared file itself.

    STEP 10 (Cross-Process Window Reparenting) directive: this process was
    built headless from Step 5 onward — no GUI modules deliberately linked
    — but tracktion_engine itself has ALWAYS transitively pulled in the
    FULL juce_audio_processors (which depends on juce_gui_extra ->
    juce_gui_basics), confirmed by inspecting this exact target's own build
    log (juce_gui_basics*.cpp, juce_gui_extra.cpp, juce_graphics.cpp all
    already compile into CrateSandbox.exe, unused, since Step 7). No new
    CMake dependency was needed for this step — loadHostedPlugin() switches
    from juce::addHeadlessDefaultFormatsToManager() (which the JUCE source
    itself documents as ALWAYS returning nullptr from createEditor() / false
    from hasEditor(), regardless of what the underlying VST3 actually
    supports) to juce::addDefaultFormatsToManager(), the GUI-capable
    variant, which is what makes a real editor possible at all.

    timerCallback() below polls windowHandleRequested (the PARENT sets it
    via CrateSandboxBridge::requestEditorWindow()) on THIS process's own
    message thread — editor/Component creation is a message-thread-only
    operation in JUCE, which is exactly why this can't happen on
    AudioBridgeThread (a plain worker thread, not message-thread-affine).
    On first request: createEditorAndMakeActive() (owned via a
    std::unique_ptr — the processor only tracks a non-owning reference
    internally, per juce::AudioProcessor::getActiveEditor()'s own doc
    comment), addToDesktop(0) to force JUCE to create a REAL top-level
    native ComponentPeer/HWND (never shown to a user directly — it's
    reparented into the PARENT's own window instead), then
    getPeer()->getNativeHandle() for the raw HWND, published as a plain
    int64_t (see ControlBlock's own doc comment on why an integer, not a
    pointer, crosses the shared-memory boundary).

    THE GOLDEN RULE (Complete Decoupling) holds here for free, not because
    of anything extra built — genuine OS-level SetParent() reparenting (see
    CrateSandboxBridge's own comment on juce::HWNDComponent) means the
    PARENT'S window is just displaying a real OS surface the Desktop Window
    Manager keeps compositing regardless of whether THIS process's message
    loop is currently responsive. If this process's UI thread hangs, the
    embedded view simply stops updating (a frozen last-rendered frame) —
    the PARENT never blocks waiting on anything from this process to render
    its OWN window, because there is nothing to wait on: the window just
    IS, at the OS level, independent of either process's own responsiveness.

    STEP 11 (Absolute Muscle Memory / Continuous State Sync) directive: a
    StateChangeListener (juce::AudioProcessorListener) attached to
    hostedPlugin in loadHostedPlugin() reacts to
    audioProcessorParameterChangeGestureEnd() (the "endEdit" moment — the
    user released a knob) and audioProcessorChanged() (generic state-change
    notifications, e.g. a program change). BOTH can fire SYNCHRONOUSLY from
    whatever thread the plugin itself calls back on — JUCE's own header
    comment on these methods explicitly warns they may run "during their
    audio callback" — so the listener does the absolute minimum possible:
    set one atomic<bool> and call StateExtractionThread::notify(). The
    actual getStateInformation() call (which can mean copying MEGABYTES for
    a sampler) happens entirely on StateExtractionThread, never blocking
    whatever thread the plugin's own notification arrived on.

    pluginAccessLock (a std::atomic_flag spinlock, same idiom as
    CrateAnticipativeWrapper's own dspLock) is a NEW protection this step
    requires: AudioBridgeThread's processBlock() call and
    StateExtractionThread's getStateInformation()/setStateInformation()
    calls are now two INDEPENDENT threads that can both touch the SAME
    plugin instance concurrently — most plugins are not guaranteed
    thread-safe for that. Both sides try-lock-or-skip/back-off rather than
    block indefinitely, matching this codebase's "never let one path wait
    forever on another" convention.

    The extracted chunk is published through the PUSH channel
    (ControlBlock::stateChunkData/stateChunkSize/stateChunkAvailable,
    guarded by stateChunkLock) — the PARENT (CrateSandboxBridge) polls it
    on its own message-thread timer and keeps the latest bytes in
    lastKnownState. On every launch (including a restart after this exact
    process crashes or stalls), the PARENT writes lastKnownState into the
    INITIAL-LOAD channel (initialStateData/initialStateSize) BEFORE the new
    child even starts — loadHostedPlugin() below applies it via
    setStateInformation() immediately after prepareToPlay(), which is the
    entire mechanism behind the "Ghost Reload": a freshly spawned,
    completely different OS process ends up with the exact same plugin
    state the dead one had a moment before.
*/
class CrateSandboxApplication : public juce::JUCEApplication,
                                 private juce::Timer,
                                 private juce::ComponentListener
{
public:
    CrateSandboxApplication() = default;

    const juce::String getApplicationName() override    { return "CrateSandbox"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override           { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Step 74 (The Flight Recorder) directive, Task 1a: same
        // installation as the Host's own initialise() — the CHILD process
        // is exactly as likely (more so, given it's the one loading
        // untrusted third-party VST3 binaries directly) to hit a fault
        // this flight recorder needs to survive.
        installFlightRecorderCrashHandler();
        CRATE_FR_LOG ("LIFECYCLE", "Child process initialise() entered.");

        // Step 15.1/15.2 (Multi-Tenant IPC & The Shared Host Engine)
        // directive: checked FIRST, before any of the per-instance
        // ControlBlock mapping below even starts — a Shared Sandbox host is
        // a fundamentally different process role (many tenants, no single
        // pluginPath/instanceId of its own) from everything after this
        // branch, which is the existing one-process-one-plugin isolated
        // mode.
        if (commandLine.trim() == CrateIPC::getSharedHostModeFlag())
        {
            runningAsSharedHost = true;
            logToFile ("CHILD: Booted in Shared Multi-Tenant Mode. Listening for dynamic instantiation commands...");

            // The PARENT (SandboxManager::ensureSharedHostRunning()) already
            // created/sized this file and wrote nothing else into it before
            // launching this process — same "parent prepares the file
            // first" discipline as the per-instance ControlBlock, applied
            // to the Master Control Channel instead.
            auto file = CrateIPC::getSharedHostCommandChannelFile();
            commandChannelMemory = std::make_unique<juce::MemoryMappedFile> (file, juce::MemoryMappedFile::readWrite);

            const bool mapped = commandChannelMemory->getData() != nullptr
                                 && commandChannelMemory->getSize() == (size_t) CrateIPC::sharedHostCommandChannelBytes;

            if (! mapped)
            {
                logToFile ("CrateSandbox (Shared Host): FAILED to map Master Control Channel at " + file.getFullPathName());
                return;
            }

            commandBlock = CrateIPC::getSharedHostCommandBlock (commandChannelMemory->getData());
            commandReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getSharedHostCommandReadyEventName());

            commandListenerThread = std::make_unique<CommandListenerThread> (*commandBlock, *commandReadyEvent,
                [this] (CommandListenerThread::CommandType type, juce::String pluginUID, juce::String pluginPath, juce::String commandInstanceId)
                {
                    // Step 15.4 (The Teardown Protocol) directive: route by
                    // command type — see CommandListenerThread's own doc
                    // comment on why this queue now carries two kinds of
                    // command.
                    if (type == CommandListenerThread::CommandType::Spawn)
                        spawnTenant (pluginUID, pluginPath, commandInstanceId);
                    else
                        unloadTenant (commandInstanceId);
                });
            commandListenerThread->startThread (juce::Thread::Priority::low);

            // Same cadence as the isolated path's own window/poison-pill
            // poll — timerCallback() below branches on runningAsSharedHost
            // and iterates every tenant instead of assuming a single one.
            startTimer (CrateIPC::livenessCheckIntervalMs);
            return;
        }

        // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture)
        // directive: a pooled process is launched with BOTH its instanceId
        // AND the cryosleep flag as separate tokens (e.g. "<uuid> --cryosleep")
        // — split them apart here rather than treating the whole trimmed
        // string as the instanceId the way every other launch mode does.
        auto trimmedCommandLine = commandLine.trim();
        isCryosleeping = trimmedCommandLine.contains (CrateIPC::getCryosleepModeFlag());

        if (isCryosleeping)
            instanceId = trimmedCommandLine.upToFirstOccurrenceOf (CrateIPC::getCryosleepModeFlag(), false, false).trim();
        else
            instanceId = trimmedCommandLine;

        // Step 9 directive: the ONE piece of bootstrap info this process
        // needs before it can create/map anything — see class doc comment.
        // Falls back to a fixed name if launched standalone with no
        // argument (e.g. manual testing), matching this process's original
        // single-instance behaviour rather than failing outright.
        if (instanceId.isEmpty())
            instanceId = "standalone";

        auto file = CrateIPC::getSharedMemoryFile (instanceId);

        // Fixed-Size Pre-Allocation directive: same shared helper the
        // PARENT now also calls (see CrateSandboxBridge::launchSandboxProcess())
        // — a juce::MemoryMappedFile maps an EXISTING byte range, it doesn't
        // grow the file itself. As of Step 7 this is usually already sized
        // correctly by the time this process starts (the parent does it
        // before ever launching the child), but calling it here too keeps
        // this process correct even if launched standalone for testing.
        CrateIPC::ensureSharedMemoryFileIsSized (file);

        sharedMemory = std::make_unique<juce::MemoryMappedFile> (file, juce::MemoryMappedFile::readWrite);

        const bool mapped = sharedMemory->getData() != nullptr
                             && sharedMemory->getSize() == (size_t) CrateIPC::sharedMemoryBytes;

        logToFile (mapped
            ? "CrateSandbox: CrateIPC_Memory mapped OK (" + juce::String (CrateIPC::sharedMemoryBytes) + " bytes) at "
                  + file.getFullPathName()
            : "CrateSandbox: FAILED to map CrateIPC_Memory at " + file.getFullPathName());

        if (mapped)
        {
            auto* block = CrateIPC::getControlBlock (sharedMemory->getData());

            // The Initialization Payload directive (Step 7): the PARENT
            // writes pluginPath/hostSampleRate/hostBlockSize into this same
            // file BEFORE this process ever starts — cache them before the
            // reset below (placement-new default-constructs the WHOLE
            // struct, which would otherwise wipe them along with everything
            // else) and restore them immediately after.
            char preservedPluginPath[CrateIPC::ControlBlock::maxPluginPathLength];
            std::memcpy (preservedPluginPath, block->pluginPath, sizeof (preservedPluginPath));
            const double preservedSampleRate = block->hostSampleRate;
            const int preservedBlockSize     = block->hostBlockSize;

            // Step 11 directive: initialStateSize/initialStateData are the
            // SAME write-once-before-launch category as pluginPath above —
            // the PARENT writes its lastKnownState here BEFORE every
            // launch, including a restart after THIS exact process
            // crashes, so it must survive the reset just like pluginPath
            // does. Copied to the HEAP (not the stack — up to 4MB, too
            // large to risk on a thread's default stack) and only for the
            // ACTUAL reported size, not the full 4MB buffer every time.
            const int64_t preservedStateSize = block->initialStateSize;
            std::vector<char> preservedStateData;

            if (preservedStateSize > 0 && preservedStateSize <= CrateIPC::ControlBlock::maxStateChunkBytes)
            {
                preservedStateData.resize ((size_t) preservedStateSize);
                std::memcpy (preservedStateData.data(), block->initialStateData, (size_t) preservedStateSize);
            }

            // Step 10 directive: if a window had already been requested
            // before this exact process died, a fresh child should
            // automatically recreate and republish one too — the PARENT's
            // own test harness watches for a NEW handle value and re-embeds
            // (see MainComponent's own reconnect-watcher) rather than
            // needing to notice the restart and re-request manually.
            const bool preservedWindowHandleRequested = block->windowHandleRequested.load (std::memory_order_relaxed);

            new (block) CrateIPC::ControlBlock(); // fresh heartbeat=0 every launch — see class doc comment

            std::memcpy (block->pluginPath, preservedPluginPath, sizeof (preservedPluginPath));
            block->hostSampleRate = preservedSampleRate;
            block->hostBlockSize  = preservedBlockSize;

            block->initialStateSize = preservedStateSize;
            if (! preservedStateData.empty())
                std::memcpy (block->initialStateData, preservedStateData.data(), preservedStateData.size());

            block->windowHandleRequested.store (preservedWindowHandleRequested, std::memory_order_relaxed);

            controlBlock = block;

            // RESTART LIVELOCK (caught live — the parent's own restart log
            // showed hundreds of restarts per second): starting the
            // heartbeat AFTER loadHostedPlugin() meant a real VST3's
            // scan+instantiate time (easily >50ms — a whole DLL load, not a
            // trivial operation) NEVER left a chance for a single heartbeat
            // tick to land before the PARENT's heartbeatTimeoutMs elapsed
            // and declared this process dead, killing it mid-load — every
            // single restart attempt, forever, since the replacement
            // process would hit the exact same fate. The heartbeat must
            // start FIRST, decoupling "this process is alive" from "the
            // plugin has finished loading" — a slow plugin now just means a
            // brief window where the audio bridge is running with no plugin
            // yet (plain passthrough, see AudioBridgeThread), never a false
            // "dead" verdict.
            heartbeatThread = std::make_unique<HeartbeatThread> (*controlBlock);

            // Step 34 (Zero-CPU Heartbeat) directive: set BEFORE
            // startThread() so a cryosleeping launch never ticks fast even
            // for the first cycle.
            if (isCryosleeping)
                heartbeatThread->setActivelyServing (false);

            heartbeatThread->startThread (juce::Thread::Priority::normal);

            // Step 34 (Zero-CPU Heartbeat) directive — a second, larger
            // source of idle CPU than the heartbeat itself, found by direct
            // measurement AFTER throttling the heartbeat alone didn't reach
            // anywhere near 0%: both of these threads block on their own
            // readyEvent with only a 50ms timeout (so threadShouldExit()
            // gets checked regularly), meaning ~20 wake-ups/sec EACH, all
            // the time, whether or not a plugin is even loaded. A
            // cryosleeping, unclaimed pool process has NO plugin and NOTHING
            // in the Edit referencing its instanceId yet, so NEITHER thread
            // has any real work they could ever be woken for — the
            // genuinely correct fix isn't throttling them (another flag,
            // another wait-interval branch), it's not starting them AT ALL
            // until a claim actually happens. See the onClaimed callback
            // below, which constructs and starts both at that point instead
            // — the exact same objects, just created only once there's a
            // real reason for them to exist.
            if (! isCryosleeping)
            {
                audioBridgeThread = std::make_unique<AudioBridgeThread> (*controlBlock, hostedPluginPtr, instanceId, pluginAccessLock, isCorrupted);
                audioBridgeThread->startThread (juce::Thread::Priority::high);

                lookaheadWorkerThread = std::make_unique<LookaheadWorkerThread> (*controlBlock, hostedPluginPtr, instanceId, pluginAccessLock, isCorrupted);
                lookaheadWorkerThread->startThread (juce::Thread::Priority::low);
            }

            // Step 33 (Cryosleep Architecture) directive: a pooled process
            // has NO plugin assigned yet at this point — block->pluginPath
            // is empty, this is exactly what "warm but idle" means. Instead
            // of loading immediately, block on the per-instance claim event
            // via a dedicated thread (see CryosleepWaitThread's own doc
            // comment) — genuinely 0% CPU while waiting, not a spin-loop —
            // and defer loadHostedPlugin() until SandboxManager actually
            // claims this slot and writes a real pluginPath in.
            if (isCryosleeping)
            {
                logToFile ("CrateSandbox: booted in Cryosleep mode [instanceId=" + instanceId + "] — waiting for claim.");

                cryosleepWaitThread = std::make_unique<CryosleepWaitThread> (instanceId, [this]
                {
                    // Step 34 (Zero-CPU Heartbeat) directive: flipped back
                    // to fast IMMEDIATELY, right here on CryosleepWaitThread's
                    // own thread (setActivelyServing() is safe to call from
                    // any thread) — before the message-thread hop below even
                    // happens, so there's no window where the PARENT's own
                    // liveness poll (which starts once claimFromPool()
                    // returns, on ITS side) could observe this process still
                    // ticking slow.
                    if (heartbeatThread != nullptr)
                        heartbeatThread->setActivelyServing (true);

                    // Fires from CryosleepWaitThread's own thread the
                    // instant the claim event is signalled — hopped onto
                    // the message thread since loadHostedPlugin() (and
                    // constructing the two threads below) touches
                    // juce::Component-adjacent state (the AudioPluginFormatManager/
                    // editor machinery)/should only ever start from there.
                    juce::MessageManager::callAsync ([this]
                    {
                        if (controlBlock == nullptr)
                            return;

                        // Step 34 directive: created here, now that a real
                        // plugin is actually about to load — exactly the
                        // same construction/start a normal (non-pooled)
                        // launch already does in initialise(), just deferred
                        // to the moment there's real work for them.
                        audioBridgeThread = std::make_unique<AudioBridgeThread> (*controlBlock, hostedPluginPtr, instanceId, pluginAccessLock, isCorrupted);
                        audioBridgeThread->startThread (juce::Thread::Priority::high);

                        lookaheadWorkerThread = std::make_unique<LookaheadWorkerThread> (*controlBlock, hostedPluginPtr, instanceId, pluginAccessLock, isCorrupted);
                        lookaheadWorkerThread->startThread (juce::Thread::Priority::low);

                        logToFile ("CrateSandbox: claimed from Cryosleep pool [instanceId=" + instanceId + "] — loading assigned plugin now.");
                        loadHostedPlugin (juce::String (controlBlock->pluginPath),
                                          controlBlock->hostSampleRate, controlBlock->hostBlockSize);
                    });
                });
                cryosleepWaitThread->startThread (juce::Thread::Priority::low);
            }
            else
            {
                loadHostedPlugin (juce::String (block->pluginPath), block->hostSampleRate, block->hostBlockSize);
            }

            // Step 10 directive: same cadence as the PARENT's own health-
            // check timer (CrateIPC::livenessCheckIntervalMs) — polls
            // windowHandleRequested on THIS process's message thread, the
            // only thread Component/editor creation is legal on.
            startTimer (CrateIPC::livenessCheckIntervalMs);
        }
    }

    void shutdown() override
    {
        stopTimer();

        // Step 33 (Cryosleep Architecture) directive: an unclaimed pooled
        // process shutting down (e.g. the whole app closing before this
        // slot was ever claimed) needs this stopped like any other thread
        // — signalThreadShouldExit() alone wouldn't wake it early (it's
        // blocked in claimEvent.wait(), not polling threadShouldExit()
        // directly), so stopThread()'s own timeout-then-force path is what
        // actually unblocks it here.
        if (cryosleepWaitThread != nullptr)
            cryosleepWaitThread->stopThread (2000);
        cryosleepWaitThread.reset();

        // Step 15.2 directive: tear down every tenant BEFORE the command
        // channel mapping itself — commandListenerThread first (it's the
        // only thing that could otherwise still be dequeuing a command
        // mid-teardown), then the tenants map (each TenantContext's own
        // destructor handles its own thread/plugin/editor teardown, same
        // ordering discipline as the isolated path below), then the
        // channel mapping last.
        if (commandListenerThread != nullptr)
            commandListenerThread->stopThread (1000);
        commandListenerThread.reset();

        tenants.clear();

        commandBlock = nullptr;
        commandChannelMemory.reset();
        commandReadyEvent.reset();

        if (audioBridgeThread != nullptr)
            audioBridgeThread->stopThread (1000);
        audioBridgeThread.reset();

        if (lookaheadWorkerThread != nullptr)
            lookaheadWorkerThread->stopThread (1000);
        lookaheadWorkerThread.reset();

        if (heartbeatThread != nullptr)
            heartbeatThread->stopThread (1000);
        heartbeatThread.reset();

        // Step 11 directive: stop BEFORE removing the listener/destroying
        // the plugin — same "owner outlives what it owns" ordering as
        // everything else here. stopThread() calls notify() internally to
        // wake a thread blocked in wait(-1) (JUCE's own documented
        // behaviour), so this doesn't need its own special wake call.
        if (stateExtractionThread != nullptr)
            stateExtractionThread->stopThread (2000); // getStateInformation() on a huge chunk may briefly outlast the more typical 1000ms budget elsewhere
        stateExtractionThread.reset();

        if (hostedPlugin != nullptr && stateChangeListener != nullptr)
            hostedPlugin->removeListener (stateChangeListener.get());
        stateChangeListener.reset();

        // Step 10 directive: destroy the editor BEFORE the plugin instance
        // it belongs to — same "owner outlives what it owns" ordering
        // discipline as everything else torn down in this method.
        activeEditor.reset();

        // The VST3 Host Engine directive: release AFTER the thread that
        // calls processBlock() on it has fully stopped — same teardown
        // ordering discipline as everything else in this codebase (worker
        // before disk streams, etc.). audioBridgeThread is already stopped
        // by this point, so clearing hostedPluginPtr here is belt-and-
        // suspenders hygiene, not a race fix.
        hostedPluginPtr.store (nullptr, std::memory_order_release);

        if (hostedPlugin != nullptr)
            hostedPlugin->releaseResources();
        hostedPlugin.reset();

        sharedMemory.reset();

        // Step 74 (The Flight Recorder) directive, Task 1c: normal,
        // graceful exit — last, after everything else above has already
        // torn down cleanly.
        FlightRecorder::getInstance().flushToDisk ("GRACEFUL_SHUTDOWN");
    }

    // Step 81 (Event-Driven Geometry Publish) directive — QA finding:
    // relying purely on this class's own 20ms poll (see timerCallback()'s
    // own call site, below) to notice the editor's real size means the
    // Host learns it up to one whole tick late in the worst case. Some
    // heavy third-party UIs (Direct2D/custom-toolkit plugins in
    // particular) don't finish laying themselves out on the exact tick
    // addToDesktop() returns — attaching a juce::ComponentListener directly
    // to the editor and republishing the INSTANT it reports its own real
    // bounds closes that gap at the source, rather than waiting for the
    // next poll to notice. Factored out so BOTH the poll (timerCallback())
    // and this listener call the exact same throttled-store logic — never
    // two copies that could drift apart.
    void publishEditorSizeIfChanged (int currentWidth, int currentHeight)
    {
        if (controlBlock == nullptr || currentWidth <= 0 || currentHeight <= 0)
            return; // Step 68's own established sentinel: 0 never means "a real size," only "not known yet"

        if (currentWidth  == controlBlock->windowWidth.load (std::memory_order_relaxed)
            && currentHeight == controlBlock->windowHeight.load (std::memory_order_relaxed))
            return;

        const auto nowOutboundMs = juce::Time::getMillisecondCounter();

        if (nowOutboundMs - lastOutboundReportMs < (juce::uint32) outboundReportThrottleMs)
            return;

        lastOutboundReportMs = nowOutboundMs;
        controlBlock->windowWidth.store (currentWidth, std::memory_order_relaxed);
        controlBlock->windowHeight.store (currentHeight, std::memory_order_release);
    }

    // Step 81 directive: componentMovedOrResized() fires synchronously,
    // on this SAME message thread, the instant the editor's own layout
    // pass actually changes its bounds — registered once, right after
    // addToDesktop() (see that call site's own comment), removed
    // automatically by juce::Component's own destructor bookkeeping
    // whenever activeEditor itself is destroyed, so there's no dangling-
    // listener risk across a recreation.
    void componentMovedOrResized (juce::Component& component, bool /*wasMoved*/, bool wasResized) override
    {
        if (wasResized && activeEditor != nullptr && &component == activeEditor.get())
            publishEditorSizeIfChanged (activeEditor->getWidth(), activeEditor->getHeight());
    }

    // Step 10 directive: the ONLY thing this timer does — see class doc
    // comment for why editor creation must happen here (message thread)
    // and can't happen on AudioBridgeThread.
    //
    // Step 15.2 directive: branches at the top — a Shared Sandbox host
    // services EVERY tenant's poison-pill/window-request state each tick
    // (see serviceTenant() below), instead of the single controlBlock/
    // activeEditor pair the isolated-mode body further down still assumes.
    // Never both in the same process (see runningAsSharedHost's own doc
    // comment).
    void timerCallback() override
    {
        if (runningAsSharedHost)
        {
            for (auto& entry : tenants)
                serviceTenant (*entry.second);

            return;
        }

        // The Poison Pill directive (Step 12.1): checked FIRST,
        // unconditionally, every tick — a genuine, deliberate, unhandled
        // access violation. This crashes the WHOLE process immediately;
        // nothing after this line ever executes once triggered. Debug/test
        // mechanism only — see CrateIPC::ControlBlock::triggerCrashRequested's
        // own doc comment for why this exists at all (proving the parent's
        // resurrection logic against a REAL OS-level crash, not just a
        // clean kill).
        if (controlBlock != nullptr && controlBlock->triggerCrashRequested.load (std::memory_order_acquire))
        {
            logToFile ("CrateSandbox: poison pill triggered — crashing deliberately now.");
            volatile int* p = nullptr;
            *p = 42;
        }

        if (controlBlock == nullptr)
            return;

        // Step 31 (Real IPC Parameter Sync) directive: the value-readback
        // half — runs every tick regardless of whether the editor is even
        // open, since DAW-side automation could move a parameter's VALUE
        // independent of any UI (this only reads CURRENT values already
        // updated by drainParameterQueue()'s own setValue() calls, or by
        // the plugin's own internal UI). Only bumps paramValueRevision when
        // something actually changed, so the PARENT can cheaply skip the
        // common "nothing moved" case.
        if (controlBlock->paramMetadataReady.load (std::memory_order_acquire) && hostedPlugin != nullptr)
        {
            auto& liveParams = hostedPlugin->getParameters();
            const int syncedCount = controlBlock->paramCount.load (std::memory_order_relaxed);
            bool anyChanged = false;

            for (int i = 0; i < syncedCount && i < liveParams.size(); ++i)
            {
                const float newValue = liveParams[i]->getValue();
                const float oldValue = controlBlock->paramCurrentValues[i].load (std::memory_order_relaxed);

                if (std::abs (newValue - oldValue) > 0.0001f)
                {
                    controlBlock->paramCurrentValues[i].store (newValue, std::memory_order_relaxed);
                    anyChanged = true;
                }
            }

            if (anyChanged)
                controlBlock->paramValueRevision.fetch_add (1, std::memory_order_release);
        }

        // Step 39 (Live Telemetry) directive, Task 3: latency rarely
        // changes after load, so this is a cheap "only act on genuine
        // change" comparison every tick, same convention as windowWidth/
        // Height. lastProcessBlockMicros itself is written directly by
        // AudioBridgeThread (see its own comment) — nothing to do here for
        // that field, it's already live in the shared ControlBlock.
        if (hostedPlugin != nullptr)
        {
            const int32_t currentLatency = hostedPlugin->getLatencySamples();

            if (currentLatency != controlBlock->pluginLatencySamples.load (std::memory_order_relaxed))
                controlBlock->pluginLatencySamples.store (currentLatency, std::memory_order_relaxed);
        }

        // Step 39 (A/B Testing) directive, Task 4: a LIVE state restore
        // request from the PARENT (the A/B toggle) — see
        // ControlBlock::liveRestoreRequested's own doc comment for why this
        // is a separate channel from the crash-resurrection payload.
        // Guarded by the SAME bounded try-lock-or-skip idiom every other
        // cross-thread touch of hostedPlugin already uses in this codebase
        // (Step 37 fixed StateExtractionThread's own version of exactly
        // this pattern) — never blocks this tick indefinitely, and simply
        // tries again next tick if AudioBridgeThread currently holds the
        // lock.
        if (controlBlock->liveRestoreRequested.load (std::memory_order_acquire) && hostedPlugin != nullptr)
        {
            int attempts = 0;
            bool acquired = false;

            while (! (acquired = ! pluginAccessLock.test_and_set (std::memory_order_acquire)))
            {
                if (++attempts > 100)
                    break;
                juce::Thread::sleep (1);
            }

            if (acquired)
            {
                const int64_t size = controlBlock->liveRestoreStateSize.load (std::memory_order_relaxed);

                if (size > 0 && size <= CrateIPC::ControlBlock::maxStateChunkBytes)
                {
                    // Store A/B Amnesia Fix (Step 53) directive: bounded
                    // spin on liveRestoreLock, held for the duration of the
                    // read (the sehSetStateInformation() call below copies
                    // these bytes internally before returning, so holding
                    // the lock just long enough to cover that one call is
                    // sufficient) — see ControlBlock::liveRestoreLock's own
                    // doc comment for the torn-buffer race this closes
                    // (a fast A->B->A switch overwriting this SAME buffer
                    // while it's still being read out of).
                    int lockAttempts = 0;
                    bool lockAcquired = false;

                    while (! (lockAcquired = ! controlBlock->liveRestoreLock.test_and_set (std::memory_order_acquire)))
                    {
                        if (++lockAttempts > 100)
                            break;
                        juce::Thread::sleep (1);
                    }

                    if (lockAcquired)
                    {
                        // Step 51 (Silent Crash Hardening) directive: see
                        // sehSetStateInformation()'s own doc comment. A caught
                        // fault here is logged loudly rather than propagating a
                        // raw access violation into an unhandled process crash
                        // — the plugin instance may well be poisoned afterward
                        // (this is NOT a claim that continuing to use it is
                        // safe), but at minimum the sandbox process itself, and
                        // every OTHER tenant it may be hosting, survives this
                        // one bad restore attempt.
                        if (sehSetStateInformation (hostedPlugin.get(), controlBlock->liveRestoreStateData, (int) size))
                            logToFile ("CrateSandbox: DIAG A/B — applied live restore, " + juce::String (size) + " bytes via setStateInformation().");
                        else
                            logToFile ("CrateSandbox: setStateInformation() faulted (caught via SEH) during live restore — plugin instance may be unstable.");

                        controlBlock->liveRestoreLock.clear (std::memory_order_release);
                    }
                    else
                    {
                        logToFile ("CrateSandbox: DIAG A/B — liveRestoreLock contended, skipping this restore request rather than risking a torn buffer.");
                    }
                }
                else
                {
                    logToFile ("CrateSandbox: DIAG A/B — restore request seen but size invalid (" + juce::String (size) + " bytes), skipped.");
                }

                pluginAccessLock.clear (std::memory_order_release);
                controlBlock->liveRestoreRequested.store (false, std::memory_order_release);
            }
            else
            {
                logToFile ("CrateSandbox: DIAG A/B — restore request seen but pluginAccessLock contended, retrying next tick.");
            }
        }

        // Step 89 (Fresh Store Fix) directive: PARENT's Store A/B click
        // asking for a fresh, non-debounced extraction right now — see
        // ControlBlock::forceStateExtractionRequested's own doc comment
        // and StateExtractionThread::triggerImmediateExtraction(). Cleared
        // immediately (this side's job is just to kick the thread off;
        // the PARENT doesn't wait on this flag at all, only on the
        // resulting fresh stateChunkData push that follows).
        if (controlBlock->forceStateExtractionRequested.exchange (false, std::memory_order_acq_rel))
            stateExtractionThread->triggerImmediateExtraction();

        // Step 23 (Geometry Polish & Dynamic Resize) directive: see
        // serviceTenant()'s matching comment — keeps windowWidth/
        // windowHeight live for as long as the editor exists, not just at
        // creation, so the PARENT's poll can follow a live resize.
        if (activeEditor != nullptr)
        {
            const int currentWidth  = activeEditor->getWidth();
            const int currentHeight = activeEditor->getHeight();

            // Step 24 (Editor View Recovery Guard) directive: see
            // initialEditorWidth/Height's own doc comment for the full
            // evidence trail. Checked BEFORE the ordinary republish below,
            // so a corrupted snap-back is never mistaken for a legitimate
            // resize and forwarded to the PARENT.
            //
            // Step 57 (The Liar's Tolerance) directive — QA finding:
            // MAutoDynamicEq (MeldaProduction) was falsely convicted as a
            // fixed-size liar. Melda's own highly dynamic UI recalculates
            // its internal grid on every resize and can legitimately
            // settle a few pixels off from — or, by coincidence of its own
            // grid math, land EXACTLY back on — the editor's original
            // creation size, without that meaning the plugin ignores
            // resize requests the way VoxDucker provably does. Pixel-exact
            // equality was too strict a signal for "this is corruption/a
            // liar" versus "this is normal grid rounding." recoveryGuardTolerancePixels
            // (15, matching the user's own directive) widens BOTH checks
            // below identically: "has moved away from creation size" now
            // means "moved away by MORE than tolerance" (so a sub-tolerance
            // wobble is never mistaken for a real resize in the first
            // place), and "snapped back" means "within tolerance of
            // creation size" (so a few pixels of legitimate grid rounding
            // is silently accepted as the new bounds, never triggering
            // teardown or a Liar's Penalty conviction).
            if (! editorHasEverResized)
            {
                if (std::abs (currentWidth - initialEditorWidth) > recoveryGuardTolerancePixels
                        || std::abs (currentHeight - initialEditorHeight) > recoveryGuardTolerancePixels)
                    editorHasEverResized = true;
            }
            else if (std::abs (currentWidth - initialEditorWidth) <= recoveryGuardTolerancePixels
                         && std::abs (currentHeight - initialEditorHeight) <= recoveryGuardTolerancePixels)
            {
                logToFile ("CrateSandbox: editor view snapped back to within " + juce::String (recoveryGuardTolerancePixels)
                               + "px of its creation size (" + juce::String (initialEditorWidth) + "x" + juce::String (initialEditorHeight)
                               + ", currently " + juce::String (currentWidth) + "x" + juce::String (currentHeight)
                               + ") after being resized away from it — recreating the editor to recover.");

                // Step 55 (The Liar's Penalty) directive — proven live by
                // VoxDucker's own log: canResize=true, probed max
                // 10000x10000, yet every real resize snapped straight back
                // to 800x800. A plugin that CLAIMS resizability through
                // every API surface (canResize(), checkSizeConstraint())
                // but IGNORES applied sizes in practice is a liar this
                // guard has just caught red-handed — the snap-back IS the
                // proof. Without this, the recreated editor re-published
                // canResize=true and re-probed the same lying min/max,
                // trapping the user in an endless resize -> snap-back ->
                // recreate loop with the black void visible the whole
                // time. Latch it: this instance is FIXED-SIZE at its
                // snapped-back dimensions from this moment on, forever —
                // editorProvedFixedSizeLiar makes the upcoming recreation
                // (and every later one) skip the probe and publish
                // canResize=false + min==max, which is exactly the
                // fingerprint the PARENT's own latch
                // (PluginWindowContent::enforceResizeLimits()) and
                // canResize tick already nuke the OS resize handle for.
                // fixedSizeLiarDetected additionally tells the PARENT to
                // record this UID into PluginHealthRegistry, so ALL future
                // loads skip the plugin's false claims entirely.
                editorProvedFixedSizeLiar = true;
                controlBlock->fixedSizeLiarDetected.store (true, std::memory_order_release);
                controlBlock->editorCanResize.store (false, std::memory_order_relaxed);
                controlBlock->pluginMinWidth.store  (initialEditorWidth,  std::memory_order_relaxed);
                controlBlock->pluginMinHeight.store (initialEditorHeight, std::memory_order_relaxed);
                controlBlock->pluginMaxWidth.store  (initialEditorWidth,  std::memory_order_relaxed);
                controlBlock->pluginMaxHeight.store (initialEditorHeight, std::memory_order_relaxed);

                logToFile ("CrateSandbox: LIAR'S PENALTY — plugin claimed canResize=true but ignored applied sizes; "
                               "forcing FIXED-SIZE at " + juce::String (initialEditorWidth) + "x" + juce::String (initialEditorHeight)
                               + " for the rest of this session and flagging the PARENT to persist it.");

                controlBlock->windowHandleReady.store (false, std::memory_order_release);

                activeEditor.reset();
                return; // next tick's windowHandleRequested branch below recreates it fresh
            }

            // Step 35 (Editor View Recovery Guard v2) directive, Task 3: a
            // NEW, complementary failure signal to the snap-back fingerprint
            // above — that one only catches a view reverting to its exact
            // creation size; it says nothing about a view whose native HWND
            // has simply stopped pumping messages at all (the observed
            // stress-test failure mode: rapid corner-dragging choking the
            // VST3's own message loop until it freezes solid). SendMessage-
            // Timeout with SMTO_ABORTIFHUNG is the standard Win32 idiom for
            // "is this window's message loop still alive" — a different,
            // more general check than comparing sizes, so it also catches a
            // hang that never involves the editor's size at all. Throttled
            // to once a second — a blocking round-trip through another
            // window's message queue, however fast when healthy, is not
            // something to do every 10ms tick.
            const auto nowMs = juce::Time::getMillisecondCounter();

            if (nowMs - lastEditorHealthCheckMs >= 1000)
            {
                lastEditorHealthCheckMs = nowMs;
                bool editorResponsive = true;

               #if JUCE_WINDOWS
                if (auto* peer = activeEditor->getPeer())
                {
                    if (auto hwnd = (HWND) peer->getNativeHandle())
                    {
                        DWORD_PTR result = 0;
                        editorResponsive = (SendMessageTimeoutW (hwnd, WM_NULL, 0, 0,
                                                                 SMTO_ABORTIFHUNG, 500, &result) != 0);
                    }
                }
               #endif

                // Step 69 (Watchdog Strike System) directive — QA finding:
                // a SINGLE SendMessageTimeout failure is NOT proof of a
                // genuine hang. Root-caused via direct log evidence (Step
                // 63/66/67's own investigation trail): an aggressive,
                // continuous drag of the plugin's own internal resize
                // handle can legitimately keep Melda's own heavy EQ-grid
                // recalculation busy on this SAME message thread for a
                // stretch approaching (or exceeding) the probe's own
                // 500ms timeout — a temporarily bottlenecked, but
                // perfectly healthy, plugin, not a frozen one. Convicting
                // on the FIRST failure and immediately tearing the editor
                // down mid-drag was the actual root cause of the original
                // "Melda crashed" report: the Watchdog assassinated a busy
                // plugin, and (before Step 68's own lifecycle-integrity
                // fix) the resulting recreation left the Host holding
                // stale HWND/dimension/throttle state, which is what
                // turned a false-positive conviction into a real crash.
                // Requiring editorHealthCheckStrikeThreshold CONSECUTIVE
                // failures — at this check's own 1-second cadence, that's
                // several full seconds of genuine unresponsiveness, far
                // longer than any single heavy UI frame could plausibly
                // stretch — before concluding "hung," while resetting the
                // counter to 0 the instant a single successful probe comes
                // back, is what actually distinguishes "busy" from "dead."
                if (! editorResponsive)
                    ++editorHealthCheckStrikes;
                else
                    editorHealthCheckStrikes = 0;

                if (editorHealthCheckStrikes >= editorHealthCheckStrikeThreshold)
                {
                    logToFile ("CrateSandbox: editor HWND failed " + juce::String (editorHealthCheckStrikes)
                                   + " consecutive Win32 responsiveness checks (SendMessageTimeout) — recreating "
                                     "editor view to recover. Audio processing untouched.");

                    // Step 74 (The Flight Recorder) directive — QA finding:
                    // this conviction path (a hung UI/message thread that
                    // still passes the HOST's own heartbeat/DSP-round-trip
                    // watchdogs, since HeartbeatThread/AudioBridgeThread are
                    // independent threads unaffected by a message-thread
                    // deadlock) never flushed the flight recorder at all —
                    // the only kind of "kill" that had NO black box capture.
                    // SendMessageTimeoutW's own bounded 500ms timeout is
                    // exactly what lets this line execute even though the
                    // editor's own message loop may be fully wedged.
                    FlightRecorder::getInstance().flushToDisk ("CHILD_EDITOR_HEALTHCHECK_STRIKE_LIMIT");

                    controlBlock->windowHandleReady.store (false, std::memory_order_release);

                    editorHealthCheckStrikes = 0;
                    activeEditor.reset();
                    return;
                }
            }

            // Step 68 (Outbound Throttle & Lifecycle Integrity) directive,
            // Task 1: rapidly, continuously dragging the plugin's own
            // internal resize handle republishes a new size to shared
            // memory every single tick this mismatch condition holds —
            // this timer already runs at CrateIPC::livenessCheckIntervalMs
            // (20ms/50Hz), comfortably inside a safe band on paper, but a
            // fixed nominal interval alone isn't a real guarantee: if
            // Melda's own internal grid recalculation (triggered
            // synchronously, on THIS SAME message thread, by the plugin's
            // native handle responding to raw OS mouse-move messages)
            // ever makes a single tick's own work run long, this Timer
            // has no idle gap left to wait out and the next tick fires
            // immediately back-to-back — the exact same wall-clock-drift
            // hazard proven on the HOST side (CrateSandboxBridge's own
            // Step 66 follower throttle). Stamping the actual time of the
            // last real write and checking it explicitly, rather than
            // trusting the Timer's nominal period, closes that gap here
            // too. No separate catch-up/idle-flush is needed: the
            // mismatch condition above re-evaluates true every tick until
            // the write actually lands, so the very next tick past the
            // throttle window flushes the latest value on its own.
            publishEditorSizeIfChanged (currentWidth, currentHeight);

            // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive:
            // the PARENT is the only side that can still reliably detect a
            // real monitor-DPI change (see ControlBlock::displayScale1000's
            // own doc comment for why the CHILD's own auto-detection breaks
            // once reparented). setScaleFactor() is JUCE's own VST3 hosting
            // wrapper's host-override hook — it forwards straight into the
            // plugin's IPlugViewContentScaleSupport::setContentScaleFactor()
            // and calls resizeToFit() internally (see
            // juce_VST3PluginFormat.cpp's VST3PluginWindow::setScaleFactor()),
            // so this one call is the entire rescale — no raw VST3 API
            // needed. Any resulting size renegotiation from the plugin
            // rides out through the windowWidth/windowHeight republish
            // above on the very next tick, same as any other resize.
            const float requestedScale = controlBlock->displayScale1000.load (std::memory_order_relaxed) / 1000.0f;

            if (std::abs (requestedScale - lastAppliedDisplayScale) > 0.001f)
            {
                lastAppliedDisplayScale = requestedScale;
                activeEditor->setScaleFactor (requestedScale);
            }

            // Step 65 (Pure Follower Architecture) directive: the Host
            // never publishes a resize request to the CHILD any more —
            // the whole hostRequestedWidth/Height apply path (Steps 34/35/
            // 36/61) that used to live here is gone. The Host is now a
            // pure follower of whatever size THIS process's own editor
            // reports (via the windowWidth/windowHeight republish above),
            // driven exclusively by the plugin's own internal resize
            // handle (Melda-style) or its own fixed size — there is no
            // more reverse direction to apply.
            return; // already handled — nothing more to attach
        }

        if (! controlBlock->windowHandleRequested.load (std::memory_order_acquire))
            return;

        // Step 79 (Pre-Emptive Native Parenting) directive: the PARENT
        // guarantees (see CrateSandboxBridge::requestEditorWindow()'s own
        // doc comment) that windowHandleRequested is never set until
        // AFTER hostSlotHwndValue has already been published — this is
        // this side's own belt-and-suspenders check of that same
        // contract, since creating an editor before a real slot exists
        // would leave it floating as its own top-level window again, the
        // exact bug this whole step fixes.
        const int64_t hostSlotHwndValue = controlBlock->hostSlotHwndValue.load (std::memory_order_acquire);

        if (hostSlotHwndValue == 0)
        {
            logToFile ("CrateSandbox: window handle requested, but the Host hasn't published its Airlock slot yet — waiting.");
            return;
        }

        if (hostedPlugin == nullptr || ! hostedPlugin->hasEditor())
        {
            logToFile ("CrateSandbox: window handle requested, but no editor-capable plugin is loaded.");
            return;
        }

        activeEditor.reset (hostedPlugin->createEditorAndMakeActive());

        if (activeEditor == nullptr)
        {
            logToFile ("CrateSandbox: createEditorAndMakeActive() returned nullptr.");
            return;
        }

        // Step 36 (The Fixed-Size Lock) directive, Task 1: MUST be read
        // BEFORE setResizable(true, true) below — that call unconditionally
        // overwrites AudioProcessorEditor's own resizableByHost flag to
        // true (see its implementation), which is exactly what the next
        // comment explains is deliberately needed for OUR OWN programmatic
        // resize plumbing to function at all. isResizable() right here,
        // this one time, is the only point where it still reflects
        // VST3PluginWindow's constructor-time query view->canResize() ==
        // kResultTrue — the plugin's REAL, honest answer to "can a host
        // change my size at all," queried once directly against the
        // IPlugView the same way JUCE's own wrapper already does
        // internally (juce_VST3PluginFormat.cpp's VST3PluginWindow
        // constructor). Published once here, never re-queried afterward —
        // canResize() is a fixed property of a given editor instance, not
        // something that changes tick-to-tick.
        // Step 55 (The Liar's Penalty) directive: the plugin's own claim
        // is overridden by EITHER conviction source — this process's own
        // in-session latch (editorProvedFixedSizeLiar, set by the
        // Recovery Guard the moment a snap-back proves the claim false)
        // or the PARENT's pre-launch verdict from PluginHealthRegistry
        // (ControlBlock::forceFixedSizeEditor, written before this
        // process even spawned, for a plugin convicted in a PAST
        // session). Folding the override into editorCanResize itself
        // means every downstream consumer — the probe-vs-fixed branch,
        // the editorCanResize publish, the host-resize ignore guard —
        // sees ONE consistent verdict with no extra plumbing.
        const bool editorCanResize = ! editorProvedFixedSizeLiar
                                        && ! controlBlock->forceFixedSizeEditor.load (std::memory_order_relaxed)
                                        && activeEditor->isResizable();

        // The One-Way Zoom Trap directive (Step 10.1 patch 2) — SUPERSEDED
        // by Step 23: this used to be setResizable(false, false), disabling
        // the editor's own resize grip entirely, because the PARENT locked
        // its window to a fixed size with no way to follow a plugin that
        // resized itself afterward — a real, previously-unsolved mismatch,
        // not a hypothetical one. Step 23 closes that gap properly: the
        // CHILD now republishes windowWidth/windowHeight continuously (see
        // this method's own top, above the windowHandleRequested check),
        // and the PARENT (SandboxEditorTestWindow::timerCallback()) follows
        // a live size change the same way it already followed a post-crash
        // handle change. Re-enabling resize here is what actually lets a
        // plugin like FabFilter's corner-drag do anything at all. This is
        // OUR side's own resize plumbing being enabled — entirely separate
        // from editorCanResize above, which is what gets told to the
        // PARENT so IT can decide whether to expose a resize grip to the
        // user at all (see Step 36's own comment on editorCanResize).
        activeEditor->setResizable (true, true);

        // Step 88 (Probe Before Attach) directive — QA finding, confirmed
        // via direct instrumentation of JUCE's own vendored
        // juce_VST3PluginFormat.cpp (Step 86): this probe used to run
        // AFTER addToDesktop()/setVisible(), on the assumption (Step 52
        // Task 2's own original comment) that "nothing on this side of
        // the sandbox boundary is ever shown directly," which was TRUE
        // under the OLD architecture (the editor was its own separate,
        // never-shown floating window, reparented into the Host
        // afterward) but became FALSE the moment Step 79 (Pre-Emptive
        // Native Parenting) started calling addToDesktop() with the
        // Host's OWN already-visible Airlock slot as the native parent —
        // from that point on, this editor IS the user's actual on-screen
        // content the instant it's shown. The ground-truth diagnostic log
        // (CrateVST3WindowDiag.log) proved this probe was firing two real,
        // live, on-screen componentMovedOrResized() calls — one growing
        // the ALREADY-VISIBLE, ALREADY-ATTACHED Melda view to a violent
        // 10000x10000, one shrinking it back — on top of the natural,
        // correct onSize() JUCE already fires by itself the instant
        // addToDesktop() creates a peer for an already-visible Component
        // (via componentVisibilityChanged()). Melda's own Direct2D
        // rendering evidently doesn't recover cleanly from that violent,
        // synchronous, fully-visible resize round-trip, leaving the
        // white-box artifact — a genuine VST3PluginWindow-level fact this
        // codebase's own Airlock architecture could never have shown up
        // in Win32-level measurement (every native rect measured 100%
        // correct at every step; the damage is inside Melda's own
        // rendering, not in any HWND geometry this process owns).
        //
        // Fix: run the ENTIRE probe (or the fixed-size min/max fallback)
        // BEFORE addToDesktop() — at this point the editor genuinely has
        // no peer and isn't shown to the user at all (matching Step 52's
        // own ORIGINAL, now-restored assumption), so the same 1/1 and
        // 10000/10000 constrainer probes measure the exact same
        // plugin-reported limits (VST3PluginWindow::checkBounds() calls
        // view->checkSizeConstraint(), which needs no peer at all) with
        // zero visible side effect. The editor is left sized back at its
        // own authored creationWidth/Height by the time addToDesktop()
        // finally runs, exactly once, on the final correct size.
        if (editorCanResize)
        {
            if (auto* probeConstrainer = activeEditor->getConstrainer())
            {
                const int creationWidth  = activeEditor->getWidth();
                const int creationHeight = activeEditor->getHeight();

                // Minimum probe: an absurdly small candidate — whatever the
                // plugin clamps this down to IS its real minimum. If it
                // refuses to shrink at all, the clamp lands back on
                // creationWidth/Height, which correctly means "this plugin
                // has no smaller size than the one it opened at."
                probeConstrainer->setBoundsForComponent (activeEditor.get(), { 0, 0, 1, 1 }, false, false, true, true);
                const int probedMinWidth  = activeEditor->getWidth();
                const int probedMinHeight = activeEditor->getHeight();

                // Maximum probe: an absurdly large candidate, same idea in
                // reverse.
                probeConstrainer->setBoundsForComponent (activeEditor.get(), { 0, 0, 10000, 10000 }, false, false, true, true);
                const int probedMaxWidth  = activeEditor->getWidth();
                const int probedMaxHeight = activeEditor->getHeight();

                // Restore the plugin's own authored creation size — the
                // two probes above are throwaway measurements, not a real
                // resize the plugin should be left sitting at.
                probeConstrainer->setBoundsForComponent (activeEditor.get(), { 0, 0, creationWidth, creationHeight }, false, false, true, true);

                controlBlock->pluginMinWidth.store  (probedMinWidth,  std::memory_order_relaxed);
                controlBlock->pluginMinHeight.store (probedMinHeight, std::memory_order_relaxed);
                controlBlock->pluginMaxWidth.store  (probedMaxWidth,  std::memory_order_relaxed);
                controlBlock->pluginMaxHeight.store (probedMaxHeight, std::memory_order_relaxed);

                logToFile ("CrateSandbox: probed plugin resize limits — min=" + juce::String (probedMinWidth) + "x" + juce::String (probedMinHeight)
                               + ", max=" + juce::String (probedMaxWidth) + "x" + juce::String (probedMaxHeight) + ".");
            }
        }
        else
        {
            // Fixed-size plugin (Step 36) — min==max==creation size,
            // matching editorCanResize's own "this plugin never accepts a
            // different size" contract used elsewhere.
            controlBlock->pluginMinWidth.store  (activeEditor->getWidth(),  std::memory_order_relaxed);
            controlBlock->pluginMinHeight.store (activeEditor->getHeight(), std::memory_order_relaxed);
            controlBlock->pluginMaxWidth.store  (activeEditor->getWidth(),  std::memory_order_relaxed);
            controlBlock->pluginMaxHeight.store (activeEditor->getHeight(), std::memory_order_relaxed);
        }

        // Step 79 (Pre-Emptive Native Parenting) directive — QA finding:
        // the OLD addToDesktop(0) (no parent) created this editor as its
        // own genuine TOP-LEVEL native window, then relied on the PARENT
        // reparenting it via SetParent() afterward. A heavy VST3's own
        // Direct2D/OpenGL swap chain is set up against whatever window it
        // was ORIGINALLY created for — a later SetParent() doesn't
        // retroactively fix that, which is exactly why Melda kept popping
        // out as its own top-level window regardless of any style
        // stripping performed after the fact.
        //
        // Passing the Airlock slot's own HWND (published by the PARENT
        // via hostSlotHwndValue BEFORE windowHandleRequested was ever set
        // — see requestEditorWindow()'s own doc comment for that ordering
        // guarantee) as addToDesktop()'s nativeWindowToAttachTo parameter
        // makes JUCE's own HWNDComponentPeer call CreateWindowEx with the
        // slot as hWndParent from the very FIRST frame. Confirmed by
        // reading juce_Windowing_windows.cpp directly:
        // computeNativeStyleFlags() sets WS_CHILD (and ONLY WS_CHILD —
        // never WS_POPUP/WS_CAPTION/WS_THICKFRAME/WS_SYSMENU) the instant
        // a native parent is supplied. Nothing is ever created wrong in
        // the first place, so there's nothing left to strip or
        // force-recalculate afterward, and no SetParent() call anywhere
        // in this whole path any more.
        auto* hostSlotHwnd = (void*) (intptr_t) hostSlotHwndValue;
        activeEditor->addToDesktop (0, hostSlotHwnd);

        // Step 80 (Geometry Snap) directive — REMOVED: the getWidth()/
        // getHeight() -> setSize() re-assertion this step originally added
        // here was, on inspection, provably inert — JUCE's own
        // Component::setSize() early-returns when the new size already
        // equals the current one, and reading a value straight back into
        // itself can never be anything else. It could not have been the
        // cause of a later clipping symptom; see componentMovedOrResized()'s
        // own doc comment for the actual event-driven fix (Step 81).
        activeEditor->setVisible (true);

        // Step 81 (Event-Driven Geometry Publish) directive: registered
        // once, right after the editor exists — see
        // componentMovedOrResized()'s own doc comment. juce::Component's
        // own destructor removes this listener automatically whenever
        // activeEditor is reset/recreated, so there's nothing to
        // explicitly unregister at any of this class's own teardown
        // points.
        activeEditor->addComponentListener (this);

        if (auto* peer = activeEditor->getPeer())
        {
            // Geometry Sync directive (Step 10.1): the editor's OWN
            // getWidth()/getHeight() — its authored content size — read
            // and published in the SAME command as the handle, BEFORE the
            // PARENT ever reparents anything. This is the plugin's real
            // size, not whatever the embedded HWND's bounds happen to
            // measure as after SetParent() runs on the other side.
            controlBlock->windowHandleValue.store ((int64_t) (intptr_t) peer->getNativeHandle(), std::memory_order_relaxed);
            controlBlock->windowWidth.store (activeEditor->getWidth(), std::memory_order_relaxed);
            controlBlock->windowHeight.store (activeEditor->getHeight(), std::memory_order_relaxed);
            controlBlock->editorCanResize.store (editorCanResize, std::memory_order_relaxed);
            controlBlock->windowHandleReady.store (true, std::memory_order_release);
            logToFile ("CrateSandbox: editor window created and handle published ("
                           + juce::String (activeEditor->getWidth()) + "x" + juce::String (activeEditor->getHeight())
                           + ", canResize=" + (editorCanResize ? juce::String ("true") : juce::String ("false")) + ").");

           #if JUCE_WINDOWS
            crateLogPeerNativeTree ((HWND) peer->getNativeHandle(), &logToFile);
           #endif

            // Step 24 (Editor View Recovery Guard) directive: baseline for
            // detecting the Melda-style "resize burst leaves the plugin's
            // OWN view corrupted" quirk (see this method's own snap-back
            // check, above the windowHandleRequested branch, for the full
            // reasoning) — captured fresh on every (re)creation of this
            // editor, isolated mode as well as the recovery path itself.
            initialEditorWidth  = activeEditor->getWidth();
            initialEditorHeight = activeEditor->getHeight();
            editorHasEverResized = false;
        }
        else
        {
            logToFile ("CrateSandbox: editor addToDesktop() succeeded but getPeer() returned nullptr.");
            activeEditor.reset();
        }
    }

    // Diagnostics directive: juce::Logger::writeToLog() writes via
    // OutputDebugString, invisible with no debugger attached — this
    // codebase already hit and solved the exact same problem for
    // CrateSandboxBridge.log (see that class's own doc comment); this is
    // the same fix applied to THIS process, which had no visibility at all
    // until now.
    static void logToFile (const juce::String& message)
    {
        static const auto logFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                         .getChildFile ("CrateSandbox.log");

        logFile.appendText (juce::Time::getCurrentTime().toString (true, true, true, true) + "  " + message + "\n",
                             false, false, "\n");
    }

    // The VST3 Host Engine directive: synchronous — createPluginInstance()
    // blocking the message thread briefly during startup is exactly as
    // acceptable here as it is in the main DAW's own plugin-insertion path;
    // this only ever runs once per process launch, never per audio block.
    void loadHostedPlugin (const juce::String& pluginPath, double sampleRate, int blockSize)
    {
        if (pluginPath.isEmpty())
        {
            logToFile ("CrateSandbox: no plugin path provided — running pass-through only.");
            return;
        }

        if (sampleRate <= 0.0)
            sampleRate = 44100.0;

        if (blockSize <= 0)
            blockSize = 512;

        // Step 10 directive: the GUI-CAPABLE variant, not
        // addHeadlessDefaultFormatsToManager() — JUCE's own source
        // documents the headless variant as ALWAYS returning nullptr from
        // createEditor()/false from hasEditor(), regardless of what the
        // real VST3 supports. juce_gui_basics/juce_gui_extra were already
        // being compiled into this process transitively (via
        // tracktion_engine's own dependency on the full juce_audio_processors
        // module) since Step 5 — see class doc comment.
        juce::addDefaultFormatsToManager (pluginFormatManager);

        juce::KnownPluginList knownPlugins;
        juce::OwnedArray<juce::PluginDescription> foundTypes;
        knownPlugins.scanAndAddDragAndDroppedFiles (pluginFormatManager, juce::StringArray (pluginPath), foundTypes);

        if (foundTypes.isEmpty())
        {
            logToFile ("CrateSandbox: FAILED to find/scan VST3 at " + pluginPath);
            return;
        }

        // Step 13.5 directive: publish the AUTHENTIC identifier — JUCE's own
        // per-format identifier string, built from the real VST3 Component
        // ID, not the filesystem path — before createPluginInstance() below,
        // which is the actual risky call. Written here means the PARENT
        // already knows the real UID even if instantiation itself is what
        // crashes.
        if (controlBlock != nullptr)
        {
            const auto identifier = foundTypes.getFirst()->createIdentifierString();
            std::memset (controlBlock->pluginUID, 0, sizeof (controlBlock->pluginUID));
            identifier.copyToUTF8 (controlBlock->pluginUID, (size_t) CrateIPC::ControlBlock::maxPluginUIDLength - 1);

            // Step 22 (The Profiling Database / The Warden) directive:
            // resolved from the SAME PluginDescription the UID above comes
            // from — manufacturerName is exactly what JUCE's own VST3
            // format reads from the plugin's factory info, no extra scan.
            const auto vendor = foundTypes.getFirst()->manufacturerName;
            std::memset (controlBlock->vendorName, 0, sizeof (controlBlock->vendorName));
            vendor.copyToUTF8 (controlBlock->vendorName, (size_t) CrateIPC::ControlBlock::maxVendorNameLength - 1);

            controlBlock->pluginUIDReady.store (true, std::memory_order_release);
            logToFile ("CrateSandbox: resolved authentic plugin UID: " + identifier + " (vendor: " + vendor
                           + ") [instanceId=" + instanceId + "]");
        }

        juce::String errorMessage;
        hostedPlugin = pluginFormatManager.createPluginInstance (*foundTypes.getFirst(), sampleRate, blockSize, errorMessage);

        if (hostedPlugin == nullptr)
        {
            logToFile ("CrateSandbox: FAILED to instantiate VST3 (" + pluginPath + "): " + errorMessage);
            return;
        }

        hostedPlugin->prepareToPlay (sampleRate, blockSize);

        // Continuous State Sync directive (Step 11): apply the PARENT's
        // lastKnownState — written into initialStateData/initialStateSize
        // BEFORE this process even started (see class doc comment) — right
        // after prepareToPlay(), before anything else touches the plugin.
        // Empty (size 0) on a genuinely first launch, which is correct: a
        // brand-new instance has nothing to restore.
        if (controlBlock != nullptr && controlBlock->initialStateSize > 0
            && controlBlock->initialStateSize <= CrateIPC::ControlBlock::maxStateChunkBytes)
        {
            // Step 51 (Silent Crash Hardening) directive: same SEH
            // protection as the live-restore path — a crash-resurrection
            // restore is, if anything, MORE likely to hand a plugin state
            // bytes it doesn't like (this runs after every restart,
            // including one caused by the plugin itself misbehaving).
            if (sehSetStateInformation (hostedPlugin.get(), controlBlock->initialStateData, (int) controlBlock->initialStateSize))
                logToFile ("CrateSandbox: restored plugin state from PARENT (" + juce::String (controlBlock->initialStateSize) + " bytes).");
            else
                logToFile ("CrateSandbox: setStateInformation() faulted (caught via SEH) during crash-resurrection restore — continuing with default state.");
        }

        // RESTART LIVELOCK directive: AudioBridgeThread is already running
        // by the time this line executes (see initialise()'s own comment on
        // why) — it reads hostedPluginPtr fresh on EVERY iteration rather
        // than a pointer captured once at construction, specifically so
        // this late publish is picked up correctly instead of the thread
        // being permanently stuck seeing nullptr from before loading
        // finished. Release ordering pairs with its acquire load.
        hostedPluginPtr.store (hostedPlugin.get(), std::memory_order_release);

        logToFile ("CrateSandbox: VST3 loaded OK: " + hostedPlugin->getName()
                       + " (sampleRate=" + juce::String (sampleRate) + ", blockSize=" + juce::String (blockSize) + ")");

        // Step 8 Verification directive: log every parameter's index/name
        // so the PARENT's own hardcoded test-sweep target can be identified
        // and confirmed correct (juce::Logger::writeToLog alone is invisible
        // with no debugger attached — see logToFile()'s own doc comment) —
        // this is how "Filter Cutoff"'s actual index on Rift Filter Lite was
        // confirmed for this step's test hardcode.
        auto& params = hostedPlugin->getParameters();
        for (int i = 0; i < params.size(); ++i)
            logToFile ("CrateSandbox: param[" + juce::String (i) + "] = " + params[i]->getName (64));

        // Step 31 (Real IPC Parameter Sync) directive: publish the REAL
        // parameter list once, right here — same "resolved once at load,
        // gated by a ready flag" pattern pluginUID/vendorName already use
        // (Step 13.5/22). This is what lets the PARENT replace its Step 30
        // hardcoded stub parameters with real ones built from the ACTUAL
        // target VST3. Capped at maxSyncedParams — see that constant's own
        // doc comment for why a plugin with more parameters than that just
        // doesn't get automation UI for the overflow.
        {
            const int syncedCount = juce::jmin ((int) CrateIPC::ControlBlock::maxSyncedParams, params.size());

            for (int i = 0; i < syncedCount; ++i)
            {
                auto* param = params[i];
                auto& metadata = controlBlock->paramMetadata[i];

                std::memset (metadata.name, 0, sizeof (metadata.name));
                param->getName (64).copyToUTF8 (metadata.name, (size_t) CrateIPC::ControlBlock::maxParamNameLength - 1);
                metadata.defaultValue = param->getDefaultValue();

                controlBlock->paramCurrentValues[i].store (param->getValue(), std::memory_order_relaxed);
            }

            controlBlock->paramCount.store (syncedCount, std::memory_order_relaxed);
            controlBlock->paramMetadataReady.store (true, std::memory_order_release);

            logToFile ("CrateSandbox: published parameter metadata for " + juce::String (syncedCount)
                           + " parameter(s) (of " + juce::String (params.size()) + " total).");
        }

        // The Trigger / Asynchronous State Extraction directives (Step 11):
        // stateExtractionThread must already be running before the listener
        // is attached — attaching first would let a callback fire and call
        // triggerExtraction() (which calls notify() on a thread that
        // doesn't exist yet) in the (vanishingly unlikely but real) window
        // between the two.
        stateExtractionThread = std::make_unique<StateExtractionThread> (*controlBlock, hostedPluginPtr, pluginAccessLock);
        stateExtractionThread->startThread (juce::Thread::Priority::low); // never time-critical — the whole point is staying off the DSP/UI path

        stateChangeListener = std::make_unique<StateChangeListener> (*stateExtractionThread);
        hostedPlugin->addListener (stateChangeListener.get());
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    // The Health Check (Liveness Detection) directive: a pure driver, no
    // logic of its own beyond "increment, then wait" — every other thread
    // in this codebase that only needs to signal periodically (ShadowWorker,
    // this) follows the same shape rather than each inventing its own timing
    // idiom.
    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // what a pooled-but-unclaimed process actually does while it waits —
    // genuinely 0% CPU, not a spin-loop, via the SAME NamedEvent::wait()
    // idiom AudioBridgeThread/LookaheadWorkerThread already use for their
    // own "block until real work arrives" cadence. The bounded timeout
    // (rather than an infinite wait) exists ONLY so threadShouldExit() gets
    // checked periodically for clean shutdown — a genuine claim always
    // wakes this immediately via SetEvent, it never has to wait out a full
    // timeout in the success case.
    class CryosleepWaitThread : public juce::Thread
    {
    public:
        CryosleepWaitThread (const juce::String& instanceId, std::function<void()> onClaimedToUse)
            : juce::Thread ("Crate Cryosleep Wait"),
              claimEvent (CrateIPC::getCryosleepClaimEventName (instanceId)),
              onClaimed (std::move (onClaimedToUse))
        {
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                if (claimEvent.wait (2000))
                {
                    onClaimed();
                    return; // one-shot — a claimed slot never goes back to sleep
                }
            }
        }

    private:
        CrateIPC::NamedEvent claimEvent;
        std::function<void()> onClaimed;
    };

    class HeartbeatThread : public juce::Thread
    {
    public:
        explicit HeartbeatThread (CrateIPC::ControlBlock& blockToUse)
            : juce::Thread ("Crate Sandbox Heartbeat"), block (blockToUse) {}

        // Step 34 (Zero-CPU Heartbeat) directive: a cryosleeping, unclaimed
        // pool process has NOTHING watching its heartbeat yet (SandboxManager's
        // own pool tracks liveness via the OS process handle, not IPC — see
        // its own doc comment), so ticking at the normal fast
        // heartbeatIntervalMs (10ms) the whole time it sits idle is pure
        // waste — confirmed by direct measurement (~1.2% CPU per idle
        // slot). Defaults to true (normal, non-pooled launches are
        // unaffected — fast from the very first tick, exactly as before);
        // the CHILD's own initialise() sets this false immediately after
        // construction for a cryosleeping launch, and
        // CryosleepWaitThread's onClaimed callback flips it back to true
        // the INSTANT a claim lands — before startTimer() even runs on the
        // PARENT side, so there's no window where a freshly-claimed
        // process could be timed out for still ticking slow.
        void setActivelyServing (bool nowActivelyServing)
        {
            activelyServing.store (nowActivelyServing, std::memory_order_release);
            notify(); // wakes immediately from a possibly-long slow-mode wait() — the new interval takes effect now, not whenever the current wait happens to elapse
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                block.heartbeatCounter.fetch_add (1, std::memory_order_relaxed);
                wait (activelyServing.load (std::memory_order_acquire)
                          ? CrateIPC::heartbeatIntervalMs
                          : cryosleepHeartbeatIntervalMs);
            }
        }

    private:
        static constexpr int cryosleepHeartbeatIntervalMs = 1000; // matches the user's own suggested throttle

        CrateIPC::ControlBlock& block;
        std::atomic<bool> activelyServing { true };
    };

    // Step 17 (The Lookahead IPC Pipeline / "Time-Slip Plumbing") directive:
    // drains lookaheadRequestRing as fast as the CPU allows — NOT throttled
    // to real-time, unlike AudioBridgeThread's own per-block cadence, which
    // is the entire point: this thread can run arbitrarily far ahead of the
    // real playhead for a non-live track. Blocks on readyEvent (genuine 0%
    // CPU while idle), same NamedEvent-wait idiom as AudioBridgeThread's own
    // readyEvent, with a short poll fallback so threadShouldExit() is still
    // checked even if the parent never signals again.
    //
    // STEP 18 (The Time-Slip Engine) directive: now the SOLE driver of
    // hostedPlugin->processBlock() for a tenant in lookahead mode — see
    // ControlBlock::isLookaheadMode's own doc comment and
    // AudioBridgeThread's matching skip-guard. Shares pluginAccessLock
    // with StateExtractionThread using the exact same try-lock-with-
    // bounded-backoff idiom AudioBridgeThread itself already used for that
    // same lock — this thread simply replaces AudioBridgeThread as the
    // OTHER contender for it while lookahead mode is active, not a new
    // kind of contention.
    class LookaheadWorkerThread : public juce::Thread
    {
    public:
        // Same constructor shape as AudioBridgeThread — owns its own
        // internal NamedEvent, built from instanceId, rather than
        // requiring the caller to separately construct and pass one.
        LookaheadWorkerThread (CrateIPC::ControlBlock& blockToUse, std::atomic<juce::AudioPluginInstance*>& pluginPtrToUse,
                               const juce::String& instanceId, std::atomic_flag& pluginAccessLockToUse,
                               std::atomic<bool>& isCorruptedToUse)
            : juce::Thread ("Crate Lookahead Worker"),
              block (blockToUse),
              hostedPluginPtr (pluginPtrToUse),
              readyEvent (CrateIPC::getLookaheadRequestReadyEventName (instanceId)),
              pluginAccessLock (pluginAccessLockToUse),
              isCorrupted (isCorruptedToUse)
        {
            // Zero Allocation directive, same as AudioBridgeThread's own
            // workBuffer — pre-allocated ONCE, sized down (never up) per
            // request via setSize(..., avoidReallocating=true) below.
            workBuffer.setSize (CrateIPC::ControlBlock::maxChannels, CrateIPC::ControlBlock::maxSamplesPerBlock);
        }

        void run() override
        {
            using CB = CrateIPC::ControlBlock;

            while (! threadShouldExit())
            {
                // readyEvent is a Win32 auto-reset event (see NamedEvent's
                // own CreateEventW call — bManualReset=FALSE). Confirmed by
                // direct instrumentation: when the Parent enqueues a burst
                // of several requests back-to-back (the normal case once
                // TimeSlipBuffer's cache is warm — see
                // ensureRawTrackAudioAvailable()), it calls signal() once
                // per enqueue, but repeated SetEvent() calls before a
                // waiter consumes the first one are no-ops — they coalesce
                // into a single wake-up, not one-per-call. Gating the ring
                // check behind wait()'s return value (the original Step 17
                // design) meant every enqueue after the first-in-a-burst
                // silently produced no result, ever — this is what stalled
                // the Time-Slip Engine Test at "-1 samples buffered" for
                // its entire timeout window despite requests visibly
                // reaching the ring. Fix: treat the event purely as a
                // wake-up hint, exactly like AudioBridgeThread and
                // StateExtractionThread already do elsewhere in this file —
                // check and drain the ring unconditionally every
                // wait()/timeout cycle, so a coalesced signal can never
                // strand unprocessed requests for longer than one 50ms
                // tick.
                readyEvent.wait (50);

                uint32_t tail = block.lookaheadRequestTail.load (std::memory_order_relaxed); // consumer-owned
                const uint32_t head = block.lookaheadRequestHead.load (std::memory_order_acquire);

                while (tail != head)
                {
                    auto& request = block.lookaheadRequestRing[tail];

                    // Result slot N corresponds to request slot N — see
                    // ControlBlock's own doc comment on why this ring pair
                    // is index-matched rather than independently advanced.
                    auto& result = block.lookaheadResultRing[tail];

                    result.timelinePositionSamples = request.timelinePositionSamples;
                    result.numChannels = request.numChannels;
                    result.numSamples  = request.numSamples;

                    const int numChannels = juce::jlimit (0, CB::maxChannels, request.numChannels);
                    const int numSamples  = juce::jlimit (0, CB::maxSamplesPerBlock, request.numSamples);

                    auto* plugin = hostedPluginPtr.load (std::memory_order_acquire);

                    // Step 52 (The Anti-Zombie Auto-Respawn Protocol)
                    // directive: same corruption gate as AudioBridgeThread's
                    // own — both threads call into the identical
                    // hostedPlugin instance, so a fault caught by EITHER
                    // must stop BOTH from calling processBlock() on it
                    // again. Silence into the lookahead result too, not
                    // passthrough — a pre-rendered "future" block sitting
                    // in the Time-Slip buffer is exactly as unsafe to hand
                    // to the audio thread later as a live corrupted block
                    // would be right now.
                    if (isCorrupted.load (std::memory_order_acquire))
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                            std::memset (result.audioOutput + (size_t) ch * CB::maxSamplesPerBlock, 0, (size_t) numSamples * sizeof (float));

                        tail = (tail + 1) % (uint32_t) CB::lookaheadRingCapacity;
                        block.lookaheadRequestTail.store (tail, std::memory_order_release);
                        block.lookaheadResultHead.store (tail, std::memory_order_release);
                        continue;
                    }

                    // Try-lock-or-back-off, same idiom StateExtractionThread
                    // already uses for this SAME lock — this thread is never
                    // time-critical (unlike the audio thread, it CAN afford
                    // a bounded wait), but shouldn't spin forever either.
                    bool processed = false;

                    if (plugin != nullptr && numChannels > 0 && numSamples > 0)
                    {
                        int attempts = 0;
                        bool acquired = false;

                        while (! (acquired = ! pluginAccessLock.test_and_set (std::memory_order_acquire)))
                        {
                            if (++attempts > 100)
                                break;
                            juce::Thread::sleep (1);
                        }

                        if (acquired)
                        {
                            workBuffer.setSize (numChannels, numSamples, false, false, true); // no realloc — within the capacity reserved in the constructor

                            for (int ch = 0; ch < numChannels; ++ch)
                                workBuffer.copyFrom (ch, 0, request.audioInput + (size_t) ch * CB::maxSamplesPerBlock, numSamples);

                            // Zero-Dropout Bridge directive (Step 52): the
                            // DETERMINISTIC automation value the PARENT's
                            // message thread already sampled for THIS exact
                            // future timeline position (see
                            // LookaheadRequestSlot::paramValues's own doc
                            // comment in CrateIPCConstants.h for why that
                            // sampling can only happen on the message
                            // thread, never here). Applied every request,
                            // regardless of whether request.numParamValues
                            // changed since the last one — a stateful
                            // plugin's own parameter, once set, holds until
                            // told otherwise, so re-asserting the same value
                            // is harmless and simpler than diffing.
                            if (request.numParamValues > 0)
                            {
                                const auto& params = plugin->getParameters();
                                const int count = juce::jmin (request.numParamValues, params.size());

                                for (int i = 0; i < count; ++i)
                                    params[i]->setValue (juce::jlimit (0.0f, 1.0f, request.paramValues[i]));
                            }

                            midiBuffer.clear();

                            // Step 20 (The Denormal Shield) directive: real
                            // extracted clip audio decaying through a
                            // feedback-based filter's internal state can
                            // walk into the subnormal float range, where
                            // some FPUs/algorithms take a drastically
                            // slower (or, per the observed hang, apparently
                            // unbounded) code path per sample. AudioBridgeThread's
                            // own real-time round trip has always used a
                            // synthetic sweep/tone that likely never decayed
                            // into that range, which is consistent with why
                            // this was never seen there. Scoped strictly
                            // around the call that can actually produce
                            // subnormals, not the whole thread, matching
                            // juce::ScopedNoDenormals' own "hold the FTZ/DAZ
                            // flags for exactly as long as you need them"
                            // contract.
                            bool processedOk;
                            {
                                juce::ScopedNoDenormals noDenormals;
                                processedOk = sehProcessBlock (plugin, workBuffer, midiBuffer);
                            }

                            pluginAccessLock.clear (std::memory_order_release);

                            if (! processedOk)
                            {
                                // Same one-shot latch AudioBridgeThread sets —
                                // whichever thread's processBlock() faults
                                // first wins the race to flip it, the other
                                // just observes it true on its next check.
                                isCorrupted.store (true, std::memory_order_release);
                                block.corruptionDetected.store (true, std::memory_order_release);
                                logToFile ("CrateSandbox: LookaheadWorkerThread's processBlock() faulted (caught via SEH) — "
                                               "plugin instance marked CORRUPTED, outputting silence and awaiting respawn.");

                                for (int ch = 0; ch < numChannels; ++ch)
                                    std::memset (result.audioOutput + (size_t) ch * CB::maxSamplesPerBlock, 0, (size_t) numSamples * sizeof (float));
                            }
                            else
                            {
                                for (int ch = 0; ch < numChannels; ++ch)
                                {
                                    auto* out = result.audioOutput + (size_t) ch * CB::maxSamplesPerBlock;
                                    std::memcpy (out, workBuffer.getReadPointer (ch), (size_t) numSamples * sizeof (float));
                                }
                            }

                            processed = true;
                        }
                    }

                    if (! processed)
                    {
                        // No plugin loaded, or lock contention exceeded the
                        // bounded backoff — passthrough, matching
                        // AudioBridgeThread's own no-plugin-loaded fallback
                        // rather than silently fabricating output.
                        for (int ch = 0; ch < numChannels; ++ch)
                            std::memcpy (result.audioOutput + (size_t) ch * CB::maxSamplesPerBlock,
                                         request.audioInput + (size_t) ch * CB::maxSamplesPerBlock,
                                         (size_t) numSamples * sizeof (float));
                    }

                    tail = (tail + 1) % (uint32_t) CB::lookaheadRingCapacity;
                    block.lookaheadRequestTail.store (tail, std::memory_order_release);   // free the request slot for reuse
                    block.lookaheadResultHead.store (tail, std::memory_order_release);    // publish the result slot as ready
                }
            }
        }

    private:
        CrateIPC::ControlBlock& block;
        std::atomic<juce::AudioPluginInstance*>& hostedPluginPtr;
        juce::AudioBuffer<float> workBuffer;
        juce::MidiBuffer midiBuffer;
        CrateIPC::NamedEvent readyEvent;
        std::atomic_flag& pluginAccessLock;

        // Same flag AudioBridgeThread checks — shared, not a private copy,
        // since both threads call processBlock() on the identical
        // hostedPlugin instance and either one can be the first to hit a
        // fault. Reference member, owned by the enclosing
        // CrateSandboxApplication/TenantContext, matching pluginAccessLock's
        // own pattern immediately above.
        std::atomic<bool>& isCorrupted;
    };

    // Asynchronous State Extraction directive (Step 11): a thin driver that
    // sleeps (genuine 0% CPU, via juce::Thread's own wait()/notify() pair —
    // in-process signaling, not the cross-process CrateIPC::NamedEvent
    // AudioBridgeThread uses) until StateChangeListener wakes it, then does
    // the actual getStateInformation() call and IPC push. Priority::low and
    // no timing budget of its own — the ENTIRE point of this thread is
    // staying off both the DSP path (AudioBridgeThread) and the UI/message
    // thread (wherever the plugin's own notification actually fired from).
    class StateExtractionThread : public juce::Thread
    {
    public:
        StateExtractionThread (CrateIPC::ControlBlock& blockToUse, std::atomic<juce::AudioPluginInstance*>& pluginPtrToUse,
                               std::atomic_flag& pluginAccessLockToUse)
            : juce::Thread ("Crate Sandbox State Extraction"),
              block (blockToUse), hostedPluginPtr (pluginPtrToUse), pluginAccessLock (pluginAccessLockToUse)
        {
        }

        // The Trigger directive: callable from ANY thread, including
        // potentially the plugin's own audio-processing thread (see class
        // doc comment on why) — sets one atomic and calls notify(), both
        // fast, non-blocking, allocation-free operations.
        //
        // Step 71 (State Extraction Debounce) directive — QA finding: some
        // plugins call updateHostDisplay()/sendChangeMessage() (which maps
        // to audioProcessorChanged(), see StateChangeListener above) on
        // EVERY parameter tweak during a drag, not just at gesture end —
        // vendor behavior outside this codebase's control, unlike the
        // deliberate no-op on audioProcessorParameterChanged() itself.
        // lastRequestMs records the WALL-CLOCK time of the latest request;
        // run()'s own debounce loop (below) is what actually decides when
        // to act on it.
        void triggerExtraction()
        {
            lastRequestMs.store (juce::Time::getMillisecondCounter(), std::memory_order_release);
            extractionRequested.store (true, std::memory_order_release);
            notify();
        }

        // Step 89 (Fresh Store Fix) directive: for an explicit, one-off,
        // user-initiated request (Store A/B) rather than a plugin's own
        // rapid-fire updateHostDisplay() burst during a live drag — the
        // debounce's entire reason for existing (Step 71: coalesce a
        // burst into one extraction) doesn't apply to a single manual
        // click, and making the user wait up to debounceMs for their own
        // explicit "capture this now" action would be a real, perceptible
        // UX regression, not a safety margin. Sets forceImmediate
        // alongside the normal trigger fields so run()'s own debounce loop
        // (below) skips its wait entirely on the very next check.
        void triggerImmediateExtraction()
        {
            lastRequestMs.store (juce::Time::getMillisecondCounter(), std::memory_order_release);
            extractionRequested.store (true, std::memory_order_release);
            forceImmediate.store (true, std::memory_order_release);
            notify();
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                wait (-1); // sleep until triggerExtraction()'s notify(), or threadShouldExit()

                if (threadShouldExit())
                    break;

                if (! extractionRequested.exchange (false, std::memory_order_acq_rel))
                    continue; // spurious wake guard

                // Step 71 (State Extraction Debounce) directive: collapse
                // an entire burst of rapid-fire requests into ONE
                // extraction, performed only once the request stream has
                // genuinely gone quiet for debounceMs. wait(timeoutMs)
                // returns EARLY the instant another triggerExtraction()
                // fires its own notify() during this wait — that's exactly
                // what re-loops this and re-measures against the freshly
                // updated lastRequestMs, so a continuous drag never lets
                // this fall through until it actually stops.
                for (;;)
                {
                    // Step 89 (Fresh Store Fix) directive: an explicit
                    // on-demand request always wins immediately, regardless
                    // of how recently the debounce window was last reset —
                    // see triggerImmediateExtraction()'s own doc comment.
                    if (forceImmediate.exchange (false, std::memory_order_acq_rel))
                        break;

                    const auto elapsedMs = juce::Time::getMillisecondCounter()
                                                - lastRequestMs.load (std::memory_order_acquire);

                    if (elapsedMs >= (juce::uint32) debounceMs)
                        break;

                    wait ((int) (debounceMs - elapsedMs));

                    if (threadShouldExit())
                        return;
                }

                auto* plugin = hostedPluginPtr.load (std::memory_order_acquire);

                if (plugin == nullptr)
                    continue;

                // Step 37 (The Debt Sweep) fix — a real lock-safety bug: the
                // old version of this loop called getStateInformation() and
                // pluginAccessLock.clear() UNCONDITIONALLY, even when the
                // 100-attempt backoff below exhausted WITHOUT ever
                // acquiring the lock (test_and_set() still returning true on
                // the very last attempt). That meant a fully-contended lock
                // (AudioBridgeThread's own processBlock() genuinely still
                // running) produced two compounding failures: (1) a real,
                // unsynchronized data race calling getStateInformation() on
                // the SAME plugin instance while processBlock() was still
                // touching it, and (2) releasing a spinlock this thread
                // never held, falsely handing out exclusive access to a
                // second contender while the original legitimate holder was
                // still mid-call — breaking mutual exclusion for everyone,
                // not just this thread. Fixed by tracking `acquired`
                // explicitly and skipping BOTH the plugin call and the
                // clear() when the lock was never actually taken — the
                // exact idiom LookaheadWorkerThread already gets right for
                // this SAME lock (see its own run(), above).
                int attempts = 0;
                bool acquired = false;

                while (! (acquired = ! pluginAccessLock.test_and_set (std::memory_order_acquire)))
                {
                    if (++attempts > 100)
                        break;

                    juce::Thread::sleep (1);
                }

                if (! acquired)
                    continue; // never got the lock — must not touch the plugin or clear a lock we don't hold; the next genuine edit re-triggers this anyway

                // Step 83 (Same-Thread State Extraction) directive — QA
                // finding, confirmed via a live, non-invasive WinDbg attach
                // on an actual reproduced freeze (not a guess): JUCE's own
                // VST3PluginInstanceHeadless::getStateInformation()
                // (juce_VST3PluginFormatImpl.h) unconditionally takes a
                // juce::MessageManagerLock before calling into the plugin's
                // own getState(). Calling it from THIS background thread
                // parks the Child's message thread inside
                // MessageManager::Lock::BlockingMessage::messageCallback()
                // — a raw condition_variable wait, not a message-pumping
                // wait — for however long the plugin's own getState() call
                // takes. The captured Child stack showed Melda's own
                // getState() implementation calling SetWindowPos on its own
                // editor HWND as a side effect (MeldaProductionAudioPlugin-
                // KernelV1164!... -> win32u!NtUserSetWindowPos) — a HWND
                // owned by the Child's OWN message thread. A cross-thread
                // SetWindowPos requires the target window's owning thread
                // to service synchronous WM_WINDOWPOSCHANGING/CHANGED
                // messages, which the parked message thread cannot do
                // while stuck in the wait above: the message thread waits
                // for this thread to release the MessageManagerLock, and
                // this thread (inside Melda's own code) waits for the
                // message thread to pump a window message. Circular,
                // deterministic, confirmed live on both sides at once —
                // not the cross-process Host-side mechanism first guessed;
                // the Host's own 98 threads were independently confirmed
                // all idle at the same moment.
                //
                // Fix: perform the actual plugin call ON the message
                // thread via callAsync(). juce::MessageManagerLock
                // recognises re-entrancy from the thread that already owns
                // the MessageManager and becomes a same-thread no-op
                // instead of posting a BlockingMessage — and any
                // SetWindowPos the plugin issues against its own HWND from
                // ITS OWN owning thread is a direct, same-thread Win32
                // call (never queued, never blocks). All the debounce/
                // backoff timing above stays on this background thread,
                // unchanged from Step 71 — only the single call that can
                // touch the plugin's own HWND moves to where that HWND
                // actually lives. sehGetStateInformation()'s own SEH
                // wrapper (Step 51) still protects this, now on the
                // message thread instead of here.
                juce::MessageManager::callAsync ([this, plugin]
                {
                    juce::MemoryBlock chunk;
                    const bool extractionOk = sehGetStateInformation (plugin, chunk);
                    pluginAccessLock.clear (std::memory_order_release);

                    if (! extractionOk)
                    {
                        logToFile ("CrateSandbox: getStateInformation() faulted (caught via SEH) — skipping this state push.");
                        return;
                    }

                    using CB = CrateIPC::ControlBlock;

                    if (chunk.getSize() == 0)
                        return; // nothing to push — some plugins return an empty chunk under some conditions

                    if ((int64_t) chunk.getSize() > CB::maxStateChunkBytes)
                        return; // real, disclosed limit — see ControlBlock's own doc comment on the fixed-size buffer contract

                    // Step 72 (Poison Pill / Hard Teardown) directive: this
                    // was the LAST unbounded spin against a shared-memory
                    // lock in this whole process (every sibling backoff was
                    // already capped at 100 attempts). Against a lock only
                    // ever held for a memcpy it looked safe — but a lock
                    // flag inherited from a KILLED previous generation
                    // (Guillotine, Watchdog) has no holder left to ever
                    // clear it, and an unbounded spin here permanently hung
                    // this thread, silently killing all state sync for the
                    // rest of the process's life. Now bounded exactly like
                    // every other backoff: abandon this one push and move
                    // on — the next genuine edit re-triggers extraction
                    // anyway. Running on the message thread now (Step 83)
                    // — contention here is only ever the Host's own brief
                    // read-side memcpy, so this bound is a formality, not
                    // an expected real wait.
                    int lockAttempts = 0;
                    bool chunkLockAcquired = false;

                    while (! (chunkLockAcquired = ! block.stateChunkLock.test_and_set (std::memory_order_acquire)))
                    {
                        if (++lockAttempts > 100)
                            break;
                        juce::Thread::sleep (1);
                    }

                    if (! chunkLockAcquired)
                    {
                        logToFile ("CrateSandbox: stateChunkLock still held after 100 attempts — abandoning this "
                                       "state push (the next edit re-triggers extraction).");
                        return;
                    }

                    std::memcpy (block.stateChunkData, chunk.getData(), chunk.getSize());
                    block.stateChunkSize.store ((int64_t) chunk.getSize(), std::memory_order_relaxed);
                    block.stateChunkAvailable.store (true, std::memory_order_release);

                    block.stateChunkLock.clear (std::memory_order_release);
                });
            }
        }

    private:
        CrateIPC::ControlBlock& block;
        std::atomic<juce::AudioPluginInstance*>& hostedPluginPtr;
        std::atomic_flag& pluginAccessLock;
        std::atomic<bool> extractionRequested { false };

        // Step 71 (State Extraction Debounce) directive: matches the
        // user's own explicit directive (500ms of quiet after the last
        // change before pushing a state chunk).
        static constexpr int debounceMs = 500;
        std::atomic<juce::uint32> lastRequestMs { 0 };

        // Step 89 (Fresh Store Fix) directive: see triggerImmediateExtraction()'s
        // own doc comment.
        std::atomic<bool> forceImmediate { false };
    };

    // The VST3 Edit Hook directive (Step 11): JUCE's own
    // AudioProcessorListener abstraction over VST3's IComponentHandler —
    // matches the step's own directive ("hook into IComponentHandler...
    // listen for endEdit or generic state-change notifications") without
    // touching raw VST3 COM interfaces directly, consistent with using
    // juce::AudioPluginFormatManager/AudioPluginInstance for everything
    // else in this codebase rather than the raw SDK.
    class StateChangeListener : public juce::AudioProcessorListener
    {
    public:
        explicit StateChangeListener (StateExtractionThread& threadToUse) : extractionThread (threadToUse) {}

        // endEdit — the user released a knob/fader. The PRIMARY trigger.
        void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override
        {
            extractionThread.triggerExtraction();
        }

        // Generic state-change notifications (program changes, etc.) — the
        // step's own named alternative to gesture-based endEdit, for
        // changes that don't go through a begin/end drag gesture at all.
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override
        {
            extractionThread.triggerExtraction();
        }

        // Fires on EVERY tiny value change during a drag (or Step 8's own
        // continuous sweep, which calls setValue() directly rather than
        // going through a host gesture) — deliberately a no-op; extracting
        // on every single sample of automation would defeat the entire
        // point of debouncing to gesture-end.
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    private:
        StateExtractionThread& extractionThread;
    };

    // Hybrid Sync Pivot directive: the actual DSP loop. Blocks on
    // readyEvent.wait() — genuine 0% CPU — rather than spinning on
    // parentReady. The 50ms wait() timeout exists ONLY so threadShouldExit()
    // gets checked periodically even if the parent never signals again (e.g.
    // torn down mid-wait) — it has nothing to do with the real audio-latency
    // budget, which is governed entirely by how fast SetEvent()/
    // WaitForSingleObject() actually wake a blocked thread (microseconds, in
    // practice — see CrateSandboxBridge.log's own logged round-trip timing).
    // parentReady is still checked after waking, as a defensive guard
    // against a spurious wake — it should never actually be false here, but
    // "trust the event blindly" is exactly the kind of assumption Step 5.5's
    // own stale-file bug came from, so it costs nothing to double-check.
    class AudioBridgeThread : public juce::Thread
    {
    public:
        AudioBridgeThread (CrateIPC::ControlBlock& blockToUse, std::atomic<juce::AudioPluginInstance*>& pluginPtrToUse,
                           const juce::String& instanceId, std::atomic_flag& pluginAccessLockToUse,
                           std::atomic<bool>& isCorruptedToUse)
            : juce::Thread ("Crate Sandbox Audio Bridge"),
              block (blockToUse),
              hostedPluginPtr (pluginPtrToUse),
              readyEvent (CrateIPC::getBufferReadyEventName (instanceId)),
              pluginAccessLock (pluginAccessLockToUse),
              isCorrupted (isCorruptedToUse)
        {
            // Zero Allocation directive (Step 7): pre-allocated ONCE, to the
            // worst-case size this bridge supports — sized down (never up)
            // per block via setSize(..., avoidReallocating=true) below, so
            // processBlock() never triggers a real-time allocation.
            workBuffer.setSize (CrateIPC::ControlBlock::maxChannels, CrateIPC::ControlBlock::maxSamplesPerBlock);
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                if (! readyEvent.wait (50))
                    continue; // no signal yet — loop back and re-check threadShouldExit()

                if (! block.parentReady.load (std::memory_order_acquire))
                    continue; // spurious/stale wake guard — nothing to process

                // Step 18 (The Time-Slip Engine) directive: mutually
                // exclusive modes for this tenant's DSP block — in
                // lookahead mode, LookaheadWorkerThread is the sole driver
                // of hostedPlugin->processBlock(), never this thread. In
                // practice the PARENT should simply stop dispatching
                // real-time requests once lookahead mode is active, so
                // this branch should rarely if ever actually trigger —
                // this is defense-in-depth against a stray/leftover
                // parentReady signal racing LookaheadWorkerThread for
                // pluginAccessLock, not the primary gate.
                if (block.isLookaheadMode.load (std::memory_order_acquire))
                    continue;

                const int numChannels = juce::jlimit (0, CrateIPC::ControlBlock::maxChannels,
                                                       block.numChannels.load (std::memory_order_relaxed));
                const int numSamples  = juce::jlimit (0, CrateIPC::ControlBlock::maxSamplesPerBlock,
                                                       block.numSamples.load (std::memory_order_relaxed));

                // RESTART LIVELOCK directive: re-read fresh every iteration
                // — this thread starts BEFORE loadHostedPlugin() finishes
                // (see initialise()'s own comment), so a pointer captured
                // once at construction would stay permanently null even
                // after the plugin successfully loads moments later.
                auto* currentPlugin = hostedPluginPtr.load (std::memory_order_acquire);

                // The Child Pop & Apply directive (Step 8): drained
                // UNCONDITIONALLY, even if currentPlugin is still null (a
                // plugin still loading, or one that failed to load) — the
                // queue must never back up waiting for a plugin, and
                // strictly BEFORE processBlock() below, per the step's own
                // directive, so a whole burst of automation lands before
                // the very next block that should audibly reflect it.
                drainParameterQueue (currentPlugin);

                // Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive,
                // Task 1: checked FIRST, unconditionally — once a plugin
                // instance has faulted (any thread's SEH catch, this one or
                // StateExtractionThread's), its internal DSP state is
                // UNTRUSTED. Continuing to call processBlock() on it risks
                // exactly what this protocol exists to prevent: a corrupted
                // plugin outputting maximum-level noise into a live PA. A
                // corrupted plugin gets SILENCE, not passthrough — the user's
                // own explicit directive — for as long as this instance
                // lives, which should be briefly: the PARENT's own
                // corruptionDetected poll (see ControlBlock's own doc
                // comment) kills and respawns this whole process shortly
                // after this flag is first observed set.
                if (isCorrupted.load (std::memory_order_acquire))
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                        std::memset (block.audioOutput + (size_t) ch * CrateIPC::ControlBlock::maxSamplesPerBlock, 0, (size_t) numSamples * sizeof (float));

                    block.childProcessed.store (true, std::memory_order_release);
                    block.parentReady.store (false, std::memory_order_relaxed);
                    continue;
                }

                // Step 11 directive: StateExtractionThread can now call
                // getStateInformation()/setStateInformation() on this SAME
                // plugin instance from a different thread — try-lock rather
                // than block indefinitely (this thread's own real-time-ish
                // contract still matters), and just fall through to
                // passthrough for this one block if genuinely contended,
                // exactly like a cache-miss/full-queue anywhere else in
                // this codebase degrades gracefully instead of blocking.
                const bool acquiredPluginAccess = currentPlugin != nullptr && ! pluginAccessLock.test_and_set (std::memory_order_acquire);

                if (acquiredPluginAccess && numChannels > 0 && numSamples > 0)
                {
                    workBuffer.setSize (numChannels, numSamples, false, false, true); // no realloc — within the capacity reserved in the constructor

                    for (int ch = 0; ch < numChannels; ++ch)
                        workBuffer.copyFrom (ch, 0, block.audioInput + (size_t) ch * CrateIPC::ControlBlock::maxSamplesPerBlock, numSamples);

                    // The DSP Pass-Through directive: parameter changes are
                    // applied above, via drainParameterQueue(), strictly
                    // BEFORE this call — no MIDI sync yet, per Step 8's own
                    // scope, so an empty MIDI buffer is still correct here.
                    midiBuffer.clear();

                    // Step 39 (Live Telemetry) directive, Task 3: wraps
                    // ONLY this one call — the plugin's own real CPU cost,
                    // not IPC dispatch/spin-wait overhead (which is measured
                    // separately, on the PARENT side, as
                    // CrateSandboxBridge::maxRoundTripMsObserved).
                    //
                    // Step 52 directive, Task 1: sehProcessBlock() replaces
                    // a bare call — see its own doc comment. A caught fault
                    // here is THE core scenario this whole protocol exists
                    // for: mark this instance corrupted (both locally and
                    // over IPC) and overwrite whatever workBuffer may hold
                    // (a partially-written, possibly garbage block from the
                    // instant of the fault) with silence before it ever
                    // reaches audioOutput.
                    const auto processBlockStartMs = juce::Time::getMillisecondCounterHiRes();
                    const bool processedOk = sehProcessBlock (currentPlugin, workBuffer, midiBuffer);
                    const auto processBlockMicros = (uint32_t) juce::jmax (0.0, (juce::Time::getMillisecondCounterHiRes() - processBlockStartMs) * 1000.0);
                    block.lastProcessBlockMicros.store (processBlockMicros, std::memory_order_relaxed);

                    pluginAccessLock.clear (std::memory_order_release);

                    if (! processedOk)
                    {
                        isCorrupted.store (true, std::memory_order_release);
                        block.corruptionDetected.store (true, std::memory_order_release);
                        logToFile ("CrateSandbox: processBlock() faulted (caught via SEH) — plugin instance marked CORRUPTED, "
                                       "outputting silence and awaiting respawn.");

                        for (int ch = 0; ch < numChannels; ++ch)
                            std::memset (block.audioOutput + (size_t) ch * CrateIPC::ControlBlock::maxSamplesPerBlock, 0, (size_t) numSamples * sizeof (float));
                    }
                    else
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            auto* out = block.audioOutput + (size_t) ch * CrateIPC::ControlBlock::maxSamplesPerBlock;
                            std::memcpy (out, workBuffer.getReadPointer (ch), (size_t) numSamples * sizeof (float));
                        }
                    }
                }
                else
                {
                    if (acquiredPluginAccess)
                        pluginAccessLock.clear (std::memory_order_release); // acquired but numChannels/numSamples were 0 — release what we took
                    // No plugin loaded (empty path, or load/instantiate
                    // failure) — plain passthrough, not Step 6's old phase
                    // inversion, which this step explicitly removes.
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        auto* in  = block.audioInput  + (size_t) ch * CrateIPC::ControlBlock::maxSamplesPerBlock;
                        auto* out = block.audioOutput + (size_t) ch * CrateIPC::ControlBlock::maxSamplesPerBlock;
                        std::memcpy (out, in, (size_t) numSamples * sizeof (float));
                    }
                }

                block.childProcessed.store (true, std::memory_order_release);
                block.parentReady.store (false, std::memory_order_relaxed);
            }
        }

    private:
        // The Child Pop & Apply directive (Step 8): the SOLE consumer side
        // of the parameter ring buffer — only ever writes paramQueueTail,
        // only ever reads paramQueueHead (mirror image of the PARENT's
        // setParameterEvent(), which only ever writes paramQueueHead/reads
        // paramQueueTail) — classic SPSC, no lock needed.
        void drainParameterQueue (juce::AudioPluginInstance* plugin)
        {
            using CB = CrateIPC::ControlBlock;

            uint32_t tail = block.paramQueueTail.load (std::memory_order_relaxed); // consumer-owned — only WE ever write this
            const uint32_t head = block.paramQueueHead.load (std::memory_order_acquire);

            if (tail == head)
                return; // nothing queued — the common case, most audio blocks have no automation this instant

            const juce::Array<juce::AudioProcessorParameter*>* params = plugin != nullptr ? &plugin->getParameters() : nullptr;

            while (tail != head)
            {
                const auto& change = block.paramQueue[tail];

                if (params != nullptr && change.parameterIndex >= 0 && change.parameterIndex < params->size())
                    (*params)[change.parameterIndex]->setValue (juce::jlimit (0.0f, 1.0f, change.normalizedValue));

                tail = (tail + 1) % (uint32_t) CB::paramQueueCapacity;
            }

            block.paramQueueTail.store (tail, std::memory_order_release);
        }

        CrateIPC::ControlBlock& block;
        std::atomic<juce::AudioPluginInstance*>& hostedPluginPtr;
        juce::AudioBuffer<float> workBuffer;
        juce::MidiBuffer midiBuffer;
        CrateIPC::NamedEvent readyEvent;
        std::atomic_flag& pluginAccessLock;

        // Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive: shared
        // with the OWNING scope (CrateSandboxApplication for isolated,
        // TenantContext for a tenant) and, in the tenant case, also with
        // that SAME tenant's own LookaheadWorkerThread — both threads call
        // into the identical hostedPlugin instance, so both must respect
        // the SAME corruption flag the instant either one sets it.
        std::atomic<bool>& isCorrupted;
    };

    //==============================================================================
    // Step 15.2 (The Shared Host Engine) directive: everything ABOVE this
    // point (HeartbeatThread, StateExtractionThread, StateChangeListener,
    // AudioBridgeThread) was already correctly parameterized by reference —
    // none of them assume they're the only instance of their kind in this
    // process, they just happened to only ever be constructed once each, by
    // initialise()'s own single-instance isolated-mode path above. Multi-
    // tenancy doesn't need new DSP/state/GUI machinery, it needs a place to
    // hold N complete SETS of exactly the same machinery, one per
    // dynamically-spawned tenant, plus a way to create a new set on demand.
    // TenantContext is that set; CommandListenerThread is the "on demand"
    // part.
    //
    // TenantContext bundles the SAME things initialise()'s isolated path
    // scatters across CrateSandboxApplication's own member list — one
    // ControlBlock mapping, one plugin instance, one of every worker
    // thread, one editor. Not movable/copyable (std::atomic_flag and
    // std::atomic<> members forbid it), so always owned via
    // std::unique_ptr<TenantContext>, never by value.
    struct TenantContext : public juce::ComponentListener
    {
        juce::String instanceId;

        std::unique_ptr<juce::MemoryMappedFile> sharedMemory;
        CrateIPC::ControlBlock* controlBlock = nullptr;

        juce::AudioPluginFormatManager pluginFormatManager;
        std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;
        std::atomic<juce::AudioPluginInstance*> hostedPluginPtr { nullptr };

        // Same idiom as CrateSandboxApplication's own single-tenant
        // pluginAccessLock (Step 11) — but per-TENANT here, since each
        // tenant's AudioBridgeThread/StateExtractionThread pair only ever
        // contends with ITS OWN tenant's threads, never another tenant's.
        std::atomic_flag pluginAccessLock = ATOMIC_FLAG_INIT;

        // Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive: owned
        // here (per-tenant), shared by reference into this tenant's own
        // AudioBridgeThread AND LookaheadWorkerThread — set true the instant
        // EITHER thread's processBlock() faults, checked by BOTH before
        // their next call. Starts false; a respawned tenant gets a brand
        // new TenantContext (and thus a fresh, false flag) rather than this
        // one being reset in place.
        std::atomic<bool> isCorrupted { false };

        std::unique_ptr<HeartbeatThread> heartbeatThread;
        std::unique_ptr<AudioBridgeThread> audioBridgeThread;
        std::unique_ptr<LookaheadWorkerThread> lookaheadWorkerThread;
        std::unique_ptr<StateExtractionThread> stateExtractionThread;
        std::unique_ptr<StateChangeListener> stateChangeListener;

        // Step 10 (window reparenting) equivalent, per-tenant — created
        // from the SAME app-level timerCallback(), just iterating tenants
        // now instead of assuming a single instance. Message-thread-owned,
        // same as the isolated path's own activeEditor.
        std::unique_ptr<juce::AudioProcessorEditor> activeEditor;

        // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive: same
        // per-tenant treatment as activeEditor above — see the isolated
        // path's matching member for the full reasoning.
        float lastAppliedDisplayScale = 1.0f;

        // Step 24 (Editor View Recovery Guard) directive: per-tenant
        // treatment of the same Melda-view-corruption fingerprint — see
        // the isolated path's own matching members for the full evidence
        // trail and reasoning.
        int initialEditorWidth  = 0;
        int initialEditorHeight = 0;
        bool editorHasEverResized = false;

        // Step 55 (The Liar's Penalty) directive: per-tenant treatment —
        // see the isolated path's own matching member for the full
        // reasoning. Never resets for this tenant's lifetime.
        bool editorProvedFixedSizeLiar = false;

        // Step 35 (Editor View Recovery Guard v2) directive, Task 3:
        // per-tenant treatment — see the isolated path's own matching
        // member for the full reasoning.
        juce::uint32 lastEditorHealthCheckMs = 0;

        // Step 69 (Watchdog Strike System) directive: per-tenant
        // treatment — see the isolated path's own matching member for the
        // full reasoning. editorHealthCheckStrikeThreshold itself is a
        // single shared constant (declared once, at the outer class's own
        // scope) — the same "how many strikes is enough" answer applies
        // to every tenant equally, only the running count is per-tenant.
        int editorHealthCheckStrikes = 0;

        // Step 68 (Outbound Throttle & Lifecycle Integrity) directive,
        // Task 1: per-tenant treatment — see the isolated path's own
        // matching member for the full reasoning. outboundReportThrottleMs
        // itself is a single shared constant (declared once, at the outer
        // class's own scope), but the timestamp it's checked against must
        // be tracked per-tenant — one tenant's own drag shouldn't throttle
        // a completely unrelated tenant's own outbound reports.
        juce::uint32 lastOutboundReportMs = 0;

        // Step 81 (Event-Driven Geometry Publish) directive: per-tenant
        // treatment — see the isolated path's own matching methods for the
        // full reasoning. Each tenant IS its own juce::ComponentListener
        // (rather than sharing the outer CrateSandboxApplication's single
        // one) since multiple tenants' own editors can exist at once, each
        // needing to be told apart at dispatch time.
        void publishEditorSizeIfChanged (int currentWidth, int currentHeight)
        {
            if (controlBlock == nullptr || currentWidth <= 0 || currentHeight <= 0)
                return;

            if (currentWidth  == controlBlock->windowWidth.load (std::memory_order_relaxed)
                && currentHeight == controlBlock->windowHeight.load (std::memory_order_relaxed))
                return;

            const auto nowOutboundMs = juce::Time::getMillisecondCounter();

            // Qualified explicitly — CrateSandboxApplication::
            // outboundReportThrottleMs is a private static constexpr on the
            // OUTER class; a nested struct's own member function does NOT
            // get automatic unqualified lookup into the enclosing class's
            // scope (only matching ACCESS rights, which C++11 does grant
            // nested classes to their enclosing class's private members —
            // but lookup and access are two different things).
            if (nowOutboundMs - lastOutboundReportMs < (juce::uint32) CrateSandboxApplication::outboundReportThrottleMs)
                return;

            lastOutboundReportMs = nowOutboundMs;
            controlBlock->windowWidth.store (currentWidth, std::memory_order_relaxed);
            controlBlock->windowHeight.store (currentHeight, std::memory_order_release);
        }

        void componentMovedOrResized (juce::Component& component, bool /*wasMoved*/, bool wasResized) override
        {
            if (wasResized && activeEditor != nullptr && &component == activeEditor.get())
                publishEditorSizeIfChanged (activeEditor->getWidth(), activeEditor->getHeight());
        }

        TenantContext() = default;

        // Teardown order matches CrateSandboxApplication::shutdown()'s own
        // established sequence exactly — same reasoning, just scoped to
        // ONE tenant instead of the whole process. Runs when a
        // std::unique_ptr<TenantContext> is erased from the tenants map.
        ~TenantContext()
        {
            if (audioBridgeThread != nullptr)
                audioBridgeThread->stopThread (1000);
            audioBridgeThread.reset();

            if (lookaheadWorkerThread != nullptr)
                lookaheadWorkerThread->stopThread (1000);
            lookaheadWorkerThread.reset();

            if (heartbeatThread != nullptr)
                heartbeatThread->stopThread (1000);
            heartbeatThread.reset();

            if (stateExtractionThread != nullptr)
                stateExtractionThread->stopThread (2000);
            stateExtractionThread.reset();

            if (hostedPlugin != nullptr && stateChangeListener != nullptr)
                hostedPlugin->removeListener (stateChangeListener.get());
            stateChangeListener.reset();

            activeEditor.reset();

            hostedPluginPtr.store (nullptr, std::memory_order_release);

            if (hostedPlugin != nullptr)
                hostedPlugin->releaseResources();
            hostedPlugin.reset();

            sharedMemory.reset();
        }

        TenantContext (const TenantContext&) = delete;
        TenantContext& operator= (const TenantContext&) = delete;
    };

    // Step 15.2 directive: a low-priority background thread that blocks
    // (genuine 0% CPU, same NamedEvent-wait idiom as AudioBridgeThread's own
    // readyEvent) until the PARENT signals the Master Control Channel's wake
    // event, then drains every pending SpawnCommand — the SOLE consumer side
    // of that SPSC ring buffer (mirrors the per-instance parameter queue's
    // own single-consumer contract). Dequeuing (advancing commandQueueTail)
    // happens HERE, immediately, so the ring buffer never backs up; the
    // actual tenant creation work is handed off to the MESSAGE thread via
    // MessageManager::callAsync(), because creating an AudioPluginInstance
    // and (later) its editor are both message-thread operations in JUCE —
    // same reasoning as timerCallback()'s own doc comment on why window
    // creation can't happen off the message thread.
    class CommandListenerThread : public juce::Thread
    {
    public:
        using CommandType = CrateIPC::SharedHostCommandBlock::SpawnCommand::Type;

        // Step 15.4 (The Teardown Protocol) directive: the callback now
        // also carries the command Type — Spawn or Unload — so the
        // message-thread side can route to spawnTenant() or
        // unloadTenant() accordingly, instead of this queue only ever
        // meaning "create a tenant."
        CommandListenerThread (CrateIPC::SharedHostCommandBlock& blockToUse, CrateIPC::NamedEvent& eventToUse,
                               std::function<void (CommandType, juce::String, juce::String, juce::String)> onCommand)
            : juce::Thread ("Crate Shared Host Command Listener"),
              block (blockToUse), readyEvent (eventToUse), callback (std::move (onCommand))
        {
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                if (! readyEvent.wait (200))
                    continue; // no signal yet — loop back and re-check threadShouldExit()

                uint32_t tail = block.commandQueueTail.load (std::memory_order_relaxed); // consumer-owned
                const uint32_t head = block.commandQueueHead.load (std::memory_order_acquire);

                while (tail != head)
                {
                    auto& slot = block.commandQueue[tail];

                    const auto type = slot.type;
                    juce::String pluginUID   (juce::CharPointer_UTF8 (slot.pluginUID));
                    juce::String instanceId  (juce::CharPointer_UTF8 (slot.instanceId));
                    juce::String pluginPath  (juce::CharPointer_UTF8 (slot.pluginPath));

                    tail = (tail + 1) % (uint32_t) CrateIPC::SharedHostCommandBlock::commandQueueCapacity;
                    block.commandQueueTail.store (tail, std::memory_order_release);

                    if (callback)
                    {
                        auto callbackCopy = callback;
                        juce::MessageManager::callAsync ([callbackCopy, type, pluginUID, pluginPath, instanceId]
                        {
                            callbackCopy (type, pluginUID, pluginPath, instanceId);
                        });
                    }
                }
            }
        }

    private:
        CrateIPC::SharedHostCommandBlock& block;
        CrateIPC::NamedEvent& readyEvent;
        std::function<void (CommandType, juce::String, juce::String, juce::String)> callback;
    };

    // Step 15.2 (The Shared Host Engine) directive: the multi-tenant
    // analogue of loadHostedPlugin() + the ControlBlock-mapping half of
    // initialise() — everything a single tenant needs, called once per
    // dequeued SpawnCommand, always on the message thread (see
    // CommandListenerThread's own doc comment for why). Deliberately
    // simpler than the isolated path in two ways, both honestly scoped out
    // rather than silently skipped:
    //
    //   1. No pluginPath/hostSampleRate/hostBlockSize PRESERVE-across-reset
    //      dance — the PARENT tenant bridge, not this process, is what
    //      wrote hostSampleRate/hostBlockSize into this tenant's own
    //      ControlBlock (in CrateSandboxBridge::attachAsTenant(), BEFORE
    //      dispatching this exact command), and this is the FIRST time this
    //      instanceId's block is ever touched, so placement-new needs
    //      nothing preserved.
    //   2. No Ghost Reload / restart-on-crash for tenants yet — if a
    //      tenant's own plugin instance crashes or stalls, its
    //      TenantContext just sits dead; nothing re-spawns it
    //      automatically. Real follow-up work, not silently pretended to
    //      work — see this class's own doc comment.
    void spawnTenant (const juce::String& pluginUID, const juce::String& pluginPath, const juce::String& instanceIdToSpawn)
    {
        if (tenants.find (instanceIdToSpawn) != tenants.end())
        {
            logToFile ("CrateSandbox (Shared Host): duplicate spawn command for instanceID=" + instanceIdToSpawn + " — ignoring.");
            return;
        }

        logToFile ("CrateSandbox (Shared Host): spawning tenant instanceID=" + instanceIdToSpawn
                       + ", pluginUID=" + pluginUID + ", path=" + pluginPath);

        auto tenant = std::make_unique<TenantContext>();
        tenant->instanceId = instanceIdToSpawn;

        auto file = CrateIPC::getSharedMemoryFile (instanceIdToSpawn);

        // The PARENT tenant bridge already created/sized this file and
        // wrote pluginPath/hostSampleRate/hostBlockSize into it (see
        // CrateSandboxBridge::attachAsTenant()) BEFORE dispatching this
        // command — ensureSharedMemoryFileIsSized() here is defensive only,
        // matching the isolated path's own "correct even if called twice"
        // convention, never expected to actually resize anything in the
        // normal case.
        CrateIPC::ensureSharedMemoryFileIsSized (file);

        tenant->sharedMemory = std::make_unique<juce::MemoryMappedFile> (file, juce::MemoryMappedFile::readWrite);

        const bool mapped = tenant->sharedMemory->getData() != nullptr
                             && tenant->sharedMemory->getSize() == (size_t) CrateIPC::sharedMemoryBytes;

        if (! mapped)
        {
            logToFile ("CrateSandbox (Shared Host): FAILED to map tenant shared memory for instanceID=" + instanceIdToSpawn);
            return;
        }

        auto* block = CrateIPC::getControlBlock (tenant->sharedMemory->getData());

        // Unlike the isolated path's initialise(), nothing needs to be
        // preserved across this placement-new — this is the FIRST and ONLY
        // time this instanceId's block is ever constructed (see this
        // method's own doc comment, point 1).
        const auto preservedSampleRate = block->hostSampleRate;
        const auto preservedBlockSize  = block->hostBlockSize;
        char preservedPluginPath[CrateIPC::ControlBlock::maxPluginPathLength];
        std::memcpy (preservedPluginPath, block->pluginPath, sizeof (preservedPluginPath));

        new (block) CrateIPC::ControlBlock();

        block->hostSampleRate = preservedSampleRate;
        block->hostBlockSize  = preservedBlockSize;
        std::memcpy (block->pluginPath, preservedPluginPath, sizeof (preservedPluginPath));

        tenant->controlBlock = block;

        tenant->heartbeatThread = std::make_unique<HeartbeatThread> (*tenant->controlBlock);
        tenant->heartbeatThread->startThread (juce::Thread::Priority::normal);

        tenant->audioBridgeThread = std::make_unique<AudioBridgeThread> (*tenant->controlBlock, tenant->hostedPluginPtr,
                                                                          instanceIdToSpawn, tenant->pluginAccessLock, tenant->isCorrupted);
        tenant->audioBridgeThread->startThread (juce::Thread::Priority::high);

        // Step 17 directive: same rationale as the isolated path's own
        // construction site — started alongside AudioBridgeThread, mode
        // switching is later work.
        tenant->lookaheadWorkerThread = std::make_unique<LookaheadWorkerThread> (*tenant->controlBlock, tenant->hostedPluginPtr,
                                                                                  instanceIdToSpawn, tenant->pluginAccessLock, tenant->isCorrupted);
        tenant->lookaheadWorkerThread->startThread (juce::Thread::Priority::low);

        loadPluginIntoTenant (*tenant, pluginPath, block->hostSampleRate, block->hostBlockSize);

        tenants.emplace (instanceIdToSpawn, std::move (tenant));

        logToFile ("CrateSandbox (Shared Host): tenant instanceID=" + instanceIdToSpawn
                       + " is live (" + juce::String ((int) tenants.size()) + " tenant(s) total in this process).");
    }

    // Step 15.4 (The Teardown Protocol) directive: the other half of the
    // Master Control Channel's now-dual-purpose queue — a TenantBridge on
    // the PARENT side dispatches this the moment IT is destroyed (see
    // CrateSandboxBridge::deinitialise()'s onTenantRemoved callback), so
    // this process can actually free the RAM instead of leaking a
    // forgotten tenant forever (exactly the gap flagged, then reproduced
    // as a genuine restart-storm side effect, in Step 15.2/15.3's own
    // doc comments). erase() on the map runs ~TenantContext() — same
    // thread/plugin/editor teardown ordering as shutdown() uses for the
    // isolated path, just scoped to one tenant.
    void unloadTenant (const juce::String& instanceIdToUnload)
    {
        auto it = tenants.find (instanceIdToUnload);

        if (it == tenants.end())
        {
            logToFile ("CrateSandbox (Shared Host): unload requested for unknown/already-gone instanceID=" + instanceIdToUnload);
            return;
        }

        tenants.erase (it);

        logToFile ("CrateSandbox (Shared Host): tenant instanceID=" + instanceIdToUnload
                       + " unloaded (" + juce::String ((int) tenants.size()) + " tenant(s) remain in this process).");
    }

    // The per-tenant equivalent of loadHostedPlugin() — same logic, reads
    // from/writes to a TenantContext instead of this class's own top-level
    // members. Kept as a SEPARATE method rather than templating
    // loadHostedPlugin() itself: the isolated path's version also touches
    // this->stateExtractionThread/this->stateChangeListener directly, and
    // duplicating the (short) body here is clearer than retrofitting that
    // one to take an indirection parameter for every single member it uses.
    void loadPluginIntoTenant (TenantContext& tenant, const juce::String& pluginPath, double sampleRate, int blockSize)
    {
        if (pluginPath.isEmpty())
        {
            logToFile ("CrateSandbox (Shared Host): no plugin path provided for tenant " + tenant.instanceId + " — running pass-through only.");
            return;
        }

        if (sampleRate <= 0.0)
            sampleRate = 44100.0;

        if (blockSize <= 0)
            blockSize = 512;

        juce::addDefaultFormatsToManager (tenant.pluginFormatManager);

        juce::KnownPluginList knownPlugins;
        juce::OwnedArray<juce::PluginDescription> foundTypes;
        knownPlugins.scanAndAddDragAndDroppedFiles (tenant.pluginFormatManager, juce::StringArray (pluginPath), foundTypes);

        if (foundTypes.isEmpty())
        {
            logToFile ("CrateSandbox (Shared Host): FAILED to find/scan VST3 at " + pluginPath + " for tenant " + tenant.instanceId);
            return;
        }

        if (tenant.controlBlock != nullptr)
        {
            const auto identifier = foundTypes.getFirst()->createIdentifierString();
            std::memset (tenant.controlBlock->pluginUID, 0, sizeof (tenant.controlBlock->pluginUID));
            identifier.copyToUTF8 (tenant.controlBlock->pluginUID, (size_t) CrateIPC::ControlBlock::maxPluginUIDLength - 1);

            // Step 22 (The Profiling Database / The Warden) directive: see
            // loadHostedPlugin()'s own matching comment — same source, same
            // reasoning, just the per-tenant ControlBlock instead of the
            // isolated path's top-level one.
            const auto vendor = foundTypes.getFirst()->manufacturerName;
            std::memset (tenant.controlBlock->vendorName, 0, sizeof (tenant.controlBlock->vendorName));
            vendor.copyToUTF8 (tenant.controlBlock->vendorName, (size_t) CrateIPC::ControlBlock::maxVendorNameLength - 1);

            tenant.controlBlock->pluginUIDReady.store (true, std::memory_order_release);
        }

        juce::String errorMessage;
        tenant.hostedPlugin = tenant.pluginFormatManager.createPluginInstance (*foundTypes.getFirst(), sampleRate, blockSize, errorMessage);

        if (tenant.hostedPlugin == nullptr)
        {
            logToFile ("CrateSandbox (Shared Host): FAILED to instantiate VST3 for tenant " + tenant.instanceId + ": " + errorMessage);
            return;
        }

        tenant.hostedPlugin->prepareToPlay (sampleRate, blockSize);
        tenant.hostedPluginPtr.store (tenant.hostedPlugin.get(), std::memory_order_release);

        logToFile ("CrateSandbox (Shared Host): tenant " + tenant.instanceId + " VST3 loaded OK: " + tenant.hostedPlugin->getName()
                       + " (sampleRate=" + juce::String (sampleRate) + ", blockSize=" + juce::String (blockSize) + ")");

        // Step 31 (Real IPC Parameter Sync) directive: same metadata
        // publish as the isolated path's own loadHostedPlugin() — see its
        // matching comment for the full reasoning.
        {
            auto& tenantParams = tenant.hostedPlugin->getParameters();
            const int syncedCount = juce::jmin ((int) CrateIPC::ControlBlock::maxSyncedParams, tenantParams.size());

            for (int i = 0; i < syncedCount; ++i)
            {
                auto* param = tenantParams[i];
                auto& metadata = tenant.controlBlock->paramMetadata[i];

                std::memset (metadata.name, 0, sizeof (metadata.name));
                param->getName (64).copyToUTF8 (metadata.name, (size_t) CrateIPC::ControlBlock::maxParamNameLength - 1);
                metadata.defaultValue = param->getDefaultValue();

                tenant.controlBlock->paramCurrentValues[i].store (param->getValue(), std::memory_order_relaxed);
            }

            tenant.controlBlock->paramCount.store (syncedCount, std::memory_order_relaxed);
            tenant.controlBlock->paramMetadataReady.store (true, std::memory_order_release);
        }

        tenant.stateExtractionThread = std::make_unique<StateExtractionThread> (*tenant.controlBlock, tenant.hostedPluginPtr, tenant.pluginAccessLock);
        tenant.stateExtractionThread->startThread (juce::Thread::Priority::low);

        tenant.stateChangeListener = std::make_unique<StateChangeListener> (*tenant.stateExtractionThread);
        tenant.hostedPlugin->addListener (tenant.stateChangeListener.get());
    }

    // Step 15.2 directive: the per-tenant equivalent of the isolated-mode
    // timerCallback() body — same poison-pill/window-request logic, scoped
    // to ONE TenantContext instead of this class's own top-level members.
    //
    // HONEST SCOPE NOTE on the poison pill here: triggering it crashes the
    // WHOLE shared host process, taking every OTHER tenant down with it,
    // not just the one whose ControlBlock asked for it — there is no way
    // to contain a genuine unhandled access violation to "just one tenant"
    // within a single process; that containment is exactly what a
    // SEPARATE process provides, and exactly what Shared Sandbox mode
    // trades away for lower per-instance overhead. This is not an
    // oversight — it's the actual, honest argument for why
    // PluginHealthRegistry::requiresSolitaryConfinement() exists at all:
    // a plugin proven hostile gets quarantined into its OWN isolated
    // process specifically so it can never take other tenants down with
    // it again.
    void serviceTenant (TenantContext& tenant)
    {
        if (tenant.controlBlock != nullptr && tenant.controlBlock->triggerCrashRequested.load (std::memory_order_acquire))
        {
            logToFile ("CrateSandbox (Shared Host): poison pill triggered by tenant " + tenant.instanceId + " — crashing the WHOLE shared host now.");
            volatile int* p = nullptr;
            *p = 42;
        }

        if (tenant.controlBlock == nullptr)
            return;

        // Step 31 (Real IPC Parameter Sync) directive: see loadHostedPlugin()'s
        // isolated-path equivalent for the full reasoning — same per-tenant
        // treatment.
        if (tenant.controlBlock->paramMetadataReady.load (std::memory_order_acquire) && tenant.hostedPlugin != nullptr)
        {
            auto& liveParams = tenant.hostedPlugin->getParameters();
            const int syncedCount = tenant.controlBlock->paramCount.load (std::memory_order_relaxed);
            bool anyChanged = false;

            for (int i = 0; i < syncedCount && i < liveParams.size(); ++i)
            {
                const float newValue = liveParams[i]->getValue();
                const float oldValue = tenant.controlBlock->paramCurrentValues[i].load (std::memory_order_relaxed);

                if (std::abs (newValue - oldValue) > 0.0001f)
                {
                    tenant.controlBlock->paramCurrentValues[i].store (newValue, std::memory_order_relaxed);
                    anyChanged = true;
                }
            }

            if (anyChanged)
                tenant.controlBlock->paramValueRevision.fetch_add (1, std::memory_order_release);
        }

        // Step 39 (Live Telemetry) directive, Task 3: per-tenant treatment
        // — see the isolated path's own matching comment for the full
        // reasoning.
        if (tenant.hostedPlugin != nullptr)
        {
            const int32_t currentLatency = tenant.hostedPlugin->getLatencySamples();

            if (currentLatency != tenant.controlBlock->pluginLatencySamples.load (std::memory_order_relaxed))
                tenant.controlBlock->pluginLatencySamples.store (currentLatency, std::memory_order_relaxed);
        }

        // Step 39 (A/B Testing) directive, Task 4: per-tenant treatment —
        // see the isolated path's own matching comment for the full
        // reasoning.
        if (tenant.controlBlock->liveRestoreRequested.load (std::memory_order_acquire) && tenant.hostedPlugin != nullptr)
        {
            int attempts = 0;
            bool acquired = false;

            while (! (acquired = ! tenant.pluginAccessLock.test_and_set (std::memory_order_acquire)))
            {
                if (++attempts > 100)
                    break;
                juce::Thread::sleep (1);
            }

            if (acquired)
            {
                const int64_t size = tenant.controlBlock->liveRestoreStateSize.load (std::memory_order_relaxed);

                if (size > 0 && size <= CrateIPC::ControlBlock::maxStateChunkBytes)
                {
                    // Store A/B Amnesia Fix (Step 53) directive: same
                    // bounded liveRestoreLock spin as the isolated path —
                    // see ControlBlock::liveRestoreLock's own doc comment.
                    int lockAttempts = 0;
                    bool lockAcquired = false;

                    while (! (lockAcquired = ! tenant.controlBlock->liveRestoreLock.test_and_set (std::memory_order_acquire)))
                    {
                        if (++lockAttempts > 100)
                            break;
                        juce::Thread::sleep (1);
                    }

                    if (lockAcquired)
                    {
                        // Step 51 (Silent Crash Hardening) directive: see the
                        // isolated path's own matching comment for the full
                        // reasoning — this matters MORE here, since a shared
                        // host's process crash takes every OTHER tenant down
                        // with it (see serviceTenant()'s own doc comment on
                        // that blast radius), making this exact call site the
                        // highest-value place in the whole file for this fix.
                        if (sehSetStateInformation (tenant.hostedPlugin.get(), tenant.controlBlock->liveRestoreStateData, (int) size))
                            logToFile ("CrateSandbox (Shared Host): DIAG A/B — tenant " + tenant.instanceId
                                           + " applied live restore, " + juce::String (size) + " bytes.");
                        else
                            logToFile ("CrateSandbox (Shared Host): setStateInformation() faulted (caught via SEH) for tenant "
                                           + tenant.instanceId + " during live restore — that tenant's plugin instance may be unstable.");

                        tenant.controlBlock->liveRestoreLock.clear (std::memory_order_release);
                    }
                    else
                    {
                        logToFile ("CrateSandbox (Shared Host): DIAG A/B — tenant " + tenant.instanceId
                                       + " liveRestoreLock contended, skipping this restore request rather than risking a torn buffer.");
                    }
                }
                else
                {
                    logToFile ("CrateSandbox (Shared Host): DIAG A/B — tenant " + tenant.instanceId
                                   + " restore request seen but size invalid (" + juce::String (size) + " bytes), skipped.");
                }

                tenant.pluginAccessLock.clear (std::memory_order_release);
                tenant.controlBlock->liveRestoreRequested.store (false, std::memory_order_release);
            }
            else
            {
                logToFile ("CrateSandbox (Shared Host): DIAG A/B — tenant " + tenant.instanceId
                               + " restore request seen but pluginAccessLock contended, retrying next tick.");
            }
        }

        // Step 89 (Fresh Store Fix) directive — per-tenant treatment, see
        // the isolated path's own matching comment for the full reasoning.
        if (tenant.controlBlock->forceStateExtractionRequested.exchange (false, std::memory_order_acq_rel))
            tenant.stateExtractionThread->triggerImmediateExtraction();

        // Step 23 (Geometry Polish & Dynamic Resize) directive: once an
        // editor already exists, keep republishing its CURRENT size every
        // tick rather than only once at creation — a resizable VST3 editor
        // (FabFilter-style corner drag, or its own IPlugView renegotiating
        // size) can change dimensions at any time, and the PARENT's own
        // poll (SandboxEditorTestWindow::timerCallback()) reads these SAME
        // windowWidth/windowHeight fields already established for the
        // one-shot initial size — no new IPC surface needed, just making
        // fields that were already "latest wins, both sides poll" (see
        // ControlBlock's own doc comment on that convention) actually stay
        // live instead of write-once.
        if (tenant.activeEditor != nullptr)
        {
            const int currentWidth  = tenant.activeEditor->getWidth();
            const int currentHeight = tenant.activeEditor->getHeight();

            // Step 24 (Editor View Recovery Guard) directive: see the
            // isolated path's own matching comment for the full evidence
            // trail and reasoning.
            //
            // Step 57 (The Liar's Tolerance) directive: per-tenant
            // treatment — see the isolated path's own matching comment for
            // the full MAutoDynamicEq/Melda evidence trail and reasoning.
            if (! tenant.editorHasEverResized)
            {
                if (std::abs (currentWidth - tenant.initialEditorWidth) > recoveryGuardTolerancePixels
                        || std::abs (currentHeight - tenant.initialEditorHeight) > recoveryGuardTolerancePixels)
                    tenant.editorHasEverResized = true;
            }
            else if (std::abs (currentWidth - tenant.initialEditorWidth) <= recoveryGuardTolerancePixels
                         && std::abs (currentHeight - tenant.initialEditorHeight) <= recoveryGuardTolerancePixels)
            {
                logToFile ("CrateSandbox (Shared Host): tenant " + tenant.instanceId + " editor view snapped back to within "
                               + juce::String (recoveryGuardTolerancePixels) + "px of its creation size ("
                               + juce::String (tenant.initialEditorWidth) + "x" + juce::String (tenant.initialEditorHeight)
                               + ", currently " + juce::String (currentWidth) + "x" + juce::String (currentHeight)
                               + ") after being resized away from it — recreating the editor to recover.");

                // Step 55 (The Liar's Penalty) directive: per-tenant
                // treatment — see the isolated path's own matching branch
                // for the full VoxDucker evidence trail and reasoning.
                tenant.editorProvedFixedSizeLiar = true;
                tenant.controlBlock->fixedSizeLiarDetected.store (true, std::memory_order_release);
                tenant.controlBlock->editorCanResize.store (false, std::memory_order_relaxed);
                tenant.controlBlock->pluginMinWidth.store  (tenant.initialEditorWidth,  std::memory_order_relaxed);
                tenant.controlBlock->pluginMinHeight.store (tenant.initialEditorHeight, std::memory_order_relaxed);
                tenant.controlBlock->pluginMaxWidth.store  (tenant.initialEditorWidth,  std::memory_order_relaxed);
                tenant.controlBlock->pluginMaxHeight.store (tenant.initialEditorHeight, std::memory_order_relaxed);

                logToFile ("CrateSandbox (Shared Host): LIAR'S PENALTY — tenant " + tenant.instanceId
                               + " forced FIXED-SIZE at " + juce::String (tenant.initialEditorWidth) + "x"
                               + juce::String (tenant.initialEditorHeight) + ".");

                tenant.controlBlock->windowHandleReady.store (false, std::memory_order_release);

                tenant.activeEditor.reset();
                return;
            }

            // Step 35 (Editor View Recovery Guard v2) directive, Task 3:
            // per-tenant treatment — see the isolated path's own matching
            // call site for the full reasoning.
            const auto tenantNowMs = juce::Time::getMillisecondCounter();

            if (tenantNowMs - tenant.lastEditorHealthCheckMs >= 1000)
            {
                tenant.lastEditorHealthCheckMs = tenantNowMs;
                bool editorResponsive = true;

               #if JUCE_WINDOWS
                if (auto* peer = tenant.activeEditor->getPeer())
                {
                    if (auto hwnd = (HWND) peer->getNativeHandle())
                    {
                        DWORD_PTR result = 0;
                        editorResponsive = (SendMessageTimeoutW (hwnd, WM_NULL, 0, 0,
                                                                 SMTO_ABORTIFHUNG, 500, &result) != 0);
                    }
                }
               #endif

                // Step 69 (Watchdog Strike System) directive: per-tenant
                // treatment — see the isolated path's own matching comment
                // for the full reasoning.
                if (! editorResponsive)
                    ++tenant.editorHealthCheckStrikes;
                else
                    tenant.editorHealthCheckStrikes = 0;

                if (tenant.editorHealthCheckStrikes >= editorHealthCheckStrikeThreshold)
                {
                    logToFile ("CrateSandbox (Shared Host): tenant " + tenant.instanceId
                                   + " editor HWND failed " + juce::String (tenant.editorHealthCheckStrikes)
                                   + " consecutive Win32 responsiveness checks (SendMessageTimeout) — recreating "
                                     "editor view to recover. Audio processing untouched.");

                    // Step 74 (The Flight Recorder) directive: per-tenant
                    // treatment — see the isolated path's own matching
                    // comment for the full reasoning.
                    FlightRecorder::getInstance().flushToDisk ("CHILD_EDITOR_HEALTHCHECK_STRIKE_LIMIT");

                    tenant.controlBlock->windowHandleReady.store (false, std::memory_order_release);

                    tenant.editorHealthCheckStrikes = 0;
                    tenant.activeEditor.reset();
                    return;
                }
            }

            // Step 68 (Outbound Throttle & Lifecycle Integrity) directive,
            // Task 1: per-tenant treatment — see the isolated path's own
            // matching comment for the full reasoning.
            tenant.publishEditorSizeIfChanged (currentWidth, currentHeight);

            // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive:
            // same per-tenant treatment as the isolated path's own
            // timerCallback() — see that method's matching comment for the
            // full reasoning.
            const float requestedScale = tenant.controlBlock->displayScale1000.load (std::memory_order_relaxed) / 1000.0f;

            if (std::abs (requestedScale - tenant.lastAppliedDisplayScale) > 0.001f)
            {
                tenant.lastAppliedDisplayScale = requestedScale;
                tenant.activeEditor->setScaleFactor (requestedScale);
            }

            // Step 65 (Pure Follower Architecture) directive: per-tenant
            // treatment — see the isolated path's own matching comment.
            // The Host never publishes a resize request to a tenant any
            // more; it's a pure follower of whatever size the tenant's own
            // editor reports above.
            return;
        }

        if (! tenant.controlBlock->windowHandleRequested.load (std::memory_order_acquire))
            return;

        // Step 79 (Pre-Emptive Native Parenting) directive: per-tenant
        // treatment — see the isolated path's own matching check for the
        // full reasoning.
        const int64_t hostSlotHwndValue = tenant.controlBlock->hostSlotHwndValue.load (std::memory_order_acquire);

        if (hostSlotHwndValue == 0)
        {
            logToFile ("CrateSandbox (Shared Host): window handle requested for tenant " + tenant.instanceId
                           + ", but the Host hasn't published its Airlock slot yet — waiting.");
            return;
        }

        if (tenant.hostedPlugin == nullptr || ! tenant.hostedPlugin->hasEditor())
        {
            logToFile ("CrateSandbox (Shared Host): window handle requested for tenant " + tenant.instanceId + ", but no editor-capable plugin is loaded.");
            return;
        }

        tenant.activeEditor.reset (tenant.hostedPlugin->createEditorAndMakeActive());

        if (tenant.activeEditor == nullptr)
        {
            logToFile ("CrateSandbox (Shared Host): createEditorAndMakeActive() returned nullptr for tenant " + tenant.instanceId);
            return;
        }

        // Step 36 (The Fixed-Size Lock) directive, Task 1: MUST be read
        // BEFORE setResizable(true, true) below — see the isolated path's
        // matching comment for the full reasoning.
        // Step 55 (The Liar's Penalty) directive: per-tenant treatment —
        // see the isolated path's own matching line for the reasoning.
        const bool tenantEditorCanResize = ! tenant.editorProvedFixedSizeLiar
                                              && ! tenant.controlBlock->forceFixedSizeEditor.load (std::memory_order_relaxed)
                                              && tenant.activeEditor->isResizable();

        // Step 23 directive: was setResizable(false, false) — see the
        // isolated path's matching comment for the full "One-Way Zoom
        // Trap" history and why this is now safe to re-enable.
        tenant.activeEditor->setResizable (true, true);

        // Step 88 (Probe Before Attach) directive — per-tenant treatment,
        // see the isolated path's own matching comment for the full
        // reasoning (this probe was firing a live, on-screen 10000x10000
        // resize on the already-attached, already-visible editor, which
        // is what actually corrupted Melda's own rendering — not a
        // missing onSize() call). Moved to run BEFORE addToDesktop(),
        // while the editor still has no peer and isn't shown to the user
        // at all.
        if (tenantEditorCanResize)
        {
            if (auto* probeConstrainer = tenant.activeEditor->getConstrainer())
            {
                const int creationWidth  = tenant.activeEditor->getWidth();
                const int creationHeight = tenant.activeEditor->getHeight();

                probeConstrainer->setBoundsForComponent (tenant.activeEditor.get(), { 0, 0, 1, 1 }, false, false, true, true);
                const int probedMinWidth  = tenant.activeEditor->getWidth();
                const int probedMinHeight = tenant.activeEditor->getHeight();

                probeConstrainer->setBoundsForComponent (tenant.activeEditor.get(), { 0, 0, 10000, 10000 }, false, false, true, true);
                const int probedMaxWidth  = tenant.activeEditor->getWidth();
                const int probedMaxHeight = tenant.activeEditor->getHeight();

                probeConstrainer->setBoundsForComponent (tenant.activeEditor.get(), { 0, 0, creationWidth, creationHeight }, false, false, true, true);

                tenant.controlBlock->pluginMinWidth.store  (probedMinWidth,  std::memory_order_relaxed);
                tenant.controlBlock->pluginMinHeight.store (probedMinHeight, std::memory_order_relaxed);
                tenant.controlBlock->pluginMaxWidth.store  (probedMaxWidth,  std::memory_order_relaxed);
                tenant.controlBlock->pluginMaxHeight.store (probedMaxHeight, std::memory_order_relaxed);

                logToFile ("CrateSandbox (Shared Host): tenant " + tenant.instanceId + " probed plugin resize limits — min="
                               + juce::String (probedMinWidth) + "x" + juce::String (probedMinHeight)
                               + ", max=" + juce::String (probedMaxWidth) + "x" + juce::String (probedMaxHeight) + ".");
            }
        }
        else
        {
            tenant.controlBlock->pluginMinWidth.store  (tenant.activeEditor->getWidth(),  std::memory_order_relaxed);
            tenant.controlBlock->pluginMinHeight.store (tenant.activeEditor->getHeight(), std::memory_order_relaxed);
            tenant.controlBlock->pluginMaxWidth.store  (tenant.activeEditor->getWidth(),  std::memory_order_relaxed);
            tenant.controlBlock->pluginMaxHeight.store (tenant.activeEditor->getHeight(), std::memory_order_relaxed);
        }

        // Step 79 (Pre-Emptive Native Parenting) directive: per-tenant
        // treatment — see the isolated path's own matching call site for
        // the full reasoning.
        auto* tenantHostSlotHwnd = (void*) (intptr_t) hostSlotHwndValue;
        tenant.activeEditor->addToDesktop (0, tenantHostSlotHwnd);

        // Step 80 (Geometry Snap) directive — REMOVED: per-tenant
        // treatment — see the isolated path's own matching comment for
        // why the getWidth()/getHeight() -> setSize() re-assertion this
        // step originally added here was provably inert.
        tenant.activeEditor->setVisible (true);

        // Step 81 (Event-Driven Geometry Publish) directive: per-tenant
        // treatment — see the isolated path's own matching call site and
        // TenantContext's own componentMovedOrResized() for the full
        // reasoning.
        tenant.activeEditor->addComponentListener (&tenant);

        if (auto* peer = tenant.activeEditor->getPeer())
        {
            tenant.controlBlock->windowHandleValue.store ((int64_t) (intptr_t) peer->getNativeHandle(), std::memory_order_relaxed);
            tenant.controlBlock->windowWidth.store (tenant.activeEditor->getWidth(), std::memory_order_relaxed);
            tenant.controlBlock->windowHeight.store (tenant.activeEditor->getHeight(), std::memory_order_relaxed);
            tenant.controlBlock->editorCanResize.store (tenantEditorCanResize, std::memory_order_relaxed);
            tenant.controlBlock->windowHandleReady.store (true, std::memory_order_release);
            logToFile ("CrateSandbox (Shared Host): tenant " + tenant.instanceId + " editor window created and handle published ("
                           + juce::String (tenant.activeEditor->getWidth()) + "x" + juce::String (tenant.activeEditor->getHeight())
                           + ", canResize=" + (tenantEditorCanResize ? juce::String ("true") : juce::String ("false")) + ").");

            // Step 24 (Editor View Recovery Guard) directive: see the
            // isolated path's matching comment for the full reasoning.
            tenant.initialEditorWidth  = tenant.activeEditor->getWidth();
            tenant.initialEditorHeight = tenant.activeEditor->getHeight();
            tenant.editorHasEverResized = false;
        }
        else
        {
            logToFile ("CrateSandbox (Shared Host): editor addToDesktop() succeeded but getPeer() returned nullptr for tenant " + tenant.instanceId);
            tenant.activeEditor.reset();
        }
    }

    // Step 9 directive: set once, at the top of initialise(), from this
    // process's own command line — see class doc comment.
    juce::String instanceId;

    // Step 33 (Cryosleep Architecture) directive: set alongside instanceId,
    // at the top of initialise() — see that method's own parsing comment.
    bool isCryosleeping = false;
    std::unique_ptr<CryosleepWaitThread> cryosleepWaitThread;

    std::unique_ptr<juce::MemoryMappedFile> sharedMemory;
    CrateIPC::ControlBlock* controlBlock = nullptr;
    std::unique_ptr<HeartbeatThread> heartbeatThread;
    std::unique_ptr<AudioBridgeThread> audioBridgeThread;
    std::unique_ptr<LookaheadWorkerThread> lookaheadWorkerThread;

    juce::AudioPluginFormatManager pluginFormatManager;
    std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;

    // RESTART LIVELOCK directive: the thread-safe "published" view of
    // hostedPlugin.get() — see loadHostedPlugin() and AudioBridgeThread's
    // own comments for why a pointer captured once at thread-construction
    // time isn't sufficient here.
    std::atomic<juce::AudioPluginInstance*> hostedPluginPtr { nullptr };

    // Step 10 directive: message-thread-owned (created/destroyed only from
    // timerCallback()/shutdown(), both message-thread calls) — no atomic
    // needed. See timerCallback()'s own comment for the full creation
    // sequence.
    std::unique_ptr<juce::AudioProcessorEditor> activeEditor;

    // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive: last scale
    // factor actually applied to activeEditor via setScaleFactor() — so
    // timerCallback() only calls it again when controlBlock->displayScale1000
    // genuinely changes, not on every tick. Starts at 1.0f, matching
    // ControlBlock::displayScale1000's own default (1000 = 100%).
    float lastAppliedDisplayScale = 1.0f;

    // Step 24 (Editor View Recovery Guard) directive: a real, evidence-
    // gathered MeldaProduction quirk — after a fast interactive resize
    // burst via the plugin's OWN corner grip (Step 23's mechanism, proven
    // correct: logged growing AND shrinking in tight lockstep with the
    // drag), MAutoDynamicEq's view was observed to silently snap back,
    // in one single tick, to EXACTLY its editor-creation-time native size
    // — not any size actually dragged to — and then never report another
    // size change again, permanently. That specific fingerprint (a hard
    // revert to the EXACT original creation size, after having genuinely
    // moved away from it) is what's checked for; a real user reproducing
    // that exact pixel size by chance while dragging is not a realistic
    // false-positive risk. Same resilience philosophy as the Step 21
    // Watchdog/Guillotine (detect the anomaly, recover automatically,
    // no visible crash) — just scoped to recreating the CORRUPTED VIEW
    // alone, not the whole sandboxed process, since only the plugin's UI
    // state is implicated, never its DSP/audio engine.
    int initialEditorWidth  = 0;
    int initialEditorHeight = 0;
    bool editorHasEverResized = false;

    // Step 55 (The Liar's Penalty) directive: latched TRUE, forever, the
    // moment the Recovery Guard catches this plugin ignoring an applied
    // resize (snapping back to creation size despite claiming
    // canResize=true — VoxDucker, proven via direct log evidence). Every
    // editor (re)creation checks this FIRST: when set, the probe is
    // skipped entirely and canResize=false + min==max==creation size are
    // published instead, regardless of anything the plugin's own lying
    // canResize()/checkSizeConstraint() claim. Never reset — a liar
    // caught once stays convicted for this process's whole lifetime (and
    // via ControlBlock::fixedSizeLiarDetected -> PluginHealthRegistry,
    // for all future sessions too).
    bool editorProvedFixedSizeLiar = false;

    // Step 57 (The Liar's Tolerance) directive: shared by BOTH the
    // isolated Recovery Guard check (above) and serviceTenant()'s
    // per-tenant version — a single named constant rather than a magic
    // number duplicated in two places. 15px, matching the user's own
    // directive: wide enough to absorb Melda-style internal grid
    // rounding (MAutoDynamicEq's own false-positive conviction, proven
    // via QA), narrow enough that a genuine liar (VoxDucker: probed max
    // 10000x10000, real resize 824x824, snapped back to EXACTLY 800x800
    // — nowhere close to a rounding error) still gets caught.
    static constexpr int recoveryGuardTolerancePixels = 15;

    // Step 35 (Editor View Recovery Guard v2) directive, Task 3: throttle
    // gate for the SendMessageTimeout responsiveness ping — see its own
    // call site's doc comment.
    juce::uint32 lastEditorHealthCheckMs = 0;

    // Step 69 (Watchdog Strike System) directive — QA finding: a single
    // SendMessageTimeout failure is not proof of a genuine hang. An
    // aggressive, continuous drag of the plugin's own internal resize
    // handle can legitimately keep a heavy UI recalculation (Melda's own
    // EQ-grid redraw, proven via this session's own logging) busy on this
    // SAME message thread for a stretch approaching the probe's own
    // 500ms timeout — busy, not dead. Requiring
    // editorHealthCheckStrikeThreshold CONSECUTIVE failures (at this
    // check's own 1-second cadence, several full seconds of genuine
    // unresponsiveness) before concluding "hung," and resetting the
    // count to 0 on the very next successful probe, is what actually
    // tells "busy" apart from "dead." A single shared constant — there's
    // nothing tenant-specific about how many strikes should be tolerated,
    // only the running count itself (see TenantContext's own
    // editorHealthCheckStrikes).
    static constexpr int editorHealthCheckStrikeThreshold = 3;
    int editorHealthCheckStrikes = 0;

    // Step 68 (Outbound Throttle & Lifecycle Integrity) directive, Task 1
    // — QA finding: rapidly, continuously dragging the plugin's own
    // internal resize handle republishes a new size to shared memory
    // every tick the mismatch condition holds. This class's own Timer
    // already runs at CrateIPC::livenessCheckIntervalMs (20ms/50Hz),
    // comfortably inside a safe band on paper, but a fixed nominal
    // interval alone isn't a real guarantee against the SAME wall-clock-
    // drift hazard already proven and fixed on the HOST side
    // (CrateSandboxBridge's own Step 66 follower throttle): if Melda's
    // own internal grid recalculation (triggered synchronously, on THIS
    // SAME message thread, by the plugin's native handle responding to
    // raw OS mouse-move messages) ever makes a single tick's own work run
    // long, this Timer has no idle gap left to wait out and the next tick
    // fires immediately back-to-back. A single shared constant (there's
    // nothing tenant-specific about WHAT rate is safe, only WHEN each
    // tenant last actually wrote — see TenantContext's own
    // lastOutboundReportMs) capped to ~30Hz, matching the Host's own
    // follower throttle so neither side of the pipe can flood the other.
    static constexpr int outboundReportThrottleMs = 33;
    juce::uint32 lastOutboundReportMs = 0;

    // Step 11 directive: guards EVERY call into hostedPlugin from either
    // AudioBridgeThread's processBlock() or StateExtractionThread's
    // getStateInformation()/setStateInformation() — a std::atomic_flag
    // spinlock, same idiom as CrateAnticipativeWrapper's own dspLock,
    // shared by reference with both threads (constructed here so it
    // outlives both).
    std::atomic_flag pluginAccessLock = ATOMIC_FLAG_INIT;

    // Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive: isolated-
    // mode counterpart to TenantContext::isCorrupted — shared by reference
    // into this process's own AudioBridgeThread and LookaheadWorkerThread.
    std::atomic<bool> isCorrupted { false };

    std::unique_ptr<StateExtractionThread> stateExtractionThread;
    std::unique_ptr<StateChangeListener> stateChangeListener;

    //==============================================================================
    // Step 15.2 (The Shared Host Engine) directive: this process is EITHER
    // running as a single isolated-mode plugin host (everything above this
    // point) OR as a Shared Sandbox multi-tenant host (everything below) —
    // never both; initialise() branches on CrateIPC::getSharedHostModeFlag()
    // before either path's own state is ever touched.
    bool runningAsSharedHost = false;

    std::unique_ptr<juce::MemoryMappedFile> commandChannelMemory;
    CrateIPC::SharedHostCommandBlock* commandBlock = nullptr;
    std::unique_ptr<CrateIPC::NamedEvent> commandReadyEvent;
    std::unique_ptr<CommandListenerThread> commandListenerThread;

    // Message-thread-owned (only ever touched from spawnTenant(), called via
    // MessageManager::callAsync() off CommandListenerThread, and from
    // timerCallback()/shutdown(), both already message-thread-only) — no
    // extra locking needed, same reasoning as activeEditor above.
    std::unordered_map<juce::String, std::unique_ptr<TenantContext>> tenants;
};

START_JUCE_APPLICATION (CrateSandboxApplication)
