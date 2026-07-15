#include "TimelineComponent.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    const auto backgroundColour = LAF::panel;
    const auto laneColour       = LAF::panelLight;
    const auto gridColour       = juce::Colour (0xff333338);
    const auto playheadColour   = juce::Colour (0xffff3b30);

    constexpr double secondsPerBeatAt120Bpm = 0.5;
}

TimelineComponent::TimelineComponent (te::Edit& editToShow, te::AudioTrack::Ptr trackToShow)
    : edit (editToShow), track (trackToShow)
{
    startTimerHz (30);
}

TimelineComponent::~TimelineComponent()
{
    stopTimer();
}

float TimelineComponent::xForTime (double seconds) const
{
    return (float) (seconds / visibleLengthSeconds) * (float) getWidth();
}

void TimelineComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto laneBounds = getLocalBounds().reduced (0, 8);
    g.setColour (laneColour);
    g.fillRect (laneBounds);

    g.setColour (gridColour);
    const int numBeats = (int) (visibleLengthSeconds / secondsPerBeatAt120Bpm);
    for (int i = 0; i <= numBeats; ++i)
    {
        const auto x = xForTime (i * secondsPerBeatAt120Bpm);
        g.drawVerticalLine ((int) x, (float) laneBounds.getY(), (float) laneBounds.getBottom());
    }

    if (track != nullptr)
    {
        // ClipPosition holds a tracktion::core::TimeRange in its `.time` member —
        // matches the tracktion::core:: time types your Phase 1 build confirmed working.
        for (auto* clip : track->getClips())
        {
            const auto start = clip->getPosition().time.getStart().inSeconds();
            const auto end   = clip->getPosition().time.getEnd().inSeconds();

            juce::Rectangle<float> clipRect (xForTime (start), (float) laneBounds.getY() + 4.0f,
                                              xForTime (end) - xForTime (start),
                                              (float) laneBounds.getHeight() - 8.0f);

            g.setColour (LAF::clip);
            g.fillRoundedRectangle (clipRect, 4.0f);
            g.setColour (LAF::clip.darker (0.3f));
            g.drawRoundedRectangle (clipRect, 4.0f, 1.0f);

            g.setColour (LAF::textOnClip);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (clip->getName(), clipRect.reduced (4.0f, 0.0f), juce::Justification::centredLeft);
        }
    }

    const auto playheadSeconds = edit.getTransport().getPosition().inSeconds();
    const auto playheadX = xForTime (playheadSeconds);

    if (playheadX >= 0.0f && playheadX <= (float) getWidth())
    {
        g.setColour (playheadColour);
        g.drawLine (playheadX, 0.0f, playheadX, (float) getHeight(), 2.0f);
    }
}

void TimelineComponent::resized() {}

void TimelineComponent::timerCallback()
{
    repaint();
}
