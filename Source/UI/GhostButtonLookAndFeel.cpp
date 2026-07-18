#include "GhostButtonLookAndFeel.h"

void GhostButtonLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                                    bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat();

    if (button.getToggleState())
    {
        // ON — a small solid matte cap with a 1px drop shadow so it physically
        // pops off the panel. backgroundColour arrives already resolved to
        // buttonOnColourId (the per-instance role colour Mute/Solo/Record each
        // set via setColour()).
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (bounds.translated (0.0f, 1.0f), cornerRadius);

        auto fill = backgroundColour;
        if (shouldDrawButtonAsDown)             fill = fill.brighter (0.1f);
        else if (shouldDrawButtonAsHighlighted) fill = fill.brighter (0.06f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, cornerRadius);
    }
    else
    {
        // OFF — flush with the track's own background, not a visibly distinct
        // chip at all; only a faint highlight on hover/press hints there's a
        // real button here.
        auto fill = trackBackground;
        if (shouldDrawButtonAsDown)             fill = fill.brighter (0.12f);
        else if (shouldDrawButtonAsHighlighted) fill = fill.brighter (0.06f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, cornerRadius);
    }
}

void GhostButtonLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                              bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/)
{
    g.setColour (button.getToggleState() ? juce::Colours::white : dimTextColour);
    g.setFont (juce::FontOptions (juce::jmin (12.0f, (float) button.getHeight() * 0.6f), juce::Font::bold));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}
