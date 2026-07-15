#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Zone 2 — The Crate Browser (MASTER_ARCHITECTURE.md 0.3 / section 1): the
    left-docked panel. A sleek rounded search field on top, flat PLUGINS / SAMPLES /
    FAVORITES tabs below it, and a custom-drawn (no default OS icons) list beneath
    that — seamless with the arrangement's own dark theme rather than reading as a
    floating window.

    PLUGINS is wired to the engine's real knownPluginList (same source
    PluginBrowserComponent's dialog uses). Double-click loads onto the currently
    selected track via CrateWorkflowManager; drag-and-drop (section 1's end-state)
    is now real too — each row is a genuine juce::Component (not a recycled
    ListBox row) so it can initiate an internal JUCE drag via
    DragAndDropContainer::startDragging(), landing on a MixerStrip insert slot or
    the Universal Device Chain (see PluginSlotComponent / UniversalDeviceChainComponent).
    SAMPLES and FAVORITES are honest empty states: sample indexing/tagging isn't
    built yet (MASTER_ARCHITECTURE.md Roadmap, Phase 3), so they say so rather
    than faking content.
*/
class BrowserComponent : public juce::Component,
                          private juce::ChangeListener
{
public:
    explicit BrowserComponent (CrateWorkflowManager& workflowToUse);
    ~BrowserComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // juce::ChangeListener — knownPluginList broadcasts a change whenever the
    // background scanner (MASTER_ARCHITECTURE.md: "the scanner has no UI... never
    // blocks the GUI thread") finds/updates a plugin, so the browser's list stays
    // live without polling.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void setActiveTab (int index);
    void refreshFilteredPlugins();

    // A pill-shaped search field: the juce::TextEditor itself is made fully
    // transparent/borderless and this wrapper paints the rounded background and a
    // small magnifying-glass glyph behind it, so there's no default rectangular
    // TextEditor outline visible anywhere.
    class SearchBar : public juce::Component
    {
    public:
        SearchBar();

        juce::TextEditor editor;

        void paint (juce::Graphics&) override;
        void resized() override;
    };

    // One catalog entry — a real Component (not a recycled ListBox row) so it can
    // be a genuine juce::DragAndDropContainer drag SOURCE: mouseDrag() calls
    // DragAndDropContainer::startDragging() directly once the drag exceeds JUCE's
    // standard minimum-distance threshold, carrying "plugin_drag|<identifier>" as
    // the sourceDescription and a semi-transparent snapshot of this row as the
    // drag image.
    class PluginRow : public juce::Component
    {
    public:
        explicit PluginRow (juce::PluginDescription descriptionToUse);

        std::function<void()> onDoubleClicked;

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::PluginDescription description;

    private:
        bool hovering = false;
        bool dragStarted = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginRow)
    };

    // Stacks PluginRows vertically inside pluginViewport — same "Viewport +
    // owned-child-Components" pattern ArrangementComponent's TrackListContent
    // uses, rather than a recycled-row ListBox, since rows now need real,
    // persistent Component identity to be drag sources.
    class PluginListContent : public juce::Component
    {
    public:
        // Declared explicitly: JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR below
        // declares a deleted copy constructor, and any user-declared constructor
        // (deleted or not) suppresses the implicit default constructor — without
        // this, make_unique<PluginListContent>() (no args) fails to compile.
        PluginListContent() = default;

        void rebuild (const juce::Array<juce::PluginDescription>& descriptions,
                      std::function<void (const juce::PluginDescription&)> onRowDoubleClicked);

        void setMinHeight (int h)   { minHeight = h; }

        /** Recomputes this component's size from the current row count/minHeight
            and lays out — called after rebuild() and whenever the owning
            viewport's available area changes (BrowserComponent::resized()). */
        void relayout();

    private:
        void paint (juce::Graphics&) override;
        void resized() override;

        std::vector<std::unique_ptr<PluginRow>> rows;
        int minHeight = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginListContent)
    };

    CrateWorkflowManager& workflow;

    SearchBar searchBar;

    juce::TextButton pluginsTab    { "PLUGINS" };
    juce::TextButton samplesTab    { "SAMPLES" };
    juce::TextButton favoritesTab  { "FAVORITES" };
    int activeTab = 0;

    juce::Viewport pluginViewport;
    std::unique_ptr<PluginListContent> pluginListContent;
    juce::Array<juce::PluginDescription> filteredPlugins;

    juce::Label emptyStateLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserComponent)
};
