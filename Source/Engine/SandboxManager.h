#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include "CrateSandboxBridge.h"
#include "CrateIPCConstants.h"
#include "PluginHealthRegistry.h"
#include "SandboxRouter.h"

#include <deque>
#include <map>

namespace te = tracktion::engine;

/**
    Plugin Sandboxing Step 14 (The Confinement Router / "The Crate Brain") —
    the single choke point every plugin load must pass through from here on.

    Before this step, MainComponent (and CrateStressTest/CrateWorkflowManager)
    called edit.getPluginCache().createNewPlugin(CrateSandboxBridge::xmlTypeName, ...)
    directly. That's now forbidden by convention — SandboxManager is the ONLY
    class that's allowed to authorize and dispense a sandbox, because it's
    the only class that consults the Health Registry FIRST.

    UID RESOLUTION (see PluginHealthRegistry::lookupCachedUID's own doc
    comment): this class deliberately does NOT boot a disposable child
    process just to identify a plugin before deciding how to sandbox it —
    that would cost a real VST3 load (potentially seconds, for a sample
    library) on every single load, on top of the "innocent until proven
    guilty" gap it wouldn't even close on a first-ever load (a plugin that's
    never run at all still has no crash history no matter how it's
    identified). Instead this reads the PATH -> UID cache that
    CrateSandboxBridge itself populates the moment any real launch resolves
    the authentic UID (Step 13.5). A path with no cached UID yet is, by
    definition, a plugin this registry has no history for.

    STEP 22 (The Profiling Database & Smart Confinement Routing / "The
    Warden") directive: the actual verdict — Shared vs. Solitary — is now
    SandboxRouter::route()'s call, not this class's own inline logic (see
    SandboxRouter.h for the two rules and the disclosed policy change: an
    unknown-history plugin now defaults to Solitary, not Shared, until its
    vendor is resolved and found trustworthy — a stricter posture than
    Step 14's original "innocent until proven guilty" default). This class
    stays the single interception point; SandboxRouter only decides.

    STEP 15.1 (Multi-Tenant IPC & The Control Channel) laid the plumbing: a
    fresh instanceID (a juce::Uuid) per Shared-routed request, a running
    shared-host CrateSandbox.exe (launched with --shared-host if needed),
    and a Master Control Channel (CrateIPC::SharedHostCommandBlock, an SPSC
    ring buffer in its own shared-memory file) to dispatch {pluginUID,
    pluginPath, instanceID} spawn commands into.

    STEP 15.2 (The Shared Host Engine) is where the training wheels come
    off: the shared host now actually consumes that queue and hosts
    tenants for real (see TenantContext/CommandListenerThread in
    Source/Sandbox/Main.cpp) — so createSandboxPlugin() no longer falls
    back to an isolated CrateSandboxBridge for the Shared verdict. Instead
    it configures the returned bridge as a TENANT (CrateSandboxBridge::
    configureAsTenant()): it attaches to the CrateIPC_Memory_<instanceID>
    shared memory the shared host will load a real plugin into, but never
    spawns or owns a juce::ChildProcess of its own — see
    CrateSandboxBridge::attachAsTenant()'s own doc comment for the full
    per-bridge contract. The Solitary verdict is UNCHANGED: a proven-hostile
    plugin still gets its own fully isolated CrateSandboxBridge/
    CrateSandbox.exe pair, precisely so one hostile tenant can never take
    down every OTHER tenant sharing a process with it (see Main.cpp's
    serviceTenant() for why that blast radius is real, not hypothetical).

    STEP 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    the Solitary Confinement path's own remaining latency source — spawning
    a genuinely new CrateSandbox.exe process (CreateProcess, VST3 DLL load,
    JUCE init) on every drag-and-drop — is now hidden entirely. A small pool
    (capacity cryosleepPoolCapacity) of ALREADY-LAUNCHED, cryosleeping
    processes sits idle at ~0% CPU (each blocked on its own per-instance
    NamedEvent — see Main.cpp's own CryosleepWaitThread), shared-memory
    already mapped, waiting only to be told which plugin to load. Claiming
    one is just: pop it, write pluginPath/hostSampleRate/hostBlockSize into
    its already-mapped ControlBlock, signal one event — no OS process
    creation anywhere in that path. Only the Solitary route uses this (the
    Shared route's own ensureSharedHostRunning() already has no per-plugin
    process-spawn cost at all — it's a message to an ALREADY-running host).
    Replenishment back up to capacity happens on its own low-priority
    background thread (replenishPoolAsync()) — the message/audio threads
    never feel a CreateProcess call, matching this class's own established
    "never block the real-time or UI path" discipline.

    STEP 15.3 (Self-Healing & Auto-Quarantine) closes the loop: when a
    shared host dies, EVERY tenant bridge that was attached to it notices
    independently (each one polls its OWN per-tenant ControlBlock's
    heartbeat — see CrateSandboxBridge::timerCallback()'s two death-
    detection branches) and calls back into handleTenantNeedsRerouting()
    below via the onNeedsRerouting callback configureAsTenant() wired up.
    This is "Guilt by Association" — every tenant sharing a host that died
    gets its OWN crash count incremented (already happens, unconditionally,
    before this callback even fires — see those same branches), regardless
    of which one actually caused it, because there is no way to know which
    one did. handleTenantNeedsRerouting() is "the Quarantine Shuffle": it
    re-checks THIS tenant's crash count fresh (post-increment) and either
    converts the bridge to isolated (if this death just crossed the
    Solitary Confinement threshold) or reconnects it to a fresh Shared
    Sandbox host slot (if still stable) — a per-tenant decision, not an
    all-or-nothing one, since three tenants hit by the same crash can end
    up with three different post-crash counts if their prior histories
    differed.
*/
class SandboxManager
{
public:
    static SandboxManager& getInstance()
    {
        static SandboxManager instance;
        return instance;
    }

    // Step 30 (Completing the Proxy Illusion) directive: the preferred
    // overload — CrateWorkflowManager (the native drag-drop/instrument-menu
    // path) already has the REAL, fully-scanned juce::PluginDescription in
    // hand at the call site; passing it through lets the bridge impersonate
    // the target plugin's actual name/vendor everywhere the UI queries it
    // (see CrateSandboxBridge::setImpersonatedDescription()). Delegates to
    // the path-only overload below for the actual routing/creation logic.
    te::Plugin::Ptr createSandboxPlugin (te::Edit& edit, const juce::PluginDescription& description)
    {
        auto bridgePlugin = createSandboxPlugin (edit, description.fileOrIdentifier);

        if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
            bridge->setImpersonatedDescription (description);

        return bridgePlugin;
    }

    // Step 14 directive: the original path-only overload — kept for call
    // sites that only ever had a bare file path (e.g. MainComponent's own
    // JUCE_DEBUG-only test harness), which have no PluginDescription to
    // impersonate with. A bridge created this way falls back to "Sandbox
    // Bridge" as its displayed name (see CrateSandboxBridge::getName()) —
    // acceptable for a debug harness, not for the real native UI, which
    // should always go through the overload above instead.
    te::Plugin::Ptr createSandboxPlugin (te::Edit& edit, const juce::String& pluginPath)
    {
        // Step 22 (The Warden) directive: this IS the interception point —
        // the routing verdict is decided here, BEFORE createNewPlugin()/
        // insertPlugin() below ever build the real bridge or IPC channel.
        // See SandboxRouter's own doc comment for the actual rules and the
        // disclosed policy change (crashCount>0, not >=3; unresolved vendor
        // now defaults to Solitary rather than Shared) this represents.
        const auto cachedUID = PluginHealthRegistry::getInstance().lookupCachedUID (pluginPath);
        const auto decision  = SandboxRouter::route (cachedUID);
        const bool routeToShared = decision.verdict == SandboxRouter::Verdict::SharedSandbox;

        CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: " + decision.reason);

        auto bridgePlugin = edit.getPluginCache().createNewPlugin (CrateSandboxBridge::xmlTypeName, juce::PluginDescription());

        if (routeToShared)
        {
            // Step 15.1 directive: a fresh instanceID PER REQUEST, generated
            // here regardless of whether cachedUID is known yet — this ID
            // identifies THIS tenant request, not the plugin family (that's
            // what pluginUID/cachedUID is for).
            const auto instanceID = juce::Uuid().toString();

            if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
            {
                // Step 15.2 directive: the dispatch itself is deferred to
                // attachAsTenant() (called from THIS bridge's own
                // initialise(), once sampleRate/blockSize are actually
                // known) via this callback — see configureAsTenant()'s own
                // doc comment for why the bridge invokes SandboxManager
                // through a std::function instead of a direct call (avoids
                // a header circular-include between the two classes).
                //
                // Step 15.3 directive: the second callback is the
                // Quarantine Shuffle's entry point — see
                // handleTenantNeedsRerouting()'s own doc comment.
                //
                // Step 15.4 directive: the third callback is the Teardown
                // Protocol — invoked once, from this bridge's own
                // deinitialise(), the moment it stops being a tenant of
                // ANY kind (removed from the track, not a mode-conversion
                // — see dispatchUnloadCommand()'s own doc comment).
                bridge->configureAsTenant (instanceID, pluginPath,
                    [this, cachedUID] (const juce::String& path, const juce::String& tenantInstanceId)
                    {
                        dispatchSpawnCommand (cachedUID, path, tenantInstanceId);
                    },
                    [this] (CrateSandboxBridge& reroutingBridge)
                    {
                        handleTenantNeedsRerouting (reroutingBridge);
                    },
                    [this] (const juce::String& tenantInstanceId)
                    {
                        dispatchUnloadCommand (tenantInstanceId);
                    });
            }
        }
        else if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
        {
            // Step 27 (Real Sandbox Wiring) directive — a real bug this
            // step exposed: without this call, an isolated-mode bridge
            // silently launches whatever pluginPathForLaunch's own default
            // member initializer (getTestPluginPath(), a hardcoded test
            // path) happens to be, completely ignoring the pluginPath this
            // very call was given. See
            // setPluginPathForIsolatedLaunch()'s own doc comment for the
            // full explanation of why this never surfaced before now. This
            // is what claimFromPool()/launchSandboxProcess() both actually
            // read as WHICH plugin to load, regardless of which of the two
            // ends up running (see immediately below).
            bridge->setPluginPathForIsolatedLaunch (pluginPath);

            // Step 33 (Cryosleep Architecture) directive: try to claim an
            // already-warm process first — this is the ENTIRE latency win.
            // Falls through to the untouched cold-start path
            // (launchSandboxProcess(), via initialise()'s own default
            // branch) if the pool is empty — correctness never depends on
            // the pool being warm, only responsiveness does.
            if (auto claimed = claimFromPool())
            {
                const auto claimedInstanceId = claimed->instanceId;

                {
                    const juce::ScopedLock lock (poolLock);
                    claimedPoolProcesses[claimedInstanceId] = std::move (claimed);
                }

                bridge->configureFromPool (claimedInstanceId, [this, claimedInstanceId]
                {
                    releaseClaimedProcess (claimedInstanceId);
                });

                CrateSandboxBridge::logToSharedLog (
                    "CRATE BRAIN: Cryosleep pool claim — instanceID=" + claimedInstanceId
                        + " (zero process-spawn latency for this load).");
            }
            else
            {
                CrateSandboxBridge::logToSharedLog (
                    "CRATE BRAIN: Cryosleep pool empty — falling back to a cold-started sandbox process for this load.");
            }

            replenishPoolAsync(); // top the pool back up regardless of whether this claim succeeded
        }

        return bridgePlugin;
    }

    // Step 33 (Cryosleep Architecture) directive: called once, early —
    // CrateWorkflowManager's own constructor does this right after engine
    // setup, so the pool has real wall-clock time to warm up WHILE the user
    // is still looking at an empty project, not during their first drag.
    // Safe to call more than once (replenishPoolAsync() is itself
    // idempotent-safe — see its own guard).
    void warmUpCryosleepPool()
    {
        replenishPoolAsync();
    }

private:
    SandboxManager() = default;

    // Step 33/34 (Cryosleep Architecture) directive — a real gap this exact
    // rebuild caught live: juce::ChildProcess's own destructor does NOT
    // kill its process (documented on the class itself: "deleting this
    // object won't terminate the child process") — a CLAIMED pool process
    // is fine (its owning bridge's terminateSandboxProcess() releases it
    // explicitly via onPoolProcessRelease -> releaseClaimedProcess()), but
    // an UNCLAIMED, still-cryosleeping one has no other owner at all.
    // Without this, closing the app orphaned every still-warm pool slot —
    // confirmed by directly observing leftover CrateSandbox.exe processes
    // still running after the Parent had already exited. SandboxManager is
    // a function-local static singleton, so this runs at genuine program
    // shutdown.
    ~SandboxManager()
    {
        const juce::ScopedLock lock (poolLock);

        for (auto& slot : cryosleepPool)
            if (slot->process.isRunning())
                slot->process.kill();

        for (auto& [id, slot] : claimedPoolProcesses)
            if (slot->process.isRunning())
                slot->process.kill();

        // Step 37 (The Debt Sweep) fix: the EXACT same juce::ChildProcess-
        // destructor-doesn't-kill-its-process gap the two loops above were
        // already written to close — just missed for the shared host,
        // which can be hosting multiple live tenants at once, making this
        // the worse leak of the two. Same "function-local static singleton,
        // so this runs at genuine program shutdown" reasoning as the rest
        // of this destructor.
        if (sharedHostProcess.isRunning())
            sharedHostProcess.kill();
    }

    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // one already-launched, cryosleeping (or, once claimed, actively
    // serving) CrateSandbox.exe process. Kept in EITHER cryosleepPool
    // (idle, unclaimed) OR claimedPoolProcesses (claimed, now owned by a
    // live CrateSandboxBridge via its onPoolProcessRelease callback) — never
    // both, and every instanceId this class ever generates for pooling
    // purposes ends up in exactly one of the two until released.
    struct PooledSandboxProcess
    {
        juce::String instanceId;

        // Step 37 (The Debt Sweep) directive, Task 4: CrateIPC::JobObjectProcess,
        // not juce::ChildProcess — see its own doc comment. Every pooled
        // process is now assigned to the shared sandbox Job Object the
        // instant it launches, so it can never outlive a hard-killed/
        // crashed Parent the way plain juce::ChildProcess handles could.
        CrateIPC::JobObjectProcess process;
    };

    static constexpr int cryosleepPoolCapacity = 2;

    // Step 33 directive: creates a BRAND NEW pool slot — generates a fresh
    // instanceId, creates/sizes its shared memory file (same helper
    // CrateSandboxBridge::launchSandboxProcess() itself uses), and launches
    // CrateSandbox.exe in cryosleep mode. Deliberately callable from ANY
    // thread (replenishPoolAsync()'s own background thread calls this) —
    // juce::ChildProcess::start() and plain file I/O are OS calls, not
    // JUCE Component/message-thread-only operations, unlike almost
    // everything else in CrateSandboxBridge.
    std::unique_ptr<PooledSandboxProcess> launchOnePooledProcess()
    {
        auto slot = std::make_unique<PooledSandboxProcess>();
        slot->instanceId = juce::Uuid().toString();

        CrateIPC::ensureSharedMemoryFileIsSized (CrateIPC::getSharedMemoryFile (slot->instanceId));

        auto exe = CrateIPC::resolveSandboxExecutable();

        if (! exe.existsAsFile())
        {
            CrateSandboxBridge::logToSharedLog (
                "CRATE BRAIN: Cryosleep pool — CrateSandbox.exe not found at " + exe.getFullPathName()
                    + " — cannot warm up a new pool slot.");
            return nullptr;
        }

        slot->process.start (juce::StringArray { exe.getFullPathName(), slot->instanceId, CrateIPC::getCryosleepModeFlag() });

        CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: Cryosleep pool — warmed up a new slot, instanceID=" + slot->instanceId);

        return slot;
    }

    // Step 33 directive: pops the oldest idle slot under lock. Returns
    // nullptr if the pool is empty — the caller's own cold-start fallback
    // is what makes this safe to call unconditionally; an empty pool is a
    // (temporary) responsiveness regression, never a correctness one.
    std::unique_ptr<PooledSandboxProcess> claimFromPool()
    {
        const juce::ScopedLock lock (poolLock);

        if (cryosleepPool.empty())
            return nullptr;

        auto slot = std::move (cryosleepPool.front());
        cryosleepPool.pop_front();
        return slot;
    }

    // Step 33 directive: fires once per call site that needs the pool
    // topped back up (every claim, plus the one explicit warmUpCryosleepPool()
    // call at startup) — poolReplenishInProgress is what keeps concurrent
    // callers from spinning up MULTIPLE overlapping replenishment threads at
    // once; only one is ever in flight, and it keeps working (checking the
    // pool's current size itself) until the pool is genuinely back at
    // capacity, so a burst of several claims in a row is still correctly
    // topped up by a SINGLE thread rather than needing one per claim.
    void replenishPoolAsync()
    {
        if (poolReplenishInProgress.exchange (true))
            return; // a replenishment pass is already running — it will notice and fill whatever's still short

        juce::Thread::launch (juce::Thread::Priority::low, [this]
        {
            for (;;)
            {
                int currentSize;

                {
                    const juce::ScopedLock lock (poolLock);
                    currentSize = (int) cryosleepPool.size();
                }

                if (currentSize >= cryosleepPoolCapacity)
                    break;

                if (auto slot = launchOnePooledProcess())
                {
                    const juce::ScopedLock lock (poolLock);
                    cryosleepPool.push_back (std::move (slot));
                }
                else
                {
                    break; // CrateSandbox.exe missing or launch failed — no point busy-retrying immediately
                }
            }

            poolReplenishInProgress = false;
        });
    }

    // Step 33 directive: called via a claimed bridge's own
    // onPoolProcessRelease callback (see terminateSandboxProcess()'s own
    // comment) — this is what actually kills the underlying OS process and
    // stops tracking it, the moment the plugin that claimed it is removed
    // or the app shuts down. Safe to call for an instanceId that's already
    // been released (a no-op — matches configureFromPool()'s own "cleared
    // after firing once" contract on the bridge side).
    void releaseClaimedProcess (const juce::String& claimedInstanceId)
    {
        const juce::ScopedLock lock (poolLock);

        auto it = claimedPoolProcesses.find (claimedInstanceId);

        if (it == claimedPoolProcesses.end())
            return;

        if (it->second->process.isRunning())
            it->second->process.kill();

        claimedPoolProcesses.erase (it);
    }

    juce::CriticalSection poolLock;
    std::deque<std::unique_ptr<PooledSandboxProcess>> cryosleepPool;                    // protected by poolLock
    std::map<juce::String, std::unique_ptr<PooledSandboxProcess>> claimedPoolProcesses; // protected by poolLock
    std::atomic<bool> poolReplenishInProgress { false };

    // Step 15.3 (Self-Healing & Auto-Quarantine) directive: "the Quarantine
    // Shuffle." Invoked by a tenant bridge's OWN death-detection (see
    // CrateSandboxBridge::timerCallback()'s two branches) — by this point
    // that bridge has ALREADY called PluginHealthRegistry::recordCrash()
    // for itself (Guilt by Association happens unconditionally, before
    // this ever runs), so re-checking SandboxRouter::route() here (Step 22
    // directive) reads the POST-crash count. This is a per-tenant decision
    // made fresh for each affected bridge individually — three tenants
    // that shared a host which just died can land in three different
    // places if their crash histories before this incident weren't
    // identical (e.g. one was already at 1, the others at 0 — and per
    // SandboxRouter's own stricter Rule B, even that single prior crash is
    // now enough to quarantine on its own).
    void handleTenantNeedsRerouting (CrateSandboxBridge& bridge)
    {
        const auto pluginPath = bridge.getConfiguredPluginPath();
        const auto cachedUID  = PluginHealthRegistry::getInstance().lookupCachedUID (pluginPath);
        const auto decision   = SandboxRouter::route (cachedUID);

        if (decision.verdict == SandboxRouter::Verdict::SolitaryConfinement)
        {
            CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: Quarantine Shuffle — " + decision.reason);
            bridge.convertToIsolatedAndRelaunch();
        }
        else
        {
            CrateSandboxBridge::logToSharedLog (
                "CRATE BRAIN: Quarantine Shuffle — tenant still stable. Re-routing to a fresh Shared Sandbox host.");

            // A fresh instanceID for a fresh tenant slot — the old one
            // belonged to a shared host that no longer exists. See
            // reconfigureAsTenant()'s own doc comment.
            const auto freshInstanceID = juce::Uuid().toString();

            bridge.reconfigureAsTenant (freshInstanceID,
                [this, cachedUID] (const juce::String& path, const juce::String& tenantInstanceId)
                {
                    dispatchSpawnCommand (cachedUID, path, tenantInstanceId);
                });
        }
    }

    // Step 15.1 directive: idempotent, same convention as
    // CrateSandboxBridge::launchSandboxProcess()'s own guard — safe to call
    // on every single Shared-routed request; only actually launches
    // anything the FIRST time (or if the shared host process has died —
    // no restart/health-monitoring for the shared host itself yet, that's
    // later work alongside the real consumption loop).
    void ensureSharedHostRunning()
    {
        if (sharedHostProcess.isRunning())
            return;

        // Parent creates/sizes/maps the command channel BEFORE the shared
        // host process exists — same "no race about who gets there first"
        // reasoning as CrateSandboxBridge::launchSandboxProcess() applies
        // to the per-instance ControlBlock.
        auto file = CrateIPC::getSharedHostCommandChannelFile();
        CrateIPC::ensureSharedHostCommandChannelFileIsSized (file);

        commandChannelMemory = std::make_unique<juce::MemoryMappedFile> (file, juce::MemoryMappedFile::readWrite);

        if (commandChannelMemory->getData() != nullptr
            && commandChannelMemory->getSize() == (size_t) CrateIPC::sharedHostCommandChannelBytes)
        {
            commandBlock = CrateIPC::getSharedHostCommandBlock (commandChannelMemory->getData());

            // Unlike the per-instance ControlBlock (which the CHILD resets
            // via placement-new on every one of ITS launches), nothing
            // consumes this channel yet to reset it — and the backing file
            // persists across app runs (never deleted on a clean exit), so
            // a previous session's head/tail/queue contents could otherwise
            // leak into a new one. The PARENT is the only side that has
            // ever written to this block so far, so it's the only side
            // that can safely guarantee a fresh state here, exactly once
            // per genuine (re)launch of the shared host process.
            new (commandBlock) CrateIPC::SharedHostCommandBlock();
        }
        else
        {
            commandBlock = nullptr;
            CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: FAILED to map Master Control Channel at " + file.getFullPathName());
            return;
        }

        auto exe = CrateIPC::resolveSandboxExecutable();

        if (! exe.existsAsFile())
        {
            CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: CrateSandbox.exe not found at "
                                                     + exe.getFullPathName() + " — cannot launch Shared Sandbox host.");
            return;
        }

        sharedHostProcess.start (juce::StringArray { exe.getFullPathName(), CrateIPC::getSharedHostModeFlag() });
        CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: Launched Shared Sandbox host process (" + CrateIPC::getSharedHostModeFlag() + ").");
    }

    // Step 15.1 directive: writes into the SPSC ring buffer and signals the
    // shared host's wake event — see CrateIPC::SharedHostCommandBlock's own
    // doc comment for why this is a real multi-slot queue, not a single
    // "latest wins" command.
    void dispatchSpawnCommand (const juce::String& pluginUID, const juce::String& pluginPath, const juce::String& instanceID)
    {
        ensureSharedHostRunning();
        dispatchCommand (CrateIPC::SharedHostCommandBlock::SpawnCommand::Type::Spawn, pluginUID, pluginPath, instanceID);
    }

    // Step 15.4 (The Teardown Protocol) directive: dispatched when a
    // TenantBridge is destroyed (see CrateSandboxBridge::deinitialise()'s
    // onTenantRemoved callback) so the shared host can free that tenant's
    // RAM instead of leaking it forever. Deliberately does NOT call
    // ensureSharedHostRunning() — if the shared host isn't running, there
    // is nothing to unload: its entire address space, and every tenant's
    // RAM inside it, already went away with the process. Relaunching a
    // fresh host just to hand it an unload command for a tenant that
    // process never even knew about would be pure waste.
    void dispatchUnloadCommand (const juce::String& instanceID)
    {
        if (! sharedHostProcess.isRunning())
        {
            CrateSandboxBridge::logToSharedLog (
                "CRATE BRAIN: Shared host not running — skipping unload dispatch for instanceID=" + instanceID
                + " (its RAM already went away with the process).");
            return;
        }

        dispatchCommand (CrateIPC::SharedHostCommandBlock::SpawnCommand::Type::Unload, {}, {}, instanceID);
    }

    // Shared low-level write, used by both dispatchSpawnCommand() and
    // dispatchUnloadCommand() — identical ring-buffer mechanics either way,
    // only the Type discriminator and which fields are meaningful differ.
    void dispatchCommand (CrateIPC::SharedHostCommandBlock::SpawnCommand::Type type,
                         const juce::String& pluginUID, const juce::String& pluginPath, const juce::String& instanceID)
    {
        auto* block = commandBlock;

        if (block == nullptr)
        {
            CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: Cannot dispatch command — Master Control Channel not mapped.");
            return;
        }

        const auto head     = block->commandQueueHead.load (std::memory_order_relaxed);
        const auto tail     = block->commandQueueTail.load (std::memory_order_acquire);
        const auto nextHead = (head + 1) % (uint32_t) CrateIPC::SharedHostCommandBlock::commandQueueCapacity;

        if (nextHead == tail)
        {
            CrateSandboxBridge::logToSharedLog ("CRATE BRAIN: Master Control Channel queue full — dropping command for instanceID=" + instanceID);
            return;
        }

        auto& slot = block->commandQueue[head];
        slot.type = type;
        std::memset (slot.pluginUID, 0, sizeof (slot.pluginUID));
        std::memset (slot.instanceId, 0, sizeof (slot.instanceId));
        std::memset (slot.pluginPath, 0, sizeof (slot.pluginPath));
        pluginUID.copyToUTF8 (slot.pluginUID, (size_t) CrateIPC::SharedHostCommandBlock::SpawnCommand::maxPluginUIDLength - 1);
        instanceID.copyToUTF8 (slot.instanceId, (size_t) CrateIPC::SharedHostCommandBlock::SpawnCommand::maxInstanceIdLength - 1);
        pluginPath.copyToUTF8 (slot.pluginPath, (size_t) CrateIPC::SharedHostCommandBlock::SpawnCommand::maxPluginPathLength - 1);

        block->commandQueueHead.store (nextHead, std::memory_order_release);
        commandReadyEvent.signal();

        CrateSandboxBridge::logToSharedLog (juce::String (type == CrateIPC::SharedHostCommandBlock::SpawnCommand::Type::Spawn
                                                               ? "CRATE BRAIN: Dispatched SPAWN command via Master Control Channel — pluginUID="
                                                               : "CRATE BRAIN: Dispatched UNLOAD command via Master Control Channel — pluginUID=")
                                                 + pluginUID + ", path=" + pluginPath + ", instanceID=" + instanceID);
    }

    // Step 37 (The Debt Sweep) directive, Task 4: same JobObjectProcess
    // swap as PooledSandboxProcess above — the shared host can outlive
    // several tenants at once, making it the worse of the two leaks this
    // was already found to have (see ~SandboxManager()'s own comment).
    CrateIPC::JobObjectProcess sharedHostProcess;
    std::unique_ptr<juce::MemoryMappedFile> commandChannelMemory;
    CrateIPC::SharedHostCommandBlock* commandBlock = nullptr;
    CrateIPC::NamedEvent commandReadyEvent { CrateIPC::getSharedHostCommandReadyEventName() };

    JUCE_DECLARE_NON_COPYABLE (SandboxManager)
};
