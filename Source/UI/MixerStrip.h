#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CrateMixerLookAndFeel.h"
#include "HardwareSlotLookAndFeel.h"
#include "GhostButtonLookAndFeel.h"
#include "CrateDesignSystem.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

class CrateEQThumbnail;
class CrateSendSlot;
class InsertsRackComponent;

/**
    One vertical channel strip — Logic Pro-anatomy "Heavy Hitter" (V2.0 UI/UX
    Master Manifesto + the official Logic channel-strip component reference).

    STRICT BOTTOM-UP HIERARCHY: visual weight and physical control flow build
    from the BOTTOM of the strip upward, exactly like a real hardware console
    (name plate at the base, fader/meters in the tactile middle, signal-chain
    depth rising above). resized() computes bounds bottom-first, and every
    element below is numbered by its Logic level:

      +-----------------+  <- L14 Settings button      (absolute top)
      | L13 EQ display  |
      | L12 Channel Comp|     Comp/Inserts/Sends/Routing all share ONE
      | L11 Inserts     |     "Universal Rack Width" bounding box — their
      | L10 Sends       |     edges align top-to-bottom, no exceptions.
      | L9  Routing     |  <- dark well DYNAMICALLY wraps OUT1+2 alone, or
      +-----------------+     OUT1+2+Group when a group is assigned. No Read
                              (Eradicated — see the Cleanup directive).
      | L6  Pan knob    |  <- EXTRACTED below the well, on the plain track bg
      | pan value       |  <- "C" / "20 L" / "20 R" readout
      | L5  dB readouts |  <- fader position | peak level, side by side
      | L4  FADER +     |     the tall unified fader / audio-meter (up) /
      |     METERS      |     GR-meter (down) block — fills all remaining height
      | R | S | I       |  <- equally-sized triad, Ghost Buttons
      | Track Name      |  <- IS the Mute toggle (Ableton Mute paradigm):
      |                 |     track colour when unmuted, dark grey when muted
      | Track Icon      |  <- embedded on the void bg, no plate/border
      +-----------------+     absolute bottom

    Volume/Pan/Solo/meter bind directly to the track's te::VolumeAndPanPlugin
    and te::LevelMeterPlugin (both already present on every track from
    addDefaultTrackPlugins(); this reuses them rather than inserting duplicates).
    Mute has no dedicated control — the name plate's click handler (see
    mouseDown()) IS the Mute toggle. The Sexy Fader / pan knob are drawn by
    CrateMixerLookAndFeel, scoped to just those controls via per-component
    setLookAndFeel(). MasterStrip mirrors this same bottom-up stack, minus the
    components Master lacks (no R/I, no Sends).
*/
class MixerStrip : public juce::Component,
                    private juce::Timer,
                    private te::AutomatableParameter::Listener,
                    private juce::ValueTree::Listener,
                    private juce::ComponentListener // watches the Channel Comp CallOutBox lifecycle
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
    void mouseDown (const juce::MouseEvent&) override;         // right-click name plate -> editor
    void mouseDoubleClick (const juce::MouseEvent&) override;  // double-click name plate -> editor

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

    // Deep signal-chain sections (L9 Routing / L10 Sends / L11 Inserts). Held
    // by unique_ptr behind forward declarations so the heavy JUCE/section
    // definitions (and InsertsRackComponent's complete type) stay out of this
    // header. The ChannelStripRack wrapper that used to nest these was
    // dissolved — every element is now a direct child of MixerStrip laid out
    // by ONE bottom-up resized(), per the Logic-anatomy directive.
    struct RoutingBlock;   // L9  — Output slot (top) + Group slot (bottom), two flat rects
    struct SendsSection;   // L10 — vertical CrateSendSlot stack (destination chip + mini level knob)

    void populateOutputCombo();
    void applyOutputComboSelection();
    void rebuildSends();

    // Dynamic Sends Routing — the "+" button's bus-picker menu and the actual
    // te::AuxSendPlugin creation, ported from CrateTrackInspectorComponent's
    // own addNewSend()/createSendToBus().
    void addNewSend();
    void createSendToBus (int busNumber);

    // Channel Comp popup lifecycle — opens on click, and the ComponentListener
    // un-toggles channelCompButton the instant the CallOutBox is dismissed
    // (fixing the "stuck cyan" bug where the button stayed lit after the popup
    // closed). componentBeingDeleted fires for the CallOutBox we registered on.
    void openChannelCompPopup();
    void componentBeingDeleted (juce::Component&) override;

    // L1 name plate — right-click / double-click opens a rename + track-colour
    // editor; changes go through track->setName()/setColour() (undo-wrapped) and
    // round-trip via the track's ValueTree, so the Arrangement TrackHeader
    // updates instantly and vice versa.
    void showNameColourEditor();
    void applyTrackColourToPlate();

    CrateWorkflowManager& workflow; // for CrateWorkflowManager::loadPluginOntoTrack() on a Browser plugin drop
    te::AudioTrack::Ptr track;
    te::VolumeAndPanPlugin::Ptr volumePlugin;

    // LevelMeterPlugin doesn't declare its own Ptr alias (only the base Plugin::Ptr
    // exists), so hold it as a raw pointer — lifetime is owned by track->pluginList.
    te::LevelMeterPlugin* meterPlugin = nullptr;
    te::LevelMeasurer::Client meterClient;

    bool rackExpanded = false; // gates the deep L9–L14 stack's visibility/height

    // R/S/I Return-Track Logic: computed ONCE in the constructor (a track's
    // return-ness never changes across this strip's lifetime — the strip is
    // destroyed/recreated on rebuild instead). Return tracks have no external
    // input, so inputMonitorButton is hidden entirely and recordButton becomes
    // a cosmetic Pre/Post toggle, mirroring TrackHeaderComponent's identical
    // Return Track Button directive in the Arrangement.
    bool isReturnTrackFlag = false;

    // Scoped narrowly to the fader/pan/combos/send-sliders (see setLookAndFeel
    // calls in the .cpp) — declared BEFORE the controls that use it so it's
    // still alive when they're destroyed (member destruction is
    // reverse-declaration-order); the destructor also clears them explicitly.
    CrateMixerLookAndFeel mixerLookAndFeel;

    // Scoped to JUST the Routing controls (OUT 1 combo) — the
    // premium "hardware slot" console-I/O look, deliberately distinct from
    // mixerLookAndFeel's fader/pan chrome and from TheCrateLookAndFeel's
    // app-wide "Ghosted Buttons" (Mute/Solo/Bypass keep that look untouched).
    // Same declared-before-use lifetime discipline as mixerLookAndFeel above.
    HardwareSlotLookAndFeel hardwareSlotLookAndFeel;

    // Scoped to JUST the R/S/I triad — see GhostButtonLookAndFeel's own doc
    // comment for why this is distinct from TheCrateLookAndFeel's app-wide
    // ghosted-button language. Same declared-before-use lifetime discipline.
    GhostButtonLookAndFeel ghostButtonLookAndFeel;

    // ---- Core controls (always visible), bottom → up ------------------------
    // trackNameLabel IS the Mute toggle now (Ableton Mute paradigm — see
    // mouseDown()/applyTrackColourToPlate()); no dedicated M button.
    juce::Label      trackNameLabel;                    // L1 (top row of the Scribble Strip)
    juce::TextButton soloButton   { "S" };
    juce::TextButton recordButton { "R" };
    juce::TextButton inputMonitorButton { "I" };        // R/S/I triad, directly above the name plate
    juce::Slider     volumeFader  { juce::Slider::LinearVertical, juce::Slider::NoTextBox }; // L4
    juce::Label      faderPositionLabel;                // L5 (left)  — current fader dB
    juce::Label      peakLevelLabel;                    // L5 (right) — real-time peak dB
    juce::Slider     panKnob      { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox }; // L6
    juce::Label      panValueLabel;                     // L6.5 — "C" / "20 L" / "20 R" readout directly below the knob
    // Track Icon — painted placeholder square (trackIconBounds), no child
    // needed; now the BOTTOM row of the Scribble Strip (below trackNameLabel),
    // not its own mid-strip level — see resized()'s Bottom group carving.

    // ---- Deep stack (rackExpanded only), bottom → up ------------------------
    std::unique_ptr<RoutingBlock>        routingBlock;  // L9
    std::unique_ptr<SendsSection>        sendsSection;  // L10
    std::unique_ptr<InsertsRackComponent> insertsSection; // L11 (fwd-declared type; unique_ptr avoids the header)
    juce::TextButton                     channelCompButton { "Comp" }; // L12 — opens CrateCompressorPopup
    std::unique_ptr<CrateEQThumbnail>    eqThumbnail;   // L13
    juce::TextButton                     settingsButton { "Setting" }; // L14

    // Routing / sends backing state (moved here when ChannelStripRack dissolved).
    std::map<int, std::function<void()>> outputComboActions;
    std::vector<std::unique_ptr<CrateSendSlot>> sendSlots;

    float meterLevelDb = -60.0f; // -INF floor — renders empty until timerCallback() gets real DSP levels

    // Peak Hold — UI-side only (TE's LevelMeasurer reports near-instant block
    // peaks, not a held/decaying value); the 24Hz timerCallback() is the ONLY
    // writer, reading already-drained values out of meterClient. No audio-thread
    // access, no allocation.
    float peakHoldDb = -60.0f;
    juce::int64 peakHoldLastUpdateMs = 0;

    // All computed ONCE in resized(), read by paint() — sliced from the SAME
    // L4 fader block so meter columns and the fader can never drift apart.
    juce::Rectangle<float> meterBounds;   // main audio meter (upward)
    juce::Rectangle<float> grMeterBounds; // GR meter (downward), thin column beside it
    float gainReductionDb = 0.0f;         // 0 = no reduction; positive = dB of reduction
    juce::Rectangle<int>   trackIconBounds; // embedded icon square, below trackNameLabel — no plate/border of its own

    // The track's own colour — now shown as the name plate's fill itself
    // (unmuted state; see applyTrackColourToPlate()), not a separate strip.
    juce::Colour trackAccentColour { CrateDesignSystem::Colors::mixerStripDefaultAccent };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrip)
};
