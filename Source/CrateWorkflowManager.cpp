#include "CrateWorkflowManager.h"
#include "UI/PluginWindow.h"
#include "Engine/CrateAnticipativeWrapper.h"
#include "Engine/CrateSandboxBridge.h"
#include "Engine/SandboxManager.h"

#include <set>

namespace
{
    // Step 60 (4K/HiDPI Protection for Broken Giants) directive: a
    // CURATED SEED LIST, not an exhaustive authority — disclosed honestly,
    // same spirit as this codebase's own PluginHealthRegistry doc
    // comments about its identifier caveats. There is no general way to
    // KNOW in advance which third-party plugin's own internal HiDPI/
    // scaling engine will break under a high-density display; this is a
    // manually-maintained list of ones already known to (the user's own
    // named example: iZotope Ozone, and its own suite-mates, which share
    // the same underlying UI framework) — expand it as new offenders are
    // actually observed, don't treat its absence as proof a plugin is
    // safe. Matched against EITHER the plugin's own name or its
    // manufacturer, case-insensitively, substring match (a plugin's
    // reported name often includes a version/edition suffix the vendor
    // string doesn't).
    bool isKnownBrokenGiantOnHiDPI (const juce::PluginDescription& description)
    {
        static const juce::StringArray knownOffenders { "ozone", "izotope", "neutron", "nectar", "rx " };

        const auto name   = description.name.toLowerCase();
        const auto vendor = description.manufacturerName.toLowerCase();

        for (auto& offender : knownOffenders)
            if (name.contains (offender) || vendor.contains (offender))
                return true;

        return false;
    }

    // Step 60 directive: "4K" as a DPI-SCALE signal, not a raw pixel-
    // resolution one — a scale factor is what actually determines whether
    // a plugin's own UI framework has to cope with high-density rendering
    // at all (a 4K panel running at 100% OS scale never exercises the
    // exact code path that breaks; a 1440p panel scaled to 150-200% DOES)
    // — 2.0 is the common "4K at 200%" Windows default, used as the
    // threshold. Reads the PRIMARY display only: this check runs at
    // plugin-INSERTION time, before any window (and therefore before any
    // real per-monitor placement) exists, so there is no more specific
    // display to ask yet.
    bool isHighDensityDisplay()
    {
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            return display->scale >= 2.0;

        return false;
    }

    // Same custom ValueTree property AutomationLaneComponent writes to/reads from.
    // Duplicated here (rather than including a UI header from this engine-level
    // class) since prepareAutomationForSave()'s validation pass only needs to check
    // for the property's presence, not decode it.
    const juce::Identifier anchorMetadataProperty ("crateAnchors");

    // Bus/Return Default Collapsed State directive: same key
    // TrackHeaderComponent reads to seed its fold micro-state — duplicated
    // rather than sharing a UI header, same reasoning as
    // anchorMetadataProperty just above.
    const juce::Identifier foldedPropertyID ("crateFolded");

    // BUG AUTOPSY: TE's default EngineBehaviour::getEditLimits() caps a track at
    // EditLimits::maxPluginsOnTrack == 16 total plugins (tracktion_EngineBehaviour.h)
    // — a sane guard rail for TE's own demo apps, never overridden here, so it was
    // silently in force the whole time. PluginList::insertPlugin() checks that
    // limit against the RAW pluginList.size() — every count of EVERY te::Plugin on
    // the track, not just user-visible inserts — and every track already carries 2
    // always-on utility plugins (VolumeAndPanPlugin + LevelMeterPlugin, added by
    // addDefaultTrackPlugins()) that count against the same total. That left only
    // 14 real slots of headroom before the 15th insert hit numPlugins (14 + 2) >=
    // 16, tripped the limit check, and PluginList::insertPlugin() returned a null
    // Plugin::Ptr — a debug-only jassertfalse, no exception, no user-facing error —
    // which CrateWorkflowManager::loadPluginOntoTrack() then passed straight to
    // pluginList.insertPlugin() as a no-op. Both the Mixer's ghost slot and the
    // Universal Device Chain funnel through that exact same call on the exact same
    // te::PluginList, which is why the failure was identical and silent in both:
    // this was never a UI/Viewport bug, it was one shared backend ceiling.
    struct CrateEngineBehaviour : public te::EngineBehaviour
    {
        te::EditLimits getEditLimits() override
        {
            auto limits = te::EngineBehaviour::getEditLimits();
            limits.maxPluginsOnTrack = 256; // effectively unlimited for any real mixing session
            return limits;
        }

        // Bulletproof Live Mode / Plugin Sandboxing directive: TE's OOP
        // support is SCANNING isolation, not full runtime per-plugin process
        // sandboxing (that would require a much larger out-of-process audio
        // bridge this open-source engine doesn't ship) — a plugin that
        // crashes or hangs DURING a scan takes down only the disposable
        // child scan process, never this app, which is exactly where a rogue
        // VST3 is most likely to misbehave (first load/enumeration). Requires
        // the matching PluginManager::startChildProcessPluginScan() early-out
        // in JUCEApplication::initialise() — see Main.cpp.
        bool canScanPluginsOutOfProcess() override { return true; }

        // Multi-Core directive: TE's own default already returns every
        // available core (SystemStats::getNumCpus()) — reserving one for the
        // message/UI thread instead is the actual live-set-hardening change:
        // leaves headroom for GUI hit-testing/painting to stay responsive
        // even when every audio core is saturated by a heavy session,
        // instead of the audio graph and the UI thread contending for the
        // very last core.
        int getNumberOfCPUsToUseForAudio() override
        {
            return juce::jmax (1, juce::SystemStats::getNumCpus() - 1);
        }
    };
}

CrateWorkflowManager::CrateWorkflowManager()
{
    // CrateUIBehaviour supplies plugin editor window creation — TE's default
    // UIBehaviour::createPluginWindow() returns nullptr and deliberately leaves this
    // to the host app. CrateEngineBehaviour raises the default 16-plugin-per-track
    // ceiling — see its doc comment above for why that default was being hit.
    engine = std::make_unique<te::Engine> ("The Crate Studio", std::make_unique<CrateUIBehaviour>(), std::make_unique<CrateEngineBehaviour>());

    // Engine Registration directive (Anticipative FX Step 2): the engine has
    // no way to construct a CrateAnticipativeWrapper from its own xmlTypeName
    // (createNewPlugin()/ValueTree recreation on load) until a built-in type
    // is registered for it — same mechanism TE's own PluginManager::
    // initialise() uses internally for EqualiserPlugin/CompressorPlugin/etc.,
    // just called from host app code instead of vendored engine code.
    engine->getPluginManager().createBuiltInType<CrateAnticipativeWrapper>();

    // Plugin Sandboxing Step 5 directive: register the structural bridge
    // plugin the same way — registration doesn't mean any track carries one
    // yet, just that the engine can construct it when something asks for
    // xmlTypeName (the stress test's own verification, later steps' real
    // per-plugin sandboxing).
    engine->getPluginManager().createBuiltInType<CrateSandboxBridge>();

    // Step 33 (Zero-Latency Warm Pooling / Cryosleep Architecture) directive:
    // kicked off here, at app boot, specifically so the pool has real
    // wall-clock time (each pooled process takes roughly the same
    // process-spawn latency this whole feature exists to hide) to warm up
    // WHILE the user is still looking at an empty project — not lazily on
    // their first drag, which would defeat the entire point.
    SandboxManager::getInstance().warmUpCryosleepPool();

    initialiseAudioDevice();

    edit = te::createEmptyEdit (*engine, juce::File());
    edit->getTransport().ensureContextAllocated();

    // Zero-dB Master Default directive: te::createEmptyEdit() leaves the
    // Master bus at TE's own -3dB headroom default — this DAW wants a new
    // project's Master to open at absolute unity gain instead.
    if (auto masterVolume = edit->getMasterVolumePlugin())
        masterVolume->setVolumeDb (0.0f);
}

CrateWorkflowManager::~CrateWorkflowManager()
{
    if (edit != nullptr)
    {
        edit->getTransport().stop (false, true);
        engine->getAudioFileManager().releaseAllFiles();
    }
}

void CrateWorkflowManager::initialiseAudioDevice()
{
    // TE's default PropertyStorage persists the last-used driver/device/sample
    // rate/buffer size and restores it here automatically.
    engine->getDeviceManager().initialise (0, 2);
}

//==============================================================================
void CrateWorkflowManager::loadPluginToSelectedTrack (const juce::PluginDescription& description)
{
    // Audio Thread Audit hardening: every entry point that touches
    // pluginList.insertPlugin()/edit->deleteTrack() MUST only ever be reached
    // from a UI event (mouse click, drag-drop, menu action) — all of which are
    // already message-thread-only by construction — never from a background
    // thread. This is a debug-only tripwire against a future regression (e.g.
    // a callback wired to fire from a worker thread), not a fix for anything
    // currently broken: every existing call site already satisfies it.
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    auto* audioTrack = dynamic_cast<te::AudioTrack*> (currentSelectedTrack);

    if (audioTrack == nullptr)
    {
        // Previously a silent no-op — indistinguishable from "instantiation failed"
        // to anyone testing it. Most likely cause of that exact symptom: no track
        // was selected (click a header first), not a PluginManager/KnownPluginList
        // problem — description here already came from
        // knownPluginList.getTypes()[row] in PluginBrowserComponent, so it's by
        // construction already a known type; there's nothing to re-register.
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "No Track Selected",
            "Click a track's header to select it first, then load \"" + description.name + "\" — "
            "there's currently no target track.");
        return;
    }

    if (edit == nullptr)
        return;

    // Strict Track Hierarchy (MASTER_ARCHITECTURE 3.5): same gate as
    // loadPluginOntoTrack()'s own — the Browser double-click path lands
    // here, NOT there, and QA proved an instrument could slip onto an
    // Audio track through exactly this hole. Checked BEFORE instantiation.
    if (description.isInstrument && ! trackAcceptsInstrument (*audioTrack))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Cannot Load Instrument on an Audio Track",
            "\"" + description.name + "\" is an instrument — it can only be loaded onto a MIDI/Instrument track. "
            "Select a MIDI track first, or create one.");
        return;
    }

    edit->getUndoManager().beginNewTransaction ("Load Plugin: " + description.name);

    auto plugin = instantiateExternalPlugin (description);

    if (plugin == nullptr)
        return;

    audioTrack->pluginList.insertPlugin (plugin, -1, nullptr);

    // Step 32 (Exorcise the Ghost & Fix the HWND) directive: same fix as
    // UniversalDeviceChainComponent's own updateNativeUiButtonEnablement()
    // and InsertsRackComponent's "Pop & Sync" click handler —
    // getWrappedAudioProcessor() is always nullptr for a CrateSandboxBridge
    // (the real AudioProcessor lives in the sandboxed CHILD process, never
    // locally), which silently skipped auto-opening the UI for every
    // sandboxed plugin loaded this way.
    if (auto* processor = plugin->getWrappedAudioProcessor())
    {
        if (processor->hasEditor())
            plugin->showWindowExplicitly();
    }
    else if (dynamic_cast<CrateSandboxBridge*> (plugin.get()) != nullptr)
    {
        plugin->showWindowExplicitly();
    }
}

bool CrateWorkflowManager::trackAcceptsInstrument (te::Track& track)
{
    // Strict Track Hierarchy (MASTER_ARCHITECTURE 3.5) — see this method's
    // own doc comment in the header. Only a real AudioTrack can ever host
    // an instrument (Master/folder/return tracks never can), and among
    // AudioTracks, only one this app already treats as MIDI: carrying an
    // instrument (te::Plugin::isSynth() — the SAME engine-native check
    // ArrangementComponent's trackHasInstrument() established as this
    // app's one true track-type test, rather than an app-level flag that
    // could drift from what the engine thinks the track renders), or
    // holding MIDI clips (covers a MIDI track whose instrument was
    // deleted — its clips still prove what kind of track it is).
    auto* audioTrack = dynamic_cast<te::AudioTrack*> (&track);

    if (audioTrack == nullptr)
        return false;

    for (auto* p : audioTrack->pluginList)
        if (p != nullptr && p->isSynth())
            return true;

    for (auto* c : audioTrack->getClips())
        if (dynamic_cast<te::MidiClip*> (c) != nullptr)
            return true;

    return false;
}

bool CrateWorkflowManager::loadPluginOntoTrack (const juce::PluginDescription& description, te::Track& targetTrack, int insertIndex)
{
    // See loadPluginToSelectedTrack()'s identical tripwire above.
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    if (edit == nullptr)
        return false;

    // Master is a mastering/effects bus — it can never host an
    // Instrument/Synth. Checked BEFORE instantiation (not after) so a bad
    // drop/menu pick never even spins up the plugin instance.
    if (description.isInstrument && &targetTrack == edit->getMasterTrack())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Cannot Load Instrument on Master",
            "\"" + description.name + "\" is an instrument — the Master track only accepts effects/mastering plugins.");
        return false;
    }

    // Strict Track Hierarchy (MASTER_ARCHITECTURE 3.5) — QA caught a
    // Synthesizer ("Ting") successfully loading onto an Audio Track. This
    // is the AUTHORITATIVE gate: every load path (drag-drop from any UI
    // component, menu picks, double-clicks) funnels through this method,
    // so even a drop target whose own isInterestedInDragSource validation
    // is missing or bypassed can never actually instantiate an instrument
    // onto an Audio track. Checked BEFORE instantiation, same as the
    // Master guard above — a rejected drop never even spins up the
    // (potentially expensive, sandbox-process-spawning) plugin instance.
    if (description.isInstrument && ! trackAcceptsInstrument (targetTrack))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Cannot Load Instrument on an Audio Track",
            "\"" + description.name + "\" is an instrument — it can only be loaded onto a MIDI/Instrument track. "
            "Create a MIDI track (or drop onto an existing one) instead.");
        return false;
    }

    edit->getUndoManager().beginNewTransaction ("Drop Plugin: " + description.name);

    auto plugin = instantiateExternalPlugin (description);

    if (plugin == nullptr)
        return false;

    targetTrack.pluginList.insertPlugin (plugin, insertIndex, nullptr);
    return true;
}

te::Plugin::Ptr CrateWorkflowManager::instantiateExternalPlugin (const juce::PluginDescription& description)
{
    if (edit == nullptr)
        return {};

    // Step 29 (Native Sandbox Interception) directive: this is now the ONLY
    // door any plugin load may come through — SandboxManager consults the
    // Health Registry and SandboxRouter (The Warden) before ever touching
    // the real IPC bridge, exactly the same interception every earlier
    // sandbox test harness this session already went through. What used to
    // be a direct edit->getPluginCache().createNewPlugin (te::ExternalPlugin::
    // xmlTypeName, description) call is now routed through the sandbox.
    //
    // Step 30 (Completing the Proxy Illusion) directive: passes the FULL
    // description (not just its fileOrIdentifier) — this is what lets
    // CrateSandboxBridge impersonate the target plugin's real name/vendor
    // (see SandboxManager::createSandboxPlugin()'s PluginDescription
    // overload and CrateSandboxBridge::setImpersonatedDescription()).
    // SandboxManager::createSandboxPlugin() returns a te::Plugin::Ptr (a
    // CrateSandboxBridge instance) with the exact same return contract this
    // function already had.
    auto plugin = SandboxManager::getInstance().createSandboxPlugin (*edit, description);

    if (plugin == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Plugin Load Failed",
            "\"" + description.name + "\" could not be instantiated. It may have been "
            "moved, deleted, or be incompatible with this build (bitness/format).");
    }
    else if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (plugin.get()))
    {
        // Zero-Dropout Bridge directive (Step 52): opt in HERE, at the one
        // real, user-facing plugin-insertion door this function's own doc
        // comment already establishes — deliberately NOT set inside
        // SandboxManager::createSandboxPlugin() itself, since
        // CrateStressTest.cpp calls that SAME factory directly for its own
        // round-trip-timing verification and must keep observing the
        // direct synchronous IPC path unchanged (see
        // setAutoLookaheadEnabled()'s own doc comment in
        // CrateSandboxBridge.h for the exact regression this avoids).
        bridge->setAutoLookaheadEnabled (true);

        // Step 60 (4K/HiDPI Protection for Broken Giants) directive: same
        // "one real door" reasoning as setAutoLookaheadEnabled() above —
        // checked here, once, before this bridge's first launch, using
        // `description` (already known immediately, no need to wait for
        // async UID/vendor resolution) and the CURRENT display's scale
        // (see isHighDensityDisplay()'s own doc comment for why scale,
        // not raw resolution). Forces this specific bridge into the
        // Sizing Policy's hardLockdown track and a flat 100% display
        // scale for the CHILD, regardless of what the plugin itself
        // claims or what the real monitor scale is.
        if (isHighDensityDisplay() && isKnownBrokenGiantOnHiDPI (description))
        {
            bridge->setForceFixedSizeAndDefaultScale (true);
            CrateSandboxBridge::logToSharedLog ("4K/HiDPI Protection: \"" + description.name
                                                     + "\" matches the known-broken-giants list on a high-density display — "
                                                       "forcing Fixed-Size / Default-Scale mode.");
        }
    }

    return plugin;
}

void CrateWorkflowManager::startPlayback()
{
    if (edit == nullptr)
        return;

    auto& transport = edit->getTransport();
    lastPlayStartSeconds = transport.getPosition().inSeconds();
    transport.play (false);
}

void CrateWorkflowManager::stopAndReturnToStart()
{
    if (edit == nullptr)
        return;

    auto& transport = edit->getTransport();
    transport.stop (false, false);
    transport.setPosition (tracktion::TimePosition::fromSeconds (lastPlayStartSeconds));
}

void CrateWorkflowManager::deleteSelectedTrack()
{
    // See loadPluginToSelectedTrack()'s identical tripwire above — deleting a
    // track tears down every PluginWindow on it (PluginWindowState's
    // unique_ptr<Component> RAII, see the Window Lifecycle section of the
    // Stability Audit Report), which must happen on the message thread.
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    if (currentSelectedTrack == nullptr || edit == nullptr)
        return;

    edit->getUndoManager().beginNewTransaction ("Delete Track");

    edit->deleteTrack (currentSelectedTrack);
    currentSelectedTrack = nullptr;
}

void CrateWorkflowManager::createAndRouteNewFXChannel (te::Track& sourceTrack)
{
    // Hybrid Bus/Return Architecture — "+ Create New FX Channel" macro (Sends
    // "+" menu), now REAL. An "FX Return Track" is not a special TE subclass —
    // it's a plain te::AudioTrack that happens to host a te::AuxReturnPlugin.
    // That is the ONE definition used anywhere in this app to recognise one
    // (see Source/UI/TrackUtils.h's isReturnTrack(), which the Mixer/
    // Arrangement UI split reads) — created here and nowhere else, so there
    // is exactly one place a return track's shape can ever drift from.
    if (edit == nullptr)
        return;

    jassert (juce::MessageManager::existsAndIsCurrentThread());

    // Step 1: lowest available, unused Aux Bus ID. Duplicated rather than
    // calling into SendBusUtils::scanBuses() (Source/UI/) — this is
    // engine-level code and deliberately never includes a UI header (see
    // anchorMetadataProperty's comment up top for the same rule applied to
    // prepareAutomationForSave()).
    std::set<int> busesInUse;
    for (auto* t : te::getAudioTracks (*edit))
        for (auto* p : t->pluginList)
            if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                busesInUse.insert (send->getBusNumber());

    int busNumber = 1;
    while (busesInUse.count (busNumber) > 0)
        ++busNumber;

    // One transaction for the whole macro (new track + both plugins) — a
    // single Ctrl+Z undoes the entire FX channel, send included, per the
    // Lead Architect's explicit requirement.
    edit->getUndoManager().beginNewTransaction ("Create New FX Channel (Bus " + juce::String (busNumber) + ")");

    // Step 2: the new AudioTrack. Docking it visually at the bottom of the
    // Arrangement/Mixer is the UI split's job (TrackUtils::splitTracks(),
    // read by MixerComponent/ArrangementComponent) — insertion POSITION in
    // the Edit's own track list doesn't matter for that, so end-of-tracks is
    // fine, matching ArrangementComponent::addTrack()'s identical call.
    auto returnTrack = edit->insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (*edit), nullptr);

    if (returnTrack == nullptr)
        return;

    // Step 3.
    returnTrack->setName ("Bus " + juce::String (busNumber) + " FX");

    // Fused Identity Block directive (Engine Fix): same random-colour
    // assignment ArrangementComponent::addTrack() gives a regular track —
    // harmless here even though TrackHeaderComponent::paint() always forces
    // BrandGray over a return track's Column 1 regardless of this value, but
    // keeping every new te::AudioTrack colour-assigned the same way (one
    // rule, no special-casing by track type) is simpler than carving out an
    // exception, and the colour is still real Undo-tracked track state.
    // Soft Pastel Auto-Colours directive: low saturation (0.3-0.5 range,
    // calm studio-label look), high brightness — not the harsher, more
    // saturated neon this used to generate.
    returnTrack->setColour (juce::Colour::fromHSV (juce::Random::getSystemRandom().nextFloat(), 0.35f, 0.85f, 1.0f));

    // Bus/Return Default Collapsed State directive: a brand-new FX Return
    // track opens as a minimal COLLAPSED strip, not full expanded height —
    // it's DSP plumbing the user just asked for, not something they need to
    // immediately look at Column 2/3 controls on. TrackHeaderComponent reads
    // this same property back to seed its fold micro-state.
    returnTrack->state.setProperty (foldedPropertyID, true, nullptr);

    // Step 4: the RETURN side of the bus, on the new track.
    if (auto returnPlugin = edit->getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, juce::PluginDescription()))
    {
        if (auto* ret = dynamic_cast<te::AuxReturnPlugin*> (returnPlugin.get()))
            ret->busNumber = busNumber;

        returnTrack->pluginList.insertPlugin (returnPlugin, -1, nullptr);
    }

    // Step 5: the SEND side of the bus, on the track that asked for this.
    if (auto sendPlugin = edit->getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, juce::PluginDescription()))
    {
        if (auto* send = dynamic_cast<te::AuxSendPlugin*> (sendPlugin.get()))
            send->busNumber = busNumber;

        sourceTrack.pluginList.insertPlugin (sendPlugin, -1, nullptr);
    }

    // A new track now exists — see onTrackListChanged's own doc comment for
    // why this is the one place that needs to say so.
    if (onTrackListChanged)
        onTrackListChanged();
}

//==============================================================================
void CrateWorkflowManager::prepareAutomationForSave()
{
    if (edit == nullptr)
        return;

    int flaggedCount = 0;

    for (auto plugin : te::getAllPlugins (*edit, false))
    {
        for (auto* param : plugin->getAutomatableParameters())
        {
            auto& curve = param->getCurve();

            if (curve.getNumPoints() > 1 && ! curve.state.hasProperty (anchorMetadataProperty))
                ++flaggedCount;
        }
    }

    if (flaggedCount > 0)
        DBG ("CrateWorkflowManager: " << flaggedCount
             << " automation curve(s) have points but no anchor metadata — they'll "
                "round-trip as plain baked points rather than editable macro anchors.");
}

void CrateWorkflowManager::rebuildAutomationAfterLoad()
{
    // Intentional no-op — see the header doc comment. AutomationLaneComponent reads
    // the persisted "crateAnchors" property in its own constructor, which runs when
    // MainComponent rebuilds the UI in response to safeLoadProject()'s callback.
}

//==============================================================================
void CrateWorkflowManager::saveProject()
{
    if (edit == nullptr)
        return;

    prepareAutomationForSave();

    if (currentProjectFile == juce::File())
        promptSaveAs();
    else
        writeCurrentEditToFile();
}

void CrateWorkflowManager::promptSaveAs()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Save Crate Project \xe2\x80\x94 choose a location and type a project name",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.crate");

    constexpr auto chooserFlags = juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting;

    fileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();

        if (chosen == juce::File())
            return; // cancelled

        // The typed filename (minus extension) is both the project name and the
        // root folder name — one dialog gives "a destination and a project name" in
        // the same standard Save-dialog gesture.
        const auto projectName = chosen.getFileNameWithoutExtension();
        const auto projectRoot = chosen.getParentDirectory().getChildFile (projectName + " - Crate Project");

        createProjectFolderStructure (projectRoot);

        currentProjectFile = projectRoot.getChildFile (projectName + ".crate");

        // TE's render/proxy/freeze cache — and eventually live recordings — resolve
        // through this per-Edit directory. Pointing it inside the project folder is
        // what makes the project self-contained/portable rather than scattering
        // generated audio into a system temp path.
        edit->setTempDirectory (projectRoot.getChildFile ("Audio").getChildFile ("Recorded"));

        writeCurrentEditToFile();
    });
}

void CrateWorkflowManager::writeCurrentEditToFile()
{
    if (edit == nullptr || currentProjectFile == juce::File())
        return;

    // Writes the Edit's ValueTree state directly to an arbitrary path — doesn't need
    // TE's ProjectManager/ProjectItem asset-database machinery, which is a heavier
    // system for managing many edits/media files that this app doesn't need.
    te::EditFileOperations (*edit).writeToFile (currentProjectFile, false);
}

void CrateWorkflowManager::createProjectFolderStructure (const juce::File& projectRoot) const
{
    projectRoot.createDirectory();
    projectRoot.getChildFile ("Audio").getChildFile ("Recorded").createDirectory();
    projectRoot.getChildFile ("Audio").getChildFile ("Bounces").createDirectory();
    projectRoot.getChildFile ("Imported").createDirectory();
    projectRoot.getChildFile ("Backups").createDirectory();
}

//==============================================================================
void CrateWorkflowManager::safeLoadProject (std::function<void()> onBeforeEditSwap,
                                             std::function<void (te::Edit&)> onAfterEditSwap)
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Load Crate Project",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.crate");

    constexpr auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (chooserFlags, [this, onBeforeEditSwap, onAfterEditSwap] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();

        if (chosen == juce::File() || ! chosen.existsAsFile())
            return;

        // Load into a temporary FIRST. A bad/corrupt file must leave the current
        // edit and its UI fully intact and functional — neither callback runs
        // unless the replacement is confirmed good.
        auto newEdit = te::loadEditFromFile (*engine, chosen);

        if (newEdit == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                "Load Failed",
                "\"" + chosen.getFileName() + "\" could not be read as a Crate Project — "
                "it may be corrupt or from an incompatible version. Your current project "
                "has not been changed.");
            return; // current edit/UI untouched; neither callback is invoked
        }

        // New edit confirmed good. Everything from here to the end of this lambda
        // runs synchronously — no callAsync, no yield back to the message loop — so
        // there is no window where a timer tick or queued repaint could reach a UI
        // component that's mid-teardown or pointing at an already-destroyed Edit.
        if (onBeforeEditSwap)
            onBeforeEditSwap();

        // Clear the selection before touching the edit itself so nothing still
        // alive (there shouldn't be anything after onBeforeEditSwap, but this is
        // free insurance) can dereference a track about to be freed.
        currentSelectedTrack = nullptr;

        if (edit != nullptr)
        {
            // clearDevices=true cleanly detaches the transport from the live audio
            // graph before the Edit it belongs to is destroyed, rather than leaving
            // that graph pointing at an about-to-be-freed Edit for however long it
            // takes the destructor to run.
            edit->getTransport().stop (false, true);
            engine->getAudioFileManager().releaseAllFiles();
        }

        edit = std::move (newEdit);
        currentProjectFile = chosen;

        edit->getTransport().ensureContextAllocated();
        rebuildAutomationAfterLoad();

        if (onAfterEditSwap)
            onAfterEditSwap (*edit);
    });
}
