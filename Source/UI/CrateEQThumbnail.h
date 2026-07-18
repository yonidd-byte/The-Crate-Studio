#pragma once

#include <JuceHeader.h>
#include "TheCrateLookAndFeel.h"

/**
    Logic Pro-style "Channel EQ" thumbnail — the small always-visible EQ curve
    preview that sits at the TOP of every channel strip (both the Mixer strips
    and the dual-strip Track Inspector). Purely a VISUAL scaffold for now
    (MASTER_ARCHITECTURE.md Zone 5, "Pro Mix Environment" — layout/paradigm
    pass, no DSP yet): a dark graph panel with subtle grid lines and a single
    flat curve through the vertical centre, representing a default (unedited)
    EQ. Clicking it will later open the full EQ editor; today it only renders.

    Fixed 60px tall by contract (the strips reserve exactly that at their top),
    but width-flexible so it fills whatever strip column it's dropped into.
*/
class CrateEQThumbnail : public juce::Component
{
public:
    CrateEQThumbnail()
    {
        setInterceptsMouseClicks (false, false); // display-only for now
    }

    void paint (juce::Graphics& g) override
    {
        using LAF = TheCrateLookAndFeel;

        auto bounds = getLocalBounds().toFloat();

        // Dark graph panel — deliberately darker than the strip's own panel
        // colour so the EQ "screen" reads as an inset display (same
        // lcdBackground language the transport's LCD already uses).
        g.setColour (LAF::lcdBackground);
        g.fillRoundedRectangle (bounds, 3.0f);

        // Subtle grid: 3 vertical (low/mid/high thirds) + 1 horizontal (0 dB
        // centre line the curve rests on). Kept very low-alpha so it reads as
        // texture, not chrome.
        g.setColour (juce::Colours::white.withAlpha (0.06f));

        for (int i = 1; i < 4; ++i)
        {
            const float x = bounds.getX() + bounds.getWidth() * (float) i / 4.0f;
            g.drawVerticalLine ((int) x, bounds.getY() + 2.0f, bounds.getBottom() - 2.0f);
        }

        const float centreY = bounds.getCentreY();
        g.drawHorizontalLine ((int) centreY, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);

        // The EQ curve itself — a flat line through the centre (a default,
        // unedited response). Drawn as a Path so the later "real curve" pass
        // only has to add control points, not change the rendering.
        juce::Path curve;
        curve.startNewSubPath (bounds.getX() + 2.0f, centreY);
        curve.lineTo (bounds.getRight() - 2.0f, centreY);

        g.setColour (LAF::accent.withAlpha (0.9f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        // Thin frame so the thumbnail has a crisp edge against the strip body.
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateEQThumbnail)
};
