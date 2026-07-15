#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

/**
    Phase 5: automation curve editor for a single AutomatableParameter, generic over
    getValueRange() (not hardcoded to volume/dB).

    Architecture — "baked power curve":
    TE's native AutomationPoint::curve is a bezier tension (soft, rounded bend) and
    cannot produce FL Studio's y = x^t power-curve shape (flat-then-sharp-whip). So
    this component does NOT use that field at all (every point it writes to TE has
    curve = 0). Instead:
      - `anchors` (below) is OUR OWN model — the only source of truth for what the
        user sees and edits. Each anchor stores a SegmentType + tension governing
        the segment to the NEXT anchor.
      - Persistence: `anchors` is encoded into a custom "crateAnchors" string
        property on the AutomatableParameter's AutomationCurve ValueTree state,
        written every time rebakeCurve() runs (not just before a save) — TE writes
        that property out verbatim as part of the Edit's normal serialization, and
        ignores it as opaque extra data. On construction, that property is read back
        FIRST; only if it's absent (a curve this UI has never touched) does the
        component fall back to treating the curve's raw points as plain linear
        anchors. This is what stops save/load — and an in-session track-list
        rebuild, which also destroys and reconstructs this component — from reading
        rebakeCurve()'s ~30 baked sub-points back as if they were real anchors.
      - rebakeCurve() fully regenerates the real TE AutomationCurve from `anchors`
        on every edit: for a `curve` segment it injects 32 dense linear sub-points
        sampled from the y = x^t formula; for `hold` it injects a single phantom
        point just before the next anchor's time (a true discrete step, not a
        bezier approximation); `linear` needs no extra points.
      - paint() draws by sampling the exact same y = x^t formula (and the same
        phantom-point placement for hold), so what's on screen is provably what
        rebakeCurve() actually wrote for playback — same formula, not two
        implementations that could drift apart.
      - The sub-points are pure playback plumbing: only entries in `anchors` are
        ever hit-tested, dragged, or rendered as points. The user can't see or
        touch them.

    Interaction model (Ableton-style transform layered on top):
      - Click empty space              -> add/drag a single anchor.
      - Shift + drag empty space       -> marquee a time-selection.
      - Click inside the selection     -> vertical drag scales all anchors in range
                                           proportionally (pivoted at the parameter's
                                           value-range minimum).
      - Grab a selection edge          -> horizontal drag stretches anchor positions
                                           in range, anchored at the opposite edge.
      - Double-click an anchor         -> remove it.
      - Hover a segment's midpoint     -> a hollow "tension" handle appears (for any
                                           non-linear, non-hold segment type); drag it
                                           to adjust that segment's tension/density.
                                           ABSOLUTE position mapping, not delta: the
                                           handle's value is set directly from where the
                                           mouse sits in the lane (top edge = +1, bottom
                                           edge = -1), independent of drag history or
                                           which end of the segment is numerically
                                           higher — see updateTension().
      - Right-click an anchor          -> context menu: Hold / Linear / Single Curve /
                                           Double Curve / Wave / Pulse / Stairs, for the
                                           segment starting at that anchor.

    Entering a scale/stretch drag first inserts "boundary anchor" points at the
    selection's start/end (if none exist there already) so the curve outside the
    selection can't be disturbed by the transform.

    Performance: rebakeCurve() rewrites the entire TE AutomationCurve (clear +
    re-add every point) and is too expensive to call on every mouseDrag frame —
    doing so caused visible stutter. During a drag, only `anchors` is mutated and
    repaint() draws directly from it; rebakeCurve() is deferred to mouseUp() (and to
    one-shot actions — add/delete/menu choice — which aren't per-frame).
*/
class AutomationLaneComponent : public juce::Component,
                                 private juce::Timer,
                                 private juce::ChangeListener
{
public:
    // trackForParams supplies the parameter dropdown's contents (every automatable
    // parameter on the track, across all loaded plugins) — separate from paramToEdit
    // (the initially-shown parameter, still Volume by default from the call site)
    // since the two can now diverge the moment the user picks something else.
    AutomationLaneComponent (te::Edit& editToShow, te::AudioTrack::Ptr trackForParams,
                              te::AutomatableParameter::Ptr paramToEdit);
    ~AutomationLaneComponent() override;

    /** Sets the visible time window (seconds). Match this to the arrangement grid so
        the automation curve's x-axis lines up with the clip lane above it. */
    void setVisibleLength (double seconds)   { visibleLengthSeconds = juce::jmax (0.1, seconds); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    // juce::UndoManager IS-A juce::ChangeBroadcaster (sends a change message on
    // every perform()/undo()/redo()) — this is how the lane finds out an Undo/Redo
    // happened at all, since nothing else notifies it. Read-only refresh, not a
    // second rebakeCurve(): see changeListenerCallback()'s doc comment for why.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshAnchorsFromUndoRedo();

    float xForTime (double seconds) const;
    double timeForX (float x) const;
    float yForValue (float value) const;
    float valueForY (float y) const;

    enum class SegmentType { linear, curve, hold, doubleCurve, wave, pulse, stairs };

    struct AnchorPoint
    {
        int id = -1;
        double time = 0.0;
        float value = 0.0f;
        float tension = 0.0f;                          // shape parameter for curve/doubleCurve/wave/pulse/stairs
        SegmentType segmentType = SegmentType::linear;  // governs the segment to the NEXT anchor
    };

    // Power curve (y = x^p). p is chosen by whether the segment is ascending
    // (B.value >= A.value) or descending, NOT by the sign of tension alone, so
    // that positive tension always bows the curve toward the ceiling and negative
    // tension toward the floor regardless of which end is numerically higher.
    static float powerCurveY (float normalisedX, float tension, bool ascending);

    // S-curve: blends linear -> cubic smoothstep as |tension| increases.
    static float doubleCurveY (float normalisedX, float tension);

    // Sine LFO. Full amplitude always spans A.value..B.value (no separate depth
    // control) — tension instead maps to DENSITY, 1..16 cycles across the segment.
    static float waveY (float normalisedX, float tension);

    // Square LFO, same density mapping as waveY.
    static float pulseY (float normalisedX, float tension);

    // Quantized staircase; tension maps to density, 1..16 steps.
    static float stairsY (float normalisedX, float tension);

    // Single dispatch point used by both rebakeCurve() and paint() so the two can't
    // drift apart — whatever shape this returns is exactly what gets baked AND drawn.
    static float segmentShapeY (const AnchorPoint& a, const AnchorPoint& b, float normalisedX);

    void rebuildAnchorsFromCurve();   // constructs `anchors`: read persisted metadata, else migrate raw points
    void rebakeCurve();               // anchors -> TE AutomationCurve (full rebuild every call) + persist metadata

    void persistAnchorMetadata();          // anchors -> "crateAnchors" property on curve.state
    bool loadAnchorMetadataFromState();    // "crateAnchors" property -> anchors; false if absent

    int indexOfAnchorNear (juce::Point<float> position) const;
    int indexOfAnchorById (int id) const;

    juce::Point<float> segmentMidPosition (int leftAnchorIndex) const;
    int idOfSegmentHandleNear (juce::Point<float> position) const;

    enum class EdgeHit { none, left, right };
    EdgeHit hitTestSelectionEdge (float x) const;

    void insertBoundaryAnchors();
    void snapshotSelection();

    void beginScale (float mouseY);
    void updateScale (float mouseY);

    void beginStretch (EdgeHit edge);
    void updateStretch (float mouseX);

    void updateTension (float mouseY);

    void showCurveTypeMenu (int anchorIndex);
    void setSegmentType (int anchorIndex, SegmentType);

    // " - <param name>" suffix appended to every Undo transaction name here, so a
    // future Visual Undo History List can tell "Edit Automation Point - Volume"
    // apart from the same action on a different parameter/track. Empty string if
    // param is null (shouldn't happen in practice — these call sites all early-out
    // on param == nullptr before reaching a beginNewTransaction() call).
    juce::String transactionParamSuffix() const;

    // Rebuilds parameterSelector's items from track->getAllAutomatableParams().
    // Called once at construction. Not re-run on plugin load/removal — a lane
    // reopened after the track's plugin chain changes still reflects the list as
    // of when it was created; acceptable for now per the "for now" scoping of this
    // fix, revisit if that goes stale in practice.
    void refreshParameterList();

    // Swaps the edited parameter, re-derives `anchors` from the new parameter's own
    // curve/metadata, and resets any in-progress selection/drag state that pointed
    // at the old parameter's anchor IDs.
    void selectParameter (te::AutomatableParameter::Ptr newParam);

    te::Edit& edit;
    te::AudioTrack::Ptr ownerTrack;
    te::AutomatableParameter::Ptr param;
    double visibleLengthSeconds = 4.0;

    juce::ComboBox parameterSelector;
    std::vector<te::AutomatableParameter::Ptr> availableParams; // parallel to parameterSelector's item IDs (id - 1 == index)

    std::vector<AnchorPoint> anchors;  // always kept sorted by time — our source of truth
    int nextAnchorId = 0;

    enum class DragMode { none, addOrMovePoint, marqueeSelecting, scalingSelection, stretchingLeftEdge, stretchingRightEdge, adjustingTension };
    DragMode dragMode = DragMode::none;

    int draggingAnchorId = -1;
    int draggingSegmentAnchorId = -1;
    int hoveredSegmentAnchorId = -1;

    std::optional<juce::Range<double>> selectedTimeRange;
    double marqueeAnchorTime = 0.0;

    struct SnapshotAnchor { int id; double time; float value; };
    std::vector<SnapshotAnchor> transformSnapshot;

    float transformStartValue = 0.0f;
    double transformAnchorTime = 0.0;
    double transformStartEdgeTime = 0.0;

    juce::Label titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationLaneComponent)
};
