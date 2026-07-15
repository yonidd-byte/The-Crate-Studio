#include "AutomationLaneComponent.h"
#include "TheCrateLookAndFeel.h"

#include <cmath>
#include <algorithm>

namespace
{
    namespace tcore = tracktion::core;
    using LAF = TheCrateLookAndFeel;

    constexpr float hitRadius = 8.0f;
    constexpr float edgeGrabPx = 6.0f;
    constexpr double minSelectionSeconds = 0.02;
    constexpr double minTransformSpanSeconds = 0.05;
    constexpr float minScaleDenominator = 0.01f;
    constexpr float pointEpsilonSeconds = 0.001f;
    constexpr float secondsPerBeatAt120Bpm = 0.5f;
    constexpr float tensionHandleHitRadius = 10.0f;
    constexpr float tensionHandleDrawRadius = 5.0f;
    constexpr double holdEpsilonSeconds = 0.001;
    constexpr int subPointsPerSegment = 32;      // curve / doubleCurve
    constexpr int denseSubPointsPerSegment = 64; // wave / pulse / stairs need more density

    // Custom, TE-opaque property on AutomationCurve::state carrying the macro anchor
    // list (id,time,value,tension,segmentType per anchor) so a save/load round-trip
    // (or an in-session rebuild of this component) restores the original 2-anchor
    // structure instead of reading ~30 baked sub-points back as real anchors.
    const juce::Identifier anchorMetadataProperty ("crateAnchors");
}

AutomationLaneComponent::AutomationLaneComponent (te::Edit& editToShow, te::AudioTrack::Ptr trackForParams,
                                                    te::AutomatableParameter::Ptr paramToEdit)
    : edit (editToShow), ownerTrack (trackForParams), param (paramToEdit)
{
    addAndMakeVisible (titleLabel);
    titleLabel.setText (param != nullptr ? "Shift+drag to select" : juce::String ("Automation (no parameter)"),
                         juce::dontSendNotification);
    titleLabel.setColour (juce::Label::textColourId, LAF::textDim);
    titleLabel.setFont (juce::FontOptions (11.0f));

    addAndMakeVisible (parameterSelector);
    refreshParameterList();
    parameterSelector.onChange = [this]
    {
        const auto id = parameterSelector.getSelectedId();

        if (id <= 0 || (size_t) (id - 1) >= availableParams.size())
            return;

        selectParameter (availableParams[(size_t) (id - 1)]);
    };

    rebuildAnchorsFromCurve();

    setWantsKeyboardFocus (false);
    startTimerHz (30);

    edit.getUndoManager().addChangeListener (this);
}

void AutomationLaneComponent::refreshParameterList()
{
    availableParams.clear();
    parameterSelector.clear (juce::dontSendNotification);

    if (ownerTrack == nullptr)
        return;

    int itemId = 1; // ComboBox item IDs are 1-based; 0 means "nothing selected"

    for (auto* p : ownerTrack->getAllAutomatableParams())
    {
        // getPluginAndParamName() (e.g. "4OSC: Filter Freq") disambiguates
        // same-named parameters across different plugins on the same track
        // (Volume exists on both the track's VolumeAndPanPlugin and, e.g., a
        // synth's own gain stage) — plain paramName alone would collide in the list.
        parameterSelector.addItem (p->getPluginAndParamName(), itemId);
        availableParams.push_back (p);

        if (p == param.get())
            parameterSelector.setSelectedId (itemId, juce::dontSendNotification);

        ++itemId;
    }
}

void AutomationLaneComponent::selectParameter (te::AutomatableParameter::Ptr newParam)
{
    if (newParam == param)
        return;

    param = newParam;

    // Old anchors/selection/drag state all referenced the previous parameter's
    // curve and anchor IDs — none of it is meaningful for the new one.
    anchors.clear();
    selectedTimeRange.reset();
    transformSnapshot.clear();
    dragMode = DragMode::none;
    draggingAnchorId = -1;
    draggingSegmentAnchorId = -1;
    hoveredSegmentAnchorId = -1;

    rebuildAnchorsFromCurve();

    titleLabel.setText ("Shift+drag to select", juce::dontSendNotification);
    repaint();
}

AutomationLaneComponent::~AutomationLaneComponent()
{
    stopTimer();
    edit.getUndoManager().removeChangeListener (this);
}

void AutomationLaneComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAnchorsFromUndoRedo();
}

void AutomationLaneComponent::refreshAnchorsFromUndoRedo()
{
    // Read-only re-sync, deliberately NOT rebuildAnchorsFromCurve() + rebakeCurve():
    // rebakeCurve() writes back through edit.getUndoManager() (clear + re-add every
    // point, then persistAnchorMetadata()). Calling that from inside a change
    // listener triggered BY an Undo/Redo would push a brand new mutation onto the
    // undo stack as a side effect of merely observing one — e.g. making Redo
    // unavailable immediately after an Undo, since the "observation" itself would
    // count as a new action. Safe alternative: the curve's real TE points and the
    // crateAnchors metadata property both already reverted together (both are
    // written through the same UndoManager transaction whenever this UI edits
    // anything), so this only needs to re-parse that metadata back into `anchors`.
    anchors.clear();

    if (param != nullptr)
    {
        if (! loadAnchorMetadataFromState())
        {
            // No metadata — a curve this UI has never touched (or one whose only
            // touch was itself just undone). Fall back to reading the TE curve's
            // raw points directly, same as rebuildAnchorsFromCurve()'s fallback,
            // but read-only — no rebakeCurve() call at the end.
            auto& curve = param->getCurve();

            for (int i = 0; i < curve.getNumPoints(); ++i)
            {
                AnchorPoint a;
                a.id = nextAnchorId++;
                a.time = curve.getPointTime (i).inSeconds();
                a.value = curve.getPointValue (i);
                anchors.push_back (a);
            }
        }
    }

    // Any in-progress gesture referred to anchor IDs / a selection that may no
    // longer mean anything against the reverted data.
    dragMode = DragMode::none;
    draggingAnchorId = -1;
    draggingSegmentAnchorId = -1;
    hoveredSegmentAnchorId = -1;
    selectedTimeRange.reset();
    transformSnapshot.clear();

    repaint();
}

float AutomationLaneComponent::xForTime (double seconds) const
{
    return (float) (seconds / visibleLengthSeconds) * (float) getWidth();
}

double AutomationLaneComponent::timeForX (float x) const
{
    return (double) x / (double) juce::jmax (1, getWidth()) * visibleLengthSeconds;
}

float AutomationLaneComponent::yForValue (float value) const
{
    if (param == nullptr)
        return (float) getHeight() * 0.5f;

    const auto range = param->getValueRange();
    const auto normalised = range.getLength() > 0.0f ? (value - range.getStart()) / range.getLength() : 0.5f;
    return (float) getHeight() * (1.0f - juce::jlimit (0.0f, 1.0f, normalised));
}

float AutomationLaneComponent::valueForY (float y) const
{
    if (param == nullptr)
        return 0.0f;

    const auto range = param->getValueRange();
    const auto normalised = juce::jlimit (0.0f, 1.0f, 1.0f - y / (float) juce::jmax (1, getHeight()));
    return range.getStart() + normalised * range.getLength();
}

float AutomationLaneComponent::powerCurveY (float normalisedX, float tension, bool ascending)
{
    const float x = juce::jlimit (0.0f, 1.0f, normalisedX);
    const float clampedTension = juce::jlimit (-1.0f, 1.0f, tension);

    // Which branch runs is chosen by the segment's direction, not folded into a
    // signed multiply — positive tension always bows toward the ceiling and
    // negative always toward the floor, regardless of whether A > B or A < B.
    const float p = ascending ? std::pow (10.0f, -clampedTension)
                               : std::pow (10.0f, clampedTension);
    return std::pow (x, p);
}

float AutomationLaneComponent::doubleCurveY (float normalisedX, float tension)
{
    const float x = juce::jlimit (0.0f, 1.0f, normalisedX);
    const float amount = juce::jlimit (0.0f, 1.0f, std::abs (tension));
    const float smoothstep = x * x * (3.0f - 2.0f * x);
    return x + (smoothstep - x) * amount; // tension 0 -> linear, |tension| 1 -> full S-curve
}

namespace
{
    // tension in [-1, 1] -> density in [1, 16] cycles/steps across the segment.
    float densityFromTension (float tension)
    {
        return juce::jmap (juce::jlimit (-1.0f, 1.0f, tension), -1.0f, 1.0f, 1.0f, 16.0f);
    }
}

float AutomationLaneComponent::waveY (float normalisedX, float tension)
{
    const float x = juce::jlimit (0.0f, 1.0f, normalisedX);
    const float cycles = densityFromTension (tension);
    // Full swing across A.value..B.value always — no separate amplitude control.
    return 0.5f + 0.5f * std::sin (x * cycles * juce::MathConstants<float>::twoPi);
}

float AutomationLaneComponent::pulseY (float normalisedX, float tension)
{
    const float x = juce::jlimit (0.0f, 1.0f, normalisedX);
    const float cycles = densityFromTension (tension);
    const float phase = x * cycles * juce::MathConstants<float>::twoPi;
    return std::sin (phase) >= 0.0f ? 1.0f : 0.0f;
}

float AutomationLaneComponent::stairsY (float normalisedX, float tension)
{
    const float x = juce::jlimit (0.0f, 1.0f, normalisedX);
    const int numSteps = juce::jmax (1, (int) std::round (densityFromTension (tension)));

    if (numSteps <= 1)
        return x < 1.0f ? 0.0f : 1.0f; // one step: flat until the very end, then jump

    const float stepped = std::floor (x * (float) numSteps) / (float) (numSteps - 1);
    return juce::jlimit (0.0f, 1.0f, stepped);
}

float AutomationLaneComponent::segmentShapeY (const AnchorPoint& a, const AnchorPoint& b, float normalisedX)
{
    switch (a.segmentType)
    {
        case SegmentType::curve:        return powerCurveY (normalisedX, a.tension, b.value >= a.value);
        case SegmentType::doubleCurve:  return doubleCurveY (normalisedX, a.tension);
        case SegmentType::wave:         return waveY (normalisedX, a.tension);
        case SegmentType::pulse:        return pulseY (normalisedX, a.tension);
        case SegmentType::stairs:       return stairsY (normalisedX, a.tension);

        case SegmentType::linear:
        case SegmentType::hold:
        default:
            return juce::jlimit (0.0f, 1.0f, normalisedX);
    }
}

bool AutomationLaneComponent::loadAnchorMetadataFromState()
{
    if (param == nullptr)
        return false;

    const auto encoded = param->getCurve().state[anchorMetadataProperty].toString();

    if (encoded.isEmpty())
        return false;

    anchors.clear();

    for (const auto& token : juce::StringArray::fromTokens (encoded, "|", ""))
    {
        const auto parts = juce::StringArray::fromTokens (token, ",", "");

        if (parts.size() != 5)
            continue;

        AnchorPoint a;
        a.id          = parts[0].getIntValue();
        a.time        = parts[1].getDoubleValue();
        a.value       = parts[2].getFloatValue();
        a.tension     = parts[3].getFloatValue();
        a.segmentType = (SegmentType) parts[4].getIntValue();

        anchors.push_back (a);
        nextAnchorId = juce::jmax (nextAnchorId, a.id + 1);
    }

    std::sort (anchors.begin(), anchors.end(), [] (const AnchorPoint& x, const AnchorPoint& y) { return x.time < y.time; });

    return ! anchors.empty();
}

void AutomationLaneComponent::persistAnchorMetadata()
{
    if (param == nullptr)
        return;

    juce::String encoded;

    for (auto& a : anchors)
    {
        if (! encoded.isEmpty())
            encoded << "|";

        encoded << a.id << "," << a.time << "," << a.value << "," << a.tension << "," << (int) a.segmentType;
    }

    // Set after rebakeCurve()'s curve.clear() + re-add pass, on the same state
    // ValueTree the curve just repopulated, so this survives whatever that call did.
    // Real UndoManager (not nullptr): this property write must land in the same
    // undo transaction as the curve points it describes, or Undo could restore the
    // curve's points while leaving stale/mismatched anchor metadata behind.
    param->getCurve().state.setProperty (anchorMetadataProperty, encoded, &edit.getUndoManager());
}

void AutomationLaneComponent::rebuildAnchorsFromCurve()
{
    anchors.clear();

    if (param == nullptr)
        return;

    if (loadAnchorMetadataFromState())
    {
        rebakeCurve(); // regenerate baked sub-points from the restored macro anchors
        return;
    }

    // No metadata — either a curve this UI has never touched, or a project saved
    // before this persistence existed. Fall back to treating each existing point as
    // a plain linear anchor (the original, pre-fix behaviour).
    auto& curve = param->getCurve();

    for (int i = 0; i < curve.getNumPoints(); ++i)
    {
        AnchorPoint a;
        a.id = nextAnchorId++;
        a.time = curve.getPointTime (i).inSeconds();
        a.value = curve.getPointValue (i);
        anchors.push_back (a);
    }

    rebakeCurve();
}

void AutomationLaneComponent::rebakeCurve()
{
    if (param == nullptr)
        return;

    auto& curve = param->getCurve();
    auto* um = &edit.getUndoManager();
    curve.clear (um);

    for (size_t i = 0; i < anchors.size(); ++i)
    {
        const auto& a = anchors[i];
        curve.addPoint (tcore::TimePosition::fromSeconds (a.time), a.value, 0.0f, um);

        if (i + 1 >= anchors.size())
            continue;

        const auto& b = anchors[i + 1];

        switch (a.segmentType)
        {
            case SegmentType::linear:
                break;

            case SegmentType::hold:
            {
                const auto phantomTime = b.time - holdEpsilonSeconds;

                if (phantomTime > a.time + pointEpsilonSeconds)
                    curve.addPoint (tcore::TimePosition::fromSeconds (phantomTime), a.value, 0.0f, um);

                break;
            }

            case SegmentType::curve:
            case SegmentType::doubleCurve:
            case SegmentType::wave:
            case SegmentType::pulse:
            case SegmentType::stairs:
            {
                if (a.segmentType == SegmentType::curve && std::abs (a.tension) < 0.001f)
                    break; // flat tension on a plain curve segment needs no extra points

                const bool isLfoOrStairs = a.segmentType == SegmentType::wave
                                            || a.segmentType == SegmentType::pulse
                                            || a.segmentType == SegmentType::stairs;
                const int numSubPoints = isLfoOrStairs ? denseSubPointsPerSegment : subPointsPerSegment;
                const auto range = param->getValueRange();

                for (int s = 1; s < numSubPoints; ++s)
                {
                    const float x = (float) s / (float) numSubPoints;
                    const float y = segmentShapeY (a, b, x);
                    const double t = a.time + (b.time - a.time) * (double) x;
                    const float v = juce::jlimit (range.getStart(), range.getEnd(), a.value + y * (b.value - a.value));
                    curve.addPoint (tcore::TimePosition::fromSeconds (t), v, 0.0f, um);
                }

                break;
            }
        }
    }

    persistAnchorMetadata();
}

int AutomationLaneComponent::indexOfAnchorNear (juce::Point<float> position) const
{
    for (size_t i = 0; i < anchors.size(); ++i)
    {
        const juce::Point<float> p (xForTime (anchors[i].time), yForValue (anchors[i].value));

        if (p.getDistanceFrom (position) <= hitRadius)
            return (int) i;
    }

    return -1;
}

int AutomationLaneComponent::indexOfAnchorById (int id) const
{
    for (size_t i = 0; i < anchors.size(); ++i)
        if (anchors[i].id == id)
            return (int) i;

    return -1;
}

juce::Point<float> AutomationLaneComponent::segmentMidPosition (int leftAnchorIndex) const
{
    if (leftAnchorIndex < 0 || leftAnchorIndex + 1 >= (int) anchors.size())
        return {};

    const auto& a = anchors[(size_t) leftAnchorIndex];
    const auto& b = anchors[(size_t) leftAnchorIndex + 1];

    const float midY = segmentShapeY (a, b, 0.5f);
    const double midTime = a.time + (b.time - a.time) * 0.5;
    const float midValue = a.value + midY * (b.value - a.value);

    return { xForTime (midTime), yForValue (midValue) };
}

int AutomationLaneComponent::idOfSegmentHandleNear (juce::Point<float> position) const
{
    for (int i = 0; i + 1 < (int) anchors.size(); ++i)
    {
        // Hold is a discrete step, not tension-parametrised — no handle for it.
        if (anchors[(size_t) i].segmentType == SegmentType::hold)
            continue;

        if (segmentMidPosition (i).getDistanceFrom (position) <= tensionHandleHitRadius)
            return anchors[(size_t) i].id;
    }

    return -1;
}

AutomationLaneComponent::EdgeHit AutomationLaneComponent::hitTestSelectionEdge (float x) const
{
    if (! selectedTimeRange.has_value())
        return EdgeHit::none;

    const auto leftX  = xForTime (selectedTimeRange->getStart());
    const auto rightX = xForTime (selectedTimeRange->getEnd());

    if (std::abs (x - leftX) <= edgeGrabPx)
        return EdgeHit::left;

    if (std::abs (x - rightX) <= edgeGrabPx)
        return EdgeHit::right;

    return EdgeHit::none;
}

void AutomationLaneComponent::insertBoundaryAnchors()
{
    if (param == nullptr || ! selectedTimeRange.has_value())
        return;

    for (double t : { selectedTimeRange->getStart(), selectedTimeRange->getEnd() })
    {
        bool exists = false;

        for (auto& a : anchors)
        {
            if (std::abs (a.time - t) < pointEpsilonSeconds)
            {
                exists = true;
                break;
            }
        }

        if (! exists)
        {
            const auto currentValue = param->getCurve().getValueAt (tcore::TimePosition::fromSeconds (t), param->getCurrentValue());

            AnchorPoint a;
            a.id = nextAnchorId++;
            a.time = t;
            a.value = currentValue;
            anchors.push_back (a);
        }
    }

    std::sort (anchors.begin(), anchors.end(), [] (const AnchorPoint& x, const AnchorPoint& y) { return x.time < y.time; });
    rebakeCurve();
}

void AutomationLaneComponent::snapshotSelection()
{
    transformSnapshot.clear();

    if (! selectedTimeRange.has_value())
        return;

    for (auto& a : anchors)
        if (a.time >= selectedTimeRange->getStart() - pointEpsilonSeconds && a.time <= selectedTimeRange->getEnd() + pointEpsilonSeconds)
            transformSnapshot.push_back ({ a.id, a.time, a.value });
}

void AutomationLaneComponent::beginScale (float mouseY)
{
    dragMode = DragMode::scalingSelection;
    insertBoundaryAnchors();
    snapshotSelection();
    transformStartValue = valueForY (mouseY);
}

void AutomationLaneComponent::updateScale (float mouseY)
{
    if (param == nullptr || transformSnapshot.empty())
        return;

    const auto range = param->getValueRange();
    const auto pivot = range.getStart(); // scale is anchored at the bottom of the lane

    const auto currentValue = valueForY (mouseY);

    const auto startDist = juce::jmax (minScaleDenominator, transformStartValue - pivot);
    const auto currentDist = currentValue - pivot;
    const auto scaleFactor = currentDist / startDist;

    for (auto& snap : transformSnapshot)
    {
        const auto idx = indexOfAnchorById (snap.id);

        if (idx < 0)
            continue;

        anchors[(size_t) idx].value = juce::jlimit (range.getStart(), range.getEnd(),
                                                      pivot + (snap.value - pivot) * scaleFactor);
    }

    // No rebakeCurve() here — this runs every mouseDrag frame. The heavy
    // clear-and-rebuild-the-whole-ValueTree work is batched once in mouseUp().
    repaint();
}

void AutomationLaneComponent::beginStretch (EdgeHit edge)
{
    if (! selectedTimeRange.has_value())
        return;

    dragMode = edge == EdgeHit::left ? DragMode::stretchingLeftEdge : DragMode::stretchingRightEdge;
    insertBoundaryAnchors();
    snapshotSelection();

    transformAnchorTime    = edge == EdgeHit::left ? selectedTimeRange->getEnd()   : selectedTimeRange->getStart();
    transformStartEdgeTime = edge == EdgeHit::left ? selectedTimeRange->getStart() : selectedTimeRange->getEnd();
}

void AutomationLaneComponent::updateStretch (float mouseX)
{
    if (transformSnapshot.empty() || ! selectedTimeRange.has_value())
        return;

    const auto rawEdgeTime = juce::jmax (0.0, timeForX (mouseX));

    const auto safeEdgeTime = dragMode == DragMode::stretchingLeftEdge
                                 ? juce::jmin (rawEdgeTime, transformAnchorTime - minTransformSpanSeconds)
                                 : juce::jmax (rawEdgeTime, transformAnchorTime + minTransformSpanSeconds);

    const auto originalSpan = transformStartEdgeTime - transformAnchorTime;
    const auto currentSpan  = safeEdgeTime - transformAnchorTime;
    const auto scaleFactor = currentSpan / originalSpan;

    for (auto& snap : transformSnapshot)
    {
        const auto idx = indexOfAnchorById (snap.id);

        if (idx < 0)
            continue;

        anchors[(size_t) idx].time = juce::jmax (0.0, transformAnchorTime + (snap.time - transformAnchorTime) * scaleFactor);
    }

    std::sort (anchors.begin(), anchors.end(), [] (const AnchorPoint& x, const AnchorPoint& y) { return x.time < y.time; });

    selectedTimeRange = dragMode == DragMode::stretchingLeftEdge
                           ? juce::Range<double> (safeEdgeTime, transformAnchorTime)
                           : juce::Range<double> (transformAnchorTime, safeEdgeTime);

    // No rebakeCurve() here — see updateScale().
    repaint();
}

void AutomationLaneComponent::updateTension (float mouseY)
{
    const auto idx = indexOfAnchorById (draggingSegmentAnchorId);

    if (idx < 0 || idx + 1 >= (int) anchors.size())
        return;

    // Absolute position, not relative delta: the handle's tension is read directly
    // from where the mouse sits in the lane right now — top edge = +1, bottom edge
    // = -1 — independent of drag history and of which end of the segment is
    // numerically higher. A relative delta-from-drag-start only guarantees
    // *direction* (up increases, down decreases), not a specific end result: if
    // the segment already had negative tension from an earlier edit, a modest
    // upward drag could still land negative, reproducing the "plummets instead of
    // holding high" symptom even though the direction was technically right.
    // Absolute mapping removes that ambiguity entirely.
    const auto normalisedY = juce::jlimit (0.0f, 1.0f, mouseY / (float) juce::jmax (1, getHeight()));
    const auto tension = juce::jlimit (-1.0f, 1.0f, 1.0f - 2.0f * normalisedY); // y=0 (top) -> +1, y=height (bottom) -> -1

    auto& a = anchors[(size_t) idx];
    a.tension = tension;

    // A fresh linear segment upgrades to a simple bend on drag; an already-typed
    // segment (wave, pulse, stairs, ...) keeps its type and just gets its
    // tension/density adjusted.
    if (a.segmentType == SegmentType::linear)
        a.segmentType = SegmentType::curve;

    // No rebakeCurve() here — see updateScale().
    repaint();
}

juce::String AutomationLaneComponent::transactionParamSuffix() const
{
    return param != nullptr ? (" - " + param->getParameterName()) : juce::String();
}

void AutomationLaneComponent::showCurveTypeMenu (int anchorIndex)
{
    if (anchorIndex < 0 || anchorIndex >= (int) anchors.size())
        return;

    const auto anchorId = anchors[(size_t) anchorIndex].id;

    juce::PopupMenu menu;
    menu.addItem ("Hold",         [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::hold); });
    menu.addItem ("Linear",       [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::linear); });
    menu.addItem ("Single Curve", [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::curve); });
    menu.addItem ("Double Curve", [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::doubleCurve); });
    menu.addItem ("Wave",         [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::wave); });
    menu.addItem ("Pulse",        [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::pulse); });
    menu.addItem ("Stairs",       [this, anchorId] { setSegmentType (indexOfAnchorById (anchorId), SegmentType::stairs); });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void AutomationLaneComponent::setSegmentType (int anchorIndex, SegmentType type)
{
    if (anchorIndex < 0 || anchorIndex + 1 >= (int) anchors.size())
        return;

    edit.getUndoManager().beginNewTransaction ("Change Automation Segment Type" + transactionParamSuffix());

    auto& a = anchors[(size_t) anchorIndex];
    a.segmentType = type;

    // Seed a sensible default tension so the shape is visible immediately, without
    // requiring a handle-drag first. Leaves an already-set tension alone (e.g.
    // switching Wave -> Pulse keeps whatever depth was already dialled in).
    switch (type)
    {
        case SegmentType::linear:
            a.tension = 0.0f;
            break;

        case SegmentType::curve:
        case SegmentType::doubleCurve:
        case SegmentType::stairs:
            if (std::abs (a.tension) < 0.001f)
                a.tension = 0.5f;
            break;

        case SegmentType::wave:
        case SegmentType::pulse:
            if (std::abs (a.tension) < 0.001f)
                a.tension = 0.6f;
            break;

        case SegmentType::hold:
            break;
    }

    rebakeCurve();
    repaint();
}

void AutomationLaneComponent::mouseDown (const juce::MouseEvent& e)
{
    if (param == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        const auto idx = indexOfAnchorNear (e.position);

        if (idx >= 0 && idx + 1 < (int) anchors.size())
            showCurveTypeMenu (idx);

        return;
    }

    const auto segmentId = idOfSegmentHandleNear (e.position);

    if (segmentId >= 0)
    {
        // Opens the transaction this gesture's eventual mouseUp() -> rebakeCurve()
        // will land in. Nothing between here and mouseUp calls beginNewTransaction()
        // again, so every curve.clear()/addPoint() that rebake performs coalesces
        // into the single Undo step this drag represents.
        edit.getUndoManager().beginNewTransaction ("Adjust Automation Tension" + transactionParamSuffix());
        dragMode = DragMode::adjustingTension;
        draggingSegmentAnchorId = segmentId;
        updateTension (e.position.y); // apply immediately at click position, not just on drag
        return;
    }

    if (e.mods.isShiftDown())
    {
        dragMode = DragMode::marqueeSelecting;
        marqueeAnchorTime = juce::jmax (0.0, timeForX (e.position.x));
        selectedTimeRange = juce::Range<double> (marqueeAnchorTime, marqueeAnchorTime);
        repaint();
        return;
    }

    if (selectedTimeRange.has_value())
    {
        const auto edgeHit = hitTestSelectionEdge (e.position.x);

        if (edgeHit != EdgeHit::none)
        {
            // beginStretch() calls insertBoundaryAnchors() synchronously, which can
            // rebakeCurve() immediately (not deferred to mouseUp) — the transaction
            // must be open BEFORE that call, not after, or the boundary-anchor bake
            // and the final mouseUp bake would split into two Undo steps.
            edit.getUndoManager().beginNewTransaction ("Stretch Automation Selection" + transactionParamSuffix());
            beginStretch (edgeHit);
            return;
        }

        if (selectedTimeRange->contains (timeForX (e.position.x)))
        {
            edit.getUndoManager().beginNewTransaction ("Scale Automation Selection" + transactionParamSuffix());
            beginScale (e.position.y);
            return;
        }

        selectedTimeRange.reset();
    }

    // Covers both sub-cases below (move an existing anchor / add a new one) — both
    // end in dragMode = addOrMovePoint, and mouseUp()'s rebakeCurve() for that mode
    // is the only curve mutation this gesture performs.
    edit.getUndoManager().beginNewTransaction ("Edit Automation Point" + transactionParamSuffix());

    const auto existingIdx = indexOfAnchorNear (e.position);

    if (existingIdx >= 0)
    {
        draggingAnchorId = anchors[(size_t) existingIdx].id;
    }
    else
    {
        AnchorPoint a;
        a.id = nextAnchorId++;
        a.time = juce::jmax (0.0, timeForX (e.position.x));
        a.value = valueForY (e.position.y);
        anchors.push_back (a);
        std::sort (anchors.begin(), anchors.end(), [] (const AnchorPoint& x, const AnchorPoint& y) { return x.time < y.time; });
        draggingAnchorId = a.id;
        // rebakeCurve() deferred to mouseUp() — this click may turn into a drag,
        // so batch with whatever else that gesture does rather than baking twice.
    }

    dragMode = DragMode::addOrMovePoint;
    repaint();
}

void AutomationLaneComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (param == nullptr)
        return;

    switch (dragMode)
    {
        case DragMode::marqueeSelecting:
        {
            const auto t = juce::jmax (0.0, timeForX (e.position.x));
            selectedTimeRange = juce::Range<double>::between (marqueeAnchorTime, t);
            repaint();
            break;
        }

        case DragMode::scalingSelection:
            updateScale (e.position.y);
            break;

        case DragMode::stretchingLeftEdge:
        case DragMode::stretchingRightEdge:
            updateStretch (e.position.x);
            break;

        case DragMode::adjustingTension:
            updateTension (e.position.y);
            break;

        case DragMode::addOrMovePoint:
        {
            const auto idx = indexOfAnchorById (draggingAnchorId);

            if (idx < 0)
                break;

            anchors[(size_t) idx].time = juce::jmax (0.0, timeForX (e.position.x));
            anchors[(size_t) idx].value = valueForY (e.position.y);
            std::sort (anchors.begin(), anchors.end(), [] (const AnchorPoint& x, const AnchorPoint& y) { return x.time < y.time; });

            // No rebakeCurve() here — see updateScale().
            repaint();
            break;
        }

        case DragMode::none:
            break;
    }
}

void AutomationLaneComponent::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::marqueeSelecting
        && selectedTimeRange.has_value()
        && selectedTimeRange->getLength() < minSelectionSeconds)
    {
        selectedTimeRange.reset();
    }

    // The heavy DSP work — clearing and fully rewriting TE's AutomationCurve —
    // happens exactly once here, after the gesture ends, instead of on every
    // mouseDrag frame. addOrMovePoint/scale/stretch/tension all only touched
    // `anchors` in memory during the drag itself.
    switch (dragMode)
    {
        case DragMode::addOrMovePoint:
        case DragMode::scalingSelection:
        case DragMode::stretchingLeftEdge:
        case DragMode::stretchingRightEdge:
        case DragMode::adjustingTension:
            rebakeCurve();
            break;

        case DragMode::marqueeSelecting:
        case DragMode::none:
            break;
    }

    dragMode = DragMode::none;
    draggingAnchorId = -1;
    draggingSegmentAnchorId = -1;
    transformSnapshot.clear();
    repaint();
}

void AutomationLaneComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto previousHover = hoveredSegmentAnchorId;
    hoveredSegmentAnchorId = idOfSegmentHandleNear (e.position);

    if (hoveredSegmentAnchorId != previousHover)
        repaint();

    if (hoveredSegmentAnchorId >= 0)
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    if (! selectedTimeRange.has_value())
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    const auto edgeHit = hitTestSelectionEdge (e.position.x);

    if (edgeHit != EdgeHit::none)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (selectedTimeRange->contains (timeForX (e.position.x)))
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void AutomationLaneComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto idx = indexOfAnchorNear (e.position);

    if (idx >= 0)
    {
        edit.getUndoManager().beginNewTransaction ("Delete Automation Point" + transactionParamSuffix());
        anchors.erase (anchors.begin() + idx);
        draggingAnchorId = -1;
        rebakeCurve();
        repaint();
    }
}

void AutomationLaneComponent::timerCallback()
{
    repaint();
}

void AutomationLaneComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);

    auto laneBounds = getLocalBounds().reduced (0, 4);
    g.setColour (LAF::panel);
    g.fillRect (laneBounds);

    g.setColour (LAF::panelLight);
    const int numBeats = (int) (visibleLengthSeconds / secondsPerBeatAt120Bpm);
    for (int i = 0; i <= numBeats; ++i)
    {
        const auto x = xForTime (i * (double) secondsPerBeatAt120Bpm);
        g.drawVerticalLine ((int) x, (float) laneBounds.getY(), (float) laneBounds.getBottom());
    }

    if (selectedTimeRange.has_value())
    {
        const auto selX1 = xForTime (selectedTimeRange->getStart());
        const auto selX2 = xForTime (selectedTimeRange->getEnd());
        const juce::Rectangle<float> selRect (selX1, (float) laneBounds.getY(), selX2 - selX1, (float) laneBounds.getHeight());

        g.setColour (LAF::accent.withAlpha (0.12f));
        g.fillRect (selRect);
        g.setColour (LAF::accent.withAlpha (0.6f));
        g.drawRect (selRect, 1.5f);
    }

    if (param != nullptr && ! anchors.empty())
    {
        juce::Path path;

        for (size_t i = 0; i < anchors.size(); ++i)
        {
            const auto& a = anchors[i];
            const auto x = xForTime (a.time);
            const auto y = yForValue (a.value);

            if (i == 0)
            {
                path.startNewSubPath (x, y);
                continue;
            }

            const auto& prev = anchors[i - 1];

            if (prev.segmentType == SegmentType::hold)
            {
                // Mirrors the phantom point rebakeCurve() inserts at b.time - epsilon.
                path.lineTo (xForTime (a.time - holdEpsilonSeconds), yForValue (prev.value));
                path.lineTo (x, y);
            }
            else if (prev.segmentType == SegmentType::linear)
            {
                path.lineTo (x, y);
            }
            else
            {
                // Same dispatch function rebakeCurve() sampled to build the real
                // sub-points — what you see here is what plays back.
                const bool isLfoOrStairs = prev.segmentType == SegmentType::wave
                                            || prev.segmentType == SegmentType::pulse
                                            || prev.segmentType == SegmentType::stairs;
                const int samples = isLfoOrStairs ? denseSubPointsPerSegment : subPointsPerSegment;

                for (int s = 1; s <= samples; ++s)
                {
                    const float sx = (float) s / (float) samples;
                    const float sy = segmentShapeY (prev, a, sx);
                    const double t = prev.time + (a.time - prev.time) * (double) sx;
                    const float v = prev.value + sy * (a.value - prev.value);
                    path.lineTo (xForTime (t), yForValue (v));
                }
            }
        }

        g.setColour (LAF::accent);
        g.strokePath (path, juce::PathStrokeType (2.0f));

        for (auto& a : anchors)
        {
            const auto x = xForTime (a.time);
            const auto y = yForValue (a.value);

            g.setColour (a.id == draggingAnchorId ? juce::Colours::white : LAF::accent);
            g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        }

        for (int i = 0; i + 1 < (int) anchors.size(); ++i)
        {
            const auto segId = anchors[(size_t) i].id;

            if (segId != hoveredSegmentAnchorId && segId != draggingSegmentAnchorId)
                continue;

            const auto handlePos = segmentMidPosition (i);
            g.setColour (segId == draggingSegmentAnchorId ? juce::Colours::white : LAF::accent);
            g.drawEllipse (handlePos.x - tensionHandleDrawRadius, handlePos.y - tensionHandleDrawRadius,
                            tensionHandleDrawRadius * 2.0f, tensionHandleDrawRadius * 2.0f, 1.5f);
        }

        const auto playheadX = xForTime (edit.getTransport().getPosition().inSeconds());

        if (playheadX >= 0.0f && playheadX <= (float) getWidth())
        {
            g.setColour (juce::Colour (0xffff3b30));
            g.drawLine (playheadX, 0.0f, playheadX, (float) getHeight(), 2.0f);
        }
    }
}

void AutomationLaneComponent::resized()
{
    auto topRow = getLocalBounds().removeFromTop (16);
    parameterSelector.setBounds (topRow.removeFromLeft (160).reduced (2, 0));
    titleLabel.setBounds (topRow.removeFromLeft (200).reduced (4, 0));
}
