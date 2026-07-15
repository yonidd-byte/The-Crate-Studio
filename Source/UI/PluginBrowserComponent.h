#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Phase 4: VST3 scanning + browsing. Thin wrapper around JUCE's own
    juce::PluginListComponent, bound directly to the engine's PluginManager —
    scanning, sortable columns (name/format/category/manufacturer), and crash-safe
    "dead man's pedal" handling all come from JUCE, not reimplemented here.

    Persistence is already handled by TE: PluginManager listens for changes on
    knownPluginList and writes it to PropertyStorage automatically, so there's no
    separate save/load step in this class.

    Routes instantiation through CrateWorkflowManager::loadPluginToSelectedTrack()
    rather than holding its own track pointer — the load target is always whatever
    is currently selected at click time, and the transaction/undo wrapping lives in
    one place instead of being duplicated here.
*/
class PluginBrowserComponent : public juce::Component
{
public:
    explicit PluginBrowserComponent (CrateWorkflowManager& workflowToUse);
    ~PluginBrowserComponent() override;

    void resized() override;

private:
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void loadSelectedRow();

    CrateWorkflowManager& workflow;
    juce::PluginListComponent pluginListComponent;
    juce::TextButton loadButton { "Load Selected Plugin" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowserComponent)
};
