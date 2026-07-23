#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>
#include <tracktion_engine/playback/graph/tracktion_TracktionEngineNode.h>
#include <tracktion_engine/playback/graph/tracktion_WaveNode.h>
#include <tracktion_engine/playback/graph/tracktion_TracktionNodePlayer.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "CrateIPCConstants.h"
#include "PluginHealthRegistry.h"

#include <cstring>
#include <cmath>
#include <functional>
#include <vector>

#if JUCE_INTEL
 #include <immintrin.h>
#endif

namespace te = tracktion::engine;
namespace tcore = tracktion::core;
namespace tgraph = tracktion::graph;

/**
    Plugin Sandboxing Step 5 (The Headless IPC Host Skeleton) — the PARENT
    side of the structural bridge to CrateSandbox.exe.

    STEP 5.5 (Process Management & The Atomic Heartbeat) turns that
    skeleton into something that can actually be trusted before any real
    audio crosses it. A file-backed juce::MemoryMappedFile proves the
    STORAGE bridge works, but says nothing about whether the process on the
    other end is still alive — a crashed or hung sandbox leaves the exact
    same bytes sitting there as a healthy one, which is the "ghost" this
    step exists to detect.

    1. PROCESS LIFECYCLE OWNERSHIP — juce::ChildProcess launches
       CrateSandbox.exe (CrateIPC::resolveSandboxExecutable()) from
       initialise(), and is killed from deinitialise() and the destructor
       (belt-and-suspenders, same convention CrateAnticipativeWrapper's own
       destructor uses for its worker thread). The DAW owns this process's
       entire lifetime — it never outlives the plugin that launched it.

    2. THE MEMORY-MAPPED CONTROL BLOCK — CrateIPC::ControlBlock (see
       CrateIPCConstants.h) is the fixed layout laid over the first bytes of
       the mapped file both processes agree on. This class only ever
       reinterpret_casts an ALREADY-CONSTRUCTED block (CrateSandbox is the
       one that placement-news it) — attaching is attempted once right
       after launch and then retried every timer tick until it succeeds.

    3. THE HEALTH CHECK — a private juce::Timer (livenessCheckIntervalMs,
       comfortably inside heartbeatTimeoutMs) is the ONLY thing that ever
       reads heartbeatCounter or does the elapsed-time comparison — the
       audio thread's own applyToBuffer() only ever reads the cheap,
       lock-free sandboxAlive bool this timer publishes, exactly the same
       "expensive check off the audio thread, audio thread reads a cached
       flag" contract CrateAnticipativeWrapper's liveModeRequired/
       refreshLiveModeState() already established elsewhere in this
       codebase. A stall past heartbeatTimeoutMs immediately clears
       sandboxAlive and triggers exactly ONE relaunch attempt per stall —
       restartInFlight guards against spawning a new child process on every
       single 20ms tick for as long as the sandbox stays dead, and clears
       again the instant a fresh heartbeat proves the new child is alive.
       IMPORTANT (caught during Step 5.5's own live test): sandboxAlive is
       NEVER set true merely because attachSharedMemory() succeeded — a
       stale leftover file from an already-dead process maps just as
       successfully as a genuinely live one. It only flips true once
       timerCallback() observes the counter actually CHANGE relative to the
       baseline captured at attach time — real proof of a pulse.

    STEP 6 (The IPC Audio Arteries / Echo-Phase Test) adds the actual audio
    round trip, still with no real plugin hosted in the sandbox yet — that's
    later, once this lock-free transfer itself is proven end to end:

    THE PARENT PUSH & SPIN-WAIT (applyToBuffer(), the AUDIO thread) — if
    sandboxAlive is false, silence and return immediately, exactly as
    before. Otherwise: copy the incoming block into
    ControlBlock::audioInput, publish numChannels/numSamples, set
    parentReady, and BUSY-WAIT (Thread::yield(), not a mutex/condition
    variable — the audio thread must never be suspended by the OS scheduler
    waiting on another PROCESS, the same "no blocking on the audio thread"
    contract CrateAnticipativeWrapper's dspLock spinlock already established
    for a same-process race) for childProcessed, capped at
    CrateIPC::spinWaitTimeoutMs (3ms). On success: copy audioOutput back and
    clear both flags. On timeout: silence, and — since actually LOGGING a
    DSP stall means file I/O, which is exactly as forbidden on the audio
    thread as blocking is — publish dspStallDetected (a plain atomic<bool>)
    instead of calling logEvent() directly. timerCallback() (the message
    thread) is what actually logs it and forces an immediate restart,
    exactly the same "audio thread publishes a cheap flag, message thread
    does the expensive part" contract used everywhere else in this class.

    THE CHILD DSP LOOP (CrateSandbox's AudioBridgeThread, see
    Source/Sandbox/Main.cpp) spins on parentReady and, when set, inverts
    every sample's phase (multiply by -1.0f) into audioOutput before
    signalling childProcessed. Phase inversion rather than a passthrough is
    the deliberate proof: a passthrough could be an accidental no-op hiding
    behind a green checkmark; a bit-exact inverted waveform can only be
    produced by code that genuinely executed out-of-process.

    A NEW DATA RACE THIS STEP INTRODUCES AND CLOSES: Step 5.5's controlBlock
    was a plain (non-atomic) pointer, safe ONLY because nothing on the audio
    thread ever read it. Step 6's applyToBuffer() is the first audio-thread
    code to dereference it, while timerCallback() (message thread) still
    reassigns it on every attach/restart — a genuine torn-pointer race, not
    a hypothetical one. Fixed by making controlBlock a
    std::atomic<ControlBlock*>, loaded with acquire ordering on the audio
    thread and stored with release ordering wherever the message thread
    changes it, plus a defensive null-check after the load (sandboxAlive can
    be observed stale-true for at most one more callback while a restart is
    in flight).

    Diagnostics: every state transition is appended to a plain text file
    (%TEMP%/CrateSandboxBridge.log) via logEvent() — see this codebase's own
    established reasoning (CrateStressTest.cpp's NativeMessageBox comment)
    for why DBG()/Logger output alone isn't good enough here, and why a
    message box isn't appropriate for events that fire asynchronously with
    no user interaction to hang a dialog off of.

    STEP 6.5 (The Hybrid Sync Pivot) replaced the CHILD's parentReady-polling
    loop with a genuine OS-native block (CrateIPC::NamedEvent — see
    CrateIPCConstants.h) after Step 6 measured it at 93.6% of a full CPU
    core, constantly, regardless of whether any audio was flowing. This
    class's OWN half of the contract does NOT change to match: the PARENT
    is the real-time audio thread and must NEVER sleep or block on an OS
    wait primitive — bufferReadyEvent.signal() is a fast, non-blocking
    kernel call (safe here), but applyToBuffer() still busy-waits on
    childProcessed exactly as Step 6 built it, for exactly the same reason
    dspLock in CrateAnticipativeWrapper is a spinlock and not a mutex. The
    asymmetry is deliberate: the child can afford to block (nothing else to
    do while idle); the parent never can.

    THE UNMAP RACE (caught live under Step 6.5's own test — crashed the app,
    confirmed via Windows Event Log as a 0xc0000005 access violation): a
    restart's terminateSandboxProcess() runs on the MESSAGE thread and calls
    mappedMemory.reset(), which can unmap the shared memory at the exact
    instant applyToBuffer() (AUDIO thread) is mid-dereference of it — loading
    controlBlock with acquire ordering only proves the POINTER VALUE is
    current, it does nothing to stop a concurrent unmap of what it points
    to. Fixed with audioThreadActive: set true immediately before the load,
    false only after every last dereference of the block is done;
    terminateSandboxProcess() busy-waits (message thread can afford to; the
    audio thread never does) for that flag to clear before it EVER calls
    mappedMemory.reset(). See applyToBuffer() and terminateSandboxProcess()
    for the full sequence.

    STEP 7 (The VST3 Host Engine) adds real third-party DSP inside the
    sandbox, replacing Step 6's phase-inversion proof — the round trip
    mechanics (spin-wait, hybrid sync, unmap-race guard) are UNCHANGED,
    only WHAT the child does with the samples differs now.

    THE INITIALIZATION PAYLOAD: pluginPath/hostSampleRate/hostBlockSize (see
    CrateIPC::ControlBlock) are write-once-BEFORE-launch config, not live
    per-buffer data. This class now creates/sizes the shared file and maps
    it itself (CrateIPC::ensureSharedMemoryFileIsSized() + attachSharedMemory())
    BEFORE calling sandboxProcess.start() — a deliberate reordering from
    Step 5, where only the CHILD ever created the file. Writing the payload
    into an ALREADY-MAPPED block before the child process even exists
    eliminates any race about who gets there first; the child's own
    placement-new (Source/Sandbox/Main.cpp) explicitly preserves these three
    fields across its per-launch reset rather than wiping them.

    THIS STEP'S TEST HARDCODE: launchSandboxProcess() below writes a fixed,
    hardcoded VST3 path ("Rift Filter Lite.vst3", a lightweight filter
    already installed on this machine) — per the step's own directive ("To
    test, hardcode the Parent to load a recognizable plugin"). Real plugin
    selection (chosen by the user, routed per-track) is later work, once
    real VST3 hosting itself is proven end to end.

    STEP 8 (Real-Time Parameter Automation) adds a SECOND, independent
    lock-free SPSC ring buffer (CrateIPC::ControlBlock::paramQueue — see its
    own doc comment for why moodycamel's queue can't be reused here)
    alongside the audio round-trip handshake. setParameterEvent() (callable
    from the AUDIO thread, same real-time contract as applyToBuffer() itself)
    pushes one ParamChange; a full queue drops the event and publishes
    paramQueueOverflowDetected rather than logging directly or blocking,
    exactly the same "audio thread publishes a flag, message thread does the
    expensive part" contract dspStallDetected already established. The CHILD
    (AudioBridgeThread, Source/Sandbox/Main.cpp) drains this queue completely
    every time it wakes, strictly BEFORE calling processBlock() — so a whole
    burst of automation events all land before the very next block that
    should audibly reflect them, not smeared across several blocks.

    THIS STEP'S TEST HARDCODE: applyToBuffer() below also computes a
    continuous sine sweep and pushes it as a parameter event on Rift Filter
    Lite's own Filter Cutoff parameter (index confirmed via the plugin's own
    parameter list, logged once by CrateSandbox on load) every single audio
    block — per the step's own directive ("hardcode the Parent to rapidly
    sweep a known parameter... via a sine wave calculation inside
    applyToBuffer"). Real automation (driven by the Edit's own automation
    curves, any parameter, any plugin) is later work.

    STEP 9 (The Multi-Process Scalability Stress Test) directive — REQUIRED
    ARCHITECTURE FIX, not optional: every prior step's shared memory file
    and wake event used a FIXED, GLOBAL name (CrateIPC_Memory.shm,
    CrateIPC_BufferReady_Event), correct only because exactly one
    CrateSandboxBridge/CrateSandbox pair ever existed at a time — Step 6.5's
    own doc comment already flagged this as a placeholder. Step 9 runs 50
    CONCURRENT pairs, which would otherwise all map the SAME 4MB block and
    block on the SAME event object — 50 processes corrupting one
    ControlBlock's heartbeat/audio/parameters, and a single SetEvent()
    waking an arbitrary one of 50 waiting children rather than the specific
    one the signalling parent intended. instanceId (below) is a
    juce::Uuid generated ONCE per CrateSandboxBridge, at construction —
    every CrateIPC::getSharedMemoryFile()/getBufferReadyEventName() call
    site in this class now takes it, and it's passed to this instance's own
    CrateSandbox.exe as a command-line argument in launchSandboxProcess()
    (Source/Sandbox/Main.cpp reads it back out of its own initialise()).
    Declared FIRST among this class's data members, deliberately — C++
    initializes members in DECLARATION order regardless of the constructor's
    own initializer-list order, and bufferReadyEvent's own initializer
    depends on instanceId already having its value.

    STEP 10 (Cross-Process Window Reparenting) directive: requestEditorWindow()/
    isEditorWindowReady()/getEditorWindowHandle() are a THIRD, independent
    command channel over the SAME shared ControlBlock (see its own doc
    comment on windowHandleRequested/windowHandleReady/windowHandleValue) —
    GUI-speed, not audio-speed, so plain acquire/release loads are more than
    sufficient; these are message-thread-only calls, never the audio thread.
    The CHILD (Source/Sandbox/Main.cpp) creates the VST3's real editor via
    createEditorAndMakeActive(), forces a genuine top-level native
    ComponentPeer via addToDesktop(0), and publishes its raw HWND.

    Once a handle is available, embedding it is entirely the CALLER's
    responsibility via juce::HWNDComponent::setHWND() — this class only
    ever hands back the raw value, it doesn't own or create any Component
    itself (a te::Plugin has no natural "I have a window" concept of its
    own; the debug test harness, not this class, owns the actual
    juce::HWNDComponent — see MainComponent's own hidden hotkey).
    HWNDComponent's own Pimpl (verified against modules/JUCE) converts the
    target window's style from WS_POPUP to WS_CHILD and calls Win32's
    SetParent() — genuine OS-level reparenting, not a screen-scrape or
    pixel stream, which is exactly why THE GOLDEN RULE (complete
    decoupling) holds for free: the embedded surface is composited by
    Windows' own Desktop Window Manager independent of whether the CHILD's
    message loop is currently responsive.

    STEP 11 (Absolute Muscle Memory / Continuous State Sync) directive:
    timerCallback() (already running at livenessCheckIntervalMs) also polls
    a FOURTH command channel — the PUSH channel (ControlBlock::stateChunkData/
    stateChunkSize/stateChunkAvailable, guarded by stateChunkLock, a
    std::atomic_flag spinlock rather than a lock-free ring buffer, since
    only the LATEST state ever matters — an in-flight chunk the parent
    hasn't consumed yet is simply superseded by a newer one, never queued).
    Received bytes land in lastKnownState. launchSandboxProcess() writes
    lastKnownState into the INITIAL-LOAD channel (initialStateData/
    initialStateSize) on EVERY launch, including a restart triggered by
    this exact bridge's own heartbeat-timeout/DSP-stall detection — so a
    freshly spawned child, on a completely different OS process, resurrects
    with the exact plugin state the dead one had a moment before. The CHILD
    side (Source/Sandbox/Main.cpp) does the actual VST3 IComponentHandler
    hook (via JUCE's own AudioProcessorListener abstraction) and background-
    thread extraction — this class only ever receives/retains/re-injects
    the resulting bytes, never touches the plugin's state APIs directly.
*/
class CrateSandboxBridge : public te::Plugin,
                            private juce::Timer
{
public:
    explicit CrateSandboxBridge (te::PluginCreationInfo info) : te::Plugin (info)
    {
        buildStubParameterList();
    }

    ~CrateSandboxBridge() override
    {
        notifyListenersOfDeletion(); // same first-line-of-destructor convention every other te::Plugin uses

        stopTimer();

        // Step 22 (Re-entrancy Safety) directive: by the time this
        // destructor's body is even REACHABLE, lookaheadProducerThread's
        // own run() loop is GUARANTEED to have already fully, cooperatively
        // exited — see LookaheadProducerThread's own doc comment for the
        // te::Plugin::Ptr keep-alive mechanism that makes this an actual
        // invariant, not a hope: as long as that thread was running, it
        // held a reference to `this`, and te::Plugin derives directly from
        // juce::ReferenceCountedObject, so this destructor could not have
        // started while that reference was still held. This call is
        // therefore a fast, defensive confirmation, not a real wait — it
        // should never need more than a moment, and critically should
        // never need to fall through to a force-kill. If it somehow does,
        // that's the invariant above being violated, not a hang to paper
        // over — logged loudly rather than silently tolerated.
        if (lookaheadProducerThread != nullptr)
        {
            if (! lookaheadProducerThread->stopThread (2000))
                logEvent ("~CrateSandboxBridge(): lookaheadProducerThread did not confirm stopped promptly — "
                              "the keep-alive invariant this relies on may have been violated. Investigate "
                              "before trusting this build's re-entrancy safety.");
        }

        terminateSandboxProcess();
    }

    //==============================================================================
    static const char* getPluginName()   { return NEEDS_TRANS ("Sandbox Bridge (Isolated Mode)"); }
    static const char* xmlTypeName;

    // Step 14 (The Crate Brain) directive: the ONE hardcoded plugin file
    // this whole system currently ever loads — real plugin selection is
    // still later work (see this class's own doc comment) — hoisted to a
    // static accessor so SandboxManager can key its UID cache lookup on
    // the exact same string this class launches, without either file
    // duplicating the literal or reaching into the other's private state.
    // Step 23 (Geometry Polish & Dynamic Resize) directive: TEMPORARILY
    // swapped back to CrateBulletproofTest — now genuinely resizable (see
    // its own PluginEditor.cpp) — specifically so the resize-follow
    // verification test has a real, draggable corner grip to test against.
    // See this method's own git history for the Step 22 Rule A/B routing
    // demonstrations (CrateBulletproofTest for Rule A, Rift Filter Lite
    // for Rule B) that preceded this.
    static juce::String getTestPluginPath() { return "C:\\Users\\User\\Desktop\\The Crate Studio\\build\\Source\\TestPlugins\\CrateBulletproofTest\\CrateBulletproofTest_artefacts\\Debug\\VST3\\Crate Bulletproof Test.vst3"; }

    // Step 14 directive: a thin public passthrough to the private
    // logEvent() — SandboxManager's routing verdicts belong in the exact
    // same timeline/file every previous step's own verification already
    // reads, rather than a second, disconnected log file.
    static void logToSharedLog (const juce::String& message) { logEvent (message); }

    // Step 15.2 (The Shared Host Engine) directive: called by SandboxManager
    // on a freshly-constructed bridge, BEFORE it's inserted into a track
    // (i.e. strictly before initialise() ever runs) — turns this bridge
    // into a "Tenant Bridge": it will ATTACH to an already-agreed-upon
    // instanceID's shared memory rather than generating its own and
    // spawning a private CrateSandbox.exe for it. dispatchCallback is
    // invoked exactly once, from initialise() (via attachAsTenant()), with
    // (pluginPath, instanceId) — this is how a Tenant Bridge tells
    // SandboxManager "my shared memory is prepared and waiting, go tell the
    // shared host to actually load into it" at the ONE moment
    // sampleRate/blockSize are finally known (te::Plugin::initialise()),
    // without CrateSandboxBridge needing to know SandboxManager's type at
    // all (avoids a header circular-include between this class and the one
    // that constructs it).
    // Step 15.3 (Self-Healing & Auto-Quarantine) directive: rerouteCallback
    // is invoked from THIS bridge's own death-detection (the exact same
    // heartbeat-timeout/DSP-stall branches isolated mode already uses to
    // trigger its own restart — see timerCallback()'s own updated comments)
    // — a tenant's own detection of a stalled/dead ControlBlock IS "the
    // shared host died" from this bridge's point of view: there is no way
    // for a single tenant to distinguish "the whole shared host process
    // crashed" from "just my own hosted plugin instance crashed badly
    // enough to take the process with it," and it doesn't need to —
    // Guilt by Association means every tenant sharing that host gets
    // blamed identically either way, which is exactly what already
    // happens here since every tenant bridge polls its OWN ControlBlock
    // independently and will ALL notice within one tick of each other. A
    // second, centralized "SandboxManager watches the process and
    // broadcasts to survivors" mechanism was deliberately NOT built
    // alongside this — it would only race against and potentially
    // double-count what this already-proven per-tenant detection provides
    // for free.
    // Step 15.4 (The Teardown Protocol) directive: removedCallback is
    // invoked exactly once, from deinitialise(), the moment this bridge
    // stops being a tenant of ANY kind — i.e. the plugin was actually
    // removed/unloaded, not merely mode-converted (convertToIsolatedAndRelaunch()
    // sets isTenantMode false BEFORE that would ever matter, so an isolated
    // conversion never fires this). This is how the shared host finds out
    // it's safe to free this tenant's RAM instead of leaking it forever.
    void configureAsTenant (const juce::String& externalInstanceId, const juce::String& pluginPathToLoad,
                            std::function<void (const juce::String& pluginPath, const juce::String& instanceId)> dispatchCallback,
                            std::function<void (CrateSandboxBridge&)> rerouteCallback,
                            std::function<void (const juce::String& instanceId)> removedCallback)
    {
        isTenantMode = true;
        instanceId = externalInstanceId;
        pluginPathForLaunch = pluginPathToLoad;
        onTenantReady = std::move (dispatchCallback);
        onNeedsRerouting = std::move (rerouteCallback);
        onTenantRemoved = std::move (removedCallback);
    }

    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // called by SandboxManager BEFORE this bridge is ever inserted into a
    // track — same "configure now, actually act later from initialise()"
    // shape as configureAsTenant() above, for the same reason (sampleRate/
    // blockSize aren't known until initialise() fires). poolInstanceId is
    // an ALREADY-RUNNING, cryosleeping process's own instanceId (its shared
    // memory file already created/sized, its process already launched,
    // idle, at ~0% CPU) — adopting it here means claimFromPool() only ever
    // has to WRITE into an existing mapping and signal one event, never
    // spawn a new OS process. releaseCallback lets SandboxManager reclaim
    // its OWN juce::ChildProcess handle for this instanceId when this
    // bridge is eventually torn down — see terminateSandboxProcess()'s own
    // comment for why this bridge can't just kill it directly (this
    // bridge's own sandboxProcess member was never the one that launched
    // it; SandboxManager's pool was).
    void configureFromPool (const juce::String& poolInstanceId, std::function<void()> releaseCallback)
    {
        isPooledClaim = true;
        instanceId = poolInstanceId;
        onPoolProcessRelease = std::move (releaseCallback);
    }

    // Step 15.3 directive: SandboxManager's Quarantine Shuffle needs to
    // know which plugin FILE this tenant is actually running to re-check
    // its crash history — it can't reach into this bridge's private
    // pluginPathForLaunch member directly.
    juce::String getConfiguredPluginPath() const { return pluginPathForLaunch; }

    // Step 27 (Real Sandbox Wiring) directive — a real, previously-latent
    // bug this step is the first to expose: pluginPathForLaunch's own
    // default member initializer is getTestPluginPath() (a hardcoded test
    // path), and configureAsTenant() above is the ONLY place that ever
    // overwrites it — for the SHARED/tenant route only. The
    // SOLITARY/isolated route in SandboxManager::createSandboxPlugin() had
    // no equivalent call, so an isolated-mode bridge silently launched
    // whatever getTestPluginPath() returned, ignoring the real requested
    // path entirely. Every manual test through Step 26 happened to pass
    // getTestPluginPath() itself as that argument, which is why this never
    // surfaced before now. This setter is what SandboxManager calls for the
    // isolated branch to close that gap.
    void setPluginPathForIsolatedLaunch (const juce::String& path) { pluginPathForLaunch = path; }

    // Step 27 (Real Sandbox Wiring) directive: the React frontend needs a
    // stable identifier for each loaded plugin instance to key its own
    // device-chain list on — this IS that identifier, unconditionally set
    // for every bridge (see instanceId's own default member initializer),
    // isolated or tenant alike, so this accessor works the same regardless
    // of which SandboxRouter verdict a given plugin got.
    juce::String getInstanceId() const { return instanceId; }

    // Step 15.3 directive (Quarantine Shuffle, innocent-tenant path): called
    // by SandboxManager's rerouteCallback when this tenant's crash count is
    // still below the Solitary Confinement threshold — re-attaches to a
    // FRESH per-tenant slot (a NEW instanceId; the old one belonged to a
    // shared host that no longer exists) inside whatever Shared Sandbox
    // host is running after this call (a fresh one, if the old one is what
    // just died — dispatchSpawnCommand()'s own ensureSharedHostRunning()
    // call handles that transparently). lastKnownState is untouched by any
    // of this — it lives on THIS bridge object, independent of transport
    // mode, so the Ghost Reload it feeds is unaffected by which kind of
    // child ends up reading it.
    void reconfigureAsTenant (const juce::String& freshInstanceId,
                              std::function<void (const juce::String& pluginPath, const juce::String& instanceId)> dispatchCallback)
    {
        instanceId = freshInstanceId;
        onTenantReady = std::move (dispatchCallback);
        attachAsTenant();
    }

    // Step 15.3 directive (Quarantine Shuffle, guilty-tenant path): called
    // by SandboxManager's rerouteCallback the moment this tenant's crash
    // count crosses requiresSolitaryConfinement()'s threshold — permanently
    // converts this bridge from a Tenant Bridge into a normal isolated one.
    // A fresh instanceId (the old tenant slot belonged to a dead shared
    // host and means nothing anymore); launchSandboxProcess() handles
    // everything else identically to any other isolated bridge's first
    // launch, INCLUDING writing lastKnownState into the initial-load
    // channel — the exact same Ghost Reload mechanism Step 11 built,
    // which is what makes "preserving state across the reroute" true for
    // free rather than needing any special-casing here.
    void convertToIsolatedAndRelaunch()
    {
        isTenantMode = false;
        onTenantReady = nullptr;
        onNeedsRerouting = nullptr; // isolated mode never asks to be re-routed again — see the two timerCallback() branches
        onTenantRemoved = nullptr;  // isolated mode's own RAM is reclaimed by killing its process, not an unload command — see deinitialise()
        instanceId = juce::Uuid().toString();
        launchSandboxProcess();
    }

    // Step 30 (Completing the Proxy Illusion) directive: the user must
    // NEVER see "Sandbox Bridge" anywhere — this bridge impersonates the
    // REAL target plugin's identity everywhere the UI queries it.
    // getPluginType() is DELIBERATELY left returning xmlTypeName below —
    // that's TE's own internal serialization type tag (what
    // PluginCache::createNewPlugin looks up on project reload), not
    // anything ever shown to the user (confirmed: zero callers of
    // getPluginType() anywhere in this app's own UI code). Changing it
    // would silently break save/reload, not improve the illusion.
    void setImpersonatedDescription (const juce::PluginDescription& desc) { impersonatedDescription = desc; }

    juce::String getName() const override
    {
        return impersonatedDescription.name.isNotEmpty() ? impersonatedDescription.name : TRANS ("Sandbox Bridge");
    }

    juce::String getPluginType() override { return xmlTypeName; }

    juce::String getVendor() override
    {
        return impersonatedDescription.manufacturerName.isNotEmpty() ? impersonatedDescription.manufacturerName
                                                                       : te::Plugin::getVendor();
    }

    juce::String getSelectableDescription() override
    {
        return impersonatedDescription.pluginFormatName.isNotEmpty()
                   ? impersonatedDescription.pluginFormatName + " Plugin"
                   : TRANS ("Isolated Mode Sandbox Bridge Plugin");
    }

    // Health Check directive: true only while the CHILD's own heartbeat has
    // changed within the last heartbeatTimeoutMs, as observed by
    // timerCallback() — this is what the stress test's own verification
    // reads (see CrateStressTest.cpp), and what applyToBuffer() below reads
    // on the audio thread.
    bool isConnected() const noexcept { return sandboxAlive.load (std::memory_order_acquire); }

    // Step 9 (The Multi-Process Scalability Stress Test) directive: the
    // worst (longest) round trip applyToBuffer() has observed since the
    // last reset — updated on EVERY real audio block via a lock-free
    // running-max (a CAS retry loop, not a mutex — still audio-thread-safe),
    // not just runEchoPhaseTest()'s one synthetic call. This is what lets
    // the scale test measure "did child #50 lag behind child #1" across a
    // sustained real window instead of a single snapshot.
    double getMaxRoundTripMsObserved() const noexcept { return maxRoundTripMsObserved.load (std::memory_order_relaxed); }
    void resetMaxRoundTripMsObserved() noexcept { maxRoundTripMsObserved.store (0.0, std::memory_order_relaxed); }

    // Step 10 (Cross-Process Window Reparenting) directive: asks the CHILD
    // to create its VST3's editor and publish its native window handle.
    // Message-thread-only (this is a GUI-adjacent request, not a per-block
    // audio operation) — no-ops harmlessly if no sandbox is attached yet.
    void requestEditorWindow()
    {
        // Step 15.3 hotfix directive: remembered on the BRIDGE itself, not
        // just written into the current ControlBlock — see
        // reRequestEditorWindowIfNeeded()'s own doc comment for why the
        // block-only version of this flag silently stopped working the
        // moment a reconnect started using a FRESH instanceId (a fresh
        // file) instead of reusing the same one.
        editorWindowWasRequested = true;

        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->windowHandleRequested.store (true, std::memory_order_release);
    }

private:
    // Step 30 (Completing the Proxy Illusion) directive, Task 2: STUB
    // parameters — proved UniversalDeviceChainComponent's own
    // getConfiguredParams()/MiniParamSlider rendering pipeline actually
    // populates the Device Chain, before real IPC sync existed. Still
    // called at construction (immediate visual feedback before any IPC
    // connection exists at all), but now genuinely temporary:
    // buildRealParameterListFromMetadata() below calls clearParameterList()
    // and replaces these with the REAL parameter list the moment the CHILD
    // publishes it (Step 31) — these are visible for at most the few
    // hundred ms between insert and the child connecting.
    void buildStubParameterList()
    {
        addParam ("crateStub1", "Param 1", { 0.0f, 1.0f });
        addParam ("crateStub2", "Param 2", { 0.0f, 1.0f });
        addParam ("crateStub3", "Param 3", { 0.0f, 1.0f });
    }

    // Step 31 (Real IPC Parameter Sync) directive: the REAL replacement —
    // one genuine te::AutomatableParameter PER actual VST3 parameter,
    // dynamically built from the metadata the CHILD published (see
    // ControlBlock::paramMetadataReady's own doc comment). Overrides
    // parameterChanged (float, bool) — a virtual no-op by default (see
    // tracktion_AutomatableParameter.h) — to push DAW-originated changes
    // into the EXISTING per-block ParamChange ring buffer (Step 8) via
    // bridge.setParameterEvent(), the same channel real-time automation
    // already uses. applyingReadback guards the OTHER direction: when the
    // CHILD reports the user changed a value INSIDE the reparented native
    // UI, applyReadbackValue() must update this parameter's own state
    // without re-pushing that same value straight back to the CHILD (an
    // infinite-echo risk, not just wasted IPC traffic, if left unguarded).
    struct CrateSyncedParameter : public te::AutomatableParameter
    {
        CrateSyncedParameter (const juce::String& paramID, const juce::String& name,
                              CrateSandboxBridge& bridgeToUse, int index)
            : te::AutomatableParameter (paramID, name, bridgeToUse, { 0.0f, 1.0f }),
              bridge (bridgeToUse), paramIndex (index)
        {
        }

        void parameterChanged (float newValue, bool /*byAutomation*/) override
        {
            if (! applyingReadback)
                bridge.setParameterEvent (paramIndex, newValue);
        }

        // Called ONLY from the readback path (CrateSandboxBridge::timerCallback()'s
        // own paramValueRevision check) — never from a normal DAW-side
        // automation/UI move, which goes through the base class's own
        // setParameter()/setNormalisedParameter() instead.
        void applyReadbackValue (float newValue)
        {
            applyingReadback = true;
            setParameter (newValue, juce::dontSendNotification);
            applyingReadback = false;
        }

        CrateSandboxBridge& bridge;
        int paramIndex;
        bool applyingReadback = false;
    };

    // Called once, the first tick paramMetadataReady is observed true (see
    // timerCallback()'s own call site). realParameterListBuilt guards
    // against ever running twice — this is a one-time replacement, not a
    // repeating rebuild (a plugin's own parameter COUNT/identity doesn't
    // change after load; only individual VALUES do, which the readback
    // path above already handles continuously).
    void buildRealParameterListFromMetadata (CrateIPC::ControlBlock& block)
    {
        realParameterListBuilt = true;

        clearParameterList(); // drops Step 30's stub parameters
        syncedParameters.clear();

        const int count = block.paramCount.load (std::memory_order_relaxed);

        for (int i = 0; i < count; ++i)
        {
            auto name = juce::String (juce::CharPointer_UTF8 (block.paramMetadata[i].name));

            if (name.isEmpty())
                name = "Param " + juce::String (i);

            auto* param = new CrateSyncedParameter (juce::String (i), name, *this, i);
            addAutomatableParameter (*param);
            syncedParameters.push_back (param);

            // Seeds this parameter with whatever the CHILD already reported
            // (its real current/default value) — via applyReadbackValue(),
            // so this does NOT echo straight back to the CHILD over IPC
            // (it already told us this value; there's nothing to push).
            param->applyReadbackValue (block.paramCurrentValues[i].load (std::memory_order_relaxed));
        }

        edit.pluginChanged (*this); // forces the Device Chain to re-query getAutomatableParameters() and actually show the real list

        logEvent ("Built REAL parameter list from IPC metadata: " + juce::String (count)
                      + " parameter(s) — replacing Step 30's stub list.");
    }

    // Step 15.3 hotfix directive (GUI Reconnect After Reroute): the REAL
    // bug behind "the plugins stayed completely dead in the UI" after a
    // Quarantine Shuffle — NOT a thread deadlock. watchBridgeForReconnect()
    // in MainComponent.cpp explicitly documents relying on the CHILD
    // preserving windowHandleRequested across ITS OWN reset (true for an
    // isolated restart, which reuses the SAME instanceId/file — see Step
    // 11's own preserve-across-reset dance in Source/Sandbox/Main.cpp).
    // convertToIsolatedAndRelaunch() and reconfigureAsTenant() both
    // deliberately generate a FRESH instanceId (a brand-new, never-before-
    // touched file — see their own doc comments on why reusing a dead
    // tenant's slot would be meaningless), which means there is NOTHING
    // for that preserve-dance to preserve FROM: the new file starts
    // genuinely blank, windowHandleRequested and all. Nobody was re-
    // arming it, so the plugin's editor was never recreated, and the OLD
    // embedded HWND just sat there forever showing its last composited
    // frame — Windows' own Desktop Window Manager keeps painting a dead
    // window exactly as Step 10's own doc comment describes for a HUNG
    // process, except this was a REPLACED one with nothing telling its
    // replacement to ever publish a handle at all.
    //
    // Called at the end of EVERY (re)connection path — first isolated
    // launch, isolated restart, first tenant attach, and both Quarantine
    // Shuffle reroute branches — so "a window was ever wanted" is honored
    // uniformly regardless of which kind of reconnect just happened. A
    // no-op on a bridge whose caller never called requestEditorWindow()
    // in the first place (the common case for headless/audio-only test
    // paths).
    void reRequestEditorWindowIfNeeded()
    {
        if (editorWindowWasRequested)
            requestEditorWindow();
    }

public:

    bool isEditorWindowReady() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            return block->windowHandleReady.load (std::memory_order_acquire);
        return false;
    }

    // Returns the raw HWND once isEditorWindowReady() is true (matching
    // juce::HWNDComponent::setHWND()'s own void* signature); nullptr
    // otherwise. This class never embeds it itself — see class doc comment.
    void* getEditorWindowHandle() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return (void*) (intptr_t) block->windowHandleValue.load (std::memory_order_acquire);
        return nullptr;
    }

    // Geometry Sync directive (Step 10.1): the plugin editor's EXACT native
    // size, as published directly by the CHILD (its own
    // AudioProcessorEditor::getWidth()/getHeight()) — not re-derived here
    // via GetWindowRect() after reparenting. Both return 0 if the window
    // isn't ready yet.
    int getEditorWindowWidth() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->windowWidth.load (std::memory_order_relaxed);
        return 0;
    }

    int getEditorWindowHeight() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->windowHeight.load (std::memory_order_relaxed);
        return 0;
    }

    // Step 36 (The Fixed-Size Lock) directive: the CHILD's own honest
    // IPlugView::canResize() answer, published once alongside the handle
    // (see ControlBlock::editorCanResize's own doc comment). Defaults to
    // true (matching the ControlBlock field's own optimistic default) for
    // the brief window before windowHandleReady lands, same "assume the
    // common case until told otherwise" convention as everything else this
    // class exposes before the CHILD has actually connected.
    bool getEditorCanResize() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->editorCanResize.load (std::memory_order_relaxed);
        return true;
    }

    // Step 29 (Native Sandbox Interception) directive: the "Trojan Horse"
    // proxy — wraps requestEditorWindow()/getEditorWindowHandle()/Width()/
    // Height() (already proven this session by SandboxEditorTestWindow's
    // own standalone test-harness reparenting) inside a real
    // te::Plugin::EditorComponent, so the EXISTING native PluginWindow
    // (Source/UI/PluginWindow.cpp) can host a sandboxed plugin with ZERO
    // changes of its own. PluginWindow only ever calls plugin.createEditor()
    // and setContentNonOwned(editor.get(), true) — it has no idea the
    // editor it's hosting is secretly polling for a cross-process HWND to
    // reparent; it just sees a normal resizable Component that occasionally
    // calls setSize() on itself, and PluginWindow's own
    // resizeToFitWhenContentChangesSize auto-follows exactly as it would
    // for any in-process plugin editor.
    struct CrateEditorComponent : public te::Plugin::EditorComponent,
                                  private juce::Timer
    {
        // Note: no separate MouseListener base needed — juce::Component
        // (which te::Plugin::EditorComponent derives from) already IS-A
        // MouseListener; adding it again here would create an ambiguous
        // duplicate base. mouseUp() below overrides that inherited virtual
        // directly, and Desktop::addGlobalMouseListener (constructor,
        // below) accepts `this` through that same existing base.
        explicit CrateEditorComponent (CrateSandboxBridge& bridgeToUse)
            : bridge (bridgeToUse),
              scaleNotifier (this, [this] (float newScale) { bridge.publishDisplayScaleFactor (newScale); })
        {
            addAndMakeVisible (hwndComponent);
            setSize (400, 300); // placeholder until the CHILD's real editor size is known

            // Step 30 (Completing the Proxy Illusion) directive — the actual
            // root cause of the UI Blackout: requestEditorWindow() silently
            // no-ops if the CHILD isn't connected yet (controlBlock still
            // null — see its own doc comment). A user dropping a plugin and
            // immediately clicking "show native UI" is a completely
            // realistic sequence (launch takes hundreds of ms), and calling
            // this exactly once, here, could easily fire into a not-yet-
            // connected bridge — the request is then lost forever, and the
            // poll below waits on a handle the CHILD was never actually
            // told to create. Fixed by re-issuing the request every tick
            // (see timerCallback()) until the window is actually ready —
            // requestEditorWindow() is idempotent (just sets a bool), so
            // repeating it at 30ms costs nothing once it succeeds.
            //
            // Same 30ms/~33fps cadence Step 23's own dynamic-resize follow
            // logic settled on — smooth enough to track a live drag,
            // negligible cost for a handful of atomic reads per tick.
            startTimer (30);

            // Step 35 (Resize Debouncing) directive, Task 1: the resize
            // corner the user actually drags belongs to PluginWindow's own
            // chrome (a ResizableCornerComponent living outside this
            // Component's own bounds), so this component never receives its
            // mouseUp directly — a global listener is the standard JUCE way
            // to observe a drag gesture's end regardless of which Component
            // owns it. Used only to force one final, exact, un-throttled
            // publishHostResizeRequest() the instant the drag ends (see
            // mouseUp() and dispatchPendingResizeIfDue()'s own comments) —
            // guarantees the last frame of a fast drag is never left
            // stranded inside the 30ms throttle window.
            juce::Desktop::getInstance().addGlobalMouseListener (this);
        }

        ~CrateEditorComponent() override
        {
            juce::Desktop::getInstance().removeGlobalMouseListener (this);

            // Detach before this Component (and hwndComponent along with
            // it) is destroyed — HWNDComponent's own destructor already
            // calls DestroyWindow on whatever it's holding; explicitly
            // clearing it here first avoids racing PluginWindow's own
            // teardown against a still-ticking timer callback.
            stopTimer();
            hwndComponent.setHWND (nullptr);
        }

        // Step 36 (The Fixed-Size Lock) directive, Task 1: returns the
        // CACHED value (updated in timerCallback() below, never queried
        // fresh here) rather than a hardcoded true — but PluginWindow.cpp's
        // own setEditor() only ever calls this ONCE, at construction time,
        // before the CHILD has even connected (see timerCallback()'s own
        // comment on why the ACTUAL lockdown has to happen there instead,
        // proactively, once the real answer is known). This is still worth
        // returning honestly rather than a bare "true" literal, for any
        // future caller that re-queries it after the fact.
        bool allowWindowResizing() override { return lastKnownCanResize; }
        juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return &constrainer; }

        // Step 34 (Final Polish) directive, Task 1: a plain fillAll would
        // show whatever PluginWindow's own black background is underneath
        // — indistinguishable from the "UI Blackout" bug already root-caused
        // and fixed (Step 32). Explicit "Loading..." text while waiting
        // means an ordinary in-flight wait (DLL load, IPC handshake — all
        // genuinely asynchronous, see the constructor's own comment) reads
        // as "working," not "broken," to whoever's looking at it. Painted
        // only while hwndComponent has no real HWND yet — the moment a
        // handle lands, hwndComponent's own native content fully covers
        // this component's area, so this text is never visible again after
        // that (repaint() called once, right when the handle arrives, is
        // what clears the stale "Loading..." frame promptly rather than
        // leaving it until whatever next repaint happens to occur).
        void paint (juce::Graphics& g) override
        {
            if (currentHandle != nullptr)
                return;

            g.fillAll (juce::Colours::black);
            g.setColour (juce::Colours::white);
            g.setFont (15.0f);
            g.drawFittedText ("Loading Plugin…", getLocalBounds(), juce::Justification::centred, 1);
        }

        void resized() override
        {
            hwndComponent.setBounds (getLocalBounds());
            forceRepaintEmbeddedHwnd(); // Step 35 Task 3 — see its own doc comment

            // Step 34 (Bidirectional Resize) directive: only push to the
            // CHILD when THIS resize was HOST-initiated (the user dragging
            // PluginWindow's own resize corner) — applyingChildReportedSize
            // is set around the timerCallback()'s OWN setSize() call below,
            // guarding against echoing the CHILD's report straight back to
            // it as if it were a NEW host request (which would still be
            // harmless — same value in, same value out — but is needless
            // IPC traffic every single tick the plugin's own size happens
            // to change, and blurs which direction actually originated a
            // given resize when reading the logs).
            //
            // Step 35 (Resize Debouncing) directive, Task 1: a fast corner
            // drag calls resized() dozens of times a second — publishing an
            // IPC write on every single one of those floods the CHILD's own
            // message loop with size renegotiations faster than a VST3 view
            // can actually process them, which is exactly what was observed
            // stress-testing this (visual tearing, black voids, eventual
            // freeze/crash). Only the LATEST size is ever meaningful, so
            // resized() just records it and asks
            // dispatchPendingResizeIfDue() to send it — throttled to at
            // most once every resizeThrottleMs during a continuous drag,
            // with mouseUp() (below) forcing one final untruncated dispatch
            // the instant the drag ends.
            if (! applyingChildReportedSize)
            {
                pendingResizeWidth  = getWidth();
                pendingResizeHeight = getHeight();
                resizeDirty = true;
                dispatchPendingResizeIfDue (false);
            }
        }

    private:
        // Step 35 (Resize Debouncing) directive, Task 1: see resized()'s own
        // comment for why this exists. force==true (only ever passed from
        // mouseUp()) bypasses the throttle entirely — the drag just ended,
        // so there's no more flooding risk, and the final size must reach
        // the CHILD exactly, not whatever stale value the last throttled
        // tick happened to send.
        void dispatchPendingResizeIfDue (bool force)
        {
            if (! resizeDirty)
                return;

            const auto now = juce::Time::getMillisecondCounter();

            if (! force && now - lastResizeDispatchMs < (juce::uint32) resizeThrottleMs)
                return;

            lastResizeDispatchMs = now;
            resizeDirty = false;
            bridge.publishHostResizeRequest (pendingResizeWidth, pendingResizeHeight);
        }

        // Step 35 (Resize Debouncing) directive, Task 1: global listener
        // (see constructor's own comment) — fires for EVERY mouseUp
        // anywhere in the app, not just the resize corner's, but
        // dispatchPendingResizeIfDue() is a no-op whenever resizeDirty is
        // already false, so an unrelated click elsewhere costs one cheap
        // bool check.
        void mouseUp (const juce::MouseEvent&) override
        {
            dispatchPendingResizeIfDue (true);
        }

        void timerCallback() override
        {
            if (! bridge.isEditorWindowReady())
            {
                bridge.requestEditorWindow(); // re-issued every tick until it actually lands — see constructor's own comment
                return;
            }

            // Step 35 (Resize Debouncing) directive, Task 1: catches the
            // case where the drag pauses (mouse still held, no new
            // resized() call) for longer than resizeThrottleMs — without
            // this, a pending resize would only ever flush on the NEXT
            // resized() call or the eventual mouseUp, potentially leaving
            // the CHILD stale for the whole pause. Cheap no-op when nothing
            // is pending.
            dispatchPendingResizeIfDue (false);

            // Step 36 (The Fixed-Size Lock) directive, Task 1: checked
            // every tick (one atomic load + one bool compare — negligible)
            // rather than only once at construction, because the real
            // answer isn't known THEN — the CHILD hasn't even launched yet
            // when PluginWindow.cpp's setEditor() makes its own one-time
            // allowWindowResizing() query (see that method's own comment).
            // The moment the true value arrives and differs from what we
            // last acted on, reach directly up to the actual native chrome
            // window and lock/unlock its resize grip for real —
            // allowWindowResizing() returning the right value from then on
            // is necessary but not sufficient, since nothing re-queries it
            // after construction; this is what actually moves the needle.
            const bool canResizeNow = bridge.getEditorCanResize();

            if (canResizeNow != lastKnownCanResize)
            {
                lastKnownCanResize = canResizeNow;

                if (auto* resizableWindow = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent()))
                    resizableWindow->setResizable (canResizeNow, false);
            }

            auto* newHandle = bridge.getEditorWindowHandle();
            const int w = bridge.getEditorWindowWidth();
            const int h = bridge.getEditorWindowHeight();

            if (newHandle == nullptr)
                return; // no window yet

            if (w <= 0 || h <= 0)
                return;

            const bool handleChanged = (newHandle != currentHandle);
            const bool sizeChanged   = (w != lastWidth || h != lastHeight);

            // Step 35 Task 2 fix: a fixed-size plugin (canResize() == false,
            // e.g. Opal/DualDelayX — confirmed via diagnostic logging, the
            // CHILD's constrainer clamps every requested size straight back
            // to its ORIGINAL, unchanging value) never produces a
            // sizeChanged edge at all — w/h are the SAME before and after a
            // rejected host resize attempt, so the CHILD-report comparison
            // above has nothing to trigger on. sizeMismatch instead compares
            // against THIS component's own current bounds — the thing that
            // actually needs to be pulled back down after the user's drag
            // grew it past what the plugin agreed to. Re-checked every tick
            // (cheap — two int compares), so the snap-back keeps re-applying
            // for as long as the live drag keeps pushing bounds away from
            // the CHILD's confirmed size, settling the instant the drag
            // stops.
            const bool sizeMismatch  = (w != getWidth() || h != getHeight());

            if (! handleChanged && ! sizeChanged && ! sizeMismatch)
                return;

            currentHandle = newHandle;
            lastWidth  = w;
            lastHeight = h;

            if (handleChanged)
            {
                hwndComponent.setHWND (newHandle);
                repaint(); // clears the "Loading Plugin…" placeholder promptly — see paint()'s own comment
            }

            hwndComponent.setSize (w, h);
            forceRepaintEmbeddedHwnd(); // Step 35 Task 3 — see its own doc comment

            // Triggers PluginWindow's own resizeToFitWhenContentChangesSize
            // (setContentNonOwned's second argument) — the SAME mechanism
            // that already makes a live in-process plugin resize follow
            // correctly, no special-casing needed for this being a
            // reparented cross-process window underneath. Guarded so
            // resized() (above) knows NOT to interpret this particular
            // size change as a host-initiated resize request and echo it
            // straight back to the CHILD — see that method's own comment.
            applyingChildReportedSize = true;
            setSize (w, h);
            applyingChildReportedSize = false;
        }

        // Step 35 (Force Child-Side Resize Enforcement & Snap-Back)
        // directive, Task 3: the SAME cross-process repaint force Step 24
        // established for SandboxEditorTestWindow — JUCE's own embedded-HWND
        // invalidation (HWNDComponent::Pimpl::componentMovedOrResized(),
        // via EnumChildWindows) only reaches actual native WIN32 CHILD
        // windows, never the top-level embedded HWND itself, and a fast
        // resize burst was proven (Step 23/24) to outrun the OS's own
        // default repaint often enough to leave the plugin's content
        // visibly stale/blank. CrateEditorComponent never had this call at
        // all before now — Step 24's version only ever protected the OLD
        // SandboxEditorTestWindow debug harness, not this, the actual
        // production editor path.
        void forceRepaintEmbeddedHwnd()
        {
           #if JUCE_WINDOWS
            if (auto* hwnd = (HWND) hwndComponent.getHWND())
            {
                ::InvalidateRect (hwnd, nullptr, TRUE);
                ::UpdateWindow (hwnd);
            }
           #endif
        }

        CrateSandboxBridge& bridge;

        // Step 35 (DPI Bridge) directive, Task 2: replaces the old
        // per-tick poll of getPeer()->getPlatformScaleFactor() with JUCE's
        // own event-driven mechanism — the SAME class the VST/VST3 wrappers
        // themselves use internally to keep an editor's scale in sync with
        // its containing window. Watches THIS component's own peer — the
        // real, top-level PARENT window's native DPI — which is a
        // completely different, trustworthy query from the CHILD-side
        // PluginWindow::getDesktopScaleFactor() override (hardcoded to
        // 1.0f, see Main.cpp's own doc comment on that), since this lives
        // entirely on the PARENT side, several processes and window trees
        // away from that override. Fires the instant a real DPI change
        // happens (dragging the host across monitors, a monitor's scaling
        // setting changing) rather than waiting up to a full tick.
        juce::NativeScaleFactorNotifier scaleNotifier;

        juce::HWNDComponent hwndComponent;
        juce::ComponentBoundsConstrainer constrainer;
        void* currentHandle = nullptr;
        int lastWidth = 0;
        int lastHeight = 0;
        bool applyingChildReportedSize = false;

        // Step 35 (Resize Debouncing) directive, Task 1 — see resized()'s
        // own comment.
        static constexpr int resizeThrottleMs = 30;
        juce::uint32 lastResizeDispatchMs = 0;
        int pendingResizeWidth = 0;
        int pendingResizeHeight = 0;
        bool resizeDirty = false;

        // Step 36 (The Fixed-Size Lock) directive, Task 1: starts true,
        // matching ControlBlock::editorCanResize's own optimistic default
        // and getEditorCanResize()'s pre-connection fallback — see
        // timerCallback()'s own comment for how/when this actually gets
        // acted on.
        bool lastKnownCanResize = true;
    };

    std::unique_ptr<EditorComponent> createEditor() override
    {
        return std::make_unique<CrateEditorComponent> (*this);
    }

    // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive: PARENT ->
    // CHILD, the reverse direction from getEditorWindowWidth/Height above.
    // No windowHandleReady gate needed — unlike the handle/geometry fields
    // (which only mean anything once the CHILD has actually created its
    // editor), a display scale is meaningful to publish at any time; the
    // CHILD simply applies whatever the most recent value is the moment its
    // own editor exists (see Main.cpp's own matching comment). Safe to call
    // before the bridge is even connected — silently drops if there's no
    // control block yet, same as every other "PARENT writes settings the
    // CHILD reads on its own schedule" field in this class.
    void publishDisplayScaleFactor (float scale) noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->displayScale1000.store ((int32_t) juce::roundToInt (scale * 1000.0f), std::memory_order_relaxed);
    }

    // Step 34 (Bidirectional Resize) directive: PARENT -> CHILD, the
    // reverse direction from getEditorWindowWidth/Height above — see
    // ControlBlock::hostRequestedWidth/Height's own doc comment. Same
    // "silently drops if not connected yet" contract as
    // publishDisplayScaleFactor() above.
    void publishHostResizeRequest (int width, int height) noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
        {
            block->hostRequestedWidth.store (width, std::memory_order_relaxed);
            block->hostRequestedHeight.store (height, std::memory_order_release);
        }
    }

    // Continuous State Sync directive (Step 11): the most recent plugin
    // state chunk received from the CHILD — updated by timerCallback()
    // whenever a push arrives (see that method's own comment). Empty until
    // the first push. Message-thread-only (this is only ever read from
    // debug/test code, never the audio thread).
    const juce::MemoryBlock& getLastKnownState() const noexcept { return lastKnownState; }
    size_t getLastKnownStateSize() const noexcept { return lastKnownState.getSize(); }

    // Step 12.1 (The Violent Crash Test) directive: debug/test-only — asks
    // the CHILD to deliberately segfault (a genuine unhandled access
    // violation, not a clean juce::ChildProcess::kill()), so the exact same
    // heartbeat-timeout detection and resurrection sequence proven against
    // a clean kill can also be proven against a real OS-level crash. See
    // CrateIPC::ControlBlock::triggerCrashRequested's own doc comment.
    void triggerChildCrash()
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->triggerCrashRequested.store (true, std::memory_order_release);
    }

    // The Parent Push directive (Step 8): pushes one parameter change into
    // the lock-free SPSC ring buffer for the CHILD to apply before its next
    // processBlock() — see CrateIPC::ControlBlock's own doc comment for why
    // this is a hand-rolled ring buffer rather than moodycamel's queue.
    // Callable from the AUDIO thread (this step's own test calls it from
    // applyToBuffer() to sweep a parameter every block) — never blocks,
    // never allocates, never logs directly (logEvent() is file I/O; a full
    // queue instead sets paramQueueOverflowDetected, an atomic flag,
    // exactly the same "audio thread publishes, message thread logs"
    // contract dspStallDetected already uses). With 1024 slots and this
    // ring drained once per audio round trip, an overflow would mean far
    // more than 1024 parameter pushes between two round trips — not
    // expected under any normal automation density, per the step's own
    // directive, but handled rather than assumed impossible.
    void setParameterEvent (int parameterIndex, float normalizedValue)
    {
        auto* block = controlBlock.load (std::memory_order_acquire);

        if (block == nullptr)
            return; // no sandbox attached yet — nothing to push to

        using CB = CrateIPC::ControlBlock;

        const uint32_t head = block->paramQueueHead.load (std::memory_order_relaxed); // producer-owned — only WE ever write this
        const uint32_t tail = block->paramQueueTail.load (std::memory_order_acquire);  // consumer-owned — we only ever READ this
        const uint32_t nextHead = (head + 1) % (uint32_t) CB::paramQueueCapacity;

        if (nextHead == tail)
        {
            paramQueueOverflowDetected.store (true, std::memory_order_release);
            return;
        }

        block->paramQueue[head] = CB::ParamChange { parameterIndex, normalizedValue };
        block->paramQueueHead.store (nextHead, std::memory_order_release);
    }

    // Round-Trip Verification directive: pushes a small, known non-zero
    // buffer through applyToBuffer() exactly the way a real audio block
    // would be — same code path, same spin-wait, same round trip. Originally
    // (Step 6) this asserted an EXACT phase inversion, since the sandbox's
    // only DSP was a hardcoded *-1.0f. Step 7 replaced that with a real,
    // arbitrary third-party VST3 — asserting a specific transfer function
    // against unknown third-party DSP would be meaningless (and this
    // bridge doesn't know or care WHAT the plugin does to the signal, by
    // design), so the pass criterion is now the honest, general one: the
    // round trip completed (didn't time out/stall) AND the output isn't
    // bit-identical to the input (proving the samples were actually
    // touched out-of-process, not silently passed through untouched).
    // Returns false immediately (no-op, no log) if the bridge isn't
    // confirmed alive yet — callers should wait for isConnected() first,
    // since applyToBuffer() itself silences and returns early otherwise.
    // Debug/verification use only — not called anywhere in the normal
    // audio graph.
    bool runEchoPhaseTest (double& roundTripMsOut)
    {
        if (! isConnected())
            return false;

        constexpr int numSamples = 8;
        juce::AudioBuffer<float> testBuffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                testBuffer.setSample (ch, i, 0.5f + 0.01f * (float) i);

        juce::AudioBuffer<float> original;
        original.makeCopyOf (testBuffer);

        te::MidiMessageArray midiBuffer;
        const tcore::TimeRange editTime (tcore::TimePosition::fromSeconds (0.0), tcore::TimePosition::fromSeconds (0.001));
        te::PluginRenderContext ctx (&testBuffer, juce::AudioChannelSet::stereo(), 0, numSamples,
                                     &midiBuffer, 0.0, editTime, false, false, false, true);

        const auto startMs = juce::Time::getMillisecondCounterHiRes();
        applyToBuffer (ctx);
        roundTripMsOut = juce::Time::getMillisecondCounterHiRes() - startMs;

        bool outputDiffersFromInput = false;
        for (int ch = 0; ! outputDiffersFromInput && ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                if (! juce::approximatelyEqual (testBuffer.getSample (ch, i), original.getSample (ch, i)))
                    outputDiffersFromInput = true;

        logEvent ("Round-Trip Test: " + juce::String (outputDiffersFromInput ? "PASS" : "FAIL")
                      + " (round trip " + juce::String (roundTripMsOut, 3) + "ms, output "
                      + (outputDiffersFromInput ? "differs from input — real DSP occurred" : "unchanged — passthrough or no plugin loaded") + ")");

        return outputDiffersFromInput;
    }

    // Step 17 (The Lookahead IPC Pipeline / "Time-Slip Plumbing") directive:
    // the Parent-side half of Pipeline Verification — same shape/logging
    // convention as runEchoPhaseTest() above, adapted for a RING of
    // requests instead of one single-slot round trip. Enqueues enough
    // requests to span roughly 4 real seconds of timeline position (the
    // actual Time-Slip target), signals the CHILD's LookaheadWorkerThread
    // once, then verifies every result comes back tagged with the correct
    // position AND phase-inverted (the same deterministic transform the
    // CHILD's own doc comment describes) — proving the pipe carries real
    // data correctly, out of lockstep with any per-block real-time
    // cadence. Debug/verification use only, matching runEchoPhaseTest()'s
    // own scope.
    bool runLookaheadPipelineTest (double& totalRoundTripMsOut)
    {
        if (! isConnected())
            return false;

        auto* block = controlBlock.load (std::memory_order_acquire);

        if (block == nullptr || lookaheadRequestReadyEvent == nullptr)
            return false;

        using CB = CrateIPC::ControlBlock;

        // Enough requests, each covering maxSamplesPerBlock/cachedSampleRate
        // seconds, to span ~4 real seconds of cumulative timeline position
        // — proving the pipeline actually sustains the Time-Slip depth
        // target, not just a single lonely round trip. Clamped well below
        // lookaheadRingCapacity so this test never has to handle ring
        // wraparound itself.
        const double secondsPerSlot = (double) CB::maxSamplesPerBlock / juce::jmax (1.0, cachedSampleRate);
        const int numRequests = juce::jlimit (1, CB::lookaheadRingCapacity - 1, (int) std::ceil (4.0 / secondsPerSlot));

        int64_t position = 0;

        for (int i = 0; i < numRequests; ++i)
        {
            const auto head     = block->lookaheadRequestHead.load (std::memory_order_relaxed);
            const auto tail     = block->lookaheadRequestTail.load (std::memory_order_acquire);
            const auto nextHead = (head + 1) % (uint32_t) CB::lookaheadRingCapacity;

            if (nextHead == tail)
            {
                logEvent ("Lookahead Pipeline Test: FAIL — request ring unexpectedly full after " + juce::String (i) + " requests.");
                return false;
            }

            auto& slot = block->lookaheadRequestRing[head];
            slot.timelinePositionSamples = position;
            slot.numChannels = CB::maxChannels;
            slot.numSamples  = CB::maxSamplesPerBlock;

            // Deterministic, distinct-ish per request — not silence, so a
            // passthrough-that-forgot-to-invert would be caught too.
            const float testValue = 0.5f + 0.001f * (float) (i % 100);
            for (int s = 0; s < CB::maxChannels * CB::maxSamplesPerBlock; ++s)
                slot.audioInput[s] = testValue;

            block->lookaheadRequestHead.store (nextHead, std::memory_order_release);
            position += CB::maxSamplesPerBlock;
        }

        lookaheadRequestReadyEvent->signal();

        // Message-thread verification wait, not audio-thread — a generous
        // bounded timeout is correct here (unlike anything on the real-time
        // path), same reasoning that motivated tenantFirstConnectTimeoutMs:
        // a lookahead worker doing real, potentially CPU-bound DSP per
        // request would legitimately need more than a few milliseconds,
        // even though this step's own dummy transform is trivial.
        const auto startMs = juce::Time::getMillisecondCounterHiRes();
        constexpr double timeoutMs = 5000.0;
        int resultsVerified = 0;
        bool mismatchFound = false;

        while (resultsVerified < numRequests)
        {
            const auto resultHead = block->lookaheadResultHead.load (std::memory_order_acquire);
            auto resultTail = block->lookaheadResultTail.load (std::memory_order_relaxed);

            while (resultTail != resultHead && resultsVerified < numRequests)
            {
                auto& result = block->lookaheadResultRing[resultTail];
                const int64_t expectedPosition = (int64_t) resultsVerified * CB::maxSamplesPerBlock;

                if (result.timelinePositionSamples != expectedPosition)
                {
                    logEvent ("Lookahead Pipeline Test: FAIL — result #" + juce::String (resultsVerified)
                                  + " position mismatch (expected " + juce::String (expectedPosition)
                                  + ", got " + juce::String (result.timelinePositionSamples) + ").");
                    mismatchFound = true;
                }
                else
                {
                    const float expectedSample = -(0.5f + 0.001f * (float) (resultsVerified % 100));

                    if (! juce::approximatelyEqual (result.audioOutput[0], expectedSample))
                    {
                        logEvent ("Lookahead Pipeline Test: FAIL — result #" + juce::String (resultsVerified)
                                      + " sample mismatch (expected " + juce::String (expectedSample, 4)
                                      + ", got " + juce::String (result.audioOutput[0], 4) + ").");
                        mismatchFound = true;
                    }
                }

                resultTail = (resultTail + 1) % (uint32_t) CB::lookaheadRingCapacity;
                block->lookaheadResultTail.store (resultTail, std::memory_order_release);
                ++resultsVerified;
            }

            if (resultsVerified >= numRequests)
                break;

            if (juce::Time::getMillisecondCounterHiRes() - startMs > timeoutMs)
            {
                logEvent ("Lookahead Pipeline Test: FAIL — timed out waiting for results ("
                              + juce::String (resultsVerified) + "/" + juce::String (numRequests) + " received).");
                return false;
            }

            juce::Thread::sleep (2);
        }

        totalRoundTripMsOut = juce::Time::getMillisecondCounterHiRes() - startMs;

        const bool passed = ! mismatchFound;
        logEvent ("Lookahead Pipeline Test: " + juce::String (passed ? "PASS" : "FAIL")
                      + " (" + juce::String (numRequests) + " requests spanning "
                      + juce::String ((double) numRequests * secondsPerSlot, 2) + "s of timeline, total pipeline round trip "
                      + juce::String (totalRoundTripMsOut, 1) + "ms)");

        return passed;
    }

    // Step 18 (The Time-Slip Engine) directive: switches THIS bridge from
    // the real-time IPC round trip to reading from the TimeSlipBuffer —
    // call only once connected (isConnected()), matching requestEditorWindow()'s
    // own convention. Sets the CHILD's own isLookaheadMode flag (so
    // AudioBridgeThread stops competing for pluginAccessLock and
    // LookaheadWorkerThread becomes the sole driver of processBlock()),
    // and starts this bridge's own LookaheadProducerThread if it isn't
    // already running.
    void enableLookaheadMode()
    {
        // Step 21 (Auto-Demotion) directive: the Health Registry persists
        // across sessions (see PluginHealthRegistry's own doc comment), so
        // a plugin that hung this bridge's pipeline in a PAST session — or
        // an entirely different bridge's, since the flag is keyed by
        // plugin UID, not by instance — is known bad before ever touching
        // it again. Skip lookahead mode entirely rather than rediscovering
        // the hang the hard way a second time.
        if (PluginHealthRegistry::getInstance().isUnsafeForLookahead (effectivePluginUID()))
        {
            logEvent ("Time-Slip Engine: skipping lookahead mode — this plugin is flagged "
                          "unsafeForLookahead in the Health Registry (a previous hang already proved it).");
            lookaheadPermanentlyDemoted.store (true, std::memory_order_release);
            return;
        }

        lookaheadModeRequested.store (true, std::memory_order_release);

        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->isLookaheadMode.store (true, std::memory_order_release);

        if (lookaheadProducerThread == nullptr)
        {
            lookaheadProducerThread = std::make_unique<LookaheadProducerThread> (*this);
            lookaheadProducerThread->startThread (juce::Thread::Priority::low);
        }
    }

    // Step 18 (Flush-on-Change) directive: called when the user changes a
    // parameter on a lookahead-enabled track — the buffered window was
    // rendered against the OLD parameter value and is now stale.
    //
    // SCOPE, disclosed honestly: this is the mechanism, triggered
    // explicitly/manually for now — there is deliberately no automatic
    // hook wired to Step 8's own hardcoded per-block Cutoff sweep
    // (dispatchToSandbox()'s own setParameterEvent() call), because that
    // sweep is a CONTINUOUS automation stream; wiring flush-on-change to
    // it would flush every single block and defeat the buffer's entire
    // purpose. Real per-parameter-change detection is later work, once
    // real (non-test-sweep) automation exists to detect changes in.
    //
    // Clears the buffer, discards any in-flight lookahead work that
    // predates this call (via flushGenerationStartPosition — see
    // pumpLookaheadPipeline()'s own comment on why stale results are
    // filtered rather than forcing a reset of the CHILD-owned ring
    // indices), and restarts production from the CURRENT playhead
    // position (the last position the audio thread actually asked for).
    void flushTimeSlipBuffer()
    {
        const auto currentPosition = juce::jmax ((int64_t) 0, lastReadSamplePosition.load (std::memory_order_acquire));
        const auto restartPosition = currentPosition + 1;

        timeSlipBuffer.reset();
        flushGenerationStartPosition.store (restartPosition, std::memory_order_release);
        nextProduceSamplePosition.store (restartPosition, std::memory_order_release);

        logEvent ("Time-Slip Buffer flushed (Flush-on-Change) — pipeline restarting from sample position "
                      + juce::String (restartPosition) + ".");
    }

    // Step 18 (The Time-Slip Engine) directive: verifies real VST3 audio
    // reaches the TimeSlipBuffer and that the Parent's audio-thread read
    // path (readFromTimeSlipBuffer()) can consume it, then verifies
    // Flush-on-Change actually clears and restarts the pipeline.
    //
    // Step 19 update: the ground truth for "did real DSP happen" is a
    // FRESH, direct extraction via generateRealTrackAudioInto() (the
    // standalone WaveNode pipeline — see that method's own doc comment)
    // at the same position — compared against the buffered (post-VST3)
    // output, same reasoning as runEchoPhaseTest()'s own phase-inversion
    // check, just with real track content as the baseline instead of
    // Step 18's known tone.
    bool runTimeSlipEngineTest (double& reportedBufferedSeconds)
    {
        if (! isConnected())
            return false;

        enableLookaheadMode();

        const auto startMs = juce::Time::getMillisecondCounterHiRes();
        // Step 19 directive: bumped from Step 18's 8000ms — real chunks
        // now involve an actual message-thread graph construction +
        // offline render + disk write/read per second of audio, real
        // wall-clock cost the synthetic-tone version never had.
        constexpr double timeoutMs = 20000.0;
        const int64_t oneSecondSamples = (int64_t) juce::jmax (1.0, cachedSampleRate);

        while (timeSlipBuffer.getBufferedUpToSamplePosition() < oneSecondSamples)
        {
            if (juce::Time::getMillisecondCounterHiRes() - startMs > timeoutMs)
            {
                logEvent ("Time-Slip Engine Test: FAIL — buffer did not fill within timeout ("
                              + juce::String (timeSlipBuffer.getBufferedUpToSamplePosition()) + " samples buffered).");
                return false;
            }

            juce::Thread::sleep (10);
        }

        reportedBufferedSeconds = (double) timeSlipBuffer.getBufferedUpToSamplePosition() / juce::jmax (1.0, cachedSampleRate);

        constexpr int checkSamples = 512;
        juce::AudioBuffer<float> checkBuffer (2, checkSamples);
        float* channelPtrs[2] { checkBuffer.getWritePointer (0), checkBuffer.getWritePointer (1) };

        if (! timeSlipBuffer.read (0, channelPtrs, checkSamples))
        {
            logEvent ("Time-Slip Engine Test: FAIL — read at position 0 reported an underrun despite "
                          + juce::String (reportedBufferedSeconds, 2) + "s showing buffered.");
            return false;
        }

        // Step 19 directive: ground truth is a FRESH direct extraction of
        // the raw (pre-VST3) clip audio at the same position, via the
        // same standalone WaveNode pipeline generateRealTrackAudioInto()
        // itself uses — not a cache, not a synthetic tone. A difference
        // between the buffered (post-VST3) output and the raw clip audio
        // proves real processing occurred, same reasoning as Step 18's
        // tone-based check, just with real content as the baseline
        // instead of a placeholder.
        using CB = CrateIPC::ControlBlock;
        std::vector<float> rawCheckStorage ((size_t) CB::maxChannels * CB::maxSamplesPerBlock, 0.0f);
        generateRealTrackAudioInto (rawCheckStorage.data(), 2, checkSamples, 0);

        bool sawNonSilentRawAudio = false;
        bool differsFromRawClipAudio = false;

        for (int i = 0; i < checkSamples; ++i)
        {
            const float rawSample = rawCheckStorage[(size_t) i];

            if (! juce::approximatelyEqual (rawSample, 0.0f))
                sawNonSilentRawAudio = true;

            if (! juce::approximatelyEqual (checkBuffer.getSample (0, i), rawSample))
                differsFromRawClipAudio = true;
        }

        logEvent ("Time-Slip Engine Test: " + juce::String (differsFromRawClipAudio ? "PASS" : "FAIL")
                      + " (" + juce::String (reportedBufferedSeconds, 2) + "s buffered, position-0 read "
                      + (differsFromRawClipAudio ? "differs from the raw rendered clip audio — real VST3 processing occurred" : "matches raw clip audio unchanged — no real DSP detected")
                      + (sawNonSilentRawAudio ? "" : "; WARNING: the raw clip audio itself was silent at this position — verify a real, non-silent clip exists on the test track")
                      + ")");

        if (! differsFromRawClipAudio)
            return false;

        // Flush-on-Change verification.
        flushTimeSlipBuffer();

        const bool clearedImmediately = timeSlipBuffer.getBufferedUpToSamplePosition() < 0;
        logEvent ("Time-Slip Engine Test (Flush-on-Change): " + juce::String (clearedImmediately ? "PASS" : "FAIL")
                      + " — buffer " + (clearedImmediately ? "correctly cleared" : "still reports stale content") + " immediately after flush.");

        if (! clearedImmediately)
            return false;

        const auto refillStartMs = juce::Time::getMillisecondCounterHiRes();
        const int64_t quarterSecondSamples = oneSecondSamples / 4;

        while (timeSlipBuffer.getBufferedUpToSamplePosition() < quarterSecondSamples)
        {
            if (juce::Time::getMillisecondCounterHiRes() - refillStartMs > timeoutMs)
            {
                logEvent ("Time-Slip Engine Test (Flush-on-Change): FAIL — buffer did not refill within timeout after flush.");
                return false;
            }

            juce::Thread::sleep (10);
        }

        logEvent ("Time-Slip Engine Test (Flush-on-Change): PASS — buffer refilled ("
                      + juce::String (timeSlipBuffer.getBufferedUpToSamplePosition()) + " samples) after flush, pipeline restarted from position "
                      + juce::String (flushGenerationStartPosition.load (std::memory_order_relaxed)) + ".");

        return true;
    }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // Step 19 hardening: TE re-invokes initialise() whenever the live
        // playback graph gets rebuilt — confirmed empirically to fire
        // THREE TIMES within a single ~5-second window during the Time-Slip
        // Engine Test, apparently as a side effect of
        // ensureRawTrackAudioAvailable()'s te::Renderer::renderToFile()
        // calls touching the SAME live Edit this bridge lives in (offline
        // rendering evidently still perturbs the live graph enough to
        // trigger a plugin reinit pass, despite Renderer::RenderTask
        // building its own nominally-separate graph). The previous code
        // unconditionally tore down and relaunched the sandbox child (or
        // tenant attachment) on EVERY call, which silently discarded the
        // in-flight lookahead pipeline — a fresh child process, a freshly
        // zeroed TimeSlipBuffer — every few seconds. That, not any IPC or
        // threading bug, is what actually produced "Time-Slip Engine Test:
        // FAIL -- buffer did not fill within timeout" despite chunks
        // visibly rendering successfully moments earlier. A genuine
        // sampleRate/blockSize change is rare and DOES need a fresh
        // launch (the shared ControlBlock's cached values feed the child
        // at spawn time); a same-config re-invocation from a graph rebuild
        // does not, and must be a no-op here.
        const bool configChanged = hasLaunchedOnce
                                        && (cachedSampleRate != info.sampleRate || cachedBlockSize != info.blockSizeSamples);

        // The Initialization Payload directive (Step 7): the REAL host
        // sampleRate/blockSizeSamples, cached so launchSandboxProcess() can
        // write them into the shared ControlBlock before the child exists —
        // see this class's own doc comment on why the file must be fully
        // prepared before the child process is ever launched.
        cachedSampleRate = info.sampleRate;
        cachedBlockSize  = info.blockSizeSamples;

        // Step 15.4 (The DSP Soft-Mute) directive: pre-allocated ONCE, here
        // — Zero Allocation Rule, same reasoning as AudioBridgeThread's own
        // workBuffer (Step 7). applyFadeOrSilence() only ever reads/writes
        // this on the audio thread; it must never trigger a real-time
        // allocation there. Safe to re-run on every initialise() call
        // (setSize() is a no-op once already the right size), unlike the
        // launch/attach + TimeSlipBuffer reset below.
        lastValidOutput.setSize (CrateIPC::ControlBlock::maxChannels, CrateIPC::ControlBlock::maxSamplesPerBlock);

        if (! hasLaunchedOnce || configChanged)
        {
            hasLaunchedOnce = true;

            // Step 18 (The Time-Slip Engine) directive: sized here, once
            // cachedSampleRate is finally known — same "pre-allocate as
            // soon as the real config is available" discipline as
            // lastValidOutput above. Only re-prepared on a genuine config
            // change, not on every redundant initialise() call, since
            // prepare() resets the buffer's contents.
            timeSlipBuffer.prepare (cachedSampleRate, CrateIPC::ControlBlock::maxChannels);

            // Step 15.2 directive: a Tenant Bridge attaches to an
            // already-running Shared Sandbox host instead of spawning its
            // own — see attachAsTenant()'s own doc comment for the full
            // contract.
            //
            // Step 33 (Cryosleep Architecture) directive: a pool-claimed
            // bridge is the THIRD option — like a tenant, it attaches to an
            // already-running process instead of spawning one, but unlike a
            // tenant it's a 1:1 dedicated isolated process, not a
            // many-tenants-in-one-host arrangement. See claimFromPool()'s
            // own doc comment.
            if (isTenantMode)
                attachAsTenant();
            else if (isPooledClaim && ! hasClaimedFromPoolOnce)
            {
                // One-shot: the pool only ever helps the FIRST launch. A
                // later re-invocation of this branch (a genuine sampleRate/
                // blockSize change, not a crash restart — those call
                // launchSandboxProcess() directly from timerCallback(),
                // never through here) would try to re-signal a claim event
                // the CHILD's own one-shot CryosleepWaitThread already fired
                // and stopped listening on — falling through to a normal
                // cold start instead is correct, not just simpler.
                hasClaimedFromPoolOnce = true;
                claimFromPool();
            }
            else
                launchSandboxProcess();
        }
    }

    // Step 15.4 (The Teardown Protocol) directive: the PRIMARY unload
    // trigger — call this the moment the user's intent to remove this
    // plugin is unambiguous (e.g. right before deleteFromParent() in the
    // caller's own "delete plugin" action), rather than relying solely on
    // deinitialise() firing promptly.
    //
    // WHY THIS EXISTS, HONESTLY: deinitialise() (below) was the ORIGINAL,
    // and architecturally more obvious, place to dispatch this — "the
    // bridge is being destroyed" is the natural trigger. Verified by
    // direct testing that it is NOT reliable here: te::Plugin::deinitialise()
    // only runs once the underlying te::Plugin::Ptr's reference count
    // actually reaches zero, and something in Tracktion Engine's own
    // Plugin/graph-node lifecycle held an EXTRA reference that did not
    // release even after an explicit edit.restartPlayback() call (the
    // exact documented API for "topology changed, rebuild now") and 15+
    // seconds of waiting — traced as far as te::PluginCache's own
    // self-cleanup (PluginCache::timerCallback(), which only releases its
    // own reference once refcount==1) and te::PluginList::ObjectList
    // (which DOES release synchronously, confirmed via source), but
    // something beyond both of those — most likely the render graph's own
    // PluginNode, whose lifecycle is tied to actual audio-callback-driven
    // graph rebuilds — kept it pinned indefinitely in this test harness.
    // Rather than keep excavating a large, separate TE subsystem, the
    // more ROBUST fix is this: dispatch on clear intent, not on uncertain
    // object-destruction timing. deinitialise()'s own dispatch (below)
    // stays as a defensive fallback for removal paths that don't call
    // this explicitly (e.g. closing a whole Edit) — CrateSandbox's own
    // unloadTenant() already no-ops harmlessly on an unknown/already-gone
    // instanceID, so a bridge that fires BOTH is safe, not a double-free.
    void notifyRemovalRequested()
    {
        // Step 22 (Re-entrancy Safety) directive: signal the lookahead
        // thread to wind down AS SOON AS removal intent is unambiguous —
        // well before anything is actually blocked waiting on it. A plain,
        // non-blocking request; see LookaheadProducerThread's own doc
        // comment for why nothing here waits for it, and why that's safe.
        if (lookaheadProducerThread != nullptr)
        {
            lookaheadProducerThread->signalThreadShouldExit();
            lookaheadProducerThread->notify(); // wake it immediately rather than waiting out its current backoff sleep
        }

        if (isTenantMode && onTenantRemoved)
            onTenantRemoved (instanceId);
    }

    void deinitialise() override
    {
        logEvent ("deinitialise() called [instanceId=" + instanceId + "]");

        stopTimer();

        // Step 22 (Re-entrancy Safety) directive: same defensive-fallback
        // reasoning as the onTenantRemoved dispatch just below — see
        // notifyRemovalRequested()'s own doc comment. Idempotent: calling
        // signalThreadShouldExit()/notify() again here if it already ran
        // is harmless.
        if (lookaheadProducerThread != nullptr)
        {
            lookaheadProducerThread->signalThreadShouldExit();
            lookaheadProducerThread->notify();
        }

        // Step 15.4 (The Teardown Protocol) directive: defensive fallback
        // — see notifyRemovalRequested()'s own doc comment for why THAT is
        // now the primary, reliable trigger, and why firing here too (if
        // this ever actually runs) is still safe, not a double-dispatch
        // problem.
        if (isTenantMode && onTenantRemoved)
            onTenantRemoved (instanceId);

        // Step 15.2 directive: terminateSandboxProcess() is safe to reuse
        // for BOTH modes as-is — its sandboxProcess.kill() call is a no-op
        // on a ChildProcess that was never started (which is always true
        // for a Tenant Bridge), and its audioThreadActive wait + mappedMemory
        // unmap are exactly what a Tenant Bridge needs too: it must still
        // release its OWN view of this tenant's shared memory when removed,
        // it just never owned the process on the other end of it.
        terminateSandboxProcess();

        // Step 19 hardening: a genuine deinitialise()->initialise() cycle
        // (TE's normal graph-rebuild pattern) DOES need the next
        // initialise() call to rebuild the connection — terminateSandboxProcess()
        // just above unmapped this bridge's shared memory, so controlBlock
        // is no longer valid. Without this reset, hasLaunchedOnce's guard
        // (added to stop initialise() from tearing down and relaunching a
        // perfectly healthy child on every redundant re-entry — see
        // initialise()'s own doc comment) would wrongly skip rebuilding
        // here too, leaving the bridge holding a dangling controlBlock
        // while LookaheadProducerThread/applyToBuffer() keep running
        // against it — a real access violation, not a hypothetical one.
        hasLaunchedOnce = false;
    }

    // Echo/Phase Test directive: the audio-thread half of the round trip.
    // Step 9.1 (Scatter-Gather & Yield Refactor) split this into
    // dispatchToSandbox()/gatherFromSandbox() below — applyToBuffer()
    // itself still just calls both back-to-back, so the REAL engine's
    // per-plugin, per-block contract (produce output before returning) is
    // completely unchanged; the split exists so a test driving MANY
    // bridges at once can fire every instance's dispatch FIRST and gather
    // every instance's result SECOND. See dispatchToSandbox()'s own comment
    // for why that ordering matters.
    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        // Step 18 (The Time-Slip Engine) directive: mutually exclusive
        // paths, checked first — a lookahead-enabled bridge NEVER touches
        // the real-time IPC round trip at all; it just reads whatever the
        // LookaheadProducerThread has already accumulated. No IPC waiting
        // in the hot path, per the step's own directive.
        if (lookaheadModeRequested.load (std::memory_order_acquire))
        {
            readFromTimeSlipBuffer (fc);
            return;
        }

        const bool dispatched = dispatchToSandbox (fc);
        gatherFromSandbox (fc, dispatched);
    }

    // The Scatter directive (Step 9.1): push audio + the current parameter
    // sweep value, signal the child, and return WITHOUT waiting — the
    // child can now run concurrently with every OTHER bridge's own dispatch
    // that follows, instead of this call blocking the caller until ITS OWN
    // child responds before the NEXT bridge even gets a chance to signal
    // its child. CrateStressTest::runSandboxScaleTest() calls this on all
    // 50 bridges in one pass, THEN calls gatherFromSandbox() on all 50 in a
    // second pass — Step 9's own test never did this (it relied on the real
    // Edit graph's own per-track walk, which visits one track's plugin
    // chain to completion before moving to the next), and that sequential
    // dispatch-then-wait pattern was a real, previously undiscovered
    // contributor to Step 9's uniform ~3ms clustering across all 50
    // instances, not just spin-wait CPU pressure alone.
    //
    // Returns true if a real round trip was actually dispatched
    // (gatherFromSandbox() must then be called to complete it); false if
    // this call already fully resolved the block itself (sandbox not
    // alive, or a bad/null buffer) — gatherFromSandbox() is a safe no-op in
    // that case.
    bool dispatchToSandbox (const te::PluginRenderContext& fc)
    {
        if (! sandboxAlive.load (std::memory_order_acquire))
        {
            applyFadeOrSilence (fc);
            return false;
        }

        // THE UNMAP RACE (caught live, crashed the app under Step 6.5's own
        // test): a restart's terminateSandboxProcess() runs on the MESSAGE
        // thread and calls mappedMemory.reset() — actually unmapping the OS
        // view. audioThreadActive is the fix: set true here, false only
        // once gatherFromSandbox() below has finished every last
        // dereference of `block` — terminateSandboxProcess() busy-waits
        // (message thread can afford to; audio thread never does) for this
        // to clear before it EVER calls mappedMemory.reset().
        audioThreadActive.store (true, std::memory_order_release);

        auto* block = controlBlock.load (std::memory_order_acquire);

        // Defensive: sandboxAlive can be observed stale-true for at most one
        // more callback while a restart is tearing down the old mapping —
        // see this class's own doc comment on the torn-pointer race this
        // guards against.
        if (block == nullptr || fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        {
            applyFadeOrSilence (fc);
            audioThreadActive.store (false, std::memory_order_release);
            return false;
        }

        using CB = CrateIPC::ControlBlock;

        // Message-thread-only... no — AUDIO-thread-only bookkeeping between
        // this dispatch and this SAME instance's own matching gather call.
        // Safe as a plain (non-atomic) member: exactly one thread ever
        // touches it for a given instance between dispatch and gather, in
        // both the real per-block usage (immediately sequential, same
        // thread) and the scale test's own scatter-then-gather passes
        // (still the same calling thread, just with OTHER instances'
        // dispatch calls interleaved in between — which never touch THIS
        // instance's own members).
        dispatchNumChannels = juce::jmin (fc.destBuffer->getNumChannels(), CB::maxChannels);
        dispatchNumSamples  = juce::jmin (fc.bufferNumSamples, CB::maxSamplesPerBlock);

        for (int ch = 0; ch < dispatchNumChannels; ++ch)
        {
            auto* src = fc.destBuffer->getReadPointer (ch, fc.bufferStartSample);
            std::memcpy (block->audioInput + (size_t) ch * CB::maxSamplesPerBlock, src, (size_t) dispatchNumSamples * sizeof (float));
        }

        block->numChannels.store (dispatchNumChannels, std::memory_order_relaxed);
        block->numSamples.store (dispatchNumSamples, std::memory_order_relaxed);
        block->childProcessed.store (false, std::memory_order_relaxed);
        block->parentReady.store (true, std::memory_order_release);

        // The Hybrid Sync Pivot directive: wake the child's blocked
        // AudioBridgeThread — SetEvent() is a fast, non-blocking kernel
        // call, safe to fire from the audio thread (unlike wait(), which
        // this class NEVER calls). The child was asleep at genuine 0% CPU
        // until exactly this call.
        if (bufferReadyEvent != nullptr)
            bufferReadyEvent->signal();

        dispatchStartMs = juce::Time::getMillisecondCounterHiRes();

        return true;
    }

    // The Gather directive (Step 9.1): spin-wait (with progressive
    // pause/yield back-off — see below) for childProcessed, copy the
    // result back, and record round-trip timing. `dispatched` MUST be
    // dispatchToSandbox()'s own return value for this SAME block; passing
    // false is a safe no-op (dispatch already fully resolved the buffer
    // itself).
    void gatherFromSandbox (const te::PluginRenderContext& fc, bool dispatched)
    {
        if (! dispatched)
            return;

        auto* block = controlBlock.load (std::memory_order_acquire);

        if (block == nullptr) // unreachable in practice (dispatchToSandbox() already checked) — never dereference an unverified pointer
        {
            audioThreadActive.store (false, std::memory_order_release);
            return;
        }

        using CB = CrateIPC::ControlBlock;

        // KILLING THE SPIN-WAIT STARVATION directive (Step 9.1): a
        // PROGRESSIVE back-off, not a uniform yield()-every-iteration spin.
        // Measured live under Step 9's own 50-instance test: every single
        // instance clustered at ~3.0-3.3ms, right at the cap — Thread::
        // yield() on EVERY iteration from the very first spin cycle meant
        // 50 threads ALL constantly signalling "let someone else run,"
        // which is nearly a no-op when EVERYONE does it simultaneously (a
        // "yield storm" that doesn't meaningfully relieve scheduler
        // pressure, just adds syscall overhead). _mm_pause() first — a
        // CPU-level hint that eases memory-bus/cache contention and reduces
        // power draw WITHOUT actually yielding the scheduler, ideal for the
        // common case where the child responds in microseconds — escalating
        // to Thread::yield() only once a short initial window elapses with
        // no response. Still a bounded busy-wait overall, capped at the
        // SAME spinWaitTimeoutMs as before (never loosened — see this
        // class's own doc comment on why 3ms is a hard ceiling, not a
        // tuning knob) and never a real blocking OS wait, which would
        // reintroduce the exact priority-inversion risk already rejected
        // for the PARENT side.
        constexpr int pauseSpinIterations = 200;
        int spinCount = 0;

        const auto deadline = dispatchStartMs + (double) CrateIPC::spinWaitTimeoutMs;
        bool success = false;

        while (juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            if (block->childProcessed.load (std::memory_order_acquire))
            {
                success = true;
                break;
            }

            if (spinCount < pauseSpinIterations)
            {
               #if JUCE_INTEL
                _mm_pause(); // CPU-level pause hint — JUCE has no wrapper for this, verified against modules/JUCE
               #else
                juce::Thread::yield();
               #endif
                ++spinCount;
            }
            else
            {
                juce::Thread::yield();
            }
        }

        {
            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - dispatchStartMs;
            double prevMax = maxRoundTripMsObserved.load (std::memory_order_relaxed);

            while (elapsedMs > prevMax
                   && ! maxRoundTripMsObserved.compare_exchange_weak (prevMax, elapsedMs, std::memory_order_relaxed))
            {
                // CAS retry: prevMax was updated by the loop itself on failure — no separate re-read needed.
            }
        }

        if (success)
        {
            for (int ch = 0; ch < dispatchNumChannels; ++ch)
            {
                auto* dst = fc.destBuffer->getWritePointer (ch, fc.bufferStartSample);
                std::memcpy (dst, block->audioOutput + (size_t) ch * CB::maxSamplesPerBlock, (size_t) dispatchNumSamples * sizeof (float));
            }

            block->parentReady.store (false, std::memory_order_relaxed);
            block->childProcessed.store (false, std::memory_order_relaxed);

            // Step 15.4 directive: cache this exact output as the fallback
            // source for the NEXT failure's fade-out, and re-arm the fade
            // state to idle (-1) so a FUTURE failure starts a fresh,
            // full-volume fade instead of continuing wherever a PAST one
            // left off.
            for (int ch = 0; ch < dispatchNumChannels; ++ch)
                lastValidOutput.copyFrom (ch, 0, fc.destBuffer->getReadPointer (ch, fc.bufferStartSample), dispatchNumSamples);

            lastValidNumSamples = dispatchNumSamples;
            hasValidLastOutput  = true;
            fadeSamplesRemaining = -1;
        }
        else
        {
            applyFadeOrSilence (fc);

            // DSP Stall directive: logEvent() does file I/O — never safe to
            // call from the audio thread. Publish the flag; timerCallback()
            // (message thread) does the actual logging and forces a restart.
            dspStallDetected.store (true, std::memory_order_release);
        }

        audioThreadActive.store (false, std::memory_order_release);
    }

    void restorePluginStateFromValueTree (const juce::ValueTree&) override {}
    void flushPluginStateToValueTree() override {}

private:
    //==============================================================================
    // Step 18 (The Time-Slip Engine) directive: the Parent-side
    // accumulator — ordinary process heap memory (NOT shared memory; only
    // THIS bridge's own audio thread and LookaheadProducerThread ever
    // touch it), sized for CrateIPC::ControlBlock::maxChannels channels at
    // timeSlipSeconds worth of samples at the real host sample rate.
    // Indexed by ABSOLUTE SAMPLE POSITION modulo capacity — position P
    // always lands at the same slot, the same scheme the IPC lookahead
    // rings themselves already use for request/result correspondence.
    //
    // THREADING CONTRACT: write() is called ONLY from
    // LookaheadProducerThread (the sole writer); read() is called ONLY
    // from the audio thread (the sole reader). Neither ever blocks or
    // allocates. The one piece of cross-thread state is
    // bufferedUpToSamplePosition — a release-store after every write,
    // an acquire-load before every read — which is what lets the reader
    // safely tell "genuinely buffered up to here" from "not filled that
    // far yet" (an underrun) without a lock.
    class TimeSlipBuffer
    {
    public:
        static constexpr double timeSlipSeconds = 4.0; // per this step's own directive — NOT the placeholder 500ms this replaced

        void prepare (double sampleRateIn, int numChannelsIn)
        {
            capacitySamples = juce::jmax (1, (int) std::ceil (juce::jmax (1.0, sampleRateIn) * timeSlipSeconds));
            numChannels = juce::jlimit (1, CrateIPC::ControlBlock::maxChannels, numChannelsIn);
            storage.setSize (numChannels, capacitySamples);
            storage.clear();
            bufferedUpToSamplePosition.store (-1, std::memory_order_release);
        }

        void write (int64_t position, const float* const* channelData, int numSamplesToWrite)
        {
            if (capacitySamples <= 0)
                return;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* dst = storage.getWritePointer (ch);
                const float* src = channelData[ch];

                for (int i = 0; i < numSamplesToWrite; ++i)
                    dst[(size_t) ((position + i) % capacitySamples)] = src[i];
            }

            bufferedUpToSamplePosition.store (position + numSamplesToWrite - 1, std::memory_order_release);
        }

        // Real-time safe: no allocation, no locking. Returns false (an
        // underrun — not buffered that far yet) rather than reading
        // stale/uninitialized slots.
        bool read (int64_t position, float* const* destChannelData, int numSamplesToRead) const
        {
            if (capacitySamples <= 0 || bufferedUpToSamplePosition.load (std::memory_order_acquire) < position + numSamplesToRead - 1)
                return false;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* src = storage.getReadPointer (ch);
                float* dst = destChannelData[ch];

                for (int i = 0; i < numSamplesToRead; ++i)
                    dst[i] = src[(size_t) ((position + i) % capacitySamples)];
            }

            return true;
        }

        void reset() { bufferedUpToSamplePosition.store (-1, std::memory_order_release); }

        int64_t getBufferedUpToSamplePosition() const noexcept { return bufferedUpToSamplePosition.load (std::memory_order_acquire); }
        int getCapacitySamples() const noexcept { return capacitySamples; }

    private:
        juce::AudioBuffer<float> storage;
        int capacitySamples = 0;
        int numChannels = 2;
        std::atomic<int64_t> bufferedUpToSamplePosition { -1 };
    };

    // Step 18 directive: Parent-side background producer, one per bridge —
    // same nested-class-holding-a-reference-to-its-owner shape as
    // CrateAnticipativeWrapper's own ShadowWorker (a nested class IS a
    // member of its enclosing class in C++, with the same access to
    // private members as any other member function — this isn't a new
    // pattern, it's the established one). Genuinely decoupled from
    // real-time: backs off when the buffer is already comfortably ahead,
    // catches up quickly when the read cursor advances or after a
    // Flush-on-Change reset.
    //
    // Step 19 update: feeds REAL upstream track audio into the pipeline —
    // see generateRealTrackAudioInto()/ensureRawTrackAudioAvailable()'s
    // own doc comments for how (te::Renderer::renderToFile(), TE's own
    // existing offline-render API, not a hand-rolled graph traversal).
    // Step 22 (Re-entrancy Safety / "Fix the Metal") directive: found the
    // hard way, via a real CHOC buffer-range assertion (choc_SampleBuffers.h,
    // getFrameRange().contains(range)) reproduced under heavy concurrent
    // load — two overlapping stress-test runs, each spinning up 100 fresh
    // tracks, racing an in-flight WaveNode extraction against this bridge's
    // own teardown. Root cause: this thread used to hold a bare
    // `CrateSandboxBridge& owner` reference, and ~CrateSandboxBridge()
    // called lookaheadProducerThread->stopThread(1000) — a BLOCKING wait
    // with a hard TerminateThread() fallback if the in-flight call (a
    // single, uninterruptible te::TracktionNodePlayer::process() call,
    // reading real audio via TE's file cache) hadn't finished within that
    // one second. Under contention, that timeout was reachable, and
    // TerminateThread() killing the thread MID-INSTRUCTION inside CHOC
    // buffer machinery is exactly the kind of thing that leaves buffer
    // state corrupted — not a hypothetical, the assertion above IS that
    // corruption surfacing.
    //
    // Fix: this thread now holds a te::Plugin::Ptr keep-alive reference to
    // its owner, not a bare reference. As long as this thread is running,
    // the owner's own reference count cannot reach zero, so
    // ~CrateSandboxBridge() literally cannot execute while this thread
    // might still be touching `owner` — no timeout, no force-kill, no
    // race, by construction (te::Plugin derives directly from
    // juce::ReferenceCountedObject — see that class's own declaration).
    // The keep-alive is released ONLY after this thread's own loop has
    // cooperatively, fully exited, and even then it's released via
    // callAsync so the actual decrement (and any resulting `delete this`)
    // always happens on the message thread — never on this thread itself,
    // since ~CrateSandboxBridge() calls stopTimer() and other message-
    // thread-affine JUCE calls that would violate the same threading rules
    // this whole class has spent so much effort respecting elsewhere.
    //
    // The corresponding half of this fix is signalling the stop EARLY —
    // see notifyRemovalRequested()'s own doc comment — so this thread
    // gets maximum opportunity to notice and honour it well before
    // anything is actually waiting on it.
    class LookaheadProducerThread : public juce::Thread
    {
    public:
        explicit LookaheadProducerThread (CrateSandboxBridge& ownerToUse)
            : juce::Thread ("Crate Lookahead Producer"), owner (ownerToUse), ownerKeepAlive (&ownerToUse)
        {
        }

        void run() override
        {
            int backoffMs = 1;

            while (! threadShouldExit())
            {
                const bool didWork = owner.pumpLookaheadPipeline();
                backoffMs = didWork ? 1 : juce::jmin (50, backoffMs * 2);
                wait (backoffMs);
            }

            // Release the keep-alive ON THE MESSAGE THREAD, never here —
            // see this class's own doc comment above. std::move leaves
            // ownerKeepAlive null so this thread's OWN later destruction
            // (as a unique_ptr member of the very object it was keeping
            // alive) can never double-release it.
            auto keepAliveToRelease = std::move (ownerKeepAlive);
            juce::MessageManager::callAsync ([keepAliveToRelease] {});
        }

    private:
        CrateSandboxBridge& owner;
        te::Plugin::Ptr ownerKeepAlive;
    };

    // Step 18 (The Time-Slip Engine) directive: the audio-thread half of
    // the lookahead path — called from applyToBuffer() INSTEAD OF
    // dispatchToSandbox()/gatherFromSandbox() when lookaheadModeRequested
    // is set. Real-time safe: TimeSlipBuffer::read() never allocates or
    // locks. Publishes lastReadSamplePosition unconditionally (hit or
    // miss) so LookaheadProducerThread always knows where the real
    // read cursor actually is, for its own pacing.
    void readFromTimeSlipBuffer (const te::PluginRenderContext& fc)
    {
        using CB = CrateIPC::ControlBlock;

        if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        const int64_t position = (int64_t) std::llround (fc.editTime.getStart().inSeconds() * juce::jmax (1.0, cachedSampleRate));
        const int numChannels = juce::jmin (fc.destBuffer->getNumChannels(), CB::maxChannels);

        float* channelPtrs[CB::maxChannels] {};
        for (int ch = 0; ch < numChannels; ++ch)
            channelPtrs[ch] = fc.destBuffer->getWritePointer (ch, fc.bufferStartSample);

        const bool success = timeSlipBuffer.read (position, channelPtrs, fc.bufferNumSamples);

        lastReadSamplePosition.store (position + fc.bufferNumSamples - 1, std::memory_order_release);

        if (! success)
        {
            // Underrun — buffer hasn't caught up (cold start, or right
            // after a Flush-on-Change reset). Reuse the EXISTING, proven
            // Step 15.4 Soft-Mute fallback rather than a hard silence cut
            // or (worse) fabricating a "future" block that was never
            // really rendered.
            applyFadeOrSilence (fc);
            return;
        }

        // A successful buffer read counts as "valid last output" too, so
        // a LATER underrun still gets a fade instead of an instant cut.
        for (int ch = 0; ch < numChannels; ++ch)
            lastValidOutput.copyFrom (ch, 0, fc.destBuffer->getReadPointer (ch, fc.bufferStartSample), fc.bufferNumSamples);

        lastValidNumSamples = fc.bufferNumSamples;
        hasValidLastOutput  = true;
        fadeSamplesRemaining = -1;
    }

    // Step 18 directive: called repeatedly by LookaheadProducerThread —
    // NOT the audio thread, so none of the real-time constraints that
    // shape dispatchToSandbox()/gatherFromSandbox() apply here. Does two
    // things per call: (1) drains any results the CHILD has already
    // produced into the TimeSlipBuffer — filtering out anything older
    // than flushGenerationStartPosition, which is how a Flush-on-Change
    // discards stale in-flight work WITHOUT needing to reach into the
    // CHILD-owned ring indices directly (lookaheadRequestTail/
    // lookaheadResultHead stay exclusively CHILD-written, matching the
    // established SPSC ownership contract; this just chooses not to ACT
    // on pre-flush results rather than trying to force-clear them); (2) if
    // there's room within the buffer's own 4-second capacity relative to
    // where the audio thread is currently reading, enqueues ONE more
    // request. Returns true if either step did real work, so the caller's
    // own backoff resets instead of growing when there's genuinely more
    // to do.
    bool pumpLookaheadPipeline()
    {
        if (lookaheadPermanentlyDemoted.load (std::memory_order_acquire))
            return false;

        auto* block = controlBlock.load (std::memory_order_acquire);

        if (block == nullptr || ! block->isLookaheadMode.load (std::memory_order_acquire))
            return false;

        using CB = CrateIPC::ControlBlock;
        bool didWork = false;

        // Drain results -> TimeSlipBuffer, discarding stale (pre-flush) ones.
        {
            const auto resultHead = block->lookaheadResultHead.load (std::memory_order_acquire);
            auto resultTail = block->lookaheadResultTail.load (std::memory_order_relaxed);
            const auto validFrom = flushGenerationStartPosition.load (std::memory_order_acquire);

            while (resultTail != resultHead)
            {
                auto& result = block->lookaheadResultRing[resultTail];

                if (result.timelinePositionSamples >= validFrom)
                {
                    const float* channelPtrs[CB::maxChannels];
                    for (int ch = 0; ch < CB::maxChannels; ++ch)
                        channelPtrs[ch] = result.audioOutput + (size_t) ch * CB::maxSamplesPerBlock;

                    timeSlipBuffer.write (result.timelinePositionSamples, channelPtrs, result.numSamples);
                }

                resultTail = (resultTail + 1) % (uint32_t) CB::lookaheadRingCapacity;
                block->lookaheadResultTail.store (resultTail, std::memory_order_release);
                didWork = true;
            }
        }

        // Step 21 (The Watchdog / The Guillotine) directive: confirmed via
        // direct testing that a badly-behaved third-party VST3 (Rift
        // Filter Lite) can hang LookaheadWorkerThread's processBlock() call
        // INDEFINITELY once fed real, non-silent audio — the entire child
        // process, including its own dependency-free heartbeat thread,
        // stops dead. The general heartbeat-timeout watchdog already
        // catches this eventually, but it's a coarse, generic "is the
        // child alive at all" check (seconds-scale, shared with every
        // other death-detection path) — this one is specific to the
        // lookahead ring itself and fires much faster, per the request/
        // result ring's own actual progress rather than a side-channel
        // heartbeat.
        //
        // requestHead is the last position the PARENT has enqueued;
        // resultHead is the last position the CHILD has produced a result
        // for (see the ring-pair's own index-matched contract above) — if
        // they're equal, the pipeline is fully caught up and healthy, and
        // the watchdog clock resets. If they differ, something is
        // in-flight; if that gap NEVER closes (not just shrinks slowly —
        // this resets on ANY full catch-up, not partial progress) for
        // longer than lookaheadWatchdogTimeoutMs, the child is presumed
        // hung and the Guillotine drops.
        {
            const auto requestHead = block->lookaheadRequestHead.load (std::memory_order_acquire);
            const auto resultHeadAfterDrain = block->lookaheadResultHead.load (std::memory_order_acquire);

            if (requestHead == resultHeadAfterDrain)
            {
                lookaheadWatchdogBatchStartMs = -1.0;
            }
            else
            {
                const auto nowMs = juce::Time::getMillisecondCounterHiRes();

                if (lookaheadWatchdogBatchStartMs < 0.0)
                    lookaheadWatchdogBatchStartMs = nowMs;
                else if (nowMs - lookaheadWatchdogBatchStartMs > lookaheadWatchdogTimeoutMs)
                    triggerLookaheadWatchdog();
            }
        }

        // Produce one more request if there's room.
        {
            const auto readPos  = juce::jmax ((int64_t) 0, lastReadSamplePosition.load (std::memory_order_acquire));
            const auto produceAt = nextProduceSamplePosition.load (std::memory_order_acquire);

            if (produceAt - readPos < (int64_t) timeSlipBuffer.getCapacitySamples())
            {
                const auto head     = block->lookaheadRequestHead.load (std::memory_order_relaxed);
                const auto tail     = block->lookaheadRequestTail.load (std::memory_order_acquire);
                const auto nextHead = (head + 1) % (uint32_t) CB::lookaheadRingCapacity;

                if (nextHead != tail)
                {
                    auto& slot = block->lookaheadRequestRing[head];
                    slot.timelinePositionSamples = produceAt;
                    slot.numChannels = CB::maxChannels;
                    slot.numSamples  = CB::maxSamplesPerBlock;

                    generateRealTrackAudioInto (slot.audioInput, CB::maxChannels, CB::maxSamplesPerBlock, produceAt);

                    block->lookaheadRequestHead.store (nextHead, std::memory_order_release);

                    if (lookaheadRequestReadyEvent != nullptr)
                        lookaheadRequestReadyEvent->signal();

                    nextProduceSamplePosition.store (produceAt + CB::maxSamplesPerBlock, std::memory_order_release);
                    didWork = true;
                }
            }
        }

        return didWork;
    }

    // Step 21 (The Watchdog & Graceful Fallback / The Guillotine) directive:
    // called ONCE per detected hang from pumpLookaheadPipeline() (running on
    // LookaheadProducerThread). lookaheadWatchdogBatchStartMs is reset
    // immediately below so a still-stuck-on-the-old-child pipeline can't
    // re-trigger this every subsequent tick before the async demotion below
    // actually lands (a fire-and-forget callAsync, not something this
    // method waits on).
    //
    // Dispatches the actual teardown to the message thread — same
    // te::Plugin::Ptr keep-alive discipline as every other Edit/TE-adjacent
    // touch already established in this class (see ensureLookaheadWaveNodeReady()'s
    // own doc comment) — because launchSandboxProcess() calls startTimer(),
    // which JUCE requires to run on the message thread, and because
    // touching controlBlock/sandboxProcess teardown state from two threads
    // at once is exactly the kind of race this whole file has spent a lot
    // of effort avoiding elsewhere.
    void triggerLookaheadWatchdog()
    {
        lookaheadWatchdogBatchStartMs = -1.0;

        logEvent ("Time-Slip Engine WATCHDOG: lookahead pipeline produced no result for over "
                      + juce::String (lookaheadWatchdogTimeoutMs, 0) + "ms — the child is presumed HUNG, "
                      "not merely slow. Dropping the Guillotine: force-terminating the child, flagging "
                      "this plugin unsafe for lookahead, and falling back to the standard real-time round trip.");

        te::Plugin::Ptr selfPtr { this };

        juce::MessageManager::callAsync ([selfPtr]
        {
            auto* self = dynamic_cast<CrateSandboxBridge*> (selfPtr.get());

            if (self == nullptr)
                return;

            PluginHealthRegistry::getInstance().recordLookaheadHang (self->effectivePluginUID());

            // Auto-Demotion: permanently stop asking for lookahead mode on
            // THIS bridge instance. applyToBuffer() falls back to
            // dispatchToSandbox()/gatherFromSandbox() (the normal real-time
            // round trip) from the very next audio block onward — no
            // special-casing needed there, since that's simply what
            // already happens whenever lookaheadModeRequested is false.
            // The existing DSP Soft-Mute fade (Step 15.4) covers the
            // transition itself, so this never produces a hard click.
            self->lookaheadModeRequested.store (false, std::memory_order_release);
            self->lookaheadPermanentlyDemoted.store (true, std::memory_order_release);

            if (auto* block = self->controlBlock.load (std::memory_order_acquire))
                block->isLookaheadMode.store (false, std::memory_order_release);

            // The Guillotine itself: force-terminate the hung child and
            // immediately relaunch a fresh one — do NOT wait for it to
            // recover. launchSandboxProcess() calls terminateSandboxProcess()
            // first, which is a hard sandboxProcess.kill() already bounded
            // to a ~50ms wait regardless of the child's own responsiveness
            // (see that method's own comment), so this is safe to call even
            // though the old child is completely unresponsive. The fresh
            // child starts with isLookaheadMode false by default and is
            // never told otherwise, so it simply services the normal round
            // trip from here on.
            self->logEvent ("Time-Slip Engine WATCHDOG: terminating hung child and relaunching "
                                 "(lookahead permanently disabled for this plugin instance).");
            self->launchSandboxProcess();
        });
    }

    // Step 19 REWRITE directive (The Alien Extraction): the original cut
    // of this integration called te::Renderer::renderToFile() against the
    // SAME live Edit this bridge lives in, once per second of lookahead
    // audio needed. That worked (after fixing two real crashes caused by
    // touching TE's Edit/Track model off the message thread — see git
    // history for the gory details) but had a fatal structural flaw:
    // Renderer::renderToFile(), despite building its own nominally
    // separate offline graph, still triggers TE to fully reinitialise the
    // LIVE graph's plugins — confirmed empirically via initialise() firing
    // three times in five seconds on this exact bridge, each call tearing
    // down and relaunching the sandbox child and zeroing TimeSlipBuffer.
    // Every single chunk render wiped the lookahead pipeline's progress
    // before it could ever reach one full buffered second. Rejected the
    // "render from a cloned Edit" fix as the wrong kind of heavy for what
    // should be a lightweight read.
    //
    // This rewrite drops te::Renderer entirely and reads the clip's own
    // audio file DIRECTLY via te::WaveNode (tracktion_WaveNode.h) — the
    // SAME tracktion::graph::Node primitive TE's own live playback graph
    // uses to play back a wave clip (see tracktion_EditNodeBuilder.cpp's
    // own createNodeForAudioClip(), which builds an identical WaveNode
    // for real playback). The difference is entirely in how it's driven:
    // here it's given its OWN standalone tgraph::PlayHead/PlayHeadState
    // and te::ProcessState (see buildLookaheadWaveNode()) rather than the
    // Edit's live ones, and pumped by a tgraph::NodePlayer (the same
    // simple single-threaded driver TE itself provides for exactly this
    // "manually run a small Node graph" use case — see
    // tracktion_NodePlayer.h). The clip's Track/Edit/ValueTree data is
    // touched ONLY ONCE, at setup, to read its (for our purposes
    // immutable) AudioFile/position/gain/channels — never again for the
    // rest of this bridge's lifetime. No file I/O, no live-graph
    // perturbation, no reinit storm: this is now a pure in-memory file
    // read via TE's own audio file cache, exactly as cheap as the live
    // engine's own playback of the same clip.
    std::unique_ptr<tgraph::PlayHead> lookaheadPlayHead;
    std::unique_ptr<tgraph::PlayHeadState> lookaheadPlayHeadState;
    std::unique_ptr<te::ProcessState> lookaheadProcessState;
    std::unique_ptr<te::TracktionNodePlayer> lookaheadNodePlayer;
    bool lookaheadNodeSetupFailed = false;
    juce::AudioBuffer<float> lookaheadScratchBuffer;

    // Step 38 (The Zero-Allocation Scrub) directive: hoisted out of
    // generateRealTrackAudioInto() — that method is called from
    // LookaheadProducerThread, which drains "as fast as the CPU allows"
    // per its own doc comment (a tight loop, not a throttled one), so a
    // freshly-constructed te::MidiMessageArray on every single call was a
    // real, avoidable per-call cost sitting in a hot loop for no reason —
    // this member is cleared (not reconstructed) each call instead.
    te::MidiMessageArray lookaheadScratchMidi;

    // The ONE-TIME setup call — touches getOwnerTrack()/getClipsOfType()
    // (TE's ValueTree-backed model, message-thread-affine, same reasoning
    // as every other Edit touch in this file) so it's dispatched via
    // callAsync() with the same te::Plugin::Ptr keep-alive discipline
    // already established for the Teardown Protocol (see
    // MainComponent.cpp's closeButtonPressed()) and the original
    // renderToFile()-based crash fix this replaces. Everything AFTER this
    // one call — every actual chunk extraction — runs entirely on
    // LookaheadProducerThread, no further message-thread round trips.
    bool ensureLookaheadWaveNodeReady()
    {
        if (lookaheadNodePlayer != nullptr)
            return true;

        if (lookaheadNodeSetupFailed)
            return false;

        auto pending = std::make_shared<std::atomic<bool>> (false);
        te::Plugin::Ptr selfPtr { this };

        juce::MessageManager::callAsync ([selfPtr, pending]
        {
            if (auto* self = dynamic_cast<CrateSandboxBridge*> (selfPtr.get()))
                self->buildLookaheadWaveNodeOnMessageThread();

            pending->store (true, std::memory_order_release);
        });

        // Same reasoning as the original crash fix's wait loop: bails
        // immediately on threadShouldExit() so teardown can never
        // deadlock behind this wait; the queued lambda stays safe to run
        // later regardless, since selfPtr keeps `this` alive and `pending`
        // is independently heap-owned.
        while (! pending->load (std::memory_order_acquire))
        {
            if (juce::Thread::currentThreadShouldExit())
                return false;

            juce::Thread::sleep (2);
        }

        return lookaheadNodePlayer != nullptr;
    }

    void buildLookaheadWaveNodeOnMessageThread()
    {
        auto* track = getOwnerTrack();

        if (track == nullptr)
        {
            logEvent ("Time-Slip Engine: getOwnerTrack() returned nullptr on the message thread — "
                          "this bridge plugin isn't (yet, or ever) resolvable to an owning Track; lookahead extraction disabled.");
            lookaheadNodeSetupFailed = true;
            return;
        }

        auto* clipTrack = dynamic_cast<te::ClipTrack*> (track);

        if (clipTrack == nullptr)
        {
            logEvent ("Time-Slip Engine: owning Track isn't a ClipTrack (no clip list) — lookahead extraction disabled.");
            lookaheadNodeSetupFailed = true;
            return;
        }

        auto waveClips = te::getClipsOfType<te::WaveAudioClip> (*clipTrack);

        if (waveClips.isEmpty())
        {
            logEvent ("Time-Slip Engine: no WaveAudioClip found on the owning track — lookahead extraction disabled.");
            lookaheadNodeSetupFailed = true;
            return;
        }

        auto* clip = waveClips.getFirst();
        const auto clipPosition = clip->getPosition();
        const auto destChannels = juce::AudioChannelSet::canonicalChannelSet (juce::jmax (2, clip->getActiveChannels().size()));

        lookaheadPlayHead      = std::make_unique<tgraph::PlayHead>();
        lookaheadPlayHeadState = std::make_unique<tgraph::PlayHeadState> (*lookaheadPlayHead);
        lookaheadProcessState  = std::make_unique<te::ProcessState> (*lookaheadPlayHeadState);

        auto waveNode = tgraph::makeNode<te::WaveNode> (clip->getAudioFile(),
                                                         clipPosition.time,
                                                         clipPosition.offset,
                                                         clip->getLoopRange(),
                                                         clip->getLiveClipLevel(),
                                                         clip->getSpeedRatio(),
                                                         clip->getActiveChannels(),
                                                         destChannels,
                                                         *lookaheadProcessState,
                                                         clip->itemID,
                                                         true); // isOfflineRender

        // te::TracktionNodePlayer, NOT the raw tracktion::graph::NodePlayer —
        // the raw player only tracks PlayHeadState's own loop/jump state; it
        // never calls te::ProcessState::update(), which is what actually
        // populates editTimeRange/timelineSampleRange (the fields WaveNode::
        // process() reads via getTimelineSampleRange() to know WHERE in the
        // file to read from). Missing that call was a real, confirmed bug
        // here: the very first cut of this rewrite produced clean, silent
        // audio on every extraction (numMisses=0, so the node graph itself
        // was running fine — it just never knew what position to read).
        // setNumThreads(0) keeps this synchronous and driven entirely by
        // LookaheadProducerThread, no separate thread pool spun up underneath.
        lookaheadNodePlayer = std::make_unique<te::TracktionNodePlayer> (*lookaheadProcessState);
        lookaheadNodePlayer->setNumThreads (0);
        lookaheadNodePlayer->setNode (std::move (waveNode), cachedSampleRate, CrateIPC::ControlBlock::maxSamplesPerBlock);

        logEvent ("Time-Slip Engine: standalone lookahead WaveNode built directly from the track's own clip — "
                      "bypasses te::Renderer and the live Edit's playback graph entirely.");
    }

    // Replaces Step 18's generateKnownTestSignalInto() — same call site,
    // same signature, real content instead of a known tone. Falls back to
    // silence (never fabricated audio) if the WaveNode isn't ready or the
    // requested position is past the end of the track's content.
    //
    // Each call re-seeks the standalone PlayHead to the exact requested
    // position (playSyncedToRange()) and reads exactly numSamples from
    // it — a stateless "read raw audio at position X" call, matching the
    // pull-based contract pumpLookaheadPipeline() already expects.
    // WaveNode itself doesn't need read continuity between calls (it
    // computes each block's file read position from the absolute edit
    // time, not a running cursor — see WaveNode::processSection()), so
    // jumping around between arbitrary positions (e.g. Flush-on-Change
    // restarting production from a new playhead position) is completely
    // safe.
    void generateRealTrackAudioInto (float* dest, int numChannels, int numSamples, int64_t startSamplePosition)
    {
        using CB = CrateIPC::ControlBlock;

        for (int ch = 0; ch < numChannels; ++ch)
            std::memset (dest + (size_t) ch * CB::maxSamplesPerBlock, 0, (size_t) numSamples * sizeof (float));

        if (! ensureLookaheadWaveNodeReady())
            return;

        lookaheadPlayHead->playSyncedToRange ({ startSamplePosition, startSamplePosition + numSamples });

        lookaheadScratchBuffer.setSize (numChannels, numSamples, false, false, true);
        lookaheadScratchBuffer.clear();

        // Step 38 (The Zero-Allocation Scrub) directive: cleared, not
        // reconstructed — see lookaheadScratchMidi's own doc comment for
        // why a fresh te::MidiMessageArray on every call was a real,
        // avoidable cost in this tight-loop path.
        lookaheadScratchMidi.clear();
        auto bufferView = tgraph::toBufferView (lookaheadScratchBuffer);
        tgraph::Node::ProcessContext pc { (choc::buffer::FrameCount) numSamples,
                                           { 0, numSamples },
                                           { bufferView, lookaheadScratchMidi } };

        lookaheadNodePlayer->process (pc);

        for (int ch = 0; ch < numChannels; ++ch)
            std::memcpy (dest + (size_t) ch * CB::maxSamplesPerBlock,
                         lookaheadScratchBuffer.getReadPointer (ch),
                         (size_t) numSamples * sizeof (float));
    }

    void launchSandboxProcess()
    {
        terminateSandboxProcess(); // idempotent guard — never leaves a previous child/mapping dangling before starting a new one

        // Step 15.2/15.3 directive: rebuilt only when instanceId has
        // actually changed since the last time this ran — see
        // bufferReadyEvent's own updated doc comment. Isolated restarts
        // (stable instanceId) skip this every time after the first;
        // convertToIsolatedAndRelaunch() (a fresh instanceId, Step 15.3)
        // correctly rebuilds it.
        if (bufferReadyEvent == nullptr || bufferReadyEventInstanceId != instanceId)
        {
            bufferReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getBufferReadyEventName (instanceId));
            bufferReadyEventInstanceId = instanceId;
        }

        // Step 17 directive: same lazy-construct/rebuild-on-instanceId-
        // change discipline as bufferReadyEvent above — the Quarantine
        // Shuffle's fresh instanceId on reroute needs this rebuilt too,
        // not left bound to a dead tenant's event name.
        if (lookaheadRequestReadyEvent == nullptr || lookaheadRequestReadyEventInstanceId != instanceId)
        {
            lookaheadRequestReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getLookaheadRequestReadyEventName (instanceId));
            lookaheadRequestReadyEventInstanceId = instanceId;
        }

        // The Initialization Payload directive (Step 7): THIS class now
        // creates/sizes the shared file and maps it BEFORE the child
        // process exists, so the payload below can be written into an
        // already-valid block with no race about who gets there first —
        // see this class's own doc comment.
        CrateIPC::ensureSharedMemoryFileIsSized (CrateIPC::getSharedMemoryFile (instanceId));
        attachSharedMemory();

        if (auto* block = controlBlock.load (std::memory_order_relaxed))
        {
            // THIS STEP'S TEST HARDCODE directive: see this class's own doc
            // comment — real plugin selection is later work.
            std::memset (block->pluginPath, 0, sizeof (block->pluginPath));
            pluginPathForLaunch.copyToUTF8 (block->pluginPath, (size_t) CrateIPC::ControlBlock::maxPluginPathLength - 1);

            block->hostSampleRate = cachedSampleRate;
            block->hostBlockSize  = cachedBlockSize;

            // The Resurrection Test directive (Step 11): written on EVERY
            // launch, including a restart after this exact bridge's PREVIOUS
            // child crashed or stalled — empty (size 0) on a genuinely first
            // launch, which CrateSandbox's own loadHostedPlugin() correctly
            // treats as "nothing to restore." This is the entire mechanism
            // behind the Ghost Reload: a freshly spawned, completely
            // different OS process ends up loading with the exact state the
            // dead one had a moment before, because the PARENT (this class)
            // never forgot it.
            const auto stateSize = lastKnownState.getSize();

            if (stateSize > 0 && (int64_t) stateSize <= CrateIPC::ControlBlock::maxStateChunkBytes)
            {
                std::memcpy (block->initialStateData, lastKnownState.getData(), stateSize);
                block->initialStateSize = (int64_t) stateSize;
            }
            else
            {
                block->initialStateSize = 0;
            }
        }

        auto exe = CrateIPC::resolveSandboxExecutable();

        if (exe.existsAsFile())
            // Step 9 directive: instanceId passed as a command-line argument
            // (the StringArray overload avoids any manual quoting of the exe
            // path itself, which may contain spaces) — CrateSandbox reads it
            // back in its own initialise() to resolve the SAME shared file
            // and event name this specific launch just prepared.
            sandboxProcess.start (juce::StringArray { exe.getFullPathName(), instanceId });
        else
            logEvent ("CrateSandbox.exe not found at " + exe.getFullPathName() + " — cannot launch.");

        auto* block = controlBlock.load (std::memory_order_relaxed);
        lastObservedHeartbeat = block != nullptr ? block->heartbeatCounter.load (std::memory_order_relaxed) : 0;
        lastHeartbeatChangeMs = juce::Time::getMillisecondCounterHiRes();

        // Health Check directive: deliberately NOT sandboxAlive = true here,
        // even if attach just succeeded — see this class's own doc comment.
        sandboxAlive.store (false, std::memory_order_release);
        dspStallDetected.store (false, std::memory_order_relaxed);

        // Step 13.5 (Slow Boot Guard): re-armed on every launch — this
        // child hasn't proven itself alive yet, so a death before the next
        // real heartbeat change must not count as a crash.
        hasReachedAliveState = false;

        // RESTART STORM directive (caught live under Step 7's own test —
        // the log showed hundreds of restarts per second): this function is
        // ALSO the one a restart calls, and restartInFlight's whole job is
        // to survive across THIS call until a genuine heartbeat proves the
        // new child alive — clearing it here unconditionally defeated the
        // edge-trigger guard in timerCallback() the instant a restart
        // happened, since the very next tick would see restartInFlight
        // already false again and restart AGAIN, forever. Deliberately NOT
        // reset here; only cleared where a real heartbeat change is
        // observed (see timerCallback()). The very first launch (from
        // initialise(), never a restart) is unaffected — restartInFlight
        // simply starts at its default false and nothing here needs to
        // touch it.
        reRequestEditorWindowIfNeeded();

        startTimer (CrateIPC::livenessCheckIntervalMs);
    }

    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // the pool-claim analogue of launchSandboxProcess() above — mirrors its
    // ControlBlock-writing discipline EXACTLY (same pluginPath/
    // hostSampleRate/hostBlockSize/initialStateData writes, same
    // bufferReadyEvent/lookaheadRequestReadyEvent construction, same
    // heartbeat/timer bookkeeping), but the shared memory FILE was already
    // created/sized by SandboxManager's pool at warm-up time (not here),
    // and instead of sandboxProcess.start() spawning a brand-new OS
    // process, this signals the claim event an already-running, cryosleeping
    // CrateSandbox.exe is blocked on — the entire process-creation latency
    // (CreateProcess, DLL loading, JUCE init) already happened, invisibly,
    // before this plugin was ever dropped. That process's own
    // CryosleepWaitThread wakes, reads the pluginPath this method just
    // wrote, and calls loadHostedPlugin() exactly as a cold-started child
    // would have from its own initialise().
    void claimFromPool()
    {
        terminateSandboxProcess(); // idempotent guard — same reasoning as launchSandboxProcess()'s own

        if (bufferReadyEvent == nullptr || bufferReadyEventInstanceId != instanceId)
        {
            bufferReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getBufferReadyEventName (instanceId));
            bufferReadyEventInstanceId = instanceId;
        }

        if (lookaheadRequestReadyEvent == nullptr || lookaheadRequestReadyEventInstanceId != instanceId)
        {
            lookaheadRequestReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getLookaheadRequestReadyEventName (instanceId));
            lookaheadRequestReadyEventInstanceId = instanceId;
        }

        // Unlike launchSandboxProcess(), no ensureSharedMemoryFileIsSized()
        // call here — SandboxManager's pool already created/sized this
        // exact file when it warmed this slot up, and the CHILD has
        // already mapped it and been sitting on it, idle, ever since.
        attachSharedMemory();

        if (auto* block = controlBlock.load (std::memory_order_relaxed))
        {
            std::memset (block->pluginPath, 0, sizeof (block->pluginPath));
            pluginPathForLaunch.copyToUTF8 (block->pluginPath, (size_t) CrateIPC::ControlBlock::maxPluginPathLength - 1);

            block->hostSampleRate = cachedSampleRate;
            block->hostBlockSize  = cachedBlockSize;

            // A freshly pool-claimed bridge has no prior state to restore —
            // Ghost Reload only ever matters on a RESTART after a crash,
            // which (see terminateSandboxProcess()'s own comment) falls
            // back to the normal cold-start launchSandboxProcess() path,
            // never claimFromPool() again. Explicitly zeroed rather than
            // left whatever the pool's own warm-up left behind (always 0
            // in practice, but explicit is cheap and removes any doubt).
            block->initialStateSize = 0;
        }

        auto* block = controlBlock.load (std::memory_order_relaxed);
        lastObservedHeartbeat = block != nullptr ? block->heartbeatCounter.load (std::memory_order_relaxed) : 0;
        lastHeartbeatChangeMs = juce::Time::getMillisecondCounterHiRes();

        sandboxAlive.store (false, std::memory_order_release);
        dspStallDetected.store (false, std::memory_order_relaxed);
        hasReachedAliveState = false;

        reRequestEditorWindowIfNeeded();

        // THE ACTUAL WAKE-UP: everything above prepared the ControlBlock
        // exactly as a cold launch would have — this is the one call that
        // replaces "spawn a new OS process" with "wake the one already
        // waiting." The CHILD's own CryosleepWaitThread is blocked on the
        // SAME name, constructed from the SAME instanceId.
        CrateIPC::NamedEvent (CrateIPC::getCryosleepClaimEventName (instanceId)).signal();

        startTimer (CrateIPC::livenessCheckIntervalMs);
    }

    // Step 15.2 (The Shared Host Engine) directive: the Tenant Bridge
    // analogue of launchSandboxProcess() above — mirrors its "prepare the
    // shared-memory file BEFORE anything on the other end touches it"
    // discipline exactly (this bridge, not the shared host, creates/sizes/
    // maps this tenant's OWN per-instance file and writes
    // pluginPath/hostSampleRate/hostBlockSize into it), but instead of
    // spawning a private juce::ChildProcess, it invokes onTenantReady() —
    // SandboxManager's hook for dispatching a spawn COMMAND into the
    // Master Control Channel. The already-running Shared Sandbox host is
    // the one that actually creates the AudioPluginInstance/threads that
    // attach to and drive this exact block; this bridge just waits (via
    // the same heartbeat-timeout machinery as isolated mode) for that to
    // happen.
    //
    // Called from initialise() on first attach, AND (Step 15.3) again from
    // reconfigureAsTenant() every time the Quarantine Shuffle re-routes an
    // innocent tenant to a fresh shared-host slot after its previous one
    // died — so, unlike Step 15.2's original version of this method, it
    // DOES need the same idempotent terminateSandboxProcess() guard
    // launchSandboxProcess() already uses: a reconfigure has a previous
    // (now-dead) mapping to safely tear down — waiting for audioThreadActive
    // to clear before this bridge's own controlBlock pointer gets
    // reassigned to the new tenant slot — before attachSharedMemory() below
    // swaps it in. Safe (a no-op) on the very first call too: nothing yet
    // to tear down, sandboxProcess was never started, exactly like
    // launchSandboxProcess()'s own first call.
    void attachAsTenant()
    {
        terminateSandboxProcess();

        // Step 15.2/15.3 directive: rebuilt only when instanceId has
        // actually changed — see bufferReadyEvent's own doc comment. The
        // very first attach always rebuilds (bufferReadyEvent starts
        // null); reconfigureAsTenant()'s fresh instanceId correctly
        // rebuilds again.
        if (bufferReadyEvent == nullptr || bufferReadyEventInstanceId != instanceId)
        {
            bufferReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getBufferReadyEventName (instanceId));
            bufferReadyEventInstanceId = instanceId;
        }

        // Step 17 directive: same lazy-construct/rebuild-on-instanceId-
        // change discipline as bufferReadyEvent above — the Quarantine
        // Shuffle's fresh instanceId on reroute needs this rebuilt too,
        // not left bound to a dead tenant's event name.
        if (lookaheadRequestReadyEvent == nullptr || lookaheadRequestReadyEventInstanceId != instanceId)
        {
            lookaheadRequestReadyEvent = std::make_unique<CrateIPC::NamedEvent> (CrateIPC::getLookaheadRequestReadyEventName (instanceId));
            lookaheadRequestReadyEventInstanceId = instanceId;
        }

        CrateIPC::ensureSharedMemoryFileIsSized (CrateIPC::getSharedMemoryFile (instanceId));
        attachSharedMemory();

        if (auto* block = controlBlock.load (std::memory_order_relaxed))
        {
            std::memset (block->pluginPath, 0, sizeof (block->pluginPath));
            pluginPathForLaunch.copyToUTF8 (block->pluginPath, (size_t) CrateIPC::ControlBlock::maxPluginPathLength - 1);
            block->hostSampleRate = cachedSampleRate;
            block->hostBlockSize  = cachedBlockSize;
        }

        lastObservedHeartbeat = 0;
        lastHeartbeatChangeMs = juce::Time::getMillisecondCounterHiRes();
        sandboxAlive.store (false, std::memory_order_release);
        dspStallDetected.store (false, std::memory_order_relaxed);
        hasReachedAliveState = false;

        if (onTenantReady)
            onTenantReady (pluginPathForLaunch, instanceId);
        else
            logEvent ("attachAsTenant(): no dispatch callback configured — this tenant will never actually be spawned by the shared host.");

        reRequestEditorWindowIfNeeded();

        startTimer (CrateIPC::livenessCheckIntervalMs);
    }

    void terminateSandboxProcess()
    {
        if (sandboxProcess.isRunning())
            sandboxProcess.kill();

        // Step 33 (Cryosleep Architecture) directive: a pool-claimed bridge
        // never launched a process of its OWN via sandboxProcess — the
        // ACTUAL juce::ChildProcess handle that launched it lives in
        // SandboxManager's own pool bookkeeping (moved there the instant it
        // was claimed, never handed to this bridge — see
        // configureFromPool()'s own doc comment for why). This callback is
        // what lets SandboxManager kill/release ITS OWN handle when this
        // bridge tears down. Cleared after firing once so a subsequent
        // idempotent call to this same method (this method is explicitly
        // documented as safe to call repeatedly) never double-releases.
        // Deliberately NOT cleared on a restart-triggered call from
        // launchSandboxProcess() (see that method's own doc comment on
        // why a crashed pool-claimed bridge falls back to a normal cold
        // start for its restart, not another pool claim) — once a
        // restart happens, this bridge genuinely owns its own
        // sandboxProcess from then on, and the ORIGINAL pooled process
        // this callback refers to is correctly released exactly once,
        // here, first.
        if (onPoolProcessRelease)
        {
            onPoolProcessRelease();
            onPoolProcessRelease = nullptr;
        }

        controlBlock.store (nullptr, std::memory_order_release);

        // THE UNMAP RACE directive: never actually free mappedMemory while
        // applyToBuffer() might still be mid-dereference of the pointer it
        // backs — see that method's own comment. This wait is safe HERE
        // (message thread, called from a restart or plugin teardown, never
        // real-time) even though the audio thread itself must never wait
        // for anything. In practice this loop runs zero or one iterations:
        // audioThreadActive is only ever true for the few microseconds
        // applyToBuffer() is actually touching the block.
        //
        // STEP 12.3 (IPC DETOX) hardening: this wait was previously
        // unbounded. Every currently-analysed caller of applyToBuffer()
        // clears audioThreadActive within spinWaitTimeoutMs regardless of
        // success or failure, so this loop is bounded to ~3ms in practice —
        // but "in practice" is not a guarantee. Give it an explicit ceiling
        // so a future bug in the audio-thread path can never leave the
        // message thread (and thus a restart) stuck forever. Hitting the
        // ceiling means audioThreadActive was left set with no audio thread
        // actually running — a real bug — so it is logged loudly rather
        // than silently swallowed.
        const auto unmapWaitStartMs = juce::Time::getMillisecondCounterHiRes();
        constexpr double unmapWaitTimeoutMs = 50.0;

        while (audioThreadActive.load (std::memory_order_acquire))
        {
            if (juce::Time::getMillisecondCounterHiRes() - unmapWaitStartMs > unmapWaitTimeoutMs)
            {
                logEvent ("terminateSandboxProcess: audioThreadActive still set after "
                          + juce::String (unmapWaitTimeoutMs, 0)
                          + "ms — forcing unmap anyway. This should never happen and indicates "
                            "a stuck audio-thread flag; investigate dispatchToSandbox/gatherFromSandbox.");
                break;
            }

            juce::Thread::sleep (0);
        }

        mappedMemory.reset();
    }

    void attachSharedMemory()
    {
        auto file = CrateIPC::getSharedMemoryFile (instanceId);

        if (! file.existsAsFile() || file.getSize() != CrateIPC::sharedMemoryBytes)
            return;

        auto candidate = std::make_unique<juce::MemoryMappedFile> (file, juce::MemoryMappedFile::readWrite);

        if (candidate->getData() != nullptr && candidate->getSize() == (size_t) CrateIPC::sharedMemoryBytes)
        {
            mappedMemory = std::move (candidate);
            controlBlock.store (CrateIPC::getControlBlock (mappedMemory->getData()), std::memory_order_release);
        }
    }

    // Health Check directive: runs at CrateIPC::livenessCheckIntervalMs —
    // comfortably inside heartbeatTimeoutMs, so a stall is never missed by
    // more than one extra tick. This IS the "expensive" side of the check
    // (shared-memory read, elapsed-time math, process management) — the
    // audio thread never runs any of this itself, only reads the
    // sandboxAlive bool this method publishes.
    void timerCallback() override
    {
        // The Parent Push directive (Step 8): an overflow doesn't mean the
        // sandbox is dead or misbehaving — just that more parameter events
        // arrived than were drained between two audio round trips — so
        // unlike a DSP stall, this only logs, it never triggers a restart.
        if (paramQueueOverflowDetected.exchange (false, std::memory_order_acq_rel))
            logEvent ("Parameter queue overflow — one or more parameter events dropped.");

        // Step 15.4 (The DSP Soft-Mute) directive: see fadeEngagedFlag's
        // own doc comment — this is the only place the fade path is ever
        // logged, since applyFadeOrSilence() itself runs on the audio
        // thread and can't do file I/O.
        if (fadeEngagedFlag.exchange (false, std::memory_order_acq_rel))
            logEvent ("DSP Soft-Mute engaged — fading out over " + juce::String (dspFadeOutMs, 0) + "ms instead of a hard silence cut.");

        // Continuous State Sync directive (Step 11): poll the PUSH channel
        // — try-lock only, message thread can simply check again next tick
        // if genuinely contended, no need to wait. A full copy into
        // lastKnownState happens at most once per tick, at GUI-timer speed,
        // never per audio block.
        if (auto* stateBlock = controlBlock.load (std::memory_order_relaxed))
        {
            if (! stateBlock->stateChunkLock.test_and_set (std::memory_order_acquire))
            {
                if (stateBlock->stateChunkAvailable.load (std::memory_order_relaxed))
                {
                    const auto size = (size_t) stateBlock->stateChunkSize.load (std::memory_order_relaxed);
                    lastKnownState = juce::MemoryBlock (stateBlock->stateChunkData, size);
                    stateBlock->stateChunkAvailable.store (false, std::memory_order_relaxed);
                    logEvent ("Received plugin state chunk (" + juce::String ((int) size) + " bytes).");
                }

                stateBlock->stateChunkLock.clear (std::memory_order_release);
            }

            // Step 13.5 (Authentic VST3 UID) directive: poll the CHILD's
            // one-shot pluginUID push. Only needs to happen once per bridge
            // lifetime in practice (resolvedPluginUID is never cleared —
            // see its own doc comment) but checking every tick is cheap and
            // means a restart's freshly-scanning child is picked up exactly
            // the same way as the very first launch's.
            if (resolvedPluginUID.isEmpty() && stateBlock->pluginUIDReady.load (std::memory_order_acquire))
            {
                resolvedPluginUID = juce::String (juce::CharPointer_UTF8 (stateBlock->pluginUID));
                logEvent ("Resolved authentic plugin UID from child: " + resolvedPluginUID + " [instanceId=" + instanceId + "]");

                // Step 14 directive: publish path->UID into the shared
                // registry the instant it's known, so SandboxManager's NEXT
                // load of this same file (a fresh CrateSandboxBridge
                // instance, which starts with no memory of this one) can
                // find it without booting a scan of its own.
                PluginHealthRegistry::getInstance().recordResolvedUID (pluginPathForLaunch, resolvedPluginUID);

                // Step 22 (The Profiling Database / The Warden) directive:
                // resolved in the SAME push, gated by the SAME
                // pluginUIDReady flag — see ControlBlock::vendorName's own
                // doc comment for why no second ready flag was needed.
                const auto vendorName = juce::String (juce::CharPointer_UTF8 (stateBlock->vendorName));
                logEvent ("Resolved plugin vendor from child: " + vendorName + " [instanceId=" + instanceId + "]");
                PluginHealthRegistry::getInstance().recordResolvedVendor (resolvedPluginUID, vendorName);
            }

            // Step 31 (Real IPC Parameter Sync) directive: build the REAL
            // parameter list the FIRST tick metadata is seen ready —
            // construction happens before any IPC connection exists at
            // all, so this can only ever happen later, asynchronously,
            // same timing reasoning as vendor/UID resolution above.
            if (! realParameterListBuilt && stateBlock->paramMetadataReady.load (std::memory_order_acquire))
                buildRealParameterListFromMetadata (*stateBlock);

            // Step 31 directive: the value-readback half — if the user
            // twiddled a knob INSIDE the reparented native UI, reflect that
            // into our own AutomatableParameter so the Device Chain's knob
            // position and any future automation read/write stays in sync.
            // lastSeenParamValueRevision starts at 0 (matching the
            // ControlBlock's own default), so this only does real work once
            // the CHILD has actually published at least one changed value.
            if (realParameterListBuilt)
            {
                const auto revision = stateBlock->paramValueRevision.load (std::memory_order_acquire);

                if (revision != lastSeenParamValueRevision)
                {
                    lastSeenParamValueRevision = revision;

                    for (int i = 0; i < (int) syncedParameters.size(); ++i)
                    {
                        const float childValue = stateBlock->paramCurrentValues[i].load (std::memory_order_relaxed);

                        if (std::abs (childValue - syncedParameters[i]->getCurrentValue()) > 0.0001f)
                            syncedParameters[i]->applyReadbackValue (childValue);
                    }
                }
            }
        }

        // Echo/Phase Test directive: a DSP stall reported by the audio
        // thread is treated exactly like a heartbeat timeout — dead is
        // dead, regardless of which signal noticed it first — but it's
        // checked FIRST and unconditionally, since a stall can happen while
        // the heartbeat is still ticking along fine (the heartbeat thread
        // and the DSP round trip are two independent threads in the child).
        //
        // Step 19 hardening: once lookahead mode is active, applyToBuffer()
        // never calls dispatchToSandbox()/gatherFromSandbox() again (see
        // that method's own doc comment) — gatherFromSandbox() is the ONLY
        // place that sets dspStallDetected, so the flag it publishes is
        // fundamentally stale/meaningless in this mode. Found the hard way:
        // right at the instant enableLookaheadMode() flips
        // lookaheadModeRequested, there's a genuine race where ONE regular
        // round trip can already be in flight while the CHILD's
        // AudioBridgeThread is simultaneously transitioning into its own
        // isLookaheadMode skip-guard — that single abandoned round trip
        // sets dspStallDetected exactly once, which this check (unguarded)
        // treated as a real death and restarted the entire CrateSandbox.exe
        // child process, silently killing the in-flight LookaheadWorkerThread
        // mid-run (no crash, no log — the process just got replaced out
        // from under it), which is what stalled the Time-Slip Engine Test
        // at "-1 samples buffered" for its whole timeout despite requests
        // visibly reaching the ring. Ignoring the flag once lookahead mode
        // is requested closes that race at the source rather than chasing
        // its downstream symptom.
        if (! lookaheadModeRequested.load (std::memory_order_acquire)
                && dspStallDetected.exchange (false, std::memory_order_acq_rel))
        {
            if (sandboxAlive.exchange (false, std::memory_order_acq_rel))
                logEvent ("DSP round trip exceeded " + juce::String (CrateIPC::spinWaitTimeoutMs)
                              + "ms — declaring DEAD, bridge output silenced.");

            if (! restartInFlight)
            {
                restartInFlight = true;

                // The Snitch directive (Step 13): restartInFlight is the
                // same edge-trigger guard that already prevents this branch
                // from firing more than once per genuine death — reusing
                // it here means the crash count increments exactly once per
                // dead child, not once per tick the child stays dead.
                //
                // Slow Boot Guard (Step 13.5): only count it if this launch
                // ever proved itself alive first — a DSP stall can only
                // happen once the audio round trip was actually running,
                // so in practice this branch is always past that point,
                // but the check is kept for symmetry with the heartbeat
                // branch below and as a defensive belt-and-suspenders.
                if (hasReachedAliveState)
                    PluginHealthRegistry::getInstance().recordCrash (effectivePluginUID());
                else
                    logEvent ("DSP stall before first heartbeat — treating as failed initialization, not a crash.");

                // Step 15.3 (Self-Healing & Auto-Quarantine) directive: a
                // Tenant Bridge never owns the process it depends on (the
                // shared host), so it has no process of its own to
                // relaunch — but it CAN ask SandboxManager for a fresh
                // routing verdict and reconnect accordingly (the
                // Quarantine Shuffle). recordCrash() just above already
                // ran, so by the time onNeedsRerouting() is invoked the
                // Health Registry already reflects this death — exactly
                // what lets SandboxManager correctly decide whether THIS
                // tenant just crossed the Solitary Confinement threshold.
                // Isolated mode keeps its existing resurrection behavior
                // unchanged.
                if (isTenantMode)
                {
                    logEvent ("Shared Sandbox host connection lost (DSP stall) — requesting Quarantine Shuffle re-route.");

                    if (onNeedsRerouting)
                        onNeedsRerouting (*this);
                    else
                        logEvent ("onNeedsRerouting not configured — Tenant Bridge stays dead.");
                }
                else
                {
                    logEvent ("Restarting CrateSandbox child process (DSP stall)...");
                    launchSandboxProcess();
                }
            }

            return;
        }

        auto* block = controlBlock.load (std::memory_order_relaxed);

        if (block == nullptr)
        {
            attachSharedMemory();
            block = controlBlock.load (std::memory_order_relaxed);

            if (block != nullptr)
            {
                lastObservedHeartbeat = block->heartbeatCounter.load (std::memory_order_relaxed);
                lastHeartbeatChangeMs = juce::Time::getMillisecondCounterHiRes();
                // Same reasoning as launchSandboxProcess(): attach proves the
                // bytes are readable, not that anything is actively pulsing —
                // sandboxAlive stays false until a real counter change is seen.
                logEvent ("Attached to sandbox shared memory — waiting for first heartbeat.");
            }

            return;
        }

        const auto current = block->heartbeatCounter.load (std::memory_order_relaxed);
        const auto now      = juce::Time::getMillisecondCounterHiRes();

        if (current != lastObservedHeartbeat)
        {
            lastObservedHeartbeat = current;
            lastHeartbeatChangeMs = now;

            if (! sandboxAlive.exchange (true, std::memory_order_acq_rel))
            {
                logEvent ("Sandbox heartbeat confirmed — declaring ALIVE, bridge output live.");

                // Step 13: the false->true transition is the earliest point
                // the parent can be sure THIS launch actually took (a dead
                // or hung child never reaches a first heartbeat) — the
                // natural "successfulLoads" signal.
                PluginHealthRegistry::getInstance().recordSuccessfulLoad (effectivePluginUID());
            }

            // Step 13.5 (Slow Boot Guard): set on every heartbeat change,
            // not just the false->true edge above — a plugin that's ALREADY
            // alive but hasn't hit the edge again yet (impossible given
            // sandboxAlive is already true, but kept unconditional here
            // rather than nested in the exchange's if for clarity) still
            // needs this true so a LATER death on this same launch counts.
            hasReachedAliveState = true;

            restartInFlight = false; // heartbeat proven fresh — a future stall may trigger another restart
            return;
        }

        // Step 15.3 hotfix directive: a Tenant Bridge waiting on its FIRST
        // ever heartbeat (hasReachedAliveState still false — set on THIS
        // attach cycle by attachAsTenant()/configureAsTenant()) gets a far
        // more generous grace period than a steady-state stall check — see
        // tenantFirstConnectTimeoutMs's own doc comment for the reproduced
        // restart-storm this fixes. Isolated mode is unaffected (isTenantMode
        // is always false there, so effectiveHeartbeatTimeoutMs is always
        // just heartbeatTimeoutMs, unchanged).
        //
        // Step 19 hardening (Lookahead Liveness Guard): heartbeatTimeoutMs
        // is a deliberately aggressive 50ms — tuned for the steady-state
        // round trip's tight, real-time cadence. Lookahead mode's workload
        // is structurally different for its ENTIRE duration, not just at
        // switch-on: LookaheadWorkerThread drains its ring "as fast as the
        // CPU allows" (see that class's own doc comment) in CPU-heavy
        // bursts — real VST3 processing plus (in a large multi-track test)
        // genuine contention from every other track's own sandbox process —
        // and that bursty profile can starve the heartbeat thread's own
        // scheduling for well over 50ms at any point while it's active, not
        // just in a brief window right after enabling it. A time-limited
        // grace period was tried first and confirmed insufficient — a
        // restart still landed right at the boundary, meaning the stall
        // wasn't a one-time startup blip. Using a wider timeout for the
        // WHOLE time lookahead mode is requested (not a fixed window after
        // enabling it) matches the actual shape of the workload instead of
        // guessing at how long its "slow start" lasts. Isolated mode's
        // normal round trip is completely unaffected: lookaheadModeRequested
        // is false there, so effectiveHeartbeatTimeoutMs stays the tight
        // 50ms this whole mechanism was built around.
        const bool inLookaheadMode = lookaheadModeRequested.load (std::memory_order_acquire);

        // KNOWN ISSUE (disclosed, not resolved): confirmed via direct
        // testing that the CHILD's heartbeat stalls INDEFINITELY (tested
        // up to 18s with zero recovery) the moment LookaheadWorkerThread
        // feeds Rift Filter Lite genuinely non-silent audio for the first
        // time — every earlier test in this codebase's history fed it
        // either silence or a synthetic tone through this specific path,
        // so this exact condition was never actually exercised until the
        // WaveNode rewrite started producing real extracted audio. Every
        // OTHER thread in the child (including the fully independent,
        // dependency-free HeartbeatThread) stalls too, meaning this isn't
        // a lock-contention slowdown — it's process-wide, consistent with
        // the third-party plugin itself hanging/crashing internally on
        // real signal in a way that takes the whole process down with it.
        // A wider timeout here doesn't fix the underlying hang, only how
        // quickly it's detected — kept at the same precedented
        // tenantFirstConnectTimeoutMs value used for the DSP-stall branch,
        // not loosened further, since a longer wait doesn't help once the
        // hang has already happened.
        const auto effectiveHeartbeatTimeoutMs = (isTenantMode && ! hasReachedAliveState)
                                                      ? CrateIPC::tenantFirstConnectTimeoutMs
                                                      : inLookaheadMode
                                                          ? CrateIPC::tenantFirstConnectTimeoutMs
                                                          : CrateIPC::heartbeatTimeoutMs;

        if (now - lastHeartbeatChangeMs >= (double) effectiveHeartbeatTimeoutMs)
        {
            if (sandboxAlive.exchange (false, std::memory_order_acq_rel))
                logEvent ("Sandbox heartbeat stalled for over " + juce::String (effectiveHeartbeatTimeoutMs)
                              + "ms — declaring DEAD, bridge output now silenced.");

            // Edge-triggered: without restartInFlight this would call
            // launchSandboxProcess() (and spawn a brand new child process)
            // on every single tick for as long as the sandbox stays dead —
            // a real bug, not a hypothetical one. One attempt per stall,
            // re-armed only once a fresh heartbeat is observed above.
            if (! restartInFlight)
            {
                restartInFlight = true;

                // The Snitch directive (Step 13) — see the identical
                // comment in the DSP-stall branch above; same guard, same
                // reasoning, this is the heartbeat-timeout path to the same
                // "child is dead" conclusion.
                //
                // Slow Boot Guard (Step 13.5): THIS is the branch that
                // actually matters for it — a cold VST3 load (Kontakt,
                // sample libraries, anything with a multi-second first
                // load) can legitimately blow through heartbeatTimeoutMs
                // before ever reaching its first heartbeat. That's a failed
                // (or merely slow) initialization, not evidence the plugin
                // is unstable, so it must not touch the crash count.
                if (hasReachedAliveState)
                    PluginHealthRegistry::getInstance().recordCrash (effectivePluginUID());
                else
                    logEvent ("Heartbeat timeout before first ALIVE — treating as failed/slow initialization, not a crash.");

                // Step 15.3 directive: same reasoning as the DSP-stall
                // branch above — a Tenant Bridge's heartbeat can stall
                // either because THIS tenant's own plugin died inside the
                // shared host, or because the shared host PROCESS ITSELF
                // died (taking every tenant with it — see serviceTenant()'s
                // own doc comment in Main.cpp on why one hostile tenant can
                // do exactly that; this is precisely "Guilt by
                // Association" — every tenant sharing that host detects
                // the SAME stalled heartbeat independently and gets
                // blamed identically, since there is no way to tell which
                // one actually caused it). Either way, this bridge asks
                // for a fresh routing verdict via the Quarantine Shuffle
                // instead of trying to relaunch a process it never owned.
                if (isTenantMode)
                {
                    logEvent ("Shared Sandbox host connection lost (heartbeat timeout) — requesting Quarantine Shuffle re-route.");

                    if (onNeedsRerouting)
                        onNeedsRerouting (*this);
                    else
                        logEvent ("onNeedsRerouting not configured — Tenant Bridge stays dead.");
                }
                else
                {
                    logEvent ("Restarting CrateSandbox child process...");
                    launchSandboxProcess();
                }
            }
        }
    }

    static void logEvent (const juce::String& message)
    {
        static const auto logFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                         .getChildFile ("CrateSandboxBridge.log");

        logFile.appendText (juce::Time::getCurrentTime().toString (true, true, true, true) + "  " + message + "\n",
                             false, false, "\n");
    }

    // Step 9 directive: a fresh, unique ID per bridge instance, generated
    // once at construction, shared with this instance's own CrateSandbox.exe
    // via a command-line argument in launchSandboxProcess().
    //
    // Step 15.2 directive: no longer const — configureAsTenant() overwrites
    // this with an EXTERNALLY generated instanceID (from SandboxManager)
    // for a Tenant Bridge, BEFORE initialise() ever runs (i.e. before
    // anything below derives a name from it — see bufferReadyEvent's own
    // updated comment on why it had to stop being constructed inline here
    // too).
    juce::String instanceId { juce::Uuid().toString() };

    // Step 19 hardening directive: guards initialise() against TE's own
    // repeated re-invocation of it on the SAME already-running bridge (see
    // initialise()'s own doc comment for how this was found and why it
    // matters) — true once the sandbox child/tenant attachment has been
    // launched for the first time, so a same-config re-entry becomes a
    // no-op instead of tearing down and relaunching everything.
    bool hasLaunchedOnce = false;

    // Step 15.2 directive: false by default — an isolated bridge, spawning
    // and owning its own private CrateSandbox.exe, exactly as every prior
    // step built. Set true only by configureAsTenant(); once true, this
    // bridge never touches sandboxProcess and never attempts to restart
    // anything on death (see the two timerCallback() restart sites' own
    // updated comments) — a Tenant Bridge doesn't own the process it
    // depends on, so it has no business trying to relaunch it.
    bool isTenantMode = false;

    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // set by configureFromPool(), same "false by default, opt-in" shape as
    // isTenantMode above. hasClaimedFromPoolOnce guards claimFromPool()
    // itself from ever firing twice — see initialise()'s own comment on
    // why a later re-invocation must fall through to a normal cold start
    // instead. onPoolProcessRelease is SandboxManager's hook for reclaiming
    // ITS OWN juce::ChildProcess handle on teardown — see
    // configureFromPool()'s and terminateSandboxProcess()'s own comments.
    bool isPooledClaim = false;
    bool hasClaimedFromPoolOnce = false;
    std::function<void()> onPoolProcessRelease;

    // Step 15.2 directive: set by configureAsTenant(), invoked from
    // attachAsTenant() the moment this tenant's own shared memory is
    // prepared and ready for the shared host to load into — see
    // configureAsTenant()'s own doc comment for why this indirection exists
    // instead of a direct SandboxManager call.
    std::function<void (const juce::String& pluginPath, const juce::String& instanceId)> onTenantReady;

    // Step 15.3 (Self-Healing & Auto-Quarantine) directive: set by
    // configureAsTenant() alongside onTenantReady, invoked from this
    // bridge's OWN death-detection (see the two timerCallback() restart
    // branches) — this is the Quarantine Shuffle's entry point. Takes
    // *this by reference rather than being a no-argument std::function
    // because SandboxManager needs to call this bridge's own
    // getConfiguredPluginPath()/convertToIsolatedAndRelaunch()/
    // reconfigureAsTenant() to actually carry out the reroute.
    std::function<void (CrateSandboxBridge&)> onNeedsRerouting;

    // Step 15.4 (The Teardown Protocol) directive: set by configureAsTenant(),
    // invoked from deinitialise() — see that callback's own doc comment.
    std::function<void (const juce::String& instanceId)> onTenantRemoved;

    // Step 13 directive (The Profiling Database): the plugin FILE path —
    // still hardcoded (real plugin selection is later work), used to tell
    // the child what to load. instanceId (above) identifies ONE running
    // child process; this identifies the plugin FILE — neither is the
    // Health Registry's actual key anymore as of Step 13.5, see
    // resolvedPluginUID/effectivePluginUID() below.
    //
    // Step 15.2 directive: no longer const, same reasoning as instanceId —
    // configureAsTenant() can overwrite it (currently a no-op in practice,
    // since every tenant loads the same hardcoded test plugin, but a
    // Tenant Bridge should never silently assume the FILE PATH it was
    // configured with and the one baked into this default agree).
    juce::String pluginPathForLaunch { getTestPluginPath() };

    // Step 30 (Completing the Proxy Illusion) directive: seeded by
    // SandboxManager::createSandboxPlugin() the moment this bridge is
    // created, via setImpersonatedDescription() — see getName()/getVendor()/
    // getSelectableDescription()'s own overrides above, which read from
    // this instead of ever exposing this class's own internal identity.
    juce::PluginDescription impersonatedDescription;

    // Step 13.5 directive (Authentic VST3 UID): filled in from the
    // CHILD's own pluginUID/pluginUIDReady push (see timerCallback() and
    // CrateIPCConstants.h's own comment on why this is CHILD-resolved, not
    // scanned here in the PARENT). Empty until the first child of this
    // bridge's lifetime successfully scans the plugin; deliberately never
    // cleared on restart — the same physical file resolves to the same
    // identifier every time, so once known it stays known even while a
    // fresh child is still booting.
    juce::String resolvedPluginUID;

    // effectivePluginUID() directive: falls back to the FILE PATH only for
    // the brief window before a child has ever reported the real UID (or
    // if a malformed file never scans successfully at all) — this keeps
    // the Health Registry working end-to-end from the very first launch
    // rather than silently dropping every crash that happens to land
    // before pluginUIDReady is observed.
    juce::String effectivePluginUID() const
    {
        return resolvedPluginUID.isNotEmpty() ? resolvedPluginUID : pluginPathForLaunch;
    }

    // Step 13.5 directive (Slow Boot Guard): true only once THIS launch's
    // child has proven itself alive via a real heartbeat change (see the
    // sandboxAlive.exchange(true, ...) branch below) — reset to false on
    // every launchSandboxProcess() call. A death observed before this is
    // ever set is a failed INITIALIZATION (cold VST3 load slower than
    // heartbeatTimeoutMs, a malformed file, whatever) — not a crash of a
    // plugin that was actually running — and must not increment the
    // Health Registry's crash count.
    bool hasReachedAliveState = false;

    // Step 15.3 hotfix directive (GUI Reconnect After Reroute): set once,
    // forever, the first time requestEditorWindow() is ever called — this
    // is what makes reRequestEditorWindowIfNeeded() work identically
    // across an isolated restart (same instanceId/file — the CHILD's own
    // preserve-across-reset dance would ALSO cover this case, redundantly
    // but harmlessly) and a Quarantine Shuffle reroute (a FRESH instanceId/
    // file every time — nothing on the child side to preserve FROM, since
    // that file was never touched by any previous process). The intent
    // "this bridge's caller wants a window" belongs on the bridge, which
    // outlives any one file/child/mode; it never belonged solely inside a
    // shared-memory block that a reroute can freely discard.
    bool editorWindowWasRequested = false;

    // Step 37 (The Debt Sweep) directive, Task 4: CrateIPC::JobObjectProcess,
    // not juce::ChildProcess — see its own doc comment in CrateIPCConstants.h.
    // Assigns every isolated-mode sandbox process to the shared Job Object
    // the instant it launches, so it can never outlive a hard-killed/
    // crashed Parent the way a plain juce::ChildProcess handle could.
    CrateIPC::JobObjectProcess sandboxProcess;
    std::unique_ptr<juce::MemoryMappedFile> mappedMemory;

    // The Hybrid Sync Pivot directive: constructed ONCE for this bridge
    // instance's whole lifetime, not per-launch — a named OS event is a
    // kernel object independent of the shared-memory file, so it doesn't
    // need to be (and shouldn't be) recreated across a child restart. As
    // long as this handle stays open, the CHILD's own CreateEventW on every
    // one of its launches (including after a restart) attaches to this same
    // object rather than racing to create it first.
    //
    // Step 15.2 directive: no longer a plain member constructed inline from
    // instanceId at THIS object's own construction time — a Tenant Bridge's
    // real instanceId isn't known until configureAsTenant() runs, which
    // happens AFTER construction (SandboxManager calls it on an
    // already-constructed bridge, before insertion/initialise()). A member
    // initializer can't be deferred, so this is built lazily instead,
    // exactly once per DISTINCT instanceId, inside launchSandboxProcess()/
    // attachAsTenant().
    //
    // Step 15.3 directive: "exactly once per instanceId," not "exactly
    // once ever" — the Quarantine Shuffle's reconfigureAsTenant() gives an
    // ALREADY-LAUNCHED bridge a BRAND NEW instanceId (a fresh tenant slot,
    // since the old one belonged to a shared host that just died), which
    // isolated mode's restart never does (its instanceId is stable for the
    // bridge's whole lifetime). bufferReadyEventInstanceId tracks which
    // instanceId THIS event object currently corresponds to, so the guard
    // correctly rebuilds it when (and only when) the id has actually
    // changed, instead of silently keeping a handle bound to a dead
    // tenant's event name forever.
    std::unique_ptr<CrateIPC::NamedEvent> bufferReadyEvent;
    juce::String bufferReadyEventInstanceId;

    // Step 17 (The Lookahead IPC Pipeline) directive: wakes the CHILD's
    // LookaheadWorkerThread — same lazy-construct/rebuild-on-instanceId
    // discipline as bufferReadyEvent above, same reasons.
    std::unique_ptr<CrateIPC::NamedEvent> lookaheadRequestReadyEvent;
    juce::String lookaheadRequestReadyEventInstanceId;

    // A NEW DATA RACE THIS STEP INTRODUCES AND CLOSES directive: atomic, not
    // a plain pointer — see this class's own doc comment. Loaded with
    // acquire ordering on the audio thread, stored with release ordering
    // wherever the message thread (re)assigns it.
    std::atomic<CrateIPC::ControlBlock*> controlBlock { nullptr };

    // THE UNMAP RACE directive: true for exactly the duration applyToBuffer()
    // is dereferencing controlBlock/mappedMemory — audio-thread-written,
    // message-thread-read-and-waited-on. See applyToBuffer() and
    // terminateSandboxProcess() for the full reasoning.
    std::atomic<bool> audioThreadActive { false };

    // Health Check directive: the ONE thing the audio thread reads — see
    // applyToBuffer() and this class's own doc comment.
    std::atomic<bool> sandboxAlive { false };

    // The Parent Push & Spin-Wait directive: audio-thread-written,
    // message-thread-read-and-cleared — see this class's own doc comment
    // for why the audio thread can never call logEvent() itself.
    std::atomic<bool> dspStallDetected { false };

    // Step 15.4 (The DSP Soft-Mute) directive: same contract as
    // dspStallDetected above — applyFadeOrSilence() runs on the audio
    // thread and can never call logEvent() (file I/O) directly, so it
    // just publishes this flag the FIRST time a fade episode starts;
    // timerCallback() (message thread) does the actual logging. Exists
    // purely so the fade path is independently verifiable in the log
    // during a real crash test, not just present in the source.
    std::atomic<bool> fadeEngagedFlag { false };

    // The Parent Push directive (Step 8): same contract as dspStallDetected
    // above, for the parameter ring buffer's own overflow case — see
    // setParameterEvent().
    std::atomic<bool> paramQueueOverflowDetected { false };

    // Step 9 directive: audio-thread-written (via a CAS retry loop, not a
    // mutex), read from any thread via getMaxRoundTripMsObserved() — see
    // applyToBuffer() and that getter's own comment.
    std::atomic<double> maxRoundTripMsObserved { 0.0 };

    // Message-thread-only bookkeeping — timerCallback() is the sole reader
    // and writer of all three, so no atomics needed here.
    uint32_t lastObservedHeartbeat = 0;
    double lastHeartbeatChangeMs = 0.0;
    bool restartInFlight = false;

    // Step 31 (Real IPC Parameter Sync) directive: message-thread-only,
    // same reasoning as the three above — timerCallback() is the sole
    // reader/writer. syncedParameters is non-owning (raw pointers) — same
    // convention as te::ExternalPlugin's own autoParamForParamNumbers array;
    // lifetime is owned by whatever addAutomatableParameter()'s own
    // internal storage does with the Ptr it constructs from each one.
    bool realParameterListBuilt = false;
    uint32_t lastSeenParamValueRevision = 0;
    std::vector<CrateSyncedParameter*> syncedParameters;

    // The Initialization Payload directive (Step 7): captured from the
    // REAL te::PluginInitialisationInfo in initialise(), written into the
    // shared ControlBlock by every launchSandboxProcess() call (including
    // restarts) — message-thread-only, no atomics needed.
    double cachedSampleRate = 44100.0;
    int cachedBlockSize = 512;

    // Continuous State Sync directive (Step 11): the most recent state
    // chunk received from the CHILD — see timerCallback() (receives it) and
    // launchSandboxProcess() (writes it into the NEXT child's own
    // initial-load channel, including on a restart). Message-thread-only.
    juce::MemoryBlock lastKnownState;

    // Step 9.1 (Scatter-Gather & Yield Refactor) directive: the handoff
    // between dispatchToSandbox() and this SAME instance's own matching
    // gatherFromSandbox() call — plain fields, not atomics, since exactly
    // one thread ever touches a given instance's own dispatch/gather pair
    // (see dispatchToSandbox()'s own comment on why this holds for both
    // the real per-block path and the scale test's scatter-then-gather
    // passes).
    int dispatchNumChannels = 0;
    int dispatchNumSamples = 0;
    double dispatchStartMs = 0.0;

    // Step 15.4 (The DSP Soft-Mute) directive: the LAST genuinely valid
    // block this bridge ever produced — pre-allocated once in initialise()
    // (Zero Allocation Rule, same as AudioBridgeThread's own workBuffer),
    // refreshed on every SUCCESSFUL round trip. Audio-thread-owned, same
    // plain-field reasoning as dispatchNumChannels/dispatchNumSamples
    // above — exactly one thread ever touches a given bridge instance's
    // own dispatch/gather pair.
    juce::AudioBuffer<float> lastValidOutput;
    int lastValidNumSamples = 0;
    bool hasValidLastOutput = false;

    // How many samples of the current fade-out episode are left.
    // Negative = idle (no fade in progress — the next failure starts a
    // FRESH one at full volume); zero = this episode's fade has fully
    // decayed to silence; positive = actively ramping down. Reset to -1
    // on every successful round trip — see applyFadeOrSilence()'s own doc
    // comment for the full state machine.
    int fadeSamplesRemaining = -1;

    static constexpr double dspFadeOutMs = 15.0; // within the 5-20ms range this step asked for

    // Step 18 (The Time-Slip Engine) directive: the Parent-side accumulator
    // and its supporting state. timeSlipBuffer itself is constructed
    // inline (no allocation until prepare() runs in initialise()).
    // lookaheadModeRequested is checked on the AUDIO thread (applyToBuffer()),
    // so it's atomic even though it's only ever written from the message
    // thread (enableLookaheadMode()). lastReadSamplePosition is written by
    // the audio thread, read by LookaheadProducerThread — the reverse
    // direction of nextProduceSamplePosition/flushGenerationStartPosition,
    // which are written by whichever thread calls flushTimeSlipBuffer()
    // (currently message-thread-only, but kept atomic defensively) and
    // read/advanced by LookaheadProducerThread.
    TimeSlipBuffer timeSlipBuffer;
    std::unique_ptr<LookaheadProducerThread> lookaheadProducerThread;
    std::atomic<bool> lookaheadModeRequested { false };
    std::atomic<int64_t> lastReadSamplePosition { -1 };
    std::atomic<int64_t> nextProduceSamplePosition { 0 };
    std::atomic<int64_t> flushGenerationStartPosition { 0 };

    // Step 21 (The Watchdog) directive: LookaheadProducerThread-only, same
    // "single thread owner, no atomics needed" reasoning as this class's
    // other producer-thread-private fields — see pumpLookaheadPipeline()'s
    // own doc comment for the batch-tracking logic this drives.
    double lookaheadWatchdogBatchStartMs = -1.0;
    static constexpr double lookaheadWatchdogTimeoutMs = 1000.0;

    // Step 21 (Auto-Demotion) directive: set once, forever, the moment the
    // Guillotine drops on this bridge instance — read by
    // pumpLookaheadPipeline() (LookaheadProducerThread) and written by
    // triggerLookaheadWatchdog()'s callAsync-dispatched body (message
    // thread), so genuinely cross-thread, unlike lookaheadWatchdogBatchStartMs
    // above. Once true, this bridge never attempts lookahead mode again for
    // the rest of its lifetime — see enableLookaheadMode()'s own registry
    // check for why a FUTURE bridge loading the same plugin skips it too.
    std::atomic<bool> lookaheadPermanentlyDemoted { false };

    // Step 15.4 directive: called instead of a raw fc.destBuffer->clear()
    // everywhere this bridge has no valid audio to give the engine right
    // now (sandbox not alive, a bad buffer, or a spin-wait timeout) —
    // replaces an instant, clicky silence/DC-jump with a fast linear
    // fade-out FROM the last genuinely valid block this bridge produced,
    // over dspFadeOutMs. Honestly a MASKING technique, not a fix for the
    // underlying gap: once fadeSamplesRemaining reaches zero, this still
    // outputs real silence for as long as the sandbox stays dead — there
    // is no "future" audio to substitute (see this class's own doc
    // comment on why CrateAnticipativeWrapper's pre-rendering can't apply
    // here). The goal is turning an instant discontinuity into something
    // closer to a fast ducking effect.
    void applyFadeOrSilence (const te::PluginRenderContext& fc)
    {
        using CB = CrateIPC::ControlBlock;

        if (fc.destBuffer == nullptr)
            return;

        const int numChannels = juce::jmin (fc.destBuffer->getNumChannels(), CB::maxChannels);
        const int numSamples  = fc.bufferNumSamples;

        if (! hasValidLastOutput)
        {
            fc.destBuffer->clear (fc.bufferStartSample, numSamples);
            return;
        }

        // First failed block since the last success — start a fresh,
        // full-volume fade right now.
        if (fadeSamplesRemaining < 0)
        {
            fadeSamplesRemaining = juce::jmax (1, (int) (dspFadeOutMs * 0.001 * juce::jmax (1.0, cachedSampleRate)));
            fadeEngagedFlag.store (true, std::memory_order_release);
        }

        if (fadeSamplesRemaining == 0)
        {
            fc.destBuffer->clear (fc.bufferStartSample, numSamples);
            return;
        }

        const int fadeTotalSamples = juce::jmax (1, (int) (dspFadeOutMs * 0.001 * juce::jmax (1.0, cachedSampleRate)));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* out = fc.destBuffer->getWritePointer (ch, fc.bufferStartSample);
            auto* src = lastValidOutput.getReadPointer (ch);

            for (int s = 0; s < numSamples; ++s)
            {
                const int samplesLeftAtThisSample = fadeSamplesRemaining - s;

                if (samplesLeftAtThisSample <= 0 || s >= lastValidNumSamples)
                    out[s] = 0.0f;
                else
                    out[s] = src[s] * ((float) samplesLeftAtThisSample / (float) fadeTotalSamples);
            }
        }

        for (int ch = numChannels; ch < fc.destBuffer->getNumChannels(); ++ch)
            fc.destBuffer->clear (ch, fc.bufferStartSample, numSamples);

        fadeSamplesRemaining = juce::jmax (0, fadeSamplesRemaining - numSamples);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateSandboxBridge)
};
