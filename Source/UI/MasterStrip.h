#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CrateMixerLookAndFeel.h"
#include "HardwareSlotLookAndFeel.h"
#include "InsertsRackComponent.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

class CrateEQThumbnail;

/**
    The Master channel strip — pinned to the far right of the Mixer's strip row
    (MixerComponent::StripRowContent), always rendered regardless of how many
    real tracks exist. te::MasterTrack (edit.getMasterTrack()) does NOT derive
    from te::AudioTrack — it's a lightweight wrapper with none of AudioTrack's
    volume-plugin/mute/solo API — so this is a dedicated, minimal component
    rather than forcing MasterTrack through MixerStrip's AudioTrack-typed
    machinery. Binds to the REAL master bus controls TE actually exposes:
    edit.getMasterVolumePlugin() for fader/pan (the same object the transport's
    own master fader would read), and a LevelMeterPlugin on
    edit.getMasterPluginList() for metering (created on first use if the Edit
    doesn't already have one, same pattern MixerStrip uses for a track's meter).

    Renders its Inserts rack via the SAME InsertsRackComponent a real
    MixerStrip uses (see that class's doc comment) — a plugin chain is a
    plugin chain, conceptually no different for Master, so there is exactly
    ONE Inserts UI implementation in this app, not a second Master-specific
    copy. This is the ONLY place in the Mixer that shows Master's plugin
    slots — the separate Track Inspector (CrateTrackInspectorComponent) does
    NOT have an Inserts rack of its own (Lead Architect directive: that
    left-panel Inspector shows EQ/Dynamics/Sends/Routing/Pan/Fader only, never
    plugin slots — this Mixer strip is unaffected by that directive). Loading
    a new plugin here goes through CrateWorkflowManager::loadPluginOntoTrack(),
    the same instantiate+insert pipeline the Device Chain and drag-and-drop
    use — including its one Master-specific rule (instruments rejected on
    Master), which lives entirely there, not in this UI.
*/
class MasterStrip : public juce::Component,
                     private juce::Timer,
                     private te::AutomatableParameter::Listener,
                     private juce::ComponentListener // watches the Channel Comp CallOutBox lifecycle
{
public:
    MasterStrip (te::Edit& editToUse, CrateWorkflowManager& workflowToUse);
    ~MasterStrip() override;

    /** Fires on click anywhere on the strip body (not its controls) — MixerComponent
        bubbles this up so MainComponent can push edit->getMasterTrack() into the
        Universal Device Chain, the same "select this track -> show its chain"
        gesture clicking a real MixerStrip already gives. */
    std::function<void()> onSelected;

    /** Fires when an insert slot in the Master rack is clicked — MainComponent
        focuses that exact plugin in the Universal Device Chain, same as
        MixerStrip::onPluginSlotSelected for a real track's inserts. */
    std::function<void (te::Plugin*)> onInsertSlotSelected;

    /** Visual selected state — set true when Master is the currently-focused
        track (mirrors TrackHeaderComponent's setSelected()). Task 5 fix: the
        left accent line used to be drawn UNCONDITIONALLY every paint (the
        "blue line anomaly") — now it only shows while actually selected. */
    void setSelected (bool shouldBeSelected);

    /** Master's Inserts rack participates in the Mixer's global "Expand Rack"
        toggle — shows more rows at once when expanded, same as every real
        MixerStrip. */
    void setRackExpanded (bool shouldBeExpanded);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override   { setSelected (true); if (onSelected) onSelected(); }

private:
    void timerCallback() override;
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override;

    // Seeds insertsRack once, in the constructor — InsertsRackComponent
    // attaches its OWN ValueTree::Listener (to the correct master-plugin-list
    // tree, not MasterTrack's own track->state) and rebuilds itself
    // automatically on every future plugin add/remove, so this class doesn't
    // need its own separate listener for that any more.
    void rebuildInserts();

    void refreshMuteState(); // reflects the master-volume mute threshold onto muteButton

    // Channel Comp popup lifecycle — same fix as MixerStrip: manual toggle
    // driven strictly by the CallOutBox's real lifetime, so the button never
    // stays stuck lit after the popup closes.
    void openChannelCompPopup();
    void componentBeingDeleted (juce::Component&) override;

    te::Edit& edit;
    CrateWorkflowManager& workflow; // for loadPluginOntoTrack() on a Browser plugin drop onto an insert slot

    bool selected = false;
    bool rackExpanded = false;
    te::VolumeAndPanPlugin::Ptr volumePlugin; // edit.getMasterVolumePlugin() — the real master fader/pan
    te::LevelMeterPlugin* meterPlugin = nullptr; // raw: lifetime owned by edit.getMasterPluginList()
    te::LevelMeasurer::Client meterClient;

    CrateMixerLookAndFeel mixerLookAndFeel;

    // Same tactile "hardware slot" console-I/O look as MixerStrip's Routing
    // row (OUT 1), scoped to outputSlot here — Master mirrors the real track
    // strip's chrome level-for-level, per the anatomy below.
    HardwareSlotLookAndFeel hardwareSlotLookAndFeel;

    // Mirrors MixerStrip's strict bottom-up Logic anatomy, MINUS the components
    // Master lacks: no Solo, no Record/Input (L3), no Sends (L10). Master's
    // output is a fixed "Stereo Out", so L9 routing is a single read-only slot.
    juce::Label      nameLabel;                          // L1
    juce::TextButton muteButton { "M" };                 // L2 (Mute only)
    juce::Slider     volumeFader { juce::Slider::LinearVertical, juce::Slider::NoTextBox }; // L4
    juce::Label      faderPositionLabel;                 // L5 (left)
    juce::Label      peakLevelLabel;                     // L5 (right)
    juce::Slider     panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox }; // L6
    // L7 track-icon — painted placeholder (trackIconBounds)
    juce::Label      outputSlot;                          // L9 (Stereo Out, read-only)
    InsertsRackComponent insertsRack;                    // L11 (no L10 sends on Master)
    juce::TextButton channelCompButton { "Comp" };       // L12
    std::unique_ptr<CrateEQThumbnail> eqThumbnail;       // L13
    juce::TextButton settingsButton { "Setting" };       // L14

    float meterLevelDb = -60.0f; // -INF floor — renders empty until timerCallback() gets real DSP levels
    float peakHoldDb = -60.0f;   // matches meterLevelDb
    juce::int64 peakHoldLastUpdateMs = 0;

    juce::Rectangle<float> meterBounds;   // computed in resized(), read by paint()
    juce::Rectangle<int>   trackIconBounds; // L7 placeholder

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
};
