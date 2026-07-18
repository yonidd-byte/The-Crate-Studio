#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "TheCrateLookAndFeel.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Flat, Ableton-style left-column header for one track (Zone 3 / MASTER_ARCHITECTURE.md
    0.3): editable name, an I/O routing text placeholder, flat colour-coded Record
    Arm / Solo / Mute toggles plus the 'A' automation-lane toggle, a minimal
    horizontal volume fader with a live horizontal level meter beneath it, and
    click-to-select behaviour with a highlight when selected.

    Record Arm actually calls te::InputDeviceInstance::setRecordingEnabled() for
    every input device currently routed to this track (a no-op if none are, which
    is the common case until Live/Input-routing UI lands) and persists its state as
    a plain ValueTree property on the track's own state (round-trips through the
    same .crate save/load path as everything else — MASTER_ARCHITECTURE.md
    invariant 3), so it isn't just a cosmetic toggle.

    Bidirectional sync with MixerStrip (the same track's Pro Tools-style channel
    strip, alive simultaneously per Law I's crossfade — never destroyed just
    because it's hidden): both implement te::AutomatableParameter::Listener on
    volParam/panParam, and both listen for juce::ValueTree property changes on the
    track's own state for Mute/Solo (mute/soloed are plain CachedValue<bool>
    properties, not AutomatableParameters — there is no AutomatableParameter::
    Listener equivalent for them). A tweak in either view now reaches the other.
    The live meter reuses the track's single shared te::LevelMeterPlugin (found via
    findFirstPluginOfType, same pattern MixerStrip uses) rather than inserting a
    second one — both views' meters read the one real signal.

    Also a juce::DragAndDropTarget (public inheritance — REQUIRED: JUCE's
    DragAndDropContainer discovers drop targets via an EXTERNAL
    dynamic_cast<DragAndDropTarget*>(component) while walking the parent chain
    during a drag, same mechanism/gotcha already hit with
    juce::FileDragAndDropTarget on MainComponent; private inheritance would make
    that cast well-formed but return nullptr, silently breaking drops). Accepts
    a Browser plugin drag ("plugin_drag|" prefix, see
    BrowserComponent::PluginRow) dropped anywhere on the header and appends it
    to this track's chain via CrateWorkflowManager::loadPluginOntoTrack().
*/
class TrackHeaderComponent : public juce::Component,
                              public juce::DragAndDropTarget,
                              private juce::Timer,
                              private te::AutomatableParameter::Listener,
                              private juce::ValueTree::Listener
{
public:
    TrackHeaderComponent (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse);
    ~TrackHeaderComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    void setSelected (bool shouldBeSelected);

    // Called when the header is clicked (track should become the selected track).
    std::function<void()> onSelect;
    // Called when the 'A' automation toggle changes; query getAutomationVisible().
    std::function<void()> onAutomationToggle;
    // Called when the delete ('x') button is clicked. Fires onSelect first (so the
    // receiver's "delete the selected track" logic operates on the right track
    // without needing this row to pass itself as an argument).
    std::function<void()> onDeleteRequested;

    bool getAutomationVisible() const   { return automationButton.getToggleState(); }

private:
    // Shared by the header's default member initializers below AND by the .cpp's
    // click/refresh logic — one definition each, so the "on" colour a button is
    // constructed with can never drift from the colour refreshToggleStatesFromEngine()
    // (etc.) reasons about.
    // V2.0 UI/UX Master Manifesto, section 3 ("Ghosted Buttons"): Mute=red,
    // Solo=yellow — matte, non-blinding, exact hex values from
    // TheCrateLookAndFeel::colorMuteRed/colorSoloYellow (supersedes this
    // class's earlier cyan-Solo/orange-Mute scheme). Record Arm isn't
    // specified by the Manifesto, so it keeps its own distinct bright red.
    inline static const juce::Colour armOnColour  { 0xffff1e1e };            // bright red — Record Arm ONLY
    inline static const juce::Colour soloOnColour { TheCrateLookAndFeel::colorSoloYellow };
    inline static const juce::Colour muteOnColour { TheCrateLookAndFeel::colorMuteRed };

    // Flat, square, colour-coded toggle block — Ableton style: no default JUCE
    // TextButton bevel/gradient, just a solid-filled square with a single glyph
    // letter, coloured by role (Record Arm red / Solo cyan / Mute amber /
    // Automation accent) rather than by generic on/off text-button colours.
    class ToggleBlock : public juce::Button
    {
    public:
        ToggleBlock (juce::String glyphToShow, juce::Colour onColourToUse)
            : juce::Button ({}), glyph (std::move (glyphToShow)), onColour (onColourToUse) {}

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        juce::String glyph;
        juce::Colour onColour;
    };

    // te::AutomatableParameter::Listener — fires when Volume/Pan change from
    // anywhere other than this header's own slider (MixerStrip's fader/pan knob,
    // automation playback, a script).
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override;

    // juce::ValueTree::Listener — fires for ANY property change on the track's
    // state; filtered down to just mute/solo/armed in the .cpp, since those are the
    // only things this header doesn't already own end-to-end.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

    void timerCallback() override; // polls the live level meter only — see .cpp

    void refreshVolumeFromEngine();
    void refreshToggleStatesFromEngine();

    // Right-click rename + track-colour editor — the SAME shared helper the
    // Mixer name-plate uses (CrateTrackEditor::showNameColourMenu), so recolour
    // from the timeline and from the Mixer are literally one code path.
    void showNameColourEditor();

    te::AudioTrack::Ptr track;
    CrateWorkflowManager& workflow;
    te::VolumeAndPanPlugin::Ptr volumePlugin;

    // LevelMeterPlugin doesn't declare its own Ptr alias (only the base Plugin::Ptr
    // exists), so hold it as a raw pointer — lifetime is owned by track->pluginList,
    // same reasoning as MixerStrip's identical member.
    te::LevelMeterPlugin* meterPlugin = nullptr;
    te::LevelMeasurer::Client meterClient;
    float meterLevelDb = -100.0f;
    juce::Rectangle<int> meterBounds;

    bool selected = false;
    bool isDragHovering = false;

    juce::Label nameLabel;
    juce::Label ioLabel;
    juce::Label trackNumberLabel; // persistent, non-editable "1" / "2" / ... badge
    ToggleBlock recordArmButton { "O", armOnColour };
    ToggleBlock soloButton      { "S", soloOnColour };
    ToggleBlock muteButton      { "M", muteOnColour }; // true Mute polarity: ON (lit) = muted
    ToggleBlock automationButton { "A", TheCrateLookAndFeel::accent };
    juce::TextButton deleteButton { juce::CharPointer_UTF8 ("\xc3\x97") }; // "×"
    juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderComponent)
};
