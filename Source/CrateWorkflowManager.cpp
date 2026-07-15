#include "CrateWorkflowManager.h"
#include "UI/PluginWindow.h"

namespace
{
    // Same custom ValueTree property AutomationLaneComponent writes to/reads from.
    // Duplicated here (rather than including a UI header from this engine-level
    // class) since prepareAutomationForSave()'s validation pass only needs to check
    // for the property's presence, not decode it.
    const juce::Identifier anchorMetadataProperty ("crateAnchors");

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
    };
}

CrateWorkflowManager::CrateWorkflowManager()
{
    // CrateUIBehaviour supplies plugin editor window creation — TE's default
    // UIBehaviour::createPluginWindow() returns nullptr and deliberately leaves this
    // to the host app. CrateEngineBehaviour raises the default 16-plugin-per-track
    // ceiling — see its doc comment above for why that default was being hit.
    engine = std::make_unique<te::Engine> ("The Crate Studio", std::make_unique<CrateUIBehaviour>(), std::make_unique<CrateEngineBehaviour>());

    initialiseAudioDevice();

    edit = te::createEmptyEdit (*engine, juce::File());
    edit->getTransport().ensureContextAllocated();
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

    edit->getUndoManager().beginNewTransaction ("Load Plugin: " + description.name);

    auto plugin = instantiateExternalPlugin (description);

    if (plugin == nullptr)
        return;

    audioTrack->pluginList.insertPlugin (plugin, -1, nullptr);

    if (auto* processor = plugin->getWrappedAudioProcessor())
        if (processor->hasEditor())
            plugin->showWindowExplicitly();
}

void CrateWorkflowManager::loadPluginOntoTrack (const juce::PluginDescription& description, te::AudioTrack& targetTrack, int insertIndex)
{
    // See loadPluginToSelectedTrack()'s identical tripwire above.
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    if (edit == nullptr)
        return;

    edit->getUndoManager().beginNewTransaction ("Drop Plugin: " + description.name);

    auto plugin = instantiateExternalPlugin (description);

    if (plugin == nullptr)
        return;

    targetTrack.pluginList.insertPlugin (plugin, insertIndex, nullptr);
}

te::Plugin::Ptr CrateWorkflowManager::instantiateExternalPlugin (const juce::PluginDescription& description)
{
    if (edit == nullptr)
        return {};

    // The TE-sanctioned path for instantiating an external (VST3/AU/etc.) plugin:
    // PluginManager::createNewPlugin's ExternalPlugin::xmlTypeName branch internally
    // calls ExternalPlugin::create(engine, desc) to build the initial ValueTree
    // state, then constructs the ExternalPlugin from it. This is the same call TE's
    // own test suite uses to load VST3s (tracktion_Plugins.test.cpp) — not a
    // workaround, and not something PluginManager::createPlugin(Edit&, ValueTree&,
    // bool) (a different, lower-level helper with no PluginDescription overload)
    // would improve on.
    auto plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, description);

    if (plugin == nullptr)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Plugin Load Failed",
            "\"" + description.name + "\" could not be instantiated. It may have been "
            "moved, deleted, or be incompatible with this build (bitness/format).");

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
