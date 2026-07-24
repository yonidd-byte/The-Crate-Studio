#pragma once

#include <JuceHeader.h>

#include "CrateTheme.h"
#include "CrateDesignSystem.h"

/**
    Strict I/O Grid directive — a flat, "data table" ComboBox chrome for
    TrackHeaderComponent's Column 2 routing dropdowns (Input/Output Category +
    Specific). No native JUCE rounded corners, gradients, or drop shadow: a
    solid flat fill, a crisp 1px border, and a minimalist 'V' path replacing
    the default dropdown arrow.

    Scoped ONLY to Column 2's four combos via per-component setLookAndFeel()
    (see TrackHeaderComponent's constructor) — every other combo box in the
    app keeps whatever LookAndFeel it already had.
*/
class FlatGridComboLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FlatGridComboLookAndFeel() = default;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                        int buttonX, int buttonY, int buttonW, int buttonH,
                        juce::ComboBox&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;

    // Shrinking Popup Menus directive: the combo itself is a strict 13px
    // row now, but its DROPDOWN must stay fully readable — LookAndFeel_V4's
    // default getIdealPopupMenuItemSize() derives standardMenuItemHeight
    // from the owning ComboBox's own (tiny) height, which was silently
    // shrinking every popup row to match. idealHeight is floored at
    // popupMenuItemHeight regardless of what the combo itself is sized to.
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                     int& idealWidth, int& idealHeight) override;
    juce::Font getPopupMenuFont() override;

    static constexpr int popupMenuItemHeight = 22;
    static constexpr float popupMenuFontSize = 13.0f;

    // Space reserved on the right for the 'V' arrow (see drawComboBox) — used
    // by positionComboBoxText() too, so the label text can never overlap it.
    static constexpr int arrowReserve = 20;

    // Inner Element Strokes directive: DarkBackground fill reads as a sunken
    // LCD cutout against the Track Header's LightBackground panel; the
    // border is a crisp 1px SOLID black line — not a soft grey, not a
    // low-alpha hairline — so every ComboBox reads as its own sharply
    // defined box in the strict Ableton-style grid.
    inline static const juce::Colour backgroundColour = CrateColors::DarkBackground;
    inline static const juce::Colour borderColour      = juce::Colours::black;
    inline static const juce::Colour arrowColour       = CrateColors::NeonBlue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlatGridComboLookAndFeel)
};
