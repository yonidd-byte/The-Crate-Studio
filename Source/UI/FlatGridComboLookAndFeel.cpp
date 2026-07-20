#include "FlatGridComboLookAndFeel.h"

void FlatGridComboLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                              int, int, int, int, juce::ComboBox&)
{
    // Crisp Typography directive: integer-aligned rects only, not
    // float-plus-0.5-reduce — at a strict 13px row height, sub-pixel
    // coordinates read as visibly blurred/antialiased fuzz on the fill and
    // border both.
    const juce::Rectangle<int> bounds (0, 0, width, height);

    // Flat solid block — no rounded corners, no gradient, no bevel.
    g.setColour (backgroundColour);
    g.fillRect (bounds);

    // Crisp 1px border, drawn INSIDE the bounds so it never gets clipped at
    // the component edge.
    g.setColour (borderColour);
    g.drawRect (bounds, 1);

    // Minimalist 'V' path — replaces the native JUCE dropdown arrow entirely.
    // The path itself needs float vertices (diagonal strokes), but its
    // bounding box is rounded to the nearest whole pixel first so it doesn't
    // drift onto a fractional row position inherited from the combo's own
    // (integer) bounds.
    constexpr float arrowW = 8.0f, arrowH = 4.0f, rightMargin = 8.0f;
    const auto arrowBounds = juce::Rectangle<float> ((float) width - rightMargin - arrowW,
                                                      ((float) height - arrowH) * 0.5f,
                                                      arrowW, arrowH).toNearestInt().toFloat();

    juce::Path v;
    v.startNewSubPath (arrowBounds.getX(), arrowBounds.getY());
    v.lineTo (arrowBounds.getCentreX(), arrowBounds.getBottom());
    v.lineTo (arrowBounds.getRight(), arrowBounds.getY());

    g.setColour (arrowColour);
    g.strokePath (v, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void FlatGridComboLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    // Same flat-block language as drawComboBox — the popup reads as part of
    // the same console routing screen, not a native OS menu.
    g.setColour (backgroundColour);
    g.fillRect (0, 0, width, height);

    g.setColour (borderColour);
    g.drawRect (0, 0, width, height, 1);
}

void FlatGridComboLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (2, 1, juce::jmax (0, box.getWidth() - arrowReserve - 2), box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

juce::Font FlatGridComboLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return juce::FontOptions (11.0f);
}

void FlatGridComboLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                                           int standardMenuItemHeight, int& idealWidth, int& idealHeight)
{
    // Shrinking Popup Menus directive: get a reasonable idealWidth from the
    // base implementation, but standardMenuItemHeight it was FED is derived
    // from this combo's own 13px height — floor idealHeight at
    // popupMenuItemHeight so the dropdown is never shrunk to match the combo.
    juce::LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
    idealHeight = juce::jmax (idealHeight, popupMenuItemHeight);
}

juce::Font FlatGridComboLookAndFeel::getPopupMenuFont()
{
    // Detached entirely from the 13px ComboBox constraint — the popup reads
    // at a normal, comfortable size regardless of how small the box that
    // opened it is.
    return juce::FontOptions (popupMenuFontSize);
}
