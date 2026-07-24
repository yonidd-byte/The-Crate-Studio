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
#include "SandboxAirlock.h"
#include "FlightRecorder.h"

#include <cstring>
#include <cmath>
#include <functional>
#include <vector>
#include <deque>
#include <mutex>
#include <set>

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
    //
    // Step 79 (Pre-Emptive Native Parenting) directive: the CALLER (see
    // CrateEditorComponent::timerCallback()) is responsible for never
    // reaching this call until AirlockHWNDComponent::
    // ensureSlotReadyAndPublished() has already returned true — i.e.
    // publishHostSlotHandle() has already landed in the ControlBlock. The
    // CHILD reads hostSlotHwndValue exactly once, before ever creating an
    // editor; if this were ever called first, the CHILD could create its
    // editor against a stale-or-zero slot value and fall back to floating
    // as its own top-level window again, the exact bug this whole step
    // fixes.
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

    // Zero-Dropout Bridge directive (Step 52): OPT-IN, defaulted false.
    // CrateStressTest.cpp's own 50-instance scale test AND
    // runEchoPhaseTest() both insert a CrateSandboxBridge onto a real
    // (non-live) Edit track and then directly measure
    // getMaxRoundTripMsObserved() — a figure that ONLY ever updates via
    // the direct dispatchToSandbox()/gatherFromSandbox() synchronous IPC
    // round trip (see applyToBuffer()'s own branch). Auto-engaging
    // lookahead mode unconditionally for every non-live bridge (this
    // bridge's own OWN refreshAutoLookaheadState() would otherwise judge
    // a plain test track as "not live" exactly like a real one) would
    // silently redirect those SAME instances onto readFromTimeSlipBuffer()
    // instead, permanently stalling maxRoundTripMsObserved at its initial
    // 0.0 and breaking two already-proven, previously-passing verification
    // paths for a feature they were never meant to exercise. Gating
    // real Zero-Dropout auto-engagement behind an explicit call from
    // wherever a plugin is ACTUALLY inserted for real user playback (not
    // yet wired here — see this method's own call site in
    // SandboxManager/the Device Chain insertion flow for where that call
    // needs to be added) keeps every existing test's behavior completely
    // unchanged by default.
    void setAutoLookaheadEnabled (bool shouldEnable) noexcept { autoLookaheadEnabled = shouldEnable; }

    // Step 56 (Kill The Polling Timer) directive: fires exactly once per
    // editor lifetime — the instant CrateEditorComponent's own timer
    // observes isEditorWindowReady() go false->true (editor creation, OR
    // any recreation: Editor View Recovery Guard, Liar's Penalty — see
    // that member's own doc comment). This is what lets
    // PluginWindowContent (PluginWindow.cpp) apply real resize limits the
    // moment they're actually known, replacing a 4Hz polling Timer that
    // was re-asserting corner-grip visibility and constrainer limits on a
    // fixed cadence regardless of whether anything had changed — a real,
    // if usually invisible, contributor to "random invalidation flashes"
    // uncorrelated with the user's own drag frames. Set ONCE, from
    // PluginWindowContent's own constructor; cleared in its destructor so
    // a dangling callback can never fire into a destroyed Component.
    std::function<void()> onResizeLimitsChanged;

    // Step 72 (Poison Pill / Hard Teardown) directive: fired synchronously
    // from terminateSandboxProcess() — always the message thread — the
    // instant a child teardown begins, BEFORE the process is killed and
    // the shared memory unmapped. Lets the live CrateEditorComponent (if
    // one exists) sever its reparented HWND and drop every cached
    // handle/dimension/throttle value that describes the dying
    // generation, so no window operation can ever target a process that's
    // mid-death, and nothing stale survives into the next generation
    // even if the Step 68 rising-edge reset were somehow never reached.
    // Same set-once/clear-in-destructor discipline as
    // onResizeLimitsChanged above.
    std::function<void()> onChildTeardown;

    // Step 60 (4K/HiDPI Protection for Broken Giants) directive: forces
    // this bridge's window into the Sizing Policy's hardLockdown track
    // AND forces the CHILD's display scale to a fixed 1.0 (100%, ignoring
    // whatever the real monitor's own scale factor is) — for plugins
    // whose own internal HiDPI/scaling engine is known, empirically, to
    // break under a high pixel-density display (the user's own named
    // example: iZotope Ozone). See CrateWorkflowManager's own call site
    // for the curated name/vendor list this is checked against — that
    // list is a maintained SEED, not an exhaustive authority; it can only
    // ever cover what's been observed breaking, the same "earned, not
    // guessed" spirit as PluginHealthRegistry's own crash/liar tracking,
    // just pre-emptive here since a HiDPI scaling crash isn't something
    // this codebase can safely wait to observe once and recover from —
    // by the time it's observed, the sandbox bounds are already blown up.
    //
    // Deliberately a PARENT-side, message-thread-only bool (not an
    // atomic) — set at most once, before this bridge's own first launch,
    // from CrateWorkflowManager's own instantiateExternalPlugin() (the
    // one real plugin-load door — see setAutoLookaheadEnabled()'s own
    // doc comment for why that specific call site is the right one).
    void setForceFixedSizeAndDefaultScale (bool shouldForce) noexcept { forcedHardLockdownAndDefaultScale = shouldForce; }

    // Step 60 directive: read by CrateEditorComponent::computeSizingPolicy()
    // (the Sizing Policy's own hardLockdown signal) and by
    // publishDisplayScaleFactor() (which this same flag also forces to
    // 1.0 regardless of the real monitor scale — see that method's own
    // doc comment).
    bool isForcedHardLockdown() const noexcept { return forcedHardLockdownAndDefaultScale; }

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
    struct CrateSyncedParameter : public te::AutomatableParameter,
                                   private te::AutomatableParameter::Listener
    {
        CrateSyncedParameter (const juce::String& paramID, const juce::String& name,
                              CrateSandboxBridge& bridgeToUse, int index)
            : te::AutomatableParameter (paramID, name, bridgeToUse, { 0.0f, 1.0f }),
              bridge (bridgeToUse), paramIndex (index)
        {
            // Zero-Dropout Bridge directive (Step 52): listens to ITSELF —
            // a valid, single-purpose pattern here, simpler than making the
            // whole CrateSandboxBridge a Listener across every synced
            // parameter just for this one hook (curveHasChanged, below).
            addListener (this);
        }

        ~CrateSyncedParameter() override
        {
            removeListener (this);
        }

        // Step 31's original override — DAW-originated changes still push
        // into the existing per-block ParamChange ring (Step 8) via
        // bridge.setParameterEvent(), unconditionally, regardless of the
        // Zero-Dropout Bridge logic below: that channel is what feeds the
        // DIRECT synchronous IPC path (AudioBridgeThread's own
        // drainParameterQueue()), used whenever this bridge ISN'T in
        // lookahead mode, and must keep working exactly as before.
        //
        // Zero-Dropout Bridge directive (Step 52): byAutomation is TE's
        // OWN distinction — true for ordinary timeline automation playback
        // (the transport simply reading an UNCHANGED curve forward), false
        // for anything host/plugin-driven: a live mouse-drag inside the
        // reparented native VST3 UI (via applyReadbackValue() below,
        // which calls setParameter() — TE's own "a user is actively moving
        // it" API), or external live MIDI (midiControllerMoved()). ONLY
        // the false case invalidates the Time-Slip Buffer — deterministic
        // automation playback must NOT flush on every single automated
        // block, or the buffer would never get ahead of real time at all.
        // Deliberately NOT gated by applyingReadback: a readback-driven
        // call IS exactly a live tweak (the user just moved a knob inside
        // the native UI) and must still flush; applyingReadback only
        // suppresses the echo-back to the CHILD immediately above, an
        // unrelated concern.
        void parameterChanged (float newValue, bool byAutomation) override
        {
            if (! applyingReadback)
                bridge.setParameterEvent (paramIndex, newValue);

            // Step 70 (Time-Slip Flush Storm / Auto-Demotion) directive —
            // QA finding, confirmed via direct log evidence: this fires on
            // EVERY pixel of a live UI drag (an EQ band, hundreds of times
            // a second), and the old direct bridge.flushTimeSlipBuffer()
            // call here did real synchronous work on EVERY one of those —
            // a mutex lock + futureParamCache.clear(), and (the genuinely
            // expensive part) logEvent()'s own juce::File::appendText(),
            // which opens, writes, and closes the log file on every single
            // call. Hundreds of full file-I/O round trips a second on the
            // message thread is what was actually starving everything
            // else on it, including the work that keeps the lookahead
            // pipeline ahead of real time — see markLiveEditActivity()'s
            // own doc comment for the replacement.
            if (! byAutomation)
                bridge.markLiveEditActivity();
        }

        // te::AutomatableParameter::Listener directive: curveHasChanged is
        // pure virtual on that interface. Fires when the automation
        // curve's SHAPE is actually edited (points added/moved/removed,
        // e.g. via the Arrangement automation lane) — genuinely
        // unpredictable from any already-buffered future audio's point of
        // view, unlike normal playback simply reading an unchanged curve
        // forward, so this DOES flush, same as a live value tweak above.
        void curveHasChanged (te::AutomatableParameter&) override
        {
            bridge.markLiveEditActivity();
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

    // Step 79 (Pre-Emptive Native Parenting) directive: publishes the
    // Airlock slot's own HWND value — called EXACTLY once per editor
    // generation, by AirlockHWNDComponent::ensureSlotReadyAndPublished(),
    // the instant its own slot creation succeeds. The CHILD reads this
    // once, before ever creating an editor (see requestEditorWindow()'s
    // own doc comment for the ordering contract this and that method
    // together enforce), and passes it straight into
    // AudioProcessorEditor::addToDesktop(0, ...) — see Main.cpp's own
    // call site.
    void publishHostSlotHandle (int64_t slotHwndValue) noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->hostSlotHwndValue.store (slotHwndValue, std::memory_order_release);
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

    // Step 52 Task 2 (Strict VST3-Driven Resize Limits) directive: the
    // CHILD's PROBED (not directly queried — see ControlBlock's own doc
    // comment on why IPlugView has no such API) min/max size, published
    // once alongside the handle, same convention as getEditorCanResize()
    // immediately above. 0 before windowHandleReady lands (rather than
    // editorCanResize's "assume true" default) — 0 is the correct "no
    // known limit yet" sentinel for PluginWindow's own setResizeLimits()
    // call to fall back on its own header-only minimum until a real
    // probed value arrives.
    int getPluginMinWidth() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->pluginMinWidth.load (std::memory_order_relaxed);
        return 0;
    }

    int getPluginMinHeight() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->pluginMinHeight.load (std::memory_order_relaxed);
        return 0;
    }

    int getPluginMaxWidth() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->pluginMaxWidth.load (std::memory_order_relaxed);
        return 0;
    }

    int getPluginMaxHeight() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            if (block->windowHandleReady.load (std::memory_order_acquire))
                return block->pluginMaxHeight.load (std::memory_order_relaxed);
        return 0;
    }

    // Step 79 (Pre-Emptive Native Parenting) directive — QA finding: Step
    // 73-76's approach (create the CHILD's editor as its own floating
    // top-level window, THEN reparent + forcibly strip its styles
    // afterward) survived the lifetime/idempotence fixes but never
    // actually solved the domain problem — a heavy VST3's own
    // Direct2D/OpenGL swap chain is set up against whatever window it was
    // ORIGINALLY created for, and a later SetParent() doesn't retroactively
    // fix that; Melda kept popping out as its own top-level window.
    //
    // The real fix is upstream of all of it: give the CHILD the Airlock
    // slot's own HWND value BEFORE it ever creates its editor, so JUCE's
    // own AudioProcessorEditor::addToDesktop(0, slotHwnd) calls
    // CreateWindowEx with the slot as hWndParent from the very first
    // frame. Confirmed by reading JUCE's own juce_Windowing_windows.cpp
    // directly: HWNDComponentPeer::computeNativeStyleFlags() sets WS_CHILD
    // (and ONLY WS_CHILD — never WS_POPUP/WS_CAPTION/WS_THICKFRAME/
    // WS_SYSMENU, all of which live in separate branches that are simply
    // never reached) the INSTANT parentToAddTo is non-null. Nothing is
    // ever created wrong in the first place, so there's nothing left to
    // strip or force-recalculate afterward.
    //
    // This class's role shrinks accordingly: it no longer wraps or tracks
    // any specific CHILD HWND at all (there's nothing to reparent — see
    // SandboxAirlock's own updated doc comment). It creates the Airlock
    // slot as soon as it has a real peer, publishes the slot's own HWND
    // value to the bridge (which relays it to the CHILD over IPC — see
    // CrateSandboxBridge::publishHostSlotHandle() and Main.cpp's own
    // addToDesktop() call), and keeps the slot positioned/sized over this
    // Component's own screen area for as long as it lives. Every Win32
    // call it triggers still goes through SandboxAirlock's own dedicated
    // thread, never the JUCE message thread directly — that isolation
    // guarantee (Step 73's original point) is unchanged.
    class AirlockHWNDComponent : public juce::Component
    {
    public:
        AirlockHWNDComponent() = default;

        ~AirlockHWNDComponent() override
        {
            if (slotId != 0)
                SandboxAirlock::getInstance().destroySlot (slotId);
        }

        // Step 79 directive: called every tick from
        // CrateEditorComponent::timerCallback(), BEFORE it ever asks the
        // bridge to request an editor window from the CHILD. Returns true
        // once the slot exists AND its HWND value has been published to
        // the bridge — only then is it safe for the CHILD to create an
        // editor at all (see requestEditorWindow()'s own doc comment for
        // the other half of this ordering contract). Never blocks: a slot
        // creation still in flight just means "not ready yet, try again
        // next tick," same non-blocking discipline as every other Airlock
        // operation.
        bool ensureSlotReadyAndPublished (CrateSandboxBridge& bridgeToPublishTo)
        {
            if (slotId != 0)
                return true; // already created and published this generation

            auto* peer = getPeer();

            if (peer == nullptr)
            {
                firstAttemptMs = 0; // no real window yet — not stuck, just not started; the clock starts once there's actually something to wait on
                return false; // not attached to a real top-level window yet — the next tick retries
            }

            const auto nowMs = juce::Time::getMillisecondCounter();

            if (firstAttemptMs == 0)
                firstAttemptMs = nowMs;

            // Step 82 (Airlock Self-Healing Timeout) directive — QA finding:
            // a DSP-stall Watchdog restart confirmed, via two live
            // non-invasive WinDbg attaches on separate freeze incidents,
            // that EVERY thread in both the Host and the Child stayed
            // perfectly healthy/idle, yet the app never recovered and the
            // user had to force-close it. Not a Win32 deadlock — a pure
            // logic stall: whatever the exact trigger (an async
            // MSG_CREATE_SLOT round trip that never completes, a peer HWND
            // that's valid here but already gone by the time the Airlock
            // thread processes the request, or anything else unforeseen),
            // this loop had no bound on how long it would keep waiting on
            // the SAME outstanding attempt. Mirrors this codebase's own
            // existing heartbeat/DSP-stall Watchdog discipline: bounded
            // wait, then force a clean retry rather than hang forever.
            if (nowMs - firstAttemptMs > slotCreationTimeoutMs)
            {
                CRATE_FR_LOGF ("AIRLOCK", "slot creation stalled for over %dms (request outstanding=%d) — forcing reset and fresh retry",
                                slotCreationTimeoutMs, pendingSlotCreation != nullptr ? 1 : 0);
                pendingSlotCreation.reset();
                firstAttemptMs = nowMs;
            }

            if (pendingSlotCreation == nullptr)
                pendingSlotCreation = SandboxAirlock::getInstance().requestSlotCreationAsync (peer->getNativeHandle());

            if (! pendingSlotCreation->ready.load (std::memory_order_acquire))
                return false; // still outstanding — next tick checks again, never a second request unless the timeout above just fired

            const uint64_t newId = pendingSlotCreation->slotId.load (std::memory_order_acquire);
            const int64_t hwndValue = pendingSlotCreation->slotHwndValue.load (std::memory_order_acquire);
            pendingSlotCreation.reset();

            if (newId == 0)
            {
                // Genuine failure, already logged by the Airlock itself
                // (createSlot FAILED, with the real Win32 error). Retries
                // next tick — firstAttemptMs keeps counting across repeated
                // failures too, so a permanent failure loop still trips
                // the timeout above instead of retrying forever.
                CRATE_FR_LOG ("AIRLOCK", "slot creation request came back failed — will retry next tick");
                return false;
            }

            slotId = newId;
            firstAttemptMs = 0;
            bridgeToPublishTo.publishHostSlotHandle (hwndValue);
            tryPushBounds();
            return true;
        }

        // Step 72/79 directive: called on a genuine new editor generation
        // (Liar's Penalty, Editor View Recovery Guard, a Watchdog respawn)
        // — the OLD slot's own reparented content died with the OLD
        // process; rather than try to reuse it, destroy it outright and
        // let ensureSlotReadyAndPublished() create and (re-)publish a
        // completely fresh one on the very next tick, exactly matching
        // this codebase's existing "clean slate per generation" discipline
        // (Step 72's own Poison Pill sanitize does the same for the IPC
        // ControlBlock fields).
        void resetSlot()
        {
            if (slotId != 0)
            {
                SandboxAirlock::getInstance().destroySlot (slotId);
                slotId = 0;
            }

            pendingSlotCreation.reset();
            firstAttemptMs = 0; // fresh generation — the timeout clock restarts clean, matching Step 72's own Poison Pill "clean slate per generation" discipline
        }

        void resized() override { tryPushBounds(); }
        void moved()   override { tryPushBounds(); }
        void parentHierarchyChanged() override { tryPushBounds(); }

    private:
        // Same area computation as JUCE's own HWNDComponent::Pimpl::
        // componentMovedOrResized() (peer->getAreaCoveredBy(*this), scaled
        // by the peer's own platform scale factor) — kept identical so
        // DPI behaviour doesn't change, only which window this positions.
        void tryPushBounds()
        {
            if (slotId == 0)
                return; // nothing to position yet — ensureSlotReadyAndPublished() calls this itself the instant it succeeds

            auto* peer = getPeer();

            if (peer == nullptr)
                return;

            const auto area = (peer->getAreaCoveredBy (*this).toFloat() * peer->getPlatformScaleFactor()).getSmallestIntegerContainer();
            SandboxAirlock::getInstance().requestBounds (slotId, area.getX(), area.getY(), area.getWidth(), area.getHeight());
        }

        uint64_t slotId = 0;
        std::shared_ptr<SandboxAirlock::PendingSlotResult> pendingSlotCreation;

        // Step 82 (Airlock Self-Healing Timeout) directive: 0 means "not
        // currently waiting on anything" (either already succeeded, or
        // getPeer() hasn't returned a real window yet) — see
        // ensureSlotReadyAndPublished()'s own doc comment.
        juce::int64 firstAttemptMs = 0;
        static constexpr int slotCreationTimeoutMs = 3000;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AirlockHWNDComponent)
    };

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
        // Step 60 (The Sizing Policy) directive: the two, and only two,
        // ways this class is ever allowed to treat a sandboxed window's
        // resizing — see computeSizingPolicy()'s own doc comment for the
        // exact signals that decide between them.
        enum class SizingPolicy
        {
            passiveObserver,
            hardLockdown
        };

        // Step 65 (Pure Follower Architecture) directive: no mouse
        // listener needed at all any more — there is no more Host-side
        // drag gesture to observe. The Host never offers a resize handle
        // for a sandboxed window (see resized()'s own doc comment for the
        // full architectural pivot this reflects), so this class no
        // longer overrides mouseDown()/mouseUp(), and the global mouse
        // listener registration those existed to support is gone too.
        explicit CrateEditorComponent (CrateSandboxBridge& bridgeToUse)
            : bridge (bridgeToUse),
              scaleNotifier (this, [this] (float newScale) { bridge.publishDisplayScaleFactor (newScale); })
        {
            addAndMakeVisible (hwndComponent);
            setSize (400, 300); // placeholder until the CHILD's real editor size is known

            // Step 72 (Poison Pill / Hard Teardown) directive: the bridge
            // fires this synchronously (always on the message thread) the
            // instant a child teardown begins — sever the reparented HWND
            // and drop every cached per-generation value IMMEDIATELY,
            // rather than waiting for the next editor's own rising edge
            // (Step 68) to clean up after the fact. Between a kill and
            // the replacement editor's arrival, this Component keeps
            // painting its "Loading Plugin…" placeholder over an empty
            // hwndComponent instead of holding a dead process's window.
            bridge.onChildTeardown = [this]
            {
                currentHandle = nullptr;
                hwndComponent.resetSlot();
                lastWidth = lastHeight = 0;
                followerResizeDirty   = false;
                pendingFollowerWidth  = 0;
                pendingFollowerHeight = 0;
                lastFollowerApplyMs   = 0;
                lastChildReportChangeMs = juce::Time::getMillisecondCounter();
                repaint();
            };

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
            // Same 30ms/~33fps cadence this class has always used to track
            // a live CHILD-driven resize smoothly, at negligible cost for a
            // handful of atomic reads per tick.
            startTimer (30);
        }

        ~CrateEditorComponent() override
        {
            // Step 72 directive: cleared BEFORE anything else so a bridge
            // teardown racing this destructor can never fire into a
            // half-destroyed Component — same discipline as
            // onResizeLimitsChanged's own set-once/clear-in-destructor
            // contract.
            bridge.onChildTeardown = nullptr;

            // AirlockHWNDComponent's own destructor already destroys
            // whatever slot it's holding — stopTimer() first is what
            // actually matters here, avoiding a race between PluginWindow's
            // own teardown and a still-ticking timer callback.
            stopTimer();
        }

        // Step 65 (Pure Follower Architecture) directive: the Host NEVER
        // offers its own resize handle for a sandboxed window any more,
        // regardless of SizingPolicy — always false. See resized()'s own
        // doc comment for the full architectural pivot.
        bool allowWindowResizing() override { return false; }
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

        // Step 62 (Stack Overflow / Hang QA Audit) directive — DIAGNOSTIC
        // ONLY. Same reentrancy-depth proof as forceRepaintEmbeddedHwnd()'s
        // own instrumentation — if resized() is ever observed calling
        // itself before the outer call returns, this is the direct,
        // unambiguous signature of the synchronous recursion cycle the
        // user's hypothesis describes. RAII, not a manual ++/-- pair —
        // resized() has an early return deep inside (the Step 58 dedup
        // check); a bare decrement at the bottom would never run on that
        // path and the counter would report false-positive "recursion"
        // forever after the first dedup hit.
        int resizedReentrancyDepth = 0;

        struct ReentrancyGuard
        {
            explicit ReentrancyGuard (int& counterToUse) : counter (counterToUse) { ++counter; }
            ~ReentrancyGuard() { --counter; }
            int& counter;
        };

        // Step 65 (Pure Follower Architecture) directive: the Host's own
        // drag-tug-of-war machinery that used to live here — the
        // pendingResize*/lastDispatched*/floorCaptured clamp-and-dispatch
        // chain (Steps 34/35/58/59/61) — is gone entirely. The Host no
        // longer initiates a resize of the sandboxed plugin under ANY
        // circumstance (see allowWindowResizing()/timerCallback()'s own
        // setResizable(false, false) call, now unconditional): there is no
        // more OS drag handle for the user to grab in the first place, so
        // this override has nothing left to do beyond keeping the
        // reparented HWND's own bounds/repaint in sync with whatever size
        // applyChildReportedSize() (the ONE remaining resize authority)
        // last set this Component to.
        void resized() override
        {
            ReentrancyGuard reentrancyGuard (resizedReentrancyDepth);

            if (resizedReentrancyDepth > 1)
                logEvent ("DIAG STACK AUDIT: *** CrateEditorComponent::resized() RE-ENTERED — genuine recursion confirmed ***");

            hwndComponent.setBounds (getLocalBounds());
            forceRepaintEmbeddedHwnd(); // Step 35 Task 3 — see its own doc comment
        }

    private:
        // Step 57 (Melda Grid-Snap Feedback Loop) directive: the ONE place
        // that ever applies a CHILD-reported size to this Component/the
        // outer window.
        //
        // Step 65 (Pure Follower Architecture) directive: now the ONLY
        // resize authority left, period — called unconditionally from
        // timerCallback() the instant the CHILD reports a new size, with
        // no more drag-state deferral (there is no more competing
        // Host-driven authority to defer around) and no more
        // applyingChildReportedSize echo-guard (nothing downstream ever
        // reinterprets this as a fresh host-initiated request any more —
        // resized(), above, no longer has a host-dispatch path at all).
        //
        // Step 67 (Atomic Sync & Idle Flush) directive — QA finding: Step
        // 66's own throttle updated hwndComponent (the reparented native
        // child HWND) immediately every tick while deferring the OUTER
        // wrapper's own setSize() to a ~30Hz cap. That let the two
        // legitimately disagree about this Component's own intended size
        // for as long as the throttle held the wrapper back — a real
        // bounds desync between the native child and its own JUCE parent
        // (screenshots showed the child rendering into a black void
        // outside the wrapper's still-stale frame). A click landing
        // inside that desynchronized child sent Win32 coordinates that no
        // longer made sense relative to the wrapper's own stale bounds —
        // an access violation, not merely a visual glitch. Fixed by
        // NEVER touching hwndComponent here at all — only
        // dispatchFollowerResizeIfDue() (below) is allowed to resize
        // either the child HWND or the wrapper, and it does both in the
        // same call, gated by the exact same throttle/flush decision, so
        // the two can never legitimately disagree.
        void applyChildReportedSize (int w, int h, bool forceImmediate)
        {
            pendingFollowerWidth  = w;
            pendingFollowerHeight = h;
            followerResizeDirty   = true;

            dispatchFollowerResizeIfDue (forceImmediate);
        }

        // Step 66 (Throttle The Follower) directive: caps the expensive
        // outer-window setSize() cascade to roughly 30Hz — matching this
        // class's own 30ms poll timer, the rate the OS can actually keep
        // up with. A plain fixed interval isn't enough on its own: if
        // ANY single setSize() call itself takes longer than the timer's
        // own 30ms period (exactly what was observed — a heavy resize
        // cascade blocking well past its own tick), JUCE's Timer has no
        // idle gap left to wait out and the NEXT tick fires immediately
        // back-to-back, effectively removing all pacing. Stamping the
        // ACTUAL wall-clock time of the last real apply (lastFollowerApplyMs)
        // and checking it explicitly, rather than trusting the timer's
        // nominal interval, is what keeps the floor real regardless of how
        // fast or slow surrounding calls happen to run.
        static constexpr int followerThrottleMs = 33;
        juce::uint32 lastFollowerApplyMs = 0;
        bool followerResizeDirty = false;
        int pendingFollowerWidth = 0, pendingFollowerHeight = 0;

        // Step 67 directive, Task 3 (Idle Stabilizer): VST3 plugins never
        // send an explicit "drag ended" event — the ONLY signal available
        // is the IPC-reported size itself going quiet. timerCallback()
        // tracks how long it's been since the CHILD's own reported size
        // last actually CHANGED and, once that idle window passes
        // followerIdleFlushMs with a mismatch still outstanding, calls
        // this with forceImmediate=true — bypassing the throttle entirely
        // for one exact, final apply, so a drag that stops mid-throttle-
        // window is never left stranded at a slightly-off intermediate
        // size (or, worse per this Step's own finding, a desynchronized
        // child HWND).
        static constexpr int followerIdleFlushMs = 150;
        juce::uint32 lastChildReportChangeMs = 0;

        void dispatchFollowerResizeIfDue (bool forceImmediate)
        {
            if (! followerResizeDirty)
                return;

            // Idempotence: if the pending value already matches this
            // Component's own current size, there is nothing left to
            // apply; clears the flag without ever reaching setSize() at
            // all. PluginWindowContent::applyGeometry()'s own
            // lastAppliedGeometryWidth/Height check (Step 63) is a second,
            // independent idempotence guard one layer further down —
            // this one just avoids reaching that call in the first place
            // for the common "reported the same size again" case.
            if (pendingFollowerWidth == getWidth() && pendingFollowerHeight == getHeight())
            {
                followerResizeDirty = false;
                return;
            }

            const auto now = juce::Time::getMillisecondCounter();

            if (! forceImmediate && now - lastFollowerApplyMs < (juce::uint32) followerThrottleMs)
                return; // still inside the throttle window — the idle-flush check in timerCallback() forces this through the instant a stalled drag is detected, otherwise the next tick past the throttle flushes it normally

            lastFollowerApplyMs = now;
            followerResizeDirty = false;

            // Step 67 directive, Task 1 (Atomic Application): the native
            // child HWND and this Component's own (and therefore the
            // outer wrapper's own) bounds are set together, in the same
            // call, gated by the same throttle/flush decision above —
            // never independently.
            hwndComponent.setSize (pendingFollowerWidth, pendingFollowerHeight);

            // Triggers PluginWindow's own resizeToFitWhenContentChangesSize
            // (setContentNonOwned's second argument) — the SAME mechanism
            // that already makes a live in-process plugin resize follow
            // correctly, no special-casing needed for this being a
            // reparented cross-process window underneath. Also re-enters
            // resized() (above), which re-asserts hwndComponent's own
            // bounds against the new getLocalBounds() — belt-and-braces
            // confirmation that the child can never end up sized
            // differently from what this Component's own layout actually
            // is.
            setSize (pendingFollowerWidth, pendingFollowerHeight);

            forceRepaintEmbeddedHwnd();
        }

        // Step 60 (The Sizing Policy) directive: the ONE place this
        // bridge's window is classified into passiveObserver or
        // hardLockdown — every other method reads timerCallback()'s own
        // cached lastKnownPolicy rather than re-deriving these signals.
        //
        //   hardLockdown fires on ANY of:
        //     - canResize()==false (the VST3's own honest answer — Opal/
        //       DualDelayX-style, never claimed resizability at all).
        //     - probeProvedFixedSize (Step 52 Task 2's own
        //       checkSizeConstraint() probe came back min==max despite
        //       canResize()==true — VoxDucker: claims resizability,
        //       silently ignores every request, a "black void" trap if
        //       left alone).
        //     - bridge.isForcedHardLockdown() (Step 60's own 4K/HiDPI
        //       "broken giants" protection, OR a plugin the Liar's
        //       Penalty already convicted in this or a past session —
        //       see that method's own doc comment).
        //
        //   passiveObserver is everything else: a plugin that honestly
        //   claims AND honours resizability. Since Step 65 (Pure Follower
        //   Architecture), this distinction no longer changes how the Host
        //   RESIZES the window (it never does, for either policy — see
        //   timerCallback()'s own setResizable(false, false) call, now
        //   unconditional) — it's kept purely so the logs still say
        //   plainly which KIND of plugin this is: genuinely fixed-size
        //   (hardLockdown) vs. internally resizable via its own handle,
        //   Host just follows (passiveObserver).
        SizingPolicy computeSizingPolicy() const
        {
            const bool canResizeNow = bridge.getEditorCanResize();

            const int probedMinW = bridge.getPluginMinWidth();
            const int probedMinH = bridge.getPluginMinHeight();
            const int probedMaxW = bridge.getPluginMaxWidth();
            const int probedMaxH = bridge.getPluginMaxHeight();

            const bool probeProvedFixedSize = probedMinW > 0 && probedMinH > 0
                                                 && probedMinW == probedMaxW && probedMinH == probedMaxH;

            const bool mustLockdown = ! canResizeNow || probeProvedFixedSize || bridge.isForcedHardLockdown();

            return mustLockdown ? SizingPolicy::hardLockdown : SizingPolicy::passiveObserver;
        }

        void timerCallback() override
        {
            // Step 56 (Kill The Polling Timer) directive: computed BEFORE
            // the early-return below so the edge is caught on the exact
            // tick readiness flips, not only on ticks that get past it.
            const bool windowReadyNow = bridge.isEditorWindowReady();

            if (windowReadyNow && ! lastKnownWindowReady)
            {
                // Step 68 (Outbound Throttle & Lifecycle Integrity)
                // directive, Task 2 — QA finding: after a genuine
                // recreation (an in-process editor recreation OR a full
                // Watchdog/Guillotine process respawn — both cycle
                // controlBlock->windowHandleReady false -> true, so both
                // land here identically, matching this codebase's own
                // "uniformly regardless of which kind of reconnect just
                // happened" philosophy), currentHandle, lastWidth/
                // lastHeight, and the Step 66/67 follower-throttle state
                // below were all left describing the PREVIOUS editor/
                // process generation.
                //
                // Step 79 (Pre-Emptive Native Parenting) directive: the
                // Airlock SLOT itself is deliberately NOT touched here any
                // more — bridge.onChildTeardown (fired synchronously by
                // terminateSandboxProcess(), BEFORE any respawn) already
                // destroyed the previous generation's slot, and
                // ensureSlotReadyAndPublished() (called every tick, ABOVE
                // this whole block) already created and published a fresh
                // one for whatever generation is currently connecting —
                // by the time this rising edge fires, that fresh slot is
                // already correctly holding the CHILD's own freshly
                // embedded editor. Resetting it here would tear out the
                // very editor this edge is reporting as newly ready.
                currentHandle = nullptr;
                lastWidth = lastHeight = 0;
                followerResizeDirty   = false;
                pendingFollowerWidth  = 0;
                pendingFollowerHeight = 0;
                lastFollowerApplyMs   = 0;
                lastChildReportChangeMs = juce::Time::getMillisecondCounter();

                // Editor open (first creation, or a genuine recreation —
                // Liar's Penalty, Editor View Recovery Guard) — the CHILD's
                // probed pluginMinWidth/Height/MaxWidth/Height (and
                // editorCanResize) just became valid for this instance.
                if (bridge.onResizeLimitsChanged)
                    bridge.onResizeLimitsChanged();
            }

            lastKnownWindowReady = windowReadyNow;

            // Step 79 (Pre-Emptive Native Parenting) directive: the CHILD
            // must not be asked to create an editor until it has
            // somewhere correct to put it — the slot must exist AND be
            // published over IPC first (see AirlockHWNDComponent::
            // ensureSlotReadyAndPublished()'s own doc comment, and
            // CrateSandboxBridge::requestEditorWindow()'s for the other
            // half of this ordering contract). Never blocks: returns
            // false harmlessly while still in flight, and this method
            // simply tries again next tick.
            if (! hwndComponent.ensureSlotReadyAndPublished (bridge))
                return;

            if (! windowReadyNow)
            {
                bridge.requestEditorWindow(); // re-issued every tick until it actually lands — see constructor's own comment
                return;
            }

            // Step 60 (The Sizing Policy) directive: EVERY sandboxed window
            // is governed by exactly one of two named policies, decided
            // fresh each tick from the CHILD's own honest signals — no
            // more scattered bool combinations (shouldBeResizable,
            // probeProvedFixedSize, canResizeNow) implicitly encoding the
            // same decision in several places:
            //
            //   passiveObserver — the plugin handles its own resizing
            //   internally (Melda-style). The Host never fights it: it
            //   defers to whatever the CHILD reports (see the drag-
            //   deferral and shrink-floor-clamp logic below, both scoped
            //   to this policy), applying changes smoothly rather than
            //   forcing its own layout mid-drag.
            //
            //   hardLockdown — canResize()==false, OR the probe proved
            //   min==max (a plugin that CLAIMS resizability but silently
            //   ignores it — VoxDucker), OR the Liar's Penalty convicted
            //   it after a live snap-back, OR this bridge was explicitly
            //   forced into lockdown (see setForceFixedSizeAndDefaultScale()
            //   — the 4K/HiDPI "broken giants" protection). External
            //   resizing is disabled entirely: no drag handle, no edge
            //   cursor, no fighting possible because there is nothing left
            //   to fight over.
            //
            // computeSizingPolicy() is the ONE place this decision is
            // made; every consumer below reads its result rather than
            // re-deriving the same signals independently.
            const SizingPolicy policy = computeSizingPolicy();

            if (policy != lastKnownPolicy)
            {
                lastKnownPolicy = policy;

                if (auto* resizableWindow = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent()))
                {
                    // Step 65 (Pure Follower Architecture) directive: the
                    // Host NEVER offers its own OS resize handle for a
                    // sandboxed window any more, regardless of policy —
                    // false/false unconditionally, for both hardLockdown
                    // (genuinely fixed-size) AND passiveObserver
                    // (internally resizable — the user drags the plugin's
                    // OWN native handle instead; the Host just follows,
                    // see applyChildReportedSize()). setResizable(false,
                    // false)'s second argument governs JUCE's own
                    // bottom-right corner resizer Component; both false
                    // means no drag handle, no edge cursor, nothing left
                    // for the user to grab on the Host side at all — and
                    // (per Step 54's own doc comment) triggers
                    // recreateDesktopWindow(), which is what actually
                    // stops the OS edge-resize cursor from ever appearing.
                    resizableWindow->setResizable (false, false);

                    logEvent (juce::String ("Sizing Policy: now ")
                                  + (policy == SizingPolicy::hardLockdown ? "HARD LOCKDOWN (genuinely fixed-size)" : "PASSIVE OBSERVER (internally resizable)")
                                  + " — Host-side resizing disabled either way; the Host is a pure follower of whatever size the CHILD reports.");
                }
            }

            auto* newHandle = bridge.getEditorWindowHandle();
            const int w = bridge.getEditorWindowWidth();
            const int h = bridge.getEditorWindowHeight();

            if (newHandle == nullptr)
                return; // no window yet

            if (w <= 0 || h <= 0)
                return;

            const bool handleChanged = (newHandle != currentHandle);

            // Step 67 (Atomic Sync & Idle Flush) directive, Task 3: tracks
            // the RAW CHILD-reported size itself — independent of whether
            // or when it's actually been applied to this Component's own
            // bounds (that's sizeMismatch, below) — updated the instant
            // the report changes, every tick, regardless of throttle
            // state. lastChildReportChangeMs is what lets the idle check
            // further down answer "how long has it been quiet."
            const bool sizeChanged = (w != lastWidth || h != lastHeight);

            if (sizeChanged)
            {
                lastWidth  = w;
                lastHeight = h;
                lastChildReportChangeMs = juce::Time::getMillisecondCounter();
            }

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

            if (handleChanged)
            {
                // Step 79 (Pre-Emptive Native Parenting) directive: no
                // more reparenting call here — the CHILD's editor was
                // already embedded into the Airlock slot at creation time
                // (see ensureSlotReadyAndPublished()/Main.cpp's own
                // addToDesktop() call). newHandle is tracked purely to
                // detect that a (re)creation happened, so the placeholder
                // clears promptly.
                currentHandle = newHandle;
                repaint(); // clears the "Loading Plugin…" placeholder promptly — see paint()'s own comment
            }

            // Step 67 directive, Task 3 (Idle Stabilizer): a VST3 plugin
            // never sends an explicit "drag ended" event — the CHILD's
            // own reported size going quiet for followerIdleFlushMs while
            // a mismatch is still outstanding is the only available
            // signal that a drag has genuinely stopped. Bypasses the
            // throttle for one exact, final, ATOMIC apply (both the
            // wrapper and the child HWND together — see
            // dispatchFollowerResizeIfDue()'s own doc comment) rather than
            // potentially leaving the two stuck at a stale intermediate
            // size indefinitely.
            const bool idleWithMismatch = sizeMismatch
                && (juce::Time::getMillisecondCounter() - lastChildReportChangeMs) > (juce::uint32) followerIdleFlushMs;

            // Step 65 (Pure Follower Architecture) directive: applied
            // immediately and unconditionally — there is no more
            // Host-side drag authority left to race against (see
            // resized()'s own doc comment), so the deferral this used to
            // need (Step 57's isHostDragging gate, coalescing into
            // mouseUp()) no longer has anything to defer around. The
            // CHILD's own native resize handle already reports a
            // grid-settled size at whatever cadence ITS OWN UI produces
            // it, not several independent authorities fighting over the
            // same frame. forceImmediate (Step 67) bypasses Step 66's own
            // throttle for exactly one call the instant a stalled drag is
            // detected — every other tick still goes through the normal
            // throttled path.
            applyChildReportedSize (w, h, idleWithMismatch);
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
        // Step 64 (Kill The Synchronous Repaint) directive — the Step 62
        // audit's own measured evidence (captured via THIS function's own
        // prior instrumentation): UpdateWindow() against a non-empty
        // update region sends WM_PAINT directly to hwnd's own window
        // procedure, bypassing the message queue — i.e. SYNCHRONOUSLY,
        // like SendMessage. hwnd is a CROSS-PROCESS HWND (the CHILD's own
        // reparented editor). Real, measured blocks of 7-10ms were logged
        // on this exact call during a shrink drag (CHILD busy running
        // checkSizeConstraint() rejections), tens of times a second —
        // real thread contention, independent of the Step 63 oscillation
        // fix (which stops SPURIOUS re-triggers, but does nothing about
        // the legitimate, expected resize the user's own live mouse
        // movement produces every frame — each of THOSE still hit this
        // blocking call).
        //
        // Fix: InvalidateRect(..., FALSE) only — marks the region dirty
        // (FALSE = don't erase the background first, avoiding a redundant
        // erase/repaint pass) and returns immediately, WITHOUT sending
        // WM_PAINT anywhere. Windows' own DWM coalesces the actual repaint
        // at the display's refresh rate on its own schedule, off this
        // thread entirely — this Component no longer waits on the CHILD's
        // UI thread for anything. The reentrancy guard and timing check
        // stay: InvalidateRect() is a lightweight call that posts no
        // message and should never legitimately show a measurable elapsed
        // time or reentry — if a future log ever DOES show either, that's
        // now real, actionable evidence something else has regressed here,
        // not expected behaviour from this call.
        int forceRepaintReentrancyDepth = 0;

        void forceRepaintEmbeddedHwnd()
        {
           #if JUCE_WINDOWS
            ++forceRepaintReentrancyDepth;

            if (forceRepaintReentrancyDepth > 1)
                logEvent ("DIAG STACK AUDIT: forceRepaintEmbeddedHwnd() RE-ENTERED at depth "
                              + juce::String (forceRepaintReentrancyDepth) + " — genuine recursion, not just a slow call.");

            // Step 79 (Pre-Emptive Native Parenting) directive: this
            // entire mechanism (Steps 24/35/64) existed only because the
            // CHILD's editor used to be reparented cross-process AFTER
            // being created as its own top-level window — JUCE's own
            // EnumChildWindows-based invalidation never reached that
            // embedded top-level HWND, and forcing a repaint was this
            // class's own responsibility. Under pre-emptive parenting the
            // CHILD's editor is a genuine, natively-created WS_CHILD of
            // the Airlock slot from birth (see AirlockHWNDComponent's own
            // doc comment) — it paints itself exactly like any other
            // native window, no cross-process invalidation kludge needed.
            // Kept as an intentional no-op (rather than deleting this
            // method and hunting down every call site) — the reentrancy
            // accounting stays harmless scaffolding in case a future
            // regression ever needs it again.

            --forceRepaintReentrancyDepth;
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

        AirlockHWNDComponent hwndComponent;
        juce::ComponentBoundsConstrainer constrainer;
        void* currentHandle = nullptr;
        int lastWidth = 0;
        int lastHeight = 0;

        // Step 60 (The Sizing Policy) directive: replaces the old
        // standalone lastKnownCanResize bool — starts at passiveObserver,
        // matching ControlBlock::editorCanResize's own optimistic true
        // default and getEditorCanResize()'s pre-connection fallback (see
        // computeSizingPolicy()'s own doc comment for how/when this
        // actually gets re-evaluated and acted on).
        SizingPolicy lastKnownPolicy = SizingPolicy::passiveObserver;

        // Step 56 (Kill The Polling Timer) directive: rising-edge tracker
        // for bridge.isEditorWindowReady() — starts false (matching the
        // ControlBlock field's own default before any editor exists).
        // false -> true is the EXACT moment the CHILD's probed
        // pluginMinWidth/Height/MaxWidth/Height (and editorCanResize)
        // become valid for THIS editor instance (see that struct's own
        // doc comment: "populated once at editor-creation time before the
        // window is shown"). Also naturally re-fires after ANY editor
        // recreation (Editor View Recovery Guard, Liar's Penalty) since
        // Main.cpp resets windowHandleReady to false before tearing the
        // old editor down — see timerCallback()'s own call site for what
        // this drives.
        bool lastKnownWindowReady = false;
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
        // Step 60 (4K/HiDPI Protection for Broken Giants) directive:
        // forced to a flat 100% for a plugin whose own scaling engine is
        // known to break on a high-density display — see
        // setForceFixedSizeAndDefaultScale()'s own doc comment. The real
        // monitor scale this method was actually called with is simply
        // discarded in that case; the CHILD never learns the true value.
        const float effectiveScale = forcedHardLockdownAndDefaultScale ? 1.0f : scale;

        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->displayScale1000.store ((int32_t) juce::roundToInt (effectiveScale * 1000.0f), std::memory_order_relaxed);
    }

    // Continuous State Sync directive (Step 11): the most recent plugin
    // state chunk received from the CHILD — updated by timerCallback()
    // whenever a push arrives (see that method's own comment). Empty until
    // the first push. Message-thread-only (this is only ever read from
    // debug/test code, never the audio thread).
    const juce::MemoryBlock& getLastKnownState() const noexcept { return lastKnownState; }
    size_t getLastKnownStateSize() const noexcept { return lastKnownState.getSize(); }

    // Step 39 (Universal Dry/Wet Mix & Smart Bypass) directive, Tasks 2/4:
    // PluginWindowHeader's own knob/button callbacks — message-thread-only
    // writes, lock-free loads from the audio thread inside applyDryWetMix().
    // normalizedMix is clamped 0-1; the header itself is what maps its 0%-
    // 100% display range onto this.
    void setDryWetMix (float normalizedMix) noexcept
    {
        dryWetMix.store (juce::jlimit (0.0f, 1.0f, normalizedMix), std::memory_order_relaxed);
    }

    float getDryWetMix() const noexcept { return dryWetMix.load (std::memory_order_relaxed); }

    void setBypassed (bool shouldBypass) noexcept
    {
        bypassRequested.store (shouldBypass, std::memory_order_relaxed);
    }

    bool isBypassed() const noexcept { return bypassRequested.load (std::memory_order_relaxed); }

    // Step 39 (Live Telemetry) directive, Task 3: both default to 0 before
    // the CHILD's first publish (no window/plugin yet) — the header
    // displays "—" for that case rather than a misleading "0".
    int getPluginLatencySamples() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            return block->pluginLatencySamples.load (std::memory_order_relaxed);
        return 0;
    }

    uint32_t getLastProcessBlockMicros() const noexcept
    {
        if (auto* block = controlBlock.load (std::memory_order_acquire))
            return block->lastProcessBlockMicros.load (std::memory_order_relaxed);
        return 0;
    }

    // Step 39 (Live Telemetry) directive, Task 3: PluginWindowHeader's own
    // samples -> milliseconds conversion for the latency readout. Message-
    // thread-only read of a message-thread-only-written field (set once in
    // initialise(), never touched from the audio thread).
    double getCachedSampleRate() const noexcept { return cachedSampleRate; }

    // Step 39 (A/B Testing) directive, Task 4: "Store A"/"Store B" —
    // snapshots the SAME lastKnownState the Continuous State Sync channel
    // (Step 11's "Muscle Memory") already keeps continuously fresh, into
    // one of two RAM-only slots. No disk I/O anywhere in this path — the
    // instant, click-to-click swap the user actually wants.
    void storeCurrentStateToSlot (char whichSlot)
    {
        auto& slot     = (whichSlot == 'A') ? stateSlotA : stateSlotB;
        auto& hasState = (whichSlot == 'A') ? hasStateSlotA : hasStateSlotB;

        slot = lastKnownState;
        hasState = slot.getSize() > 0;

        logEvent ("DIAG A/B: stored slot " + juce::String (whichSlot) + " — "
                      + juce::String (slot.getSize()) + " bytes captured from lastKnownState.");
    }

    bool hasStateSlot (char whichSlot) const noexcept
    {
        return (whichSlot == 'A') ? hasStateSlotA : hasStateSlotB;
    }

    // Step 39 (A/B Testing) directive, Task 4: pushes the requested slot's
    // bytes over the LIVE restore channel (ControlBlock::liveRestoreRequested
    // — see its own doc comment for why this is deliberately separate from
    // the crash-resurrection initialStateData channel) — the CHILD applies
    // it on its own next message-thread-equivalent tick, never the audio
    // thread. A no-op if the requested slot has never been stored yet.
    void restoreStateFromSlot (char whichSlot)
    {
        auto& slot     = (whichSlot == 'A') ? stateSlotA : stateSlotB;
        auto& hasState = (whichSlot == 'A') ? hasStateSlotA : hasStateSlotB;

        if (! hasState || slot.getSize() == 0)
        {
            logEvent ("DIAG A/B: restoreStateFromSlot(" + juce::String (whichSlot) + ") — no-op, slot never stored.");
            return;
        }

        auto* block = controlBlock.load (std::memory_order_acquire);

        if (block == nullptr)
        {
            logEvent ("DIAG A/B: restoreStateFromSlot(" + juce::String (whichSlot) + ") — no-op, controlBlock is null.");
            return;
        }

        using CB = CrateIPC::ControlBlock;
        const auto size = juce::jmin ((int64_t) slot.getSize(), CB::maxStateChunkBytes);

        // Store A/B Amnesia Fix (Step 53) directive: bounded spin, message
        // thread can afford it — see ControlBlock::liveRestoreLock's own
        // doc comment for the exact torn-buffer race this closes (a fast
        // A->B->A switch overwriting liveRestoreStateData while the CHILD
        // is still mid-read of the PREVIOUS request's bytes). 100 attempts
        // at ~1ms matches every other bounded-backoff idiom already used
        // across this codebase for a lock the CHILD could plausibly hold
        // for a brief moment — never an indefinite wait.
        int attempts = 0;
        bool acquired = false;

        while (! (acquired = ! block->liveRestoreLock.test_and_set (std::memory_order_acquire)))
        {
            if (++attempts > 100)
                break;
            juce::Thread::sleep (1);
        }

        if (! acquired)
        {
            logEvent ("DIAG A/B: restoreStateFromSlot(" + juce::String (whichSlot) + ") — could not acquire "
                          "liveRestoreLock within the bounded wait, skipping this request rather than risking a torn buffer.");
            return;
        }

        std::memcpy (block->liveRestoreStateData, slot.getData(), (size_t) size);
        block->liveRestoreStateSize.store (size, std::memory_order_relaxed);
        block->liveRestoreLock.clear (std::memory_order_release);
        block->liveRestoreRequested.store (true, std::memory_order_release);

        logEvent ("DIAG A/B: restoreStateFromSlot(" + juce::String (whichSlot) + ") — pushed "
                      + juce::String (size) + " bytes, liveRestoreRequested set.");
    }

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
            CRATE_FR_LOG ("IPC", "paramQueue OVERFLOW - push dropped.");
            return;
        }

        block->paramQueue[head] = CB::ParamChange { parameterIndex, normalizedValue };
        block->paramQueueHead.store (nextHead, std::memory_order_release);

        // Step 74 (The Flight Recorder) directive: this function is
        // callable from the AUDIO thread (see its own doc comment above) —
        // logEvent()'s file I/O would never be safe to call from there,
        // which is exactly why this exists. Lock-free, allocation-free,
        // safe from any thread.
        CRATE_FR_LOGF ("IPC", "paramQueue push idx=%d val=%.4f", parameterIndex, (double) normalizedValue);
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

    // Zero-Dropout Bridge directive (Step 52): the symmetric counterpart
    // to enableLookaheadMode() — flips the SAME flag back off, letting
    // applyToBuffer() fall through to the direct dispatchToSandbox()/
    // gatherFromSandbox() round trip on the very next block. Deliberately
    // does NOT stop/destroy lookaheadProducerThread: pumpLookaheadPipeline()
    // already early-returns false the instant it observes
    // block->isLookaheadMode false (see its own top-of-function guard),
    // so the thread simply goes idle (and its own exponential backoff
    // settles it to near-zero CPU) rather than needing to be torn down and
    // later recreated — re-enabling later is then just one atomic store
    // away, not a fresh thread spin-up. Used by refreshAutoLookaheadState()
    // to instantly drop OUT of lookahead mode the moment a track becomes
    // live — the Zero Latency Override contract (see
    // CrateAnticipativeWrapper's own doc comment for the original version
    // of this rule) is absolute: ANY queued latency is unacceptable the
    // instant a performer might be listening to their own input through
    // this plugin.
    void disableLookaheadMode()
    {
        lookaheadModeRequested.store (false, std::memory_order_release);

        if (auto* block = controlBlock.load (std::memory_order_acquire))
            block->isLookaheadMode.store (false, std::memory_order_release);
    }

    // Step 70 (Time-Slip Flush Storm / Auto-Demotion) directive: the
    // NEW entry point for a live parameter tweak (replacing the old
    // direct flushTimeSlipBuffer() call in parameterChanged()/
    // curveHasChanged()) — cheap, message-thread-only bookkeeping ONLY.
    // The instant the FIRST live edit of a burst is seen, lookahead is
    // demoted immediately via the SAME disableLookaheadMode() the Zero
    // Latency Override already uses for a track going live — applyToBuffer()
    // falls through to the ordinary real-time round trip on the very next
    // audio block, so the user hears their edit with no added latency
    // while the drag is still happening, same as a genuinely live track.
    // Every SUBSEQUENT edit in the same burst just updates the timestamp
    // — no repeated flush, no repeated disableLookaheadMode() call, no
    // repeated logEvent() file I/O. refreshAutoLookaheadState() (this
    // bridge's own per-tick decision hub, already running every
    // timerCallback() tick) is what watches for the burst going quiet and
    // does the ONE flush-and-re-engage once it has.
    void markLiveEditActivity()
    {
        lastLiveParamEditMs = juce::Time::getMillisecondCounter();

        if (! lookaheadSuspendedForLiveEdit && lookaheadModeRequested.load (std::memory_order_acquire))
        {
            lookaheadSuspendedForLiveEdit = true;
            logEvent ("Time-Slip Engine: live parameter edit detected — temporarily suspending lookahead "
                          "(Auto-Demotion) and falling back to the real-time round trip until the edit "
                          "stream goes quiet.");
            disableLookaheadMode();
        }
    }

    // Step 18 (Flush-on-Change) directive: called when the user changes a
    // parameter on a lookahead-enabled track — the buffered window was
    // rendered against the OLD parameter value and is now stale.
    //
    // Step 52 (Zero-Dropout Bridge) directive closes the gap this method's
    // own comment used to flag as "later work, once real (non-test-sweep)
    // automation exists to detect changes in": CrateSyncedParameter now
    // overrides te::AutomatableParameter's parameterChanged(float,
    // byAutomation) and calls THIS method only when byAutomation is false
    // (a genuinely live change — the reparented native VST3 UI being
    // dragged, or external live MIDI, NEVER ordinary timeline automation
    // playback) or when a synced parameter's own curveHasChanged() Listener
    // callback fires (the automation SHAPE itself was just edited, not
    // merely played back). Deterministic timeline automation (the common,
    // per-block case as transport plays an UNCHANGED curve forward) does
    // NOT flush at all — pumpLookaheadPipeline() instead bakes the correct
    // future curve value into each pre-rendered request as it goes, via
    // futureParamCache (see that member's own doc comment).
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

        // Zero-Dropout Bridge directive: the param-value precompute cache
        // is just as stale as the audio it would have been baked into —
        // restarted from the SAME position, on the SAME "message thread
        // owns this cursor" contract as nextParamPrecomputePosition's own
        // doc comment describes.
        {
            std::lock_guard<std::mutex> lock (futureParamCacheMutex);
            futureParamCache.clear();
        }
        nextParamPrecomputePosition = restartPosition;

        logEvent ("Time-Slip Buffer flushed (Flush-on-Change) — pipeline restarting from sample position "
                      + juce::String (restartPosition) + ".");
    }

    // Zero-Dropout Bridge directive (Step 52): keeps futureParamCache
    // topped up, one bounded batch per message-thread tick — see that
    // member's own doc comment for why this MUST run here (getValueAt()
    // is message-thread-only) rather than on LookaheadProducerThread,
    // which only ever CONSUMES what this method produces.
    //
    // Deliberately a no-op whenever lookahead mode isn't actually
    // requested, or the real parameter list hasn't been built yet
    // (syncedParameters empty) — nothing downstream would ever read the
    // cache in either case, so there's no reason to spend message-thread
    // cycles filling it.
    void topUpFutureParamCache()
    {
        if (! lookaheadModeRequested.load (std::memory_order_acquire) || ! realParameterListBuilt || syncedParameters.empty())
            return;

        const auto workerNeedsUpTo = nextProduceSamplePosition.load (std::memory_order_acquire)
                                          + (int64_t) futureParamCacheTargetBlocks * CrateIPC::ControlBlock::maxSamplesPerBlock;

        int64_t position = nextParamPrecomputePosition.load (std::memory_order_relaxed);
        int blocksThisTick = 0;

        std::vector<FutureParamBlock> newBlocks;

        while (position < workerNeedsUpTo && blocksThisTick < maxParamPrecomputeBlocksPerTick)
        {
            FutureParamBlock fpb;
            fpb.positionSamples = position;
            fpb.numValues = (int32_t) juce::jmin (syncedParameters.size(), (size_t) CrateIPC::ControlBlock::maxSyncedParams);

            const auto timePos = tcore::TimePosition::fromSamples (position, juce::jmax (1.0, cachedSampleRate));

            for (int i = 0; i < fpb.numValues; ++i)
                fpb.values[i] = te::getValueAt (*syncedParameters[(size_t) i], timePos);

            newBlocks.push_back (fpb);

            position += CrateIPC::ControlBlock::maxSamplesPerBlock;
            ++blocksThisTick;
        }

        if (newBlocks.empty())
            return;

        {
            std::lock_guard<std::mutex> lock (futureParamCacheMutex);

            for (auto& fpb : newBlocks)
                futureParamCache.push_back (fpb);

            // Defensive cap — matches this codebase's own established "no
            // unbounded growth, ever" contract for every fixed-purpose
            // buffer. Only reachable if the worker stalls far longer than
            // futureParamCacheTargetBlocks would ever normally allow.
            while (futureParamCache.size() > (size_t) (futureParamCacheTargetBlocks * 2))
                futureParamCache.pop_front();
        }

        nextParamPrecomputePosition.store (position, std::memory_order_relaxed);
    }

    // Zero-Dropout Bridge directive (Step 52): auto-engages Time-Slip/
    // lookahead mode for this bridge whenever its own track (and nothing
    // upstream of it) is live — same graph-walk shape as
    // CrateAnticipativeWrapper's own refreshLiveModeState()/
    // isTrackOrUpstreamLive() (Zero Latency Override, Step 4.5/4.8),
    // DELIBERATELY DUPLICATED here rather than shared: that class is an
    // already-stable, independently-proven component touched by many prior
    // steps, and refactoring it to share code with this NEW, higher-risk
    // feature in the same change would risk destabilizing working code for
    // no strict necessity — Rule of Three isn't met yet (exactly two call
    // sites across the whole codebase). If a THIRD consumer of this exact
    // walk ever appears, that's the point to factor it out for real.
    //
    // Checked every tick, not just once at connection time — the
    // transition matters more than the steady state: disengaging the
    // MOMENT a track goes live is the actual safety contract (Zero Latency
    // Override — "any queued latency at all is unacceptable the instant a
    // performer might be listening").
    void refreshAutoLookaheadState()
    {
        if (! isConnected() || lookaheadPermanentlyDemoted.load (std::memory_order_acquire))
            return;

        // Step 70 (Auto-Demotion during Edits) directive: runs BEFORE the
        // autoLookaheadEnabled opt-in gate below, deliberately —
        // markLiveEditActivity() (called from parameterChanged()/
        // curveHasChanged(), above) demotes UNCONDITIONALLY, for any
        // bridge with lookahead active regardless of whether that came
        // from the auto engage/disengage feature or a direct
        // enableLookaheadMode() call elsewhere. The matching re-engage
        // must be reachable just as unconditionally, or a bridge that
        // never opted into the auto feature would demote once on the
        // first live edit and never recover. Stays demoted until the
        // edit stream has been quiet for liveEditQuietPeriodMs; once it
        // has, flush exactly ONCE (restarting production from the
        // now-settled, POST-edit playhead — never a moving target).
        if (lookaheadSuspendedForLiveEdit)
        {
            if (juce::Time::getMillisecondCounter() - lastLiveParamEditMs < (juce::uint32) liveEditQuietPeriodMs)
                return; // still actively being edited — stay demoted

            lookaheadSuspendedForLiveEdit = false;

            // Forces the isLive comparison further down to actually act
            // on this same tick, rather than seeing "no change" against
            // a cached value that disableLookaheadMode() bypassed when
            // markLiveEditActivity() called it out-of-band.
            lastKnownTrackIsLive = -1;

            flushTimeSlipBuffer();
            logEvent ("Time-Slip Engine: live parameter edit stream went quiet for "
                          + juce::String (liveEditQuietPeriodMs) + "ms — flushed once, re-evaluating lookahead.");

            if (! autoLookaheadEnabled)
            {
                // No ongoing auto engage/disengage feature governing this
                // bridge — just restore the lookahead state it had before
                // this edit burst began, then stop; the isLive graph-walk
                // below is exclusively that opt-in feature's own concern.
                enableLookaheadMode();
                return;
            }
            // else: fall through — the isLive check below (which the auto
            // feature already re-evaluates every tick) decides whether to
            // re-engage or stay demoted, using the freshly-forced
            // lastKnownTrackIsLive sentinel above.
        }

        // Zero-Dropout Bridge directive (Step 52): OPT-IN, defaulted off —
        // see setAutoLookaheadEnabled()'s own doc comment for why this
        // can't simply be unconditional for every CrateSandboxBridge that
        // exists. Real user-facing plugin loads must explicitly request
        // this; anything that doesn't ask keeps today's exact behavior.
        if (! autoLookaheadEnabled)
            return;

        bool isLive = false;
        bool hasExtractableClip = false;

        if (auto* track = getOwnerTrack())
        {
            std::set<te::Track*> visited;
            isLive = isTrackOrUpstreamLive (track, visited);

            // Step 72 (Poison Pill / Hard Teardown) directive — QA
            // finding, proven via the OPAL guillotine logs: lookahead was
            // being auto-engaged on a track with NO WaveAudioClip at all
            // ("no WaveAudioClip found — lookahead extraction disabled"),
            // i.e. a pipeline pre-rendering pure SILENCE through the
            // plugin, on a background thread, for zero benefit — while
            // the lookahead watchdog stood armed over it. OPAL's own
            // processBlock() jammed servicing one of those pointless
            // silence requests (background-thread processBlock racing the
            // plugin's own live UI edits — a known third-party hazard the
            // Guillotine exists for), the watchdog correctly saw a stuck
            // pipeline, and a perfectly healthy child was executed over
            // work that should never have been queued. On an EMPTY,
            // idle project. No clip = nothing to look ahead at = no
            // pipeline, no watchdog, no execution. Checked here (message
            // thread — the only thread allowed to touch the Track/clip
            // model) every tick, so a clip being ADDED later engages
            // lookahead normally on the next tick.
            if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
                hasExtractableClip = ! te::getClipsOfType<te::WaveAudioClip> (*clipTrack).isEmpty();
        }

        const int isLiveInt = isLive ? 1 : 0;
        const int hasClipInt = hasExtractableClip ? 1 : 0;

        if (isLiveInt == lastKnownTrackIsLive && hasClipInt == lastKnownClipPresent)
            return;

        lastKnownTrackIsLive = isLiveInt;
        lastKnownClipPresent = hasClipInt;

        if (isLive)
        {
            logEvent ("Zero-Dropout Bridge: track went LIVE — disengaging lookahead mode (Zero Latency Override).");
            disableLookaheadMode();
        }
        else if (! hasExtractableClip)
        {
            logEvent ("Zero-Dropout Bridge: track is not live but has no WaveAudioClip — lookahead stays "
                          "disengaged (nothing to look ahead at; engaging would only pre-render silence "
                          "under an armed watchdog).");
            disableLookaheadMode();
        }
        else
        {
            // A clip exists now — if a PREVIOUS engage attempt latched
            // lookaheadNodeSetupFailed while the track was still empty,
            // un-latch it so buildLookaheadWaveNodeOnMessageThread() gets
            // a genuine retry against the clip that now exists (the
            // player is still null in that case, so the rebuild path
            // re-runs naturally on the next pump).
            if (lookaheadNodeSetupFailed && lookaheadNodePlayer == nullptr)
                lookaheadNodeSetupFailed = false;

            logEvent ("Zero-Dropout Bridge: track is not live — auto-engaging lookahead mode.");
            enableLookaheadMode();
        }
    }

    // Direct copy of CrateAnticipativeWrapper::isTrackDirectlyLive() — see
    // refreshAutoLookaheadState()'s own doc comment for why this is
    // duplicated rather than shared.
    bool isTrackDirectlyLive (te::Track& track) const
    {
        if (auto* playbackContext = edit.getCurrentPlaybackContext())
            for (auto* input : playbackContext->getAllInputs())
                if (input->isLivePlayEnabled (track) || input->isRecordingEnabled (track.itemID))
                    return true;

        return false;
    }

    // Direct copy of CrateAnticipativeWrapper::isTrackOrUpstreamLive() —
    // see refreshAutoLookaheadState()'s own doc comment for why this is
    // duplicated rather than shared. Covers both routing mechanisms TE
    // exposes (Direct Serial Output via Track::getInputTracks(), and Aux
    // Send/Return matched by bus number) — a live vocal feeding a reverb
    // bus feeding Master makes all three evaluate true. `visited` is a
    // cycle guard shared across the whole recursive walk from one
    // refreshAutoLookaheadState() tick.
    bool isTrackOrUpstreamLive (te::Track* track, std::set<te::Track*>& visited) const
    {
        if (track == nullptr || ! visited.insert (track).second)
            return false;

        if (isTrackDirectlyLive (*track))
            return true;

        for (auto* upstream : track->getInputTracks())
            if (isTrackOrUpstreamLive (upstream, visited))
                return true;

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

        // Step 39 (Universal Dry/Wet Mix) directive: recompute now that
        // the REAL sample rate is known — see mixRampStepPerSample's own
        // doc comment for the 48kHz fallback this replaces.
        if (cachedSampleRate > 0.0)
            mixRampStepPerSample = 1.0f / (0.005f * (float) cachedSampleRate);

        // Step 15.4 (The DSP Soft-Mute) directive: pre-allocated ONCE, here
        // — Zero Allocation Rule, same reasoning as AudioBridgeThread's own
        // workBuffer (Step 7). applyFadeOrSilence() only ever reads/writes
        // this on the audio thread; it must never trigger a real-time
        // allocation there. Safe to re-run on every initialise() call
        // (setSize() is a no-op once already the right size), unlike the
        // launch/attach + TimeSlipBuffer reset below.
        lastValidOutput.setSize (CrateIPC::ControlBlock::maxChannels, CrateIPC::ControlBlock::maxSamplesPerBlock);

        // Step 39 (Universal Dry/Wet Mix) directive: pre-allocated ONCE,
        // same Zero Allocation Rule reasoning as lastValidOutput just
        // above — readFromTimeSlipBuffer()'s TimeSlipBuffer::read() call
        // overwrites fc.destBuffer's own memory in place, so the dry
        // signal has to be captured into separate storage BEFORE that call
        // or it's gone by the time applyDryWetMix() needs it.
        dryCaptureBuffer.setSize (CrateIPC::ControlBlock::maxChannels, CrateIPC::ControlBlock::maxSamplesPerBlock);

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
            // Step 39 (Universal Dry/Wet Mix) directive: replaces the old
            // unconditional memcpy — fc.destBuffer still holds the ORIGINAL
            // dry signal at this exact point (dispatchToSandbox() only
            // ever READ from it, never modified it), and block->audioOutput
            // holds the CHILD's fully wet result. applyDryWetMix() blends
            // them and writes the result into fc.destBuffer itself — at the
            // default dryWetMix=1.0/bypassRequested=false, this reduces to
            // exactly the old memcpy's behaviour (100% wet), so an untouched
            // header changes nothing.
            {
                const float* dryChannels[CB::maxChannels] {};
                const float* wetChannels[CB::maxChannels] {};

                for (int ch = 0; ch < dispatchNumChannels; ++ch)
                {
                    dryChannels[ch] = fc.destBuffer->getReadPointer (ch, fc.bufferStartSample);
                    wetChannels[ch] = block->audioOutput + (size_t) ch * CB::maxSamplesPerBlock;
                }

                applyDryWetMix (fc, dryChannels, wetChannels, dispatchNumChannels, dispatchNumSamples);
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

        // Step 40 (Dry/Wet Buffer Safety) directive: dryCaptureBuffer is
        // pre-allocated in initialise() (Zero Allocation Rule — never
        // resized here, on the audio thread), but initialise() and
        // applyToBuffer() are not otherwise ordered against each other by
        // anything this method can see directly — a defensive bounds check
        // costs one comparison and closes off any possible path (a plugin
        // instance mid-teardown/reinit, a config change race) where this
        // buffer could be smaller than what THIS block needs. Falls back to
        // skipping the dry capture (and, below, the blend) entirely rather
        // than writing out of bounds — TimeSlipBuffer::read() still runs
        // and fc.destBuffer still ends up with valid (100% wet) audio
        // either way, exactly matching pre-Step-39 behaviour for this one
        // block.
        const bool dryCaptureBufferReady = dryCaptureBuffer.getNumChannels() >= numChannels
                                             && dryCaptureBuffer.getNumSamples() >= fc.bufferNumSamples;

        // Step 39 (Universal Dry/Wet Mix) directive: capture the dry signal
        // BEFORE TimeSlipBuffer::read() below overwrites fc.destBuffer's
        // own memory IN PLACE — unlike the real-time round trip (which
        // writes wet audio into a separate shared-memory buffer, leaving
        // fc.destBuffer untouched-dry until the blend), read() targets
        // fc.destBuffer's own write pointers directly, so the dry signal
        // is gone the instant it succeeds unless copied out first.
        if (dryCaptureBufferReady)
            for (int ch = 0; ch < numChannels; ++ch)
                dryCaptureBuffer.copyFrom (ch, 0, fc.destBuffer->getReadPointer (ch, fc.bufferStartSample), fc.bufferNumSamples);

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

        // Step 39 (Universal Dry/Wet Mix) directive: fc.destBuffer now
        // holds the just-read WET audio; dryCaptureBuffer (captured above)
        // holds the DRY signal. Blend them, writing the result back into
        // fc.destBuffer — at the default dryWetMix=1.0/bypassRequested=false
        // this reduces to exactly the prior behaviour (100% wet). Skipped
        // entirely if the dry capture itself was skipped (see
        // dryCaptureBufferReady's own comment above) — fc.destBuffer
        // already holds valid wet audio from the read() call either way.
        if (dryCaptureBufferReady)
        {
            const float* dryChannels[CB::maxChannels] {};
            const float* wetChannels[CB::maxChannels] {};

            for (int ch = 0; ch < numChannels; ++ch)
            {
                dryChannels[ch] = dryCaptureBuffer.getReadPointer (ch);
                wetChannels[ch] = fc.destBuffer->getReadPointer (ch, fc.bufferStartSample);
            }

            applyDryWetMix (fc, dryChannels, wetChannels, numChannels, fc.bufferNumSamples);
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

                    // Zero-Dropout Bridge directive (Step 52): pop the
                    // message-thread-precomputed automation values for
                    // THIS exact position off the front of futureParamCache
                    // — topUpFutureParamCache() only ever appends in
                    // strictly increasing position order (matching this
                    // same nextProduceSamplePosition-driven cadence), so
                    // the front of the deque is always either an exact
                    // match or (if the message thread hasn't caught up yet)
                    // strictly AHEAD of produceAt, never behind it.
                    // numParamValues == 0 (no match found yet) is a
                    // graceful degrade, not an error — see
                    // LookaheadRequestSlot::numParamValues's own doc
                    // comment in CrateIPCConstants.h.
                    slot.numParamValues = 0;
                    {
                        std::lock_guard<std::mutex> lock (futureParamCacheMutex);

                        while (! futureParamCache.empty() && futureParamCache.front().positionSamples < produceAt)
                            futureParamCache.pop_front(); // stale (pre-flush) entries this position's own restart already invalidated

                        if (! futureParamCache.empty() && futureParamCache.front().positionSamples == produceAt)
                        {
                            const auto& fpb = futureParamCache.front();
                            slot.numParamValues = fpb.numValues;
                            std::memcpy (slot.paramValues, fpb.values, (size_t) fpb.numValues * sizeof (float));
                            futureParamCache.pop_front();
                        }
                    }

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

        // Step 74 (The Flight Recorder) directive, Task 1b: see the
        // general restart sites' own matching comment.
        FlightRecorder::getInstance().flushToDisk ("WATCHDOG_KILL_LOOKAHEAD_HUNG");

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
            // Step 72 (Poison Pill / Hard Teardown) directive — QA
            // finding, proven via the OPAL guillotine logs: a RESTART
            // reuses this bridge's existing instanceId, and therefore the
            // existing shared-memory FILE, exactly as it was the moment
            // the previous child died. ensureSharedMemoryFileIsSized()
            // deliberately never re-zeroes an already-correctly-sized
            // file, and NOTHING else ever re-initialized the ControlBlock
            // across a restart — so a child killed mid-operation (the
            // Guillotine's whole purpose is killing children at arbitrary
            // moments) left every in-flight bit of IPC state poisoned for
            // the NEXT generation: a stateChunkLock/liveRestoreLock
            // atomic_flag left permanently SET by a killer-interrupted
            // holder (the fresh child's own StateExtractionThread then
            // spins on it forever — the observed "telemetry frozen after
            // respawn"); a lookahead request backlog the dead child never
            // serviced (the fresh child's LookaheadWorkerThread then
            // immediately drives the brand-new plugin instance through
            // the DEAD generation's stale requests, mid-initialization);
            // stale param-queue indices; a leftover triggerCrashRequested
            // that would make the fresh child execute itself on arrival.
            //
            // This is the ONE safe place to sanitize: the old child is
            // already dead (terminateSandboxProcess() above), the new one
            // hasn't been spawned yet (sandboxProcess.start() below) —
            // for this exact window the PARENT is provably the block's
            // only living user. Every cross-process handshake flag, ring
            // index, and spinlock is reset to its constructed default;
            // persistent PARENT-owned payload (pluginPath, sample rate,
            // initial state, registry verdicts) is written fresh
            // immediately after, exactly as before.
            block->parentReady.store (false, std::memory_order_relaxed);
            block->childProcessed.store (false, std::memory_order_relaxed);
            block->paramQueueHead.store (0, std::memory_order_relaxed);
            block->paramQueueTail.store (0, std::memory_order_relaxed);
            block->paramMetadataReady.store (false, std::memory_order_relaxed);
            block->paramValueRevision.store (0, std::memory_order_relaxed);
            block->windowHandleRequested.store (false, std::memory_order_relaxed);
            block->windowHandleReady.store (false, std::memory_order_relaxed);
            block->windowHandleValue.store (0, std::memory_order_relaxed);
            // Step 79 (Pre-Emptive Native Parenting) directive: the
            // Airlock slot from the PREVIOUS generation was already
            // destroyed by bridge.onChildTeardown (fired earlier in this
            // same restart sequence, before this sanitize block even
            // runs) — zeroing this too means the fresh CHILD can never
            // mistake a stale, now-destroyed slot handle for a valid one
            // before AirlockHWNDComponent republishes its own fresh slot.
            block->hostSlotHwndValue.store (0, std::memory_order_relaxed);
            block->windowWidth.store (0, std::memory_order_relaxed);
            block->windowHeight.store (0, std::memory_order_relaxed);
            block->stateChunkLock.clear (std::memory_order_release);
            block->stateChunkAvailable.store (false, std::memory_order_relaxed);
            block->stateChunkSize.store (0, std::memory_order_relaxed);
            block->liveRestoreRequested.store (false, std::memory_order_relaxed);
            block->liveRestoreStateSize.store (0, std::memory_order_relaxed);
            block->liveRestoreLock.clear (std::memory_order_release);
            block->corruptionDetected.store (false, std::memory_order_relaxed);
            block->triggerCrashRequested.store (false, std::memory_order_relaxed);
            block->pluginUIDReady.store (false, std::memory_order_relaxed);
            block->isLookaheadMode.store (false, std::memory_order_relaxed);
            block->fixedSizeLiarDetected.store (false, std::memory_order_relaxed);
            block->lookaheadRequestHead.store (0, std::memory_order_relaxed);
            block->lookaheadRequestTail.store (0, std::memory_order_relaxed);
            block->lookaheadResultHead.store (0, std::memory_order_relaxed);
            block->lookaheadResultTail.store (0, std::memory_order_release);

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

            // Step 55 (The Liar's Penalty) directive: pre-launch verdict
            // from the registry — a plugin convicted (in ANY past session,
            // or earlier in THIS one) of claiming canResize=true while
            // ignoring applied sizes gets forceFixedSizeEditor set before
            // the CHILD ever reads the block, so its editor-creation code
            // skips the lying probe entirely and publishes
            // canResize=false + min==max from the very first frame.
            // Step 60 directive: OR'd with this bridge's own explicit
            // 4K/HiDPI "broken giants" override — see
            // setForceFixedSizeAndDefaultScale()'s own doc comment.
            block->forceFixedSizeEditor.store (
                PluginHealthRegistry::getInstance().isForcedFixedSize (effectivePluginUID()) || forcedHardLockdownAndDefaultScale,
                std::memory_order_relaxed);
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

            // Step 55 (The Liar's Penalty) directive: same pre-read
            // verdict write as launchSandboxProcess() — the claim event
            // below is what wakes the CHILD into loadHostedPlugin(), so
            // this lands strictly before the editor could ever be created.
            // Step 60 directive: OR'd with this bridge's own explicit
            // 4K/HiDPI "broken giants" override — see
            // setForceFixedSizeAndDefaultScale()'s own doc comment.
            block->forceFixedSizeEditor.store (
                PluginHealthRegistry::getInstance().isForcedFixedSize (effectivePluginUID()) || forcedHardLockdownAndDefaultScale,
                std::memory_order_relaxed);
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

            // Step 55 (The Liar's Penalty) directive: same pre-dispatch
            // verdict write as launchSandboxProcess() — onTenantReady()
            // below is what makes the shared host actually load into this
            // block, so this lands strictly first.
            // Step 60 directive: OR'd with this bridge's own explicit
            // 4K/HiDPI "broken giants" override — see
            // setForceFixedSizeAndDefaultScale()'s own doc comment.
            block->forceFixedSizeEditor.store (
                PluginHealthRegistry::getInstance().isForcedFixedSize (effectivePluginUID()) || forcedHardLockdownAndDefaultScale,
                std::memory_order_relaxed);
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
        // Step 72 (Poison Pill / Hard Teardown) directive: sever the
        // editor's reparented HWND and cached per-generation state FIRST,
        // before the process is killed or anything is unmapped — see
        // onChildTeardown's own doc comment. Runs synchronously on this
        // same (message) thread, so there is no window in which any other
        // UI code could still be operating on the dying child's window.
        if (onChildTeardown)
            onChildTeardown();

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

        // Zero-Dropout Bridge directive (Step 52): auto-engage/disengage
        // lookahead mode based on this track's own live/armed state, and
        // keep the automation precompute cache topped up while engaged —
        // see refreshAutoLookaheadState()/topUpFutureParamCache()'s own
        // doc comments. Both are cheap no-ops in the common cases they
        // don't apply to (not yet connected, no synced parameters, not
        // in lookahead mode), so unconditional per-tick calls cost
        // negligible message-thread time.
        refreshAutoLookaheadState();
        topUpFutureParamCache();

        // Continuous State Sync directive (Step 11): poll the PUSH channel
        // — try-lock only, message thread can simply check again next tick
        // if genuinely contended, no need to wait. A full copy into
        // lastKnownState happens at most once per tick, at GUI-timer speed,
        // never per audio block.
        if (auto* stateBlock = controlBlock.load (std::memory_order_relaxed))
        {
            // Step 52 (The Anti-Zombie Auto-Respawn Protocol) directive: a
            // SEH-caught hardware fault inside the CHILD's own
            // AudioBridgeThread/LookaheadWorkerThread processBlock() call is
            // treated exactly like a DSP stall or heartbeat timeout —
            // dead is dead — and checked FIRST, unconditionally, for the
            // same reason dspStallDetected is checked first above: the
            // CHILD's HeartbeatThread is a completely separate thread from
            // the two that touch hostedPlugin, so a corrupted plugin
            // instance can keep ticking heartbeats along fine forever,
            // silently muted (silence, not passthrough — see
            // AudioBridgeThread/LookaheadWorkerThread's own isCorrupted
            // handling in Main.cpp) rather than ever being noticed and
            // respawned, unless this flag is polled independently of the
            // heartbeat check below. One-shot exchange, same edge-trigger
            // idiom as dspStallDetected — the CHILD never clears this flag
            // itself (a fresh instance after respawn starts with a fresh
            // placement-newed ControlBlock, see corruptionDetected's own
            // doc comment in CrateIPCConstants.h), so the PARENT owns
            // clearing it exactly once here.
            if (stateBlock->corruptionDetected.exchange (false, std::memory_order_acq_rel))
            {
                if (sandboxAlive.exchange (false, std::memory_order_acq_rel))
                    logEvent ("Plugin instance reported CORRUPTED (SEH fault caught by child) — declaring DEAD, bridge output silenced.");

                if (! restartInFlight)
                {
                    restartInFlight = true;

                    if (hasReachedAliveState)
                        PluginHealthRegistry::getInstance().recordCrash (effectivePluginUID());
                    else
                        logEvent ("Corruption reported before first heartbeat — treating as failed initialization, not a crash.");

                    // Same Quarantine Shuffle vs. direct-relaunch split as
                    // the DSP stall branch above: a Tenant Bridge asks
                    // SandboxManager for a fresh routing verdict instead of
                    // relaunching a process it doesn't own; isolated mode
                    // relaunches CrateSandbox.exe directly. Either path's
                    // normal launch sequence already writes lastKnownState
                    // into the initial-load channel (see launchSandboxProcess()'s
                    // own doc comment) — the existing Ghost Reload mechanism
                    // restores the plugin's last-known state into the
                    // freshly-spawned replacement with no extra code needed
                    // here.
                    if (isTenantMode)
                    {
                        logEvent ("Shared Sandbox host connection lost (plugin corrupted) — requesting Quarantine Shuffle re-route.");

                        if (onNeedsRerouting)
                            onNeedsRerouting (*this);
                        else
                            logEvent ("onNeedsRerouting not configured — Tenant Bridge stays dead.");
                    }
                    else
                    {
                        logEvent ("Restarting CrateSandbox child process (plugin corrupted)...");

                        // Step 74 (The Flight Recorder) directive, Task 1b:
                        // Watchdog-forced kill — one of the three flush
                        // triggers. Captures the exact sequence of
                        // HWND/IPC/heartbeat breadcrumbs leading up to this
                        // decision, before the process actually dies.
                        FlightRecorder::getInstance().flushToDisk ("WATCHDOG_KILL_PLUGIN_CORRUPTED");
                        launchSandboxProcess();
                    }
                }

                return;
            }

            if (! stateBlock->stateChunkLock.test_and_set (std::memory_order_acquire))
            {
                // Store A/B Amnesia Fix (Step 53) directive: while a live
                // restore is still PENDING (stateBlock->liveRestoreRequested
                // is set until the CHILD consumes it — see that field's own
                // doc comment), ANY state chunk the CHILD pushes here still
                // reflects its PRE-restore content — the CHILD hasn't
                // called setStateInformation() on the incoming slot's bytes
                // yet. Merging that stale content into lastKnownState is
                // exactly the race that made Store A/B lose data: a Store
                // click fired in this window would capture the OUTGOING
                // slot's content under the INCOMING slot's name. Deferred,
                // not dropped — stateChunkAvailable is left set, and the
                // CHILD's own "latest wins" convention (see this field's own
                // doc comment) means whatever it pushes AFTER the restore
                // actually lands is what a LATER tick consumes instead.
                const bool restorePending = stateBlock->liveRestoreRequested.load (std::memory_order_acquire);

                if (! restorePending && stateBlock->stateChunkAvailable.load (std::memory_order_relaxed))
                {
                    const auto size = (size_t) stateBlock->stateChunkSize.load (std::memory_order_relaxed);
                    lastKnownState = juce::MemoryBlock (stateBlock->stateChunkData, size);
                    stateBlock->stateChunkAvailable.store (false, std::memory_order_relaxed);
                    logEvent ("Received plugin state chunk (" + juce::String ((int) size) + " bytes).");
                }

                stateBlock->stateChunkLock.clear (std::memory_order_release);
            }

            // Step 55 (The Liar's Penalty) directive: the CHILD's Recovery
            // Guard just caught this plugin ignoring an applied resize
            // (claiming canResize=true, snapping back to creation size —
            // see ControlBlock::fixedSizeLiarDetected's own doc comment
            // for the VoxDucker evidence). Persist the conviction so ALL
            // future loads of this UID pre-write forceFixedSizeEditor and
            // never re-run the lying probe at all. The UI side needs no
            // extra handling here: the CHILD republished canResize=false
            // and min==max alongside this flag, which
            // PluginWindowContent::enforceResizeLimits()'s latch and
            // CrateEditorComponent's own canResize tick already translate
            // into setResizable(false, false) — nuking the OS resize
            // handle — within one tick.
            if (stateBlock->fixedSizeLiarDetected.exchange (false, std::memory_order_acq_rel))
            {
                PluginHealthRegistry::getInstance().recordFixedSizeLiar (effectivePluginUID());
                logEvent ("LIAR'S PENALTY: child proved this plugin ignores applied resizes despite claiming canResize=true — "
                              "recorded forcedFixedSize for UID " + effectivePluginUID() + "; all future loads will treat it as fixed-size.");
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

                    // Step 74 (The Flight Recorder) directive, Task 1b: see
                    // the plugin-corrupted restart's own matching comment.
                    FlightRecorder::getInstance().flushToDisk ("WATCHDOG_KILL_DSP_STALL");
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

            // Step 74 (The Flight Recorder) directive: a cheap breadcrumb
            // on every observed heartbeat pulse (not every 20ms poll tick —
            // only when the counter actually moved), so a flush triggered
            // by a subsequent stall/kill shows exactly when the Watchdog
            // last saw this child genuinely alive.
            CRATE_FR_LOGF ("WATCHDOG", "heartbeat pulse counter=%u [instanceId=%s]",
                            current, instanceId.toRawUTF8());

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

                    // Step 74 (The Flight Recorder) directive, Task 1b: see
                    // the plugin-corrupted restart's own matching comment.
                    FlightRecorder::getInstance().flushToDisk ("WATCHDOG_KILL_HEARTBEAT_TIMEOUT");
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

    // Step 39 (A/B Testing) directive, Task 4: two RAM-only snapshots of
    // lastKnownState — message-thread-only, same as lastKnownState itself
    // (PluginWindowHeader's own button callbacks run on the message
    // thread). hasStateSlotA/B distinguish "never stored" from "stored an
    // empty chunk" (a real, if unusual, possibility for a plugin with no
    // meaningful state).
    juce::MemoryBlock stateSlotA, stateSlotB;
    bool hasStateSlotA = false, hasStateSlotB = false;

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

    // Step 39 (Universal Dry/Wet Mix) directive: audio-thread-owned scratch
    // storage — see initialise()'s own comment for why this exists
    // (capturing the dry signal before TimeSlipBuffer::read() overwrites it
    // in place). Only ever touched from applyToBuffer()'s own thread.
    juce::AudioBuffer<float> dryCaptureBuffer;

    // Step 39 (Universal Dry/Wet Mix & Smart Bypass) directive: dryWetMix
    // is the user-facing knob value (0.0 = fully dry/original signal, 1.0 =
    // fully wet/processed — matches this bridge's pre-Step-39 behaviour
    // exactly at the default of 1.0, so an untouched header changes
    // nothing). bypassRequested overrides it toward 0.0 when set. Both
    // written from the message thread (PluginWindowHeader's own knob/
    // button callbacks), read with a lock-free load from the audio thread.
    // currentAppliedMix is the audio-thread-OWNED, per-sample-ramped actual
    // mix in effect right now — never written from any other thread — so
    // neither a knob twist nor a bypass toggle ever produces an audible
    // click, only a fast (~5ms) ramp toward the new target.
    std::atomic<float> dryWetMix { 1.0f };
    std::atomic<bool> bypassRequested { false };
    float currentAppliedMix = 1.0f;

    // Step 39 directive: a full 0.0<->1.0 ramp takes ~5ms regardless of
    // sample rate — matches the DSP Soft-Mute fade's own "fast enough to
    // feel instant, slow enough to never click" target. Computed from
    // cachedSampleRate once known; 48kHz/5ms is the conservative fallback
    // used before that (a slightly slower-than-intended ramp for one
    // block on a non-48kHz session is inaudible and harmless).
    float mixRampStepPerSample = 1.0f / (0.005f * 48000.0f);
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

    // Zero-Dropout Bridge directive (Step 52): a precomputed automation
    // value for every synced parameter, at one future block position —
    // computed on the MESSAGE THREAD ONLY (see this struct's own doc
    // comment for why: AutomationCurve::getValueAt() is
    // TRACKTION_ASSERT_MESSAGE_THREAD-guarded, confirmed via direct source
    // read of tracktion_AutomationCurve.cpp, unlike generateRealTrackAudioInto()'s
    // own offline-render call, which IS safe from LookaheadProducerThread).
    struct FutureParamBlock
    {
        int64_t positionSamples = -1;
        int32_t numValues = 0;
        float values[CrateIPC::ControlBlock::maxSyncedParams] {};
    };

    // Guarded by futureParamCacheMutex, NOT lock-free — deliberately: the
    // two participants are the MESSAGE thread (refreshAutoLookaheadState()/
    // timerCallback()'s own precompute top-up, below) and
    // LookaheadProducerThread (pumpLookaheadPipeline(), which already
    // tolerates blocking disk I/O and synchronous offline renders elsewhere
    // in this same method) — NEITHER is the real audio thread, so a plain
    // std::mutex is the honest, simple choice here rather than forcing a
    // lock-free structure neither side actually needs. Front = oldest
    // still-pending block; pumpLookaheadPipeline() pops from the front as
    // it consumes each position in order.
    std::mutex futureParamCacheMutex;
    std::deque<FutureParamBlock> futureParamCache;

    // How far ahead of nextProduceSamplePosition the message thread should
    // keep the cache filled — generous relative to lookaheadRingCapacity's
    // own in-flight pipe depth (64 slots), so the worker essentially never
    // has to fall back to "no param data yet for this block."
    static constexpr int futureParamCacheTargetBlocks = 128;

    // Bounded work per message-thread tick — a large gap (e.g. right after
    // a flush, or auto-engaging lookahead mode for the first time on a
    // long session) fills in gradually across several ticks instead of
    // stalling the message thread in one burst, same "bounded work per
    // tick" discipline as every other per-tick loop in this codebase.
    static constexpr int maxParamPrecomputeBlocksPerTick = 16;

    // MESSAGE-THREAD-OWNED cursor — how far precomputation has reached.
    // Kept atomic defensively, same convention as flushGenerationStartPosition/
    // nextProduceSamplePosition immediately above: flushTimeSlipBuffer()
    // (this field's only writer besides the precompute loop itself) is
    // documented message-thread-only today but treated with the same
    // "atomic, just in case" caution as its siblings rather than assumed
    // single-threaded by convention alone.
    std::atomic<int64_t> nextParamPrecomputePosition { 0 };

    // Zero-Dropout Bridge directive (Step 52): message-thread-only (only
    // ever touched from refreshAutoLookaheadState(), itself only ever
    // called from timerCallback()) — -1 = not yet determined (forces the
    // very first tick after connection to actually act, rather than a
    // plain bool defaulting to false and silently matching the common
    // "not live" case with no action taken).
    int lastKnownTrackIsLive = -1;

    // Step 72 (Poison Pill / Hard Teardown) directive: same -1 "not yet
    // determined" sentinel and message-thread-only ownership as
    // lastKnownTrackIsLive above — tracks whether the owning track had an
    // extractable WaveAudioClip on the last evaluated tick, so the
    // engage/disengage decision only re-fires (and re-logs) on a genuine
    // transition of EITHER signal.
    int lastKnownClipPresent = -1;

    // Step 70 (Time-Slip Flush Storm / Auto-Demotion) directive: same
    // message-thread-only ownership as lastKnownTrackIsLive above — both
    // only ever touched from markLiveEditActivity()/refreshAutoLookaheadState(),
    // themselves only ever called from parameter-change listener callbacks
    // and timerCallback(), all message-thread. liveEditQuietPeriodMs
    // matches the user's own explicit directive (500ms of silence before
    // trusting a drag has genuinely ended, rather than one heavy frame).
    static constexpr int liveEditQuietPeriodMs = 500;
    bool lookaheadSuspendedForLiveEdit = false;
    juce::uint32 lastLiveParamEditMs = 0;

    // Zero-Dropout Bridge directive (Step 52): see setAutoLookaheadEnabled()'s
    // own doc comment. Message-thread-only (only ever read from
    // refreshAutoLookaheadState()/timerCallback(), only ever written via
    // the public setter, itself intended to be called once, at real
    // plugin-insertion time).
    bool autoLookaheadEnabled = false;

    // Step 60 (4K/HiDPI Protection for Broken Giants) directive: see
    // setForceFixedSizeAndDefaultScale()'s own public doc comment.
    bool forcedHardLockdownAndDefaultScale = false;

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

    // Step 39 (Universal Dry/Wet Mix & Smart Bypass) directive, Task 2/4:
    // ONE shared blend implementation for BOTH audio-return paths
    // (gatherFromSandbox()'s real-time round trip AND readFromTimeSlipBuffer()'s
    // lookahead read) — each just supplies its own dry/wet source pointers,
    // since where those two live differs between the paths (see each call
    // site's own comment), but the actual blend math and click-free ramp
    // are identical either way. drySource/wetSource may safely alias
    // fc.destBuffer's own memory (both real call sites do, in different
    // directions) — dry[i]/wet[i] are read to compute the RHS BEFORE dst[i]
    // is written, which is safe even when dst, dry, or wet are the same
    // underlying pointer, since a single assignment expression fully
    // evaluates its right-hand side first.
    //
    // Smart Bypass (Task 4) is implemented as a ramp of THIS SAME mix
    // value toward 0.0 (fully dry) rather than a separate mechanism — a
    // deliberate, disclosed design choice: this bridge hosts an arbitrary
    // remote VST3 process, and there is no generic, reliable way to drive
    // a hosted plugin's own internal "soft bypass" parameter (if it even
    // has one) from here. Ramping toward the dry signal, exactly like a
    // knob turn, is honest about what it actually does (a host-side dry
    // pass-through, not the plugin's own bypass logic) and is click-free
    // by construction, which is the actual requirement ("prevent clicks").
    void applyDryWetMix (const te::PluginRenderContext& fc,
                        const float* const* drySource, const float* const* wetSource,
                        int numChannels, int numSamples)
    {
        if (fc.destBuffer == nullptr || numSamples <= 0)
            return;

        const float targetMix = bypassRequested.load (std::memory_order_relaxed)
                                     ? 0.0f
                                     : dryWetMix.load (std::memory_order_relaxed);

        float mix = currentAppliedMix;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* dst = fc.destBuffer->getWritePointer (ch, fc.bufferStartSample);
            auto* dry = drySource[ch];
            auto* wet = wetSource[ch];

            mix = currentAppliedMix; // every channel ramps along the identical trajectory (same target, same step)

            for (int i = 0; i < numSamples; ++i)
            {
                mix += juce::jlimit (-mixRampStepPerSample, mixRampStepPerSample, targetMix - mix);
                dst[i] = dry[i] * (1.0f - mix) + wet[i] * mix;
            }
        }

        currentAppliedMix = mix; // persist the ramp's end-of-block position — identical across channels, so the last one computed is correct for all
    }

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
