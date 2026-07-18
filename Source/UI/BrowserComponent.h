#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"
#include "CrateTrackInspectorComponent.h"

namespace te = tracktion::engine;

class CrateMidiInspectorComponent;

/**
    Zone 2 — The Crate Browser / Left Panel (MASTER_ARCHITECTURE.md 0.3 / section
    1): the left-docked panel. A sleek rounded search field on top, flat PLUGINS /
    SAMPLES / FAVORITES / INSPECTOR tabs below it, and — beneath that — EITHER the
    custom-drawn plugin/sample/favorites list OR the Context-Aware Inspector,
    whichever tab is active. Seamless with the arrangement's own dark theme rather
    than reading as a floating window.

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

    INSPECTOR is Context-Aware: it shows CrateTrackInspectorComponent (owned
    directly by this class — self-contained, no Edit-lifecycle wiring) while the
    Arrangement/Mixer is the active workspace, or CrateMidiInspectorComponent
    (owned by MainComponent, whose clip-lifecycle wiring stays untouched there —
    this class only holds a non-owning pointer to it for display/visibility)
    while the Piano Roll is open — see setPianoRollActive().

    Tab selection is a genuine user choice (any tab is always clickable), but
    MainComponent applies "Smart Defaults" on each workspace transition — see
    its showArrangementView()/showMixerView()/enterMidiEditor() — by calling
    setActiveTab() itself; this class never forces a tab on its own.
*/
class BrowserComponent : public juce::Component,
                          private juce::ChangeListener
{
public:
    explicit BrowserComponent (CrateWorkflowManager& workflowToUse);
    ~BrowserComponent() override;

    /** Public: MainComponent calls this to apply a Smart Default on a workspace
        transition (0=PLUGINS, 1=SAMPLES, 2=FAVORITES, 3=INSPECTOR). Also called
        internally when a tab button is clicked — same single code path either
        way, so the two can never disagree about what "tab 3 is active" means. */
    void setActiveTab (int index);

    /** Non-owning: CrateMidiInspectorComponent's lifecycle (construction, destruction
        on project Load, setActiveClip() wiring) stays entirely in MainComponent — this
        class only needs a pointer to show/hide/position it under the INSPECTOR tab.
        Pass nullptr before destroying the real object (e.g. mid-Load teardown) so
        this class never holds a dangling pointer. */
    void setMidiInspector (CrateMidiInspectorComponent* insp);

    /** Context switch for the INSPECTOR tab's content: true while the Piano Roll
        overlay is open (show CrateMidiInspectorComponent), false otherwise (show
        CrateTrackInspectorComponent, the dual-strip track/output view). */
    void setPianoRollActive (bool isActive);

    /** Forwards to the owned CrateTrackInspectorComponent — wired to track
        selection (see MainComponent's arrangement->onTrackSelected) so clicking a
        track/clip in the Timeline keeps the Inspector's dual strips current even
        while a different tab happens to be showing. */
    void setSelectedTrack (te::Track* t);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // juce::ChangeListener — knownPluginList broadcasts a change whenever the
    // background scanner (MASTER_ARCHITECTURE.md: "the scanner has no UI... never
    // blocks the GUI thread") finds/updates a plugin, so the browser's list stays
    // live without polling.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // Single place that decides which of {pluginViewport/emptyStateLabel,
    // trackInspector, midiInspectorPtr} is actually visible right now — driven by
    // BOTH activeTab and pianoRollActive, so either changing alone re-evaluates
    // the same correct combination instead of two half-updated call sites drifting
    // apart.
    void updateContentVisibility();

    void refreshFilteredPlugins();

    // Task 2 — icon-based tabs: a plug (Plugins), a waveform (Samples), a star
    // (Favorites), and two mixer-slider tracks (Inspector), all drawn as
    // juce::Path rather than text labels. A plain juce::Button subclass (not
    // TextButton) since there's no text at all — paintButton() draws the icon
    // directly, accent-coloured when toggled on (the active tab) and dimmed
    // otherwise, mirroring the old text tabs' on/off colour language.
    class IconTabButton : public juce::Button
    {
    public:
        enum class Icon { Plug, Waveform, Star, MixerSliders };

        explicit IconTabButton (Icon iconToUse) : juce::Button ({}), icon (iconToUse)
        {
            setClickingTogglesState (false); // driven manually via setActiveTab(), same as the old TextButtons
        }

        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

    private:
        Icon icon;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconTabButton)
    };

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

    IconTabButton pluginsTab    { IconTabButton::Icon::Plug };
    IconTabButton samplesTab    { IconTabButton::Icon::Waveform };
    IconTabButton favoritesTab  { IconTabButton::Icon::Star };
    IconTabButton inspectorTab  { IconTabButton::Icon::MixerSliders };
    int activeTab = 0;

    juce::Viewport pluginViewport;
    std::unique_ptr<PluginListContent> pluginListContent;
    juce::Array<juce::PluginDescription> filteredPlugins;

    juce::Label emptyStateLabel;

    // INSPECTOR tab content — see this class's own doc comment above for the
    // ownership split between these two.
    std::unique_ptr<CrateTrackInspectorComponent> trackInspector;
    CrateMidiInspectorComponent* midiInspectorPtr = nullptr; // non-owning, see setMidiInspector()
    bool pianoRollActive = false;

    // The exact content-area rect pluginViewport/emptyStateLabel/trackInspector/
    // midiInspectorPtr all share — computed once in resized(), so every one of
    // them occupies the identical span no matter which is actually visible.
    juce::Rectangle<int> contentArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserComponent)
};
