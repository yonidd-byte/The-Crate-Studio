#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Hybrid Mixer — horizontal row of MixerStrip, one per te::AudioTrack in the
    active te::Edit. The Center Zone's "Mixer" half of the Arrangement/Mixer
    crossfade view (MASTER_ARCHITECTURE.md Law I: this and ArrangementComponent
    both stay resident and swap via setVisible(), never a reload).

      +--------------------------------------------------+
      | [Expand/Collapse Rack]                            |  <- global toggle, 28px
      +--------------------------------------------------+
      | [Kick ] [Snare] [Bass ] [Synth] [Vox  ]  ...      |  <- MixerStrip, fixed
      |                                                    |     width; height
      +--------------------------------------------------+     grows uniformly for
                                                                 all strips when the
                                                                 rack is expanded
                                                                 (scrollable row)
*/
class MixerComponent : public juce::Component
{
public:
    MixerComponent (te::Edit& editToShow, CrateWorkflowManager& workflowToUse);
    ~MixerComponent() override;

    /** Re-reads the Edit's current track list and rebuilds the strip row. Call
        after any track add/delete — same refresh contract as
        ArrangementComponent::rebuildTracks(), which the two views must be kept in
        sync with since they're two faces of the same Edit. */
    void rebuildStrips();

    /** Fires when an insert slot in any strip's ChannelStripRack is clicked —
        bridges MixerStrip::onPluginSlotSelected up to whoever owns
        UniversalDeviceChainComponent (MainComponent), so the deep-tweak view can
        focus that device. */
    std::function<void (te::AudioTrack*, te::Plugin*)> onPluginSlotSelected;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class StripRowContent; // horizontal row of MixerStrip

    void layoutContent();

    te::Edit& edit;
    CrateWorkflowManager& workflow; // threaded down to MixerStrip -> ChannelStripRack, so a
                                    // dropped plugin can be instantiated via
                                    // CrateWorkflowManager::loadPluginOntoTrack()

    juce::TextButton expandRackButton { "Expand Rack" };
    bool rackExpanded = false;

    juce::Viewport viewport;
    std::unique_ptr<StripRowContent> content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};
