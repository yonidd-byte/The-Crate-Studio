#include "TimeRulerComponent.h"
#include "ArrangementLayout.h"
#include "TheCrateLookAndFeel.h"

using namespace CrateArrangement;
using LAF = TheCrateLookAndFeel;

TimeRulerComponent::TimeRulerComponent (te::Edit& editToUse) : edit (editToUse) {}
TimeRulerComponent::~TimeRulerComponent() = default;

void TimeRulerComponent::setHorizontalOffset (int newOffsetPx)
{
    if (horizontalOffset == newOffsetPx)
        return;

    horizontalOffset = newOffsetPx;
    repaint();
}

void TimeRulerComponent::mouseDown (const juce::MouseEvent& e)   { scrubToLocalX (e.x); }
void TimeRulerComponent::mouseDrag (const juce::MouseEvent& e)   { scrubToLocalX (e.x); }

void TimeRulerComponent::scrubToLocalX (int localX)
{
    // Ignore clicks over the fixed "Bars" corner label — only the actual
    // timeline portion of the ruler scrubs.
    if (localX < headerWidth)
        return;

    const auto absoluteX = localX + horizontalOffset;
    edit.getTransport().setPosition (tracktion::TimePosition::fromSeconds (xToTime (absoluteX)));
}

void TimeRulerComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);

    const auto w = getWidth();
    const auto h = (float) getHeight();

    // Header-column corner label — NOT offset by scroll (it's a fixed label over
    // the also-fixed header column, not part of the scrolling timeline).
    g.setColour (LAF::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Bars", juce::Rectangle<int> (0, 0, headerWidth, getHeight()).reduced (8, 0),
                juce::Justification::centredLeft);

    // BUG FIX: this used to loop a fixed 0..barsToShow*beatsPerBar range
    // regardless of scroll/zoom, which both (a) went visibly blank once you
    // scrolled/zoomed past that fixed range ("stuck at 9") and (b) wastefully
    // recomputed every tick's position even when off-screen. Now it only
    // computes the beats that actually intersect the visible [horizontalOffset,
    // horizontalOffset + w) window, however far into the (now generous,
    // 500-bar) project that is.
    int firstBeat, lastBeat;
    visibleBeatRange (horizontalOffset, horizontalOffset + w, firstBeat, lastBeat);

    for (int beat = firstBeat; beat <= lastBeat; ++beat)
    {
        const auto x = timeToX (beat * secondsPerBeat) - (float) horizontalOffset;
        const bool isBar = (beat % beatsPerBar) == 0;

        g.setColour (isBar ? LAF::textDim : LAF::panelLight);
        g.drawVerticalLine ((int) x, isBar ? 2.0f : h * 0.55f, h);

        if (isBar)
        {
            g.setColour (LAF::text);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (beat / beatsPerBar + 1),
                         juce::Rectangle<float> (x + 3.0f, 1.0f, 30.0f, h - 2.0f),
                         juce::Justification::centredLeft);
        }
    }

    g.setColour (LAF::panelLight);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) w);
}
