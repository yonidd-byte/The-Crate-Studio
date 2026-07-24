#pragma once

#include <JuceHeader.h>

#include "CrateTheme.h"

/**
    Mute / Solo / Record / Input-monitor "invisible until active" console
    buttons — V2.0 Manifesto's "Ghosted Buttons" pushed further per the Lead
    Architect's directive: OFF is flush with the TRACK strip's own background
    (not a visibly distinct grey chip), so an inactive button genuinely
    disappears into the panel instead of reading as "a button that happens to
    be off." ON pops out as a small solid matte cap with a hairline drop
    shadow, using whichever per-instance role colour (Mute/Solo/Record) the
    caller supplied via TextButton::buttonOnColourId.

    Scoped ONLY to MixerStrip's M/S/R/I row via per-component setLookAndFeel()
    — every other button in the app (Bypass, Routing chips, etc.) keeps its
    own distinct look untouched.
*/
class GhostButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GhostButtonLookAndFeel() = default;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    static constexpr float cornerRadius = 3.0f;
    inline static const juce::Colour trackBackground = CrateColors::LightBackground; // OFF fill — blends into the track background
    inline static const juce::Colour dimTextColour    = CrateColors::BrandGray;      // OFF text — barely there

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GhostButtonLookAndFeel)
};
