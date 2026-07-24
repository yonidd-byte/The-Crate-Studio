#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "TheCrateLookAndFeel.h"
#include "FlatGridComboLookAndFeel.h"
#include "CrateTheme.h"
#include "CrateDesignSystem.h"
#include "CrateValueBox.h"
#include "CrateFoldArrow.h"
#include "TrackUtils.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    The "Sweet Spot" hybrid header for one track (Zone 3 / MASTER_ARCHITECTURE.md
    0.3) — Ableton's structural efficiency + Logic/SSL tactile controls + Cubase
    micro-states + Pro Tools routing discipline. A strict 3-column layout:

      Column 1 (Identity):        THE FUSED IDENTITY BLOCK — fold/collapse
                                  arrow, the editable track name, AND the
                                  track-number Mute Plate all sit directly on
                                  top of a FULL, opaque, full-HEIGHT fill of
                                  the track's own colour (Ableton style — not
                                  a translucent tint, this one column, solid).
                                  Both the name and the number switch their
                                  text colour to trackColor.contrasting() so
                                  they stay readable regardless of how light
                                  or dark the track's colour is.
      Column 2 (Routing):         Two-Tier Ableton-style I/O (Hybrid Bus/Return
                                  Architecture directive) — Input Category /
                                  Input Specific combos, an IN/AUTO/OFF
                                  monitoring toggle, then Output Category /
                                  Output Specific combos. CONTENT-DRIVEN
                                  DYNAMIC HEIGHT: each is its own row, and a
                                  row that doesn't apply (Input Specific +
                                  the monitor triad when Input Category is
                                  "No Input"; Output Specific when Output
                                  Category is "Master") is not just hidden —
                                  it's REMOVED from the stack, and everything
                                  below it shifts up, shrinking the track's
                                  total height (see computePreferredHeight()).
                                  Flat/dark, zero bevel, display-only (no real
                                  device enumeration or hardware monitoring
                                  exists in this engine — see the constructor's
                                  own doc comment for exactly what's real vs.
                                  cosmetic). HIDDEN entirely when collapsed.
      Column 3 (Mini-Mixer):      Ableton Geometry directive — ABSOLUTE,
                                  hardcoded pixel math (see resized()), not a
                                  derived/relative layout: Box A (Mute Plate/
                                  track number) + Box B (Solo) + Box C (Record,
                                  hidden with the same "No Input" rule Column 2
                                  uses) flush in row 1; Volume directly under
                                  Box A at the same width in row 2; Pan — a
                                  flat horizontal drag-box (PNG Pivot directive:
                                  NOT the pan_knob.png rotary MixerStrip/
                                  MasterStrip still use — a knob breaks this
                                  column's strict row-height grid) — beside
                                  Volume in row 2. This column's content is
                                  FIXED regardless of track state, so it's the
                                  floor under Column 2's dynamic range (see
                                  CrateDesignSystem::Metrics::TrackHeader::
                                  column3FixedHeight). A slim VERTICAL LED
                                  meter is right-aligned within Column 3's own
                                  bounds, with a permanent margin so it never
                                  touches the header's absolute right edge.

    Quiet hairline separators (low-alpha black, "felt not seen" — see
    CrateDesignSystem::Metrics::TrackHeader::hairlineAlpha) are drawn at the
    Column 1/2 and Column 2/3 boundaries so the 3-column grid reads as
    segmented without the harsh, high-contrast ruled-table look an earlier
    pass over-corrected into.

    CONTENT-DRIVEN DYNAMIC HEIGHT (graduated from the earlier "EXACT
    HARDCODED 300x90" directive, which was a deliberate stabilization step,
    not the final architecture): Column 1/2's x-boundaries (0/90/180/300)
    stay fixed — only the HEIGHT flexes now, computed by
    computePreferredHeight() from exactly which rows the CURRENT state needs.
    layoutExpanded() lays out top-down against whatever height its caller
    already sized this component to (see getPreferredHeight()'s own doc
    comment on why the row/lane OWNER, not this class, must call that first).
    The collapsed micro-state (layoutCollapsed()) is separate and unaffected.

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

    // Stroke Occlusion Fix directive: the 2px global grid strokes (column
    // separators, top/bottom/right borders) live here, NOT in paint() — JUCE
    // paints child components (Column 2's combos/buttons) strictly AFTER the
    // parent's own paint() but BEFORE paintOverChildren(), so any of those
    // strokes drawn in paint() that overlapped a child's bounds got silently
    // painted over the instant that child rendered itself. Drawing them here
    // instead guarantees they act as a hardware "grill" on top of everything.
    void paintOverChildren (juce::Graphics&) override;

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
    // Content-Driven Dynamic Height directive: fires whenever ANYTHING that
    // feeds getPreferredHeight() changes — the fold arrow (query
    // getCollapsed()), OR the Input/Output Category combos (a row
    // appeared/disappeared). The receiver (TrackHeaderColumn/ReturnHeaderDock
    // -> TrackListContent/ReturnLaneDock) reads getPreferredHeight() and
    // pushes it onto the matching lane row so the row's height always tracks
    // whatever this header currently needs, not just its fold state.
    std::function<void()> onFoldToggle;
    // Called when Delete/Backspace is pressed while this header has focus.
    // Fires onSelect first (so the receiver's "delete the selected track" logic
    // operates on the right track without needing this row to pass itself as
    // an argument).
    std::function<void()> onDeleteRequested;

    bool getCollapsed() const           { return isCollapsed; }

    // Content-Driven Dynamic Height directive: the row/lane owner (TrackRow/
    // TrackHeaderColumn in ArrangementComponent.cpp) queries this to size
    // BOTH this header and its matching clip-lane row — the two are separate
    // Component trees, so this must be the ONE authority both read, not
    // something each recomputes independently and risks disagreeing on.
    int getPreferredHeight() const;

    // Static overload so ArrangementComponent's TrackRow (which has no live
    // header instance to ask) can compute the IDENTICAL height from just the
    // facts persisted on the track's own state (isCollapsed/isReturnTrack/
    // input+output category ids — see the .cpp's persisted property ids).
    static int computePreferredHeight (bool isReturnTrack, bool isCollapsedState,
                                        int inputCategoryId, int outputCategoryId);

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

    // Column 3's Box A: the Mute Plate — the track-number plate IS the Mute
    // toggle (Ableton-style, same paradigm MixerStrip's name plate already
    // uses). Toggle state == "is muted": lit CrateColors::NeonBlue when
    // audible (off/unmuted), dim CrateColors::DarkBackground when muted.
    // Shows the 1-based track number as its glyph.
    class MutePlate : public juce::Button
    {
    public:
        MutePlate() : juce::Button ({}) {}
        void setNumberText (juce::String t)   { number = std::move (t); repaint(); }

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        juce::String number { "-" };
    };

    // Column 1's fold/collapse arrow — CrateFoldArrow (CrateFoldArrow.h).
    // Extracted out of this class (was a private nested class here
    // originally) so ArrangementComponent's MasterHeaderRow can reuse the
    // identical disclosure-glyph look for Master Track Fold Parity.

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

    // Column 3's Ableton-style numeric volume drag-box and Pan control —
    // CrateVolumeBar/CratePanBar (CrateValueBox.h). Extracted out of this
    // class (they were private nested classes here originally) so
    // ArrangementComponent's MasterHeaderRow can reuse the identical
    // paint()/mouseDrag() code for 100% Column 3 parity between Master and a
    // standard track, rather than a second hand-maintained copy.

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

    // Fused Identity Block directive: Column 1 is filled SOLID with the
    // track's colour (dimmed if muted) — the name label AND the track-number
    // Mute Plate both need their text colour switched to
    // trackColor.contrasting() to stay readable. getIdentityFillColour() is
    // the ONE place that computes the effective fill (return/master grey
    // override + mute dim), read by both refreshIdentityContrast() and
    // paint() so they can never disagree on what Column 1 actually looks
    // like. Called once at construction and again whenever the track's
    // colour or mute state changes.
    juce::Colour getIdentityFillColour() const;
    void refreshIdentityContrast();

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

    // "No Input" & Monitor Logic directive: hides inputSpecificCombo, the
    // Monitor triad, AND (expanded state) Record Arm when category == No
    // Input — see the .cpp for exactly which rows survive in which state.
    void updateInputDependentVisibility();

    // resized() dispatches to one of these on isCollapsed — kept separate so the
    // full 3-column geometry and the single-strip micro-state can't tangle.
    void layoutExpanded (juce::Rectangle<int> area);
    void layoutCollapsed (juce::Rectangle<int> area);

    // Immutable Column Widths directive: Column 1 (identity) and Column 3
    // (mini-mixer) use IDENTICAL x/width/child-position math whether folded
    // or not — factored out here once so the two states can never drift
    // apart into two independently-hand-maintained copies of the same
    // numbers. Only Column 2's content differs (full routing stack vs a
    // single dummy placeholder box), so that part stays local to each of
    // layoutExpanded()/layoutCollapsed().
    void layoutIdentityColumn (juce::Rectangle<int> full);
    void layoutColumn3 (juce::Rectangle<int> full);

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

    // Collapsed Column 2 Placeholder directive: folded state still shows a
    // blank, disabled "dummy" ComboBox-styled box in Column 2 (Ableton never
    // lets a column go visually empty) — same x, 4px inset, and 13px height
    // as the expanded state's own top row. Empty when expanded (layoutExpanded()
    // never touches it); paint()/paintOverChildren() gate its fill/border on
    // isCollapsed.
    juce::Rectangle<int> collapsedRoutingPlaceholder;

    bool selected = false;
    bool isDragHovering = false;
    bool isCollapsed = false; // Cubase/Ableton fold micro-state

    // Strict I/O Grid directive — scoped ONLY to Column 2's four routing
    // combos (see the constructor's setLookAndFeel() calls / the dtor's
    // teardown).
    FlatGridComboLookAndFeel flatGridLookAndFeel;

    // Column 2's monitor toggle state (IN/AUTO/OFF) — AUTO is Ableton's own
    // default (auto-monitor on record arm). Purely local UI state: no hardware
    // zero-latency monitoring or record-arm-triggered auto-switch exists in
    // this engine, so this is NOT persisted to the track's ValueTree the way
    // Record Arm's own state is — see the constructor's doc comment.
    enum class MonitorMode { in, autoMode, off };
    MonitorMode monitorMode = MonitorMode::autoMode;
    void setMonitorMode (MonitorMode newMode)   { monitorMode = newMode; repaint(); }

    CrateFoldArrow foldArrow;
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
    MonitorButton monitorInButton   { "In" };
    MonitorButton monitorAutoButton { "Auto" };
    MonitorButton monitorOffButton  { "Off" };
    juce::ComboBox outputCategoryCombo, outputSpecificCombo;

    MutePlate mutePlate; // the track-number plate, doubling as the Mute toggle
    ToggleBlock recordArmButton { "R", recordOnColour };
    ToggleBlock soloButton      { "S", soloOnColour };
    CrateVolumeBar volumeSlider; // Ableton-style numeric drag-box
    CratePanBar panBar; // Ableton Geometry / PNG Pivot directive — flat drag-box, not a rotary knob

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderComponent)
};
