#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "TheCrateLookAndFeel.h"
#include "CrateMixerLookAndFeel.h"
#include "CrateColors.h"
#include "CrateDesignSystem.h"
#include "TrackUtils.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    The "Sweet Spot" hybrid header for one track (Zone 3 / MASTER_ARCHITECTURE.md
    0.3) — Ableton's structural efficiency + Logic/SSL tactile controls + Cubase
    micro-states + Pro Tools routing discipline. A strict 3-column layout:

      Column 1 (Identity):        fold/collapse arrow + the editable track name,
                                  both sitting directly on top of a FULL solid
                                  fill of the track's own colour (Ableton style —
                                  not a translucent tint over the whole header,
                                  just this one column, opaque). nameLabel's text
                                  colour switches white/black per-track based on
                                  that fill's perceived brightness (see
                                  refreshNameLabelContrast()) so it's always
                                  readable regardless of how light or dark the
                                  track's colour is.
      Column 2 (Routing):         Two-Tier Ableton-style I/O (Hybrid Bus/Return
                                  Architecture directive) — Input Category /
                                  Input Specific combos, an IN/AUTO/OFF
                                  monitoring toggle, then Output Category /
                                  Output Specific combos, 5 rows tiling the
                                  column's full height with zero dead space.
                                  Flat/dark, zero bevel, display-only (no real
                                  device enumeration or hardware monitoring
                                  exists in this engine — see the constructor's
                                  own doc comment for exactly what's real vs.
                                  cosmetic). HIDDEN entirely when collapsed.
                                  (The automation-overlay toggle and the
                                  delete-track button that used to live here
                                  have been removed — see their own notes below.)
      Column 3 (Mini-Mixer):      a COMPACT cluster grouped to the LEFT of the
                                  column (deliberately not spread to fill it —
                                  the remaining right-hand space is reserved,
                                  currently-unused dead space for a future Sends
                                  UI): the Mute Plate (the track-number plate IS
                                  the mute toggle — lit CrateColors::NeonBlue
                                  when audible, dim CrateColors::DarkBackground
                                  when muted), Solo (SoloYellow) + Record
                                  (RecordCrimson) stacked vertically right next
                                  to it, then a Volume-over-Pan sub-stack (the
                                  Ableton-style numeric drag-box on top, the
                                  tactile Pan knob — a rotary juce::Slider
                                  rendered by CrateMixerLookAndFeel's
                                  pan_knob.png filmstrip, same premium bipolar
                                  feel as the Mixer's — below it). A slim
                                  VERTICAL LED meter runs down the far-right
                                  edge of the whole header (outside all 3
                                  columns).

    A 1px vertical separator (CrateColors::DarkBackground.darker()) is drawn at
    the Column 1/2 and Column 2/3 boundaries so the 3-column grid reads as
    strictly segmented (Ableton-style), not bleeding together.

    This is a deliberate FLAT, structural-grid-first pass — the Lead Architect
    asked for exact geometry/layout/colour-fill mechanics now, with premium
    SSL/micro-depth styling (drop shadows, textures, bevels) explicitly
    deferred to a later pass. Nothing here should be read as the final visual
    treatment.

    EXACT HARDCODED GEOMETRY (Lead Architect directive — dynamic
    removeFromLeft()/withSizeKeepingCentre() flexbox-style math produced a
    scattered, broken layout, so layoutExpanded() below is now literal
    juce::Rectangle constants for a 300x90 header, not derived math): Column 1
    is x=[0,90), Column 2 is x=[90,180), Column 3 is x=[180,300) with every
    control's bounds a fixed number. x=[282,300) is deliberately empty — no
    control, no meter — reserved for a future Sends UI; the far-right vertical
    LED meter this class used to draw is temporarily NOT painted (meterBounds
    stays empty) so it doesn't eat into that reserved strip. The collapsed
    micro-state (layoutCollapsed()) is UNCHANGED — this hardcoding is scoped
    to the expanded 90px state only, per the directive's own "300x90 header
    area" framing.

    Cubase/Ableton FOLD (micro-state): the fold arrow collapses this row to a
    single sleek strip (collapsedLaneHeight) — Column 2, the pan knob, and the
    volume slider hide; only the fold arrow, colour strip, name, mute plate, S/R,
    and the thin meter remain. The fold toggle changes this row's height, so it
    bridges out (onFoldToggle) to the owning TrackRow, which updates
    getRowHeight() and triggers a clean timeline relayout — clip alignment is
    preserved because the lane's clips re-lay against the new row height.

    Clutter Purge: the dedicated 'A' (automation) and 'x' (delete) buttons are
    GONE from this class entirely — "Purge Clutter" directive.
      - Automation: the per-track toggle is being replaced by a global
        automation-view toggle or context-menu paradigm (not yet built) — until
        that lands, there is NO UI trigger anywhere for the automation overlay
        (TrackRow::setAutomationVisible() and the whole overlay-rendering path
        are untouched and fully intact, just currently unreachable). This is a
        deliberate, disclosed gap, not an oversight.
      - Delete: replaced by the Delete/Backspace KEY instead of a button —
        matching the same click-to-focus + local keyPressed() pattern already
        established in this codebase (UniversalDeviceChainComponent's
        MiniParamSlider, CratePianoRollComponent's note deletion). Click a
        header to select+focus it, then press Delete/Backspace. onDeleteRequested
        still fires the exact same way (deferred via callAsync, since deleting
        destroys this very row mid-callback).

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

    // Delete/Backspace deletes this track (Purge Clutter directive — no more
    // dedicated 'x' button). Only reachable once this header has keyboard
    // focus, which mouseDown() grabs on click/select — same
    // click-to-focus-then-key pattern as MiniParamSlider/CratePianoRollComponent
    // elsewhere in this codebase.
    bool keyPressed (const juce::KeyPress&) override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    void setSelected (bool shouldBeSelected);

    // Called when the header is clicked (track should become the selected track).
    std::function<void()> onSelect;
    // Called when the fold/collapse arrow is toggled; query getCollapsed(). The
    // receiver (TrackHeaderColumn -> TrackListContent) mirrors the new collapsed
    // state onto the matching lane row so the row's height changes with it.
    std::function<void()> onFoldToggle;
    // Called when Delete/Backspace is pressed while this header has focus.
    // Fires onSelect first (so the receiver's "delete the selected track" logic
    // operates on the right track without needing this row to pass itself as
    // an argument).
    std::function<void()> onDeleteRequested;

    bool getCollapsed() const           { return isCollapsed; }

private:
    // Global Color Centralization: role "on" colours now read straight from
    // CrateColors' centralized semantic-status palette, so Solo/Record render
    // identically here and in MixerStrip/InspectorStrip (the old per-panel
    // divergence is gone). Mute is no longer a coloured button at all — the
    // track-number Mute Plate is the mute toggle now (see MutePlate below).
    inline static const juce::Colour soloOnColour   { CrateColors::SoloYellow };
    inline static const juce::Colour recordOnColour { CrateColors::RecordCrimson };

    // Flat, square, colour-coded toggle block — Ableton style: no default JUCE
    // TextButton bevel/gradient, just a solid-filled square with a single glyph
    // letter, coloured by role (Record Arm crimson / Solo yellow / Automation
    // accent) rather than by generic on/off text-button colours.
    class ToggleBlock : public juce::Button
    {
    public:
        ToggleBlock (juce::String glyphToShow, juce::Colour onColourToUse)
            : juce::Button ({}), glyph (std::move (glyphToShow)), onColour (onColourToUse) {}

        // Return Track Button directive: recordArmButton's glyph switches
        // from "R" to "PRE"/"POST" for a return track (which can't record) —
        // needs a mutator since the glyph was previously fixed at
        // construction via a default member initializer. setFontSize() exists
        // alongside it since "POST" doesn't fit this box at the single-letter
        // "R"/"S" font size.
        void setGlyph (juce::String newGlyph)   { glyph = std::move (newGlyph); repaint(); }
        void setFontSize (float newSize)         { fontSize = newSize; repaint(); }

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        juce::String glyph;
        juce::Colour onColour;
        float fontSize = CrateDesignSystem::Typography::toggleGlyphFontSize;
    };

    // Column 3's Mute Plate — the track-number plate IS the Mute toggle
    // (Ableton-style, same paradigm MixerStrip's name plate already uses).
    // Toggle state == "is muted": lit CrateColors::NeonBlue when audible
    // (off/unmuted), dim CrateColors::DarkBackground when muted. Shows the
    // 1-based track number as its glyph.
    class MutePlate : public juce::Button
    {
    public:
        MutePlate() : juce::Button ({}) {}
        void setNumberText (juce::String t)   { number = std::move (t); repaint(); }

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        juce::String number { "-" };
    };

    // Column 1's fold/collapse arrow — draws a right-pointing triangle when
    // collapsed, down-pointing when expanded (Cubase/Ableton disclosure glyph).
    // Reads its state from the owning header via a callback so there's one
    // source of truth (isCollapsed) rather than a second copy here.
    class FoldArrow : public juce::Button
    {
    public:
        FoldArrow() : juce::Button ({}) {}
        std::function<bool()> isExpanded; // supplied by the header

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    };

    // Column 2's monitoring toggle (IN / AUTO / OFF) — one flat box per state,
    // same "no JUCE chrome" language as ToggleBlock/MutePlate. Unlike
    // ToggleBlock (a real on/off per button), these three share ONE group
    // state (monitorMode below) — each button asks isActive() whether IT is
    // the currently-selected one, mirroring FoldArrow's "query the owner"
    // pattern rather than owning a second copy of the state.
    class MonitorButton : public juce::Button
    {
    public:
        MonitorButton (juce::String labelToShow) : juce::Button ({}), label (std::move (labelToShow)) {}
        std::function<bool()> isActive; // supplied by the header

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        juce::String label;
    };

    // Column 3's Ableton-style numeric volume drag-box. Deliberately NOT a
    // juce::Slider: an earlier LinearBar-styled Slider subclass was
    // unresponsive to drags — juce::Slider's internal mouse handling for bar
    // styles computes its draggable region via the ACTIVE LookAndFeel's
    // getSliderLayout() (Slider::Pimpl::mouseDrag's sliderRegionStart/Size),
    // which this component never set up, leaving drag behaviour entirely at
    // the mercy of whatever LookAndFeel happens to be inherited. Same root
    // category of problem UniversalDeviceChainComponent's MiniParamSlider doc
    // comment already calls out ("simpler on a plain Component than fighting
    // Slider's own mouse handling") — so this is a plain juce::Component with
    // its own directly-implemented horizontal relative-drag, matching that
    // established in-codebase precedent instead of fighting Slider again.
    // Paints its own flat dark well + CrateColors::NeonBlue fill + centred dB
    // readout; the DSP binding (onValueChange -> setVolumeDb) is unchanged,
    // just now driven by this class's own callback instead of juce::Slider's.
    class VolumeBar : public juce::Component
    {
    public:
        void setRange (double newMin, double newMax)   { rangeMin = newMin; rangeMax = newMax; }
        void setValue (double newValue, juce::NotificationType);
        double getValue() const                         { return value; }

        std::function<void()> onDragStart;
        std::function<void()> onValueChange;
        std::function<void()> onDragEnd;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override; // resets to 0 dB (unity)

    private:
        double rangeMin = -60.0, rangeMax = 6.0;
        double value = 0.0;
        double valueOnDragStart = 0.0;
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
    void refreshPanFromEngine();
    void refreshToggleStatesFromEngine();

    // Column 1 is filled SOLID with the track's colour (or a neutral fallback
    // if none is set) — nameLabel's text colour must switch to black or white
    // depending on that fill's brightness to stay readable. Called once at
    // construction and again whenever the track's colour changes
    // (valueTreePropertyChanged's te::IDs::colour branch).
    void refreshNameLabelContrast();

    // Fold toggle handler — flips isCollapsed, re-lays this header out for the
    // new micro-state (child visibility + geometry), then fires onFoldToggle so
    // the owning lane row can match the height change.
    void toggleFold();

    // Output Combo Box Logic: outputSpecificCombo hides whenever
    // outputCategoryCombo reads "Master" (id 1) — Master doesn't need a
    // secondary routing target — ANDed with the existing collapsed-state
    // constraint. Called both from applyCollapsedVisibility() and from the
    // category combo's own onChange, so neither can leave it in a stale
    // visibility state on its own.
    void updateOutputSpecificVisibility();
    void updateInputSpecificVisibility(); // "No Input" Logic: hides inputSpecificCombo when category == No Input

    // resized() dispatches to one of these on isCollapsed — kept separate so the
    // full 3-column geometry and the single-strip micro-state can't tangle.
    void layoutExpanded (juce::Rectangle<int> area);
    void layoutCollapsed (juce::Rectangle<int> area);

    // Sets child visibility for the CURRENT isCollapsed state (Column 2, pan
    // knob and volume slider hide when collapsed). Called from resized() before
    // laying out, so a hidden control never gets stale bounds.
    void applyCollapsedVisibility();

    // Right-click rename + track-colour editor — the SAME shared helper the
    // Mixer name-plate uses (CrateTrackEditor::showNameColourMenu), so recolour
    // from the timeline and from the Mixer are literally one code path.
    void showNameColourEditor();

    te::AudioTrack::Ptr track;
    CrateWorkflowManager& workflow;
    te::VolumeAndPanPlugin::Ptr volumePlugin;

    // Hybrid Bus/Return Architecture — cached once at construction (this
    // component is fully destroyed/recreated on every track-list rebuild, so
    // it never needs to react to a track LATER gaining/losing its
    // AuxReturnPlugin). Return tracks don't record from external inputs —
    // Column 2 hides Input Category/Specific + the monitor triad entirely and
    // centres only the Output combos when this is true (see layoutExpanded()
    // and applyCollapsedVisibility()).
    bool isReturnTrackFlag = false;

    // LevelMeterPlugin doesn't declare its own Ptr alias (only the base Plugin::Ptr
    // exists), so hold it as a raw pointer — lifetime is owned by track->pluginList,
    // same reasoning as MixerStrip's identical member.
    te::LevelMeterPlugin* meterPlugin = nullptr;
    te::LevelMeasurer::Client meterClient;
    float meterLevelDb = -100.0f;
    juce::Rectangle<int> meterBounds;

    // Column 1's full bounds — filled SOLID with the track's colour (Ableton
    // style; supersedes an earlier thin 3px accent-strip version). Also the
    // rect nameLabel's text sits on top of, which is why its text colour must
    // be recomputed for contrast whenever this fill's colour changes — see
    // refreshNameLabelContrast().
    juce::Rectangle<int> column1Bounds;

    bool selected = false;
    bool isDragHovering = false;
    bool isCollapsed = false; // Cubase/Ableton fold micro-state

    // CrateMixerLookAndFeel renders the pan knob's pan_knob.png filmstrip (+ the
    // touch-gated neon glow) — declared BEFORE panKnob so panKnob is destroyed
    // first (members die in reverse declaration order); the dtor also calls
    // panKnob.setLookAndFeel(nullptr) belt-and-braces, same discipline MixerStrip
    // keeps for its own identical pairing.
    CrateMixerLookAndFeel mixerLookAndFeel;

    // Column 2's monitor toggle state (IN/AUTO/OFF) — AUTO is Ableton's own
    // default (auto-monitor on record arm). Purely local UI state: no hardware
    // zero-latency monitoring or record-arm-triggered auto-switch exists in
    // this engine, so this is NOT persisted to the track's ValueTree the way
    // Record Arm's own state is — see the constructor's doc comment.
    enum class MonitorMode { in, autoMode, off };
    MonitorMode monitorMode = MonitorMode::autoMode;
    void setMonitorMode (MonitorMode newMode)   { monitorMode = newMode; repaint(); }

    FoldArrow foldArrow;
    juce::Label nameLabel;

    // Column 2: Two-Tier Ableton-style I/O. Category combos pick a routing
    // family ("Ext. In"/"Resampling", "Master"/"Ext. Out"/"Sends Only");
    // Specific combos pick the exact channel/destination within that family.
    // Flat/dark, no bevel (styled in the .cpp, same styleFlatCombo lambda the
    // single combos used before). Display-only — see the constructor's doc
    // comment for exactly what's real (the Master output name) vs. cosmetic
    // (everything else: no input-device enumeration or output-bus routing
    // exists in this engine yet).
    juce::ComboBox inputCategoryCombo, inputSpecificCombo;
    MonitorButton monitorInButton   { "IN" };
    MonitorButton monitorAutoButton { "AUTO" };
    MonitorButton monitorOffButton  { "OFF" };
    juce::ComboBox outputCategoryCombo, outputSpecificCombo;

    MutePlate mutePlate; // the track-number plate, doubling as the Mute toggle
    ToggleBlock recordArmButton { "R", recordOnColour };
    ToggleBlock soloButton      { "S", soloOnColour };
    VolumeBar volumeSlider; // Ableton-style numeric drag-box
    juce::Slider panKnob { juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderComponent)
};
