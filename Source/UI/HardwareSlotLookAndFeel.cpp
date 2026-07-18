#include "HardwareSlotLookAndFeel.h"

// Shared "raised hardware button" drawing — a soft drop shadow UNDER the
// button, the matte fill, then a 1px HIGHLIGHT along the top edge and a
// 1px SHADOW along the bottom edge. Highlight-top/shadow-bottom is what
// reads as a RAISED physical cap catching light from above; the previous
// shadow-top/highlight-bottom combination was backwards and read as a
// sunken/recessed hole instead. Public (declared in the header) so plain
// juce::Label chips that can't route through this LookAndFeel (RoutingBlock's
// "No Group" slot, MasterStrip's "Stereo Out" slot) draw the IDENTICAL bevel
// by calling this directly, instead of a second copy that can drift.
void HardwareSlotLookAndFeel::drawRaisedChip (juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour fill)
{
    // Soft drop shadow, offset down — lifts the cap off the panel.
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (bounds.translated (0.0f, 1.5f), cornerRadius);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, cornerRadius);

    g.setColour (highlightColour);
    g.drawLine (bounds.getX() + cornerRadius, bounds.getY() + 0.5f,
                bounds.getRight() - cornerRadius, bounds.getY() + 0.5f, 1.0f);

    g.setColour (shadowColour);
    g.drawLine (bounds.getX() + cornerRadius, bounds.getBottom() - 0.5f,
                bounds.getRight() - cornerRadius, bounds.getBottom() - 0.5f, 1.0f);
}

void HardwareSlotLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                                             int, int, int, int, juce::ComboBox&)
{
    const juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
    drawRaisedChip (g, bounds, isButtonDown ? fillColour.brighter (0.08f) : fillColour);
}

void HardwareSlotLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                     bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat();

    // A genuinely toggled-ON button (e.g. Channel Comp while its popup is
    // open) still gets the accent fill it always has — this LookAndFeel adds
    // the bevel/typography language on top, it doesn't remove real state cues.
    juce::Colour fill;

    if (button.getToggleState())
        fill = CrateColors::NeonBlue.withAlpha (shouldDrawButtonAsDown ? 1.0f : 0.85f);
    else
        fill = shouldDrawButtonAsDown       ? fillColour.brighter (0.08f)
             : shouldDrawButtonAsHighlighted ? fillColour.brighter (0.04f)
                                             : fillColour;

    drawRaisedChip (g, bounds, fill);
}

void HardwareSlotLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                               bool /*shouldDrawButtonAsHighlighted*/, bool shouldDrawButtonAsDown)
{
    // Dim normally (matches the fader's own major-tick grey, #A0A0A5 — the
    // same "quiet console engraving" language elsewhere in this LookAndFeel),
    // bright white while actually pressed OR genuinely toggled on. Centred
    // both horizontally and vertically — same typography/weight/colour as
    // every other hardware-slot chip (OUT 1+2's combo font matches via
    // getComboBoxFont() below, both bold, both this exact size family).
    g.setColour ((shouldDrawButtonAsDown || button.getToggleState()) ? juce::Colours::white : dimTextColour);
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

juce::Font HardwareSlotLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return juce::FontOptions (11.0f, juce::Font::bold);
}

void HardwareSlotLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (1, 1, box.getWidth() - 2, box.getHeight() - 2); // full chip, no arrow dead-zone
    label.setFont (getComboBoxFont (box));
}

juce::Font HardwareSlotLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (12.0f, (float) buttonHeight * 0.6f), juce::Font::bold);
}

void HardwareSlotLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                              bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                              bool isMouseOver, bool isMouseDown)
{
    // No track/background fill at all — deliberately absent, per spec.
    if (thumbSize <= 0)
        return;

    // Auto-hide: fully transparent unless the scrollbar itself is hovered/
    // dragged (isMouseOver/isMouseDown, which JUCE already tracks and repaints
    // on), OR the parent Viewport is hovered anywhere (rows, knobs, caption —
    // SendsSection forwards those nested mouse-enter/exit events and repaints
    // itself, see the addMouseListener(this, true) wiring in its ctor).
    const auto* parent = scrollbar.getParentComponent();
    const bool parentHovering = parent != nullptr && parent->isMouseOver (true);

    if (! (isMouseOver || isMouseDown || parentHovering))
        return;

    constexpr float thumbThickness = 4.0f;
    juce::Rectangle<float> thumb;

    if (isScrollbarVertical)
        thumb = { (float) x + ((float) width - thumbThickness) * 0.5f, (float) thumbStartPosition,
                  thumbThickness, (float) thumbSize };
    else
        thumb = { (float) thumbStartPosition, (float) y + ((float) height - thumbThickness) * 0.5f,
                  (float) thumbSize, thumbThickness };

    g.setColour (CrateColors::BrandGray);
    g.fillRoundedRectangle (thumb, thumbThickness * 0.5f);
}

void HardwareSlotLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                 float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                                 juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (1.0f);
    const auto outerRadius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // NOTE: unreachable today — CrateSendSlot's mini-knob now uses
    // CrateMixerLookAndFeel's pan_knob.png image asset instead (see the
    // Revert Send Knobs directive), so nothing currently assigns this
    // LookAndFeel to a juce::Slider. Left in place (not this round's scope to
    // delete) but its colours are still flattened to the strict 4-colour
    // hierarchy for whenever it's reactivated: full dark backing disc
    // (DarkBackground) fills the whole footprint — this IS the "off" state of
    // the ring, so there's no separate track-arc colour to draw. The LED ring
    // (NeonBlue) strokes only the ACTIVE proportion near the outer rim,
    // deliberately floating a few px clear of the knob cap (LightBackground)
    // so the ring reads as a separate halo rather than fused to the knob
    // body. A pointer "pin" (BrandGray) rotates with the value for an
    // at-a-glance position readout, the classic hardware-knob cue.
    g.setColour (CrateColors::DarkBackground);
    g.fillEllipse (centre.x - outerRadius, centre.y - outerRadius, outerRadius * 2.0f, outerRadius * 2.0f);

    const auto ringLineWidth = juce::jmax (1.5f, outerRadius * 0.16f);
    const auto ringRadius = outerRadius - ringLineWidth * 0.6f;
    juce::Path ring;
    ring.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (CrateColors::NeonBlue.withAlpha (slider.isEnabled() ? 1.0f : 0.35f));
    g.strokePath (ring, juce::PathStrokeType (ringLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto knobRadius = outerRadius * 0.55f; // smaller than ringRadius on purpose — leaves the visible gap
    g.setColour (CrateColors::LightBackground);
    g.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);

    const auto pinLength = knobRadius * 0.8f;
    const auto pinX = centre.x + pinLength * std::sin (angle);
    const auto pinY = centre.y - pinLength * std::cos (angle);
    g.setColour (CrateColors::BrandGray);
    g.drawLine (centre.x, centre.y, pinX, pinY, juce::jmax (1.5f, knobRadius * 0.12f));
}
