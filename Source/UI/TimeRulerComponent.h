#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

/**
    Bars/beats ruler drawn above the track lanes. Uses the shared CrateArrangement
    layout so its ticks line up with the grid lines drawn down through every lane.
    The ruler component itself is fixed (not inside the scrolling viewport, so it
    stays visible while scrolling vertically through many tracks) — but since
    zooming can make the track content wider than the viewport (horizontal
    scrolling), ArrangementComponent keeps setHorizontalOffset() in sync with the
    viewport's horizontal scroll position so the ticks stay aligned with the grid
    underneath instead of drifting the moment you scroll sideways.

    Click/drag anywhere on the ruler scrubs the transport to that position.
*/
class TimeRulerComponent : public juce::Component
{
public:
    explicit TimeRulerComponent (te::Edit& editToUse);
    ~TimeRulerComponent() override;

    /** Call whenever the track viewport's horizontal scroll position changes. */
    void setHorizontalOffset (int newOffsetPx);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void scrubToLocalX (int localX);

    te::Edit& edit;
    int horizontalOffset = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeRulerComponent)
};
