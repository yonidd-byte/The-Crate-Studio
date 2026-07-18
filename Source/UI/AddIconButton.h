#pragma once

#include <JuceHeader.h>

#include "CrateColors.h"

/**
    Small, crisp "+" add-icon button — replaces a blurry juce::TextButton("+")
    glyph (which read as a tiny, unclear text cross at small sizes) with a
    hand-drawn Path cross that stays sharp at any size. Shared by MixerStrip's
    and the Inspector's Channel Strip "add a new Send" affordance, so the two
    can never visually drift apart.
*/
class AddIconButton : public juce::Button
{
public:
    AddIconButton() : juce::Button ("Add Send") {}

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.5f);

        auto colour = CrateColors::NeonBlue;
        if (shouldDrawButtonAsDown)             colour = colour.darker (0.2f);
        else if (shouldDrawButtonAsHighlighted) colour = colour.brighter (0.2f);

        const float thickness = juce::jmax (1.5f, bounds.getWidth() * 0.16f);
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float armLength = bounds.getWidth() * 0.5f;

        juce::Path plus;
        plus.addRoundedRectangle (cx - armLength, cy - thickness * 0.5f, armLength * 2.0f, thickness, thickness * 0.5f);
        plus.addRoundedRectangle (cx - thickness * 0.5f, cy - armLength, thickness, armLength * 2.0f, thickness * 0.5f);

        g.setColour (colour);
        g.fillPath (plus);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddIconButton)
};
