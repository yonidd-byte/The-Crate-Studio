#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CrateMixerLookAndFeel.h"
#include "HardwareSlotLookAndFeel.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

class CrateEQThumbnail;
class CrateSendSlot;

/**
    Logic Pro X-style Track Inspector — the left-hand panel of the Pro Mix
    Environment (MASTER_ARCHITECTURE.md Zone 5). Its defining Logic paradigm is
    the DUAL channel strip: the currently-selected track's strip on the LEFT and
    its target output/bus (e.g. Master) on the RIGHT, side by side, so a mix
    engineer sees a channel and where it lands in one glance.

      +-----------------+-----------------+
      |  SELECTED TRACK |   OUTPUT / BUS  |
      |  EQ thumbnail   |   EQ thumbnail  |
      |  [Channel Comp] |   (Master: none)|
      |  SENDS [+]      |   SENDS [+]     |
      |  [ scrolling  ] |   [ scrolling ] |   <- Task 3: sends scroll inside a
      |  [ send slots ] |   [ send slots ]|      fixed-height juce::Viewport —
      |  I/O: Master    |   I/O: -        |   <- Task 2: compact routing readout
      |  Pan knob       |   Pan knob      |      directly above Fader/Pan, not a
      |  Fader + meter  |   Fader + meter |      giant header up top
      +-----------------+-----------------+

    Both strips are the exact same InspectorStrip class/height (Task 2 Height
    Alignment) — a Master-bound strip reserves the IDENTICAL vertical space for
    its Channel Comp row as a real track's strip, just with the button hidden
    rather than the row removed, so EQ thumbnails/Sends/Pan/Fader all land on
    the same Y-axis in both columns regardless of which side is Master.

    Layout/visual scaffold pass only — controls are real juce components (so the
    look/feel is genuine) but not yet bound to engine parameters; setTrack()
    updates the strip captions and the target-bus caption. A later DSP pass wires
    the knobs/faders to the track's VolumeAndPanPlugin / send / dynamics chain.
*/
class CrateTrackInspectorComponent : public juce::Component
{
public:
    // The Inspector "Mirror" directive: takes CrateWorkflowManager& so its
    // Sends "+" menu can query workflow.getEdit() directly — the SAME
    // always-current global te::Edit the Mixer's own MixerStrip effectively
    // reaches (both ultimately resolve to the one Edit CrateWorkflowManager
    // owns) — rather than only ever going through a cached te::Track*.
    explicit CrateTrackInspectorComponent (CrateWorkflowManager& workflowToUse);
    ~CrateTrackInspectorComponent() override;

    /** Point the inspector at the selected track (or nullptr to clear) — accepts
        the te::Track BASE type since the right strip's own target can be the
        te::MasterTrack (which does not derive from AudioTrack). Updates the LEFT
        strip's caption to trackToShow's name; the RIGHT strip resolves
        trackToShow's actual routing destination: for a real te::AudioTrack, that's
        TrackOutput::getDestinationTrack() if it routes to another track, otherwise
        edit->getMasterTrack() (a null destination genuinely means "goes to the
        default output," i.e. Master, in TE's own routing model). */
    void setTrack (te::Track* trackToShow);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // One vertical channel strip in the dual layout — built twice (selected
    // track + its output bus). Nested since it's meaningless outside this
    // inspector and shares its CrateMixerLookAndFeel.
    class InspectorStrip;

    // Task 3 (Infinite Sends): the scrollable content INSIDE InspectorStrip's
    // fixed-height Sends Viewport. Nested here (not inside InspectorStrip
    // itself) purely so InspectorStrip's own class body stays readable —
    // still only ever used by InspectorStrip.
    class SendsContent;

    te::Track* track = nullptr;
    CrateWorkflowManager& workflow; // for InspectorStrip's Sends "+" menu — see the constructor's doc comment

    CrateMixerLookAndFeel mixerLookAndFeel; // declared before the strips that use it

    // Tactile "hardware slot" chrome for the Routing combos (Input/Output),
    // Channel Comp button, and each Send's destination chip + mini knob — the
    // SAME LookAndFeel MixerStrip's Routing row uses, so the collapsed
    // Inspector and the full Mixer channel strip share one visual language.
    HardwareSlotLookAndFeel hardwareSlotLookAndFeel;

    std::unique_ptr<InspectorStrip> selectedStrip; // LEFT — the selected track
    std::unique_ptr<InspectorStrip> outputStrip;    // RIGHT — its target output/bus

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateTrackInspectorComponent)
};
