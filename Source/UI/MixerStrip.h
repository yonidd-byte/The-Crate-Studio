#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CrateMixerLookAndFeel.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    One vertical channel strip — Pro Tools/Logic-grade "Heavy Hitter" macro view
    (MASTER_ARCHITECTURE.md section 9, Information Density):

      +----------------+
      | ChannelStripRack |  <- collapsible; Routing (flat dropdowns) / Inserts /
      | (Routing/       |     Sends (vertical PluginSlotComponent stacks), folded
      |  Inserts/Sends) |     to zero height when the mixer's global rack toggle
      +----------------+     is off (setRackExpanded(false)) — the "Expand Rack"
      | Track Name      |     engine that solves Ableton's "which devices are on
      | Mute | Solo     |     this track" visibility problem.
      | Pan knob        |
      | Volume fader     | <- bound to te::VolumeAndPanPlugin (always visible —
      | + level meter    |    the "Micro View" fader/pan/mute/solo baseline).
      +----------------+     Fader/pan/meter are drawn by CrateMixerLookAndFeel,
                             scoped to just these two controls (not the app-wide
                             default LookAndFeel) via per-component setLookAndFeel().

    Volume/Pan/Mute/Solo/meter bind directly to the track's te::VolumeAndPanPlugin
    and te::LevelMeterPlugin (both already present on every track from
    addDefaultTrackPlugins(); this reuses them rather than inserting duplicates).

    Formerly named MixerComponent (pre-Hybrid-Mixer, single-strip-per-window era).
    Renamed now that MixerComponent is the multi-strip container that hosts N of
    these side by side.
*/
class MixerStrip : public juce::Component,
                    private juce::Timer,
                    private te::AutomatableParameter::Listener,
                    private juce::ValueTree::Listener
{
public:
    MixerStrip (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse);
    ~MixerStrip() override;

    /** Global expand/collapse (MixerComponent drives this identically for every
        strip — there's no per-strip state, matching the "toggles ... for all
        strips simultaneously" requirement). */
    void setRackExpanded (bool shouldBeExpanded);
    bool isRackExpanded() const noexcept   { return rackExpanded; }

    /** Total height this strip wants at the current expand state — MixerComponent
        uses this (identical across all strips of the same edit) to size the shared
        row height; it does not vary per-strip. */
    int getPreferredHeight() const;

    te::AudioTrack* getTrack() const   { return track.get(); }

    /** Fires when an insert slot in this strip's InsertsBlock is clicked — bubbles
        up to MixerComponent, then MainComponent, to bring that device into focus in
        UniversalDeviceChainComponent. Track is always this strip's own track. */
    std::function<void (te::AudioTrack*, te::Plugin*)> onPluginSlotSelected;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    // AutomatableParameter::Listener — keeps the fader/pan knob in sync when their
    // value changes from somewhere other than this strip (automation, another view,
    // a script, or — same track, same VolumeAndPanPlugin — TrackHeaderComponent's
    // own volume slider in the Arrangement view, since Arrange and Mixer are two
    // views onto the one te::Edit, not two independent states).
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override;

    // juce::ValueTree::Listener — Mute/Solo are plain CachedValue<bool> track
    // properties, not AutomatableParameters, so they need this instead to catch
    // changes made from TrackHeaderComponent's own Mute/Solo buttons.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

    // Same listener, different callbacks: te::PluginList::state IS track->state
    // (PluginList::initialise() literally does state = v, not a child tree), so
    // the one addListener() call already in place also delivers plugin add/remove
    // notifications — including ones caused by Undo/Redo, which never go through
    // any of our own load/delete call sites. Filtered to IDs::PLUGIN children so a
    // clip being added/removed (also a direct child of track->state) doesn't
    // trigger a pointless rack rebuild.
    void valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree) override;
    void valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int) override;

    void refreshFromEngine();
    void refreshMuteSoloFromEngine();
    void refreshRackFromPluginListChange();

    class ChannelStripRack; // Routing / Inserts / Sends, folds to nothing when collapsed

    CrateWorkflowManager& workflow; // threaded to ChannelStripRack -> each insert PluginSlotComponent,
                                    // for CrateWorkflowManager::loadPluginOntoTrack() on a Browser plugin drop
    te::AudioTrack::Ptr track;
    te::VolumeAndPanPlugin::Ptr volumePlugin;

    // LevelMeterPlugin doesn't declare its own Ptr alias (only the base Plugin::Ptr
    // exists), so hold it as a raw pointer — lifetime is owned by track->pluginList.
    te::LevelMeterPlugin* meterPlugin = nullptr;
    te::LevelMeasurer::Client meterClient;

    std::unique_ptr<ChannelStripRack> rack;
    bool rackExpanded = false;

    // Scoped narrowly to volumeFader + panKnob (see setLookAndFeel calls in the
    // .cpp) — declared BEFORE the two controls that use it so it's still alive
    // when they're destroyed (member destruction is reverse-declaration-order),
    // though the destructor also explicitly clears both anyway, belt-and-braces.
    CrateMixerLookAndFeel mixerLookAndFeel;

    juce::Slider volumeFader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::Label trackNameLabel;
    juce::Label volumeValueLabel;

    float meterLevelDb = -100.0f;

    // Peak Hold: snaps up instantly to a new higher level, then decays slowly —
    // entirely UI-side (TE's LevelMeasurer only reports near-instant block peaks,
    // not a held/decaying value), driven by this component's own 24Hz juce::Timer
    // (see timerCallback() in the .cpp), which is the ONLY thing that ever writes
    // to these two fields — no audio-thread access, no allocation.
    float peakHoldDb = -100.0f;
    juce::int64 peakHoldLastUpdateMs = 0;

    // Computed ONCE in resized(), read directly by paint() — BUG FIX: this used
    // to be recomputed independently in paint() from getLocalBounds() (spanning
    // nearly this whole strip's height), while volumeFader's bounds were computed
    // separately in resized() as "whatever's left after trackName/buttons/pan/
    // valueLabel", a much shorter span. The two rects didn't share a source, so
    // the meter ended up taller than — and misaligned with — the fader it's
    // supposed to sit tightly next to. Now both are sliced from the exact same
    // remaining-space rect, so they can never drift apart again.
    juce::Rectangle<float> meterBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrip)
};
