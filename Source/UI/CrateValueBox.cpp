#include "CrateValueBox.h"

#include "CrateTheme.h"
#include "CrateDesignSystem.h"

namespace
{
    namespace DS = CrateDesignSystem::Metrics::TrackHeader;
}

void CrateVolumeBar::setValue (double newValue, juce::NotificationType nt)
{
    const auto clamped = juce::jlimit (rangeMin, rangeMax, newValue);

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (nt == juce::sendNotification && onValueChange)
        onValueChange();
}

void CrateVolumeBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    constexpr float corner = DS::volumeBarCornerRadius;

    // Flat dark well.
    g.setColour (CrateColors::DarkBackground);
    g.fillRoundedRectangle (b, corner);

    // NeonBlue fill growing left -> right with the value — plain linear
    // proportion (no juce::Slider skew to account for; this class has none).
    const auto prop = (rangeMax > rangeMin)
                         ? juce::jlimit (0.0, 1.0, (value - rangeMin) / (rangeMax - rangeMin))
                         : 0.0;
    if (prop > 0.0)
    {
        juce::Graphics::ScopedSaveState clip (g);
        juce::Path well;
        well.addRoundedRectangle (b, corner);
        g.reduceClipRegion (well);

        auto fill = b.withWidth (b.getWidth() * (float) prop);
        g.setColour (CrateColors::NeonBlue.withAlpha (DS::volumeBarFillAlpha));
        g.fillRect (fill);
    }

    // Inner Element Strokes directive: crisp 1px solid black rim, same
    // convention every other Column 3 box now uses.
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (b.reduced (0.5f), corner, 1.0f);

    // Centred dB readout — white over the fill, still legible over the dark well.
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (CrateDesignSystem::Typography::volumeBarFontSize, juce::Font::bold));
    g.drawText (juce::String (value, 1) + " dB", getLocalBounds(), juce::Justification::centred);
}

void CrateVolumeBar::mouseDown (const juce::MouseEvent&)
{
    valueOnDragStart = value;

    if (onDragStart)
        onDragStart();
}

void CrateVolumeBar::mouseDrag (const juce::MouseEvent& e)
{
    // Vertical Drag Axis directive: Track Headers sit against the screen's
    // right edge — a horizontal drag metaphor runs the mouse straight off
    // the monitor after a few pixels. Ableton solves this by driving every
    // value box off VERTICAL mouse movement regardless of its flat
    // horizontal shape, so this reads off getDistanceFromDragStartY()
    // instead of X now (negated: dragging UP is a negative Y delta, and
    // that must INCREASE the value). Shift held = fine-tune (Sensitivity &
    // Feel directive) — same pixel travel now covers a quarter of the range.
    constexpr float pixelsForFullRange = DS::volumeBarDragPixelsForFullRange;
    const float sensitivity = e.mods.isShiftDown() ? 4.0f : 1.0f;
    const double delta = (double) -e.getDistanceFromDragStartY() / (double) (pixelsForFullRange * sensitivity) * (rangeMax - rangeMin);
    setValue (valueOnDragStart + delta, juce::sendNotification);
}

void CrateVolumeBar::mouseUp (const juce::MouseEvent&)
{
    if (onDragEnd)
        onDragEnd();
}

void CrateVolumeBar::mouseDoubleClick (const juce::MouseEvent&)
{
    setValue (0.0, juce::sendNotification); // reset to unity gain (0 dB)
}

void CratePanBar::setValue (double newValue, juce::NotificationType nt)
{
    const auto clamped = juce::jlimit (-1.0, 1.0, newValue);

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (nt == juce::sendNotification && onValueChange)
        onValueChange();
}

void CratePanBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    constexpr float corner = DS::volumeBarCornerRadius;

    // Flat dark well — same language as CrateVolumeBar.
    g.setColour (CrateColors::DarkBackground);
    g.fillRoundedRectangle (b, corner);

    // NeonBlue fill growing from CENTRE outward toward whichever side the
    // value leans — the correct bipolar analogue of CrateVolumeBar's
    // left->right unipolar fill.
    const auto centreX = b.getCentreX();
    const auto halfWidth = b.getWidth() * 0.5f;
    const auto offset = (float) value * halfWidth;

    if (std::abs (offset) > 0.5f)
    {
        juce::Graphics::ScopedSaveState clip (g);
        juce::Path well;
        well.addRoundedRectangle (b, corner);
        g.reduceClipRegion (well);

        const auto fill = (offset > 0.0f)
                              ? juce::Rectangle<float> (centreX, b.getY(), offset, b.getHeight())
                              : juce::Rectangle<float> (centreX + offset, b.getY(), -offset, b.getHeight());
        g.setColour (CrateColors::NeonBlue.withAlpha (DS::volumeBarFillAlpha));
        g.fillRect (fill);
    }

    // Inner Element Strokes directive: crisp 1px solid black rim, same
    // convention every other Column 3 box now uses.
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (b.reduced (0.5f), corner, 1.0f);

    // Centred readout — "C" dead centre, otherwise the percentage toward
    // whichever side ("50L" / "50R" — PNG Pivot directive's own format).
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (CrateDesignSystem::Typography::volumeBarFontSize, juce::Font::bold));

    const int percent = juce::roundToInt (std::abs (value) * 100.0);
    const auto text = percent == 0 ? juce::String ("C") : (juce::String (percent) + (value < 0.0 ? "L" : "R"));
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
}

void CratePanBar::mouseDown (const juce::MouseEvent&)
{
    valueOnDragStart = value;

    if (onDragStart)
        onDragStart();
}

void CratePanBar::mouseDrag (const juce::MouseEvent& e)
{
    // Vertical Drag Axis directive: same getDistanceFromDragStartY() switch
    // as CrateVolumeBar (see its own doc comment) — dragging UP pans right
    // (increases value), DOWN pans left, full drag distance sweeps the full
    // bipolar range (2.0, i.e. -1..1). Shift held = fine-tune.
    constexpr float pixelsForFullRange = DS::volumeBarDragPixelsForFullRange;
    const float sensitivity = e.mods.isShiftDown() ? 4.0f : 1.0f;
    const double delta = (double) -e.getDistanceFromDragStartY() / (double) (pixelsForFullRange * sensitivity) * 2.0;
    setValue (valueOnDragStart + delta, juce::sendNotification);
}

void CratePanBar::mouseUp (const juce::MouseEvent&)
{
    if (onDragEnd)
        onDragEnd();
}

void CratePanBar::mouseDoubleClick (const juce::MouseEvent&)
{
    setValue (0.0, juce::sendNotification); // reset to centre ("C")
}
