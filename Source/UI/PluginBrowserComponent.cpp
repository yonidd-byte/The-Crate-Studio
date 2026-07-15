#include "PluginBrowserComponent.h"

namespace
{
    // Records "currently scanning X" before each plugin probe so a crash mid-scan
    // doesn't require rescanning everything — JUCE skips the culprit on next launch.
    juce::File getDeadMansPedalFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("The Crate Studio")
                    .getChildFile ("DeadMansPedal.txt");
    }
}

PluginBrowserComponent::PluginBrowserComponent (CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse),
      pluginListComponent (workflow.getEngine().getPluginManager().pluginFormatManager,
                            workflow.getEngine().getPluginManager().knownPluginList,
                            getDeadMansPedalFile(),
                            &workflow.getEngine().getPropertyStorage().getPropertiesFile(),
                            false)
{
    addAndMakeVisible (pluginListComponent);
    pluginListComponent.setScanDialogText ("Scanning for Plugins", "Searching VST3 folders...");

    // Double-click-to-load: PluginListComponent's row model is private, but
    // sortOrderChanged() physically re-sorts the shared knownPluginList to match
    // what's displayed, so knownPluginList.getTypes()[row] is always correct
    // regardless of current sort — verified against JUCE's own source. Kept as a
    // bonus shortcut, but the button below is the reliable path — ListBox row
    // components get recycled, which can confuse JUCE's per-component double-click
    // timing detection.
    pluginListComponent.getTableListBox().addMouseListener (this, true);

    addAndMakeVisible (loadButton);
    loadButton.onClick = [this] { loadSelectedRow(); };

    setSize (700, 540);
}

PluginBrowserComponent::~PluginBrowserComponent()
{
    pluginListComponent.getTableListBox().removeMouseListener (this);
}

void PluginBrowserComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& table = pluginListComponent.getTableListBox();
    const auto localPos = e.getEventRelativeTo (&table).getPosition();
    const auto row = table.getRowContainingPosition (localPos.x, localPos.y);

    if (row < 0)
        return;

    table.selectRow (row);
    loadSelectedRow();
}

void PluginBrowserComponent::loadSelectedRow()
{
    const auto row = pluginListComponent.getTableListBox().getSelectedRow();

    if (row < 0)
        return;

    auto& knownPluginList = workflow.getEngine().getPluginManager().knownPluginList;
    const auto types = knownPluginList.getTypes();

    if (row < types.size())
        workflow.loadPluginToSelectedTrack (types.getReference (row));
}

void PluginBrowserComponent::resized()
{
    auto area = getLocalBounds();
    loadButton.setBounds (area.removeFromBottom (36).reduced (8, 4));
    pluginListComponent.setBounds (area);
}
