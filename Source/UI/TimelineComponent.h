#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;
namespace tcore = tracktion::core;

/** Draws a bar/beat grid, the track's clips, and a playhead synced to Transport position. */
class TimelineComponent : public juce::Component,
                           private juce::Timer
{
public:
    TimelineComponent (te::Edit& editToShow, te::AudioTrack::Ptr trackToShow);
    ~TimelineComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    float xForTime (double seconds) const;

    te::Edit& edit;
    te::AudioTrack::Ptr track;
    double visibleLengthSeconds = 4.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineComponent)
};
