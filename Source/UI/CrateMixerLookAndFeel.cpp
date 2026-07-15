#include "CrateMixerLookAndFeel.h"

namespace
{
    struct DbTick { double db; const char* label; };
}

CrateMixerLookAndFeel::CrateMixerLookAndFeel() = default;

void CrateMixerLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPos, float minSliderPos, float maxSliderPos,
                                               juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        TheCrateLookAndFeel::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // Groove + tick marks occupy a fixed-width left column; tick TEXT labels only
    // get the remaining width to the right IF there's comfortably enough room —
    // narrow hosts (e.g. a cramped strip) still get the dash ticks and the fader
    // itself, just without labels, rather than clipped/overlapping text.
    constexpr float faderColumnWidth = 28.0f;
    const bool hasRoomForLabels = (float) width > 40.0f;

    const float centreX = (float) x + faderColumnWidth * 0.5f;

    // Deep, dark vertical groove.
    constexpr float grooveWidth = 4.0f;
    const juce::Rectangle<float> groove (centreX - grooveWidth * 0.5f, (float) y, grooveWidth, (float) height);
    g.setColour (juce::Colour (0xff08080a));
    g.fillRoundedRectangle (groove, 2.0f);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (groove, 2.0f, 1.0f);

    // Fixed dB reference ticks, independent of the slider's current value — always
    // at their proportional position along [getMinimum(), getMaximum()].
    const auto rangeMin = slider.getMinimum();
    const auto rangeMax = slider.getMaximum();
    const auto rangeSpan = rangeMax - rangeMin;

    auto yForDb = [&] (double db) -> float
    {
        const double clamped = juce::jlimit (rangeMin, rangeMax, db);
        const double proportion = rangeSpan > 0.0 ? (clamped - rangeMin) / rangeSpan : 0.5;
        return (float) y + (float) height * (float) (1.0 - proportion);
    };

    const DbTick ticks[] = { { 6.0, "+6" }, { 0.0, "0" }, { -6.0, "-6" }, { -18.0, "-18" }, { rangeMin, "-INF" } };

    g.setFont (juce::FontOptions (7.5f));

    for (auto& t : ticks)
    {
        const auto tickY = yForDb (t.db);

        g.setColour (juce::Colours::grey.withAlpha (0.55f));
        g.drawHorizontalLine ((int) tickY, centreX + grooveWidth * 0.5f + 1.0f, centreX + faderColumnWidth * 0.5f);

        if (hasRoomForLabels)
        {
            const auto labelX = centreX + faderColumnWidth * 0.5f + 2.0f;
            const auto labelWidth = juce::jmax (0.0f, (float) (x + width) - labelX);
            g.setColour (textDim);
            g.drawText (t.label, juce::Rectangle<float> (labelX, tickY - 5.0f, labelWidth, 10.0f),
                        juce::Justification::centredLeft);
        }
    }

    // Premium rectangular fader thumb: vertical gradient body + 3 grip ridges.
    constexpr float thumbW = 24.0f;
    constexpr float thumbH = 36.0f;
    const juce::Rectangle<float> thumb (centreX - thumbW * 0.5f, sliderPos - thumbH * 0.5f, thumbW, thumbH);

    juce::ColourGradient body (juce::Colour (0xff5a5a60), thumb.getX(), thumb.getY(),
                                juce::Colour (0xff2c2c30), thumb.getX(), thumb.getBottom(), false);
    g.setGradientFill (body);
    g.fillRoundedRectangle (thumb, 2.0f);

    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawRoundedRectangle (thumb, 2.0f, 1.0f);

    // 3 grip ridges across the middle — physical-fader affordance.
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    const float middleY = thumb.getCentreY();
    for (int i = -1; i <= 1; ++i)
        g.drawHorizontalLine ((int) (middleY + (float) i * 4.0f), thumb.getX() + 3.0f, thumb.getRight() - 3.0f);

    // Bright centre-line at the exact value position — same "precision line" cue
    // TheCrateLookAndFeel's flat sliders use elsewhere in the app.
    g.setColour (accent);
    g.fillRect (juce::Rectangle<float> (thumb.getX() + 2.0f, sliderPos - 1.0f, thumb.getWidth() - 4.0f, 2.0f));
}

void CrateMixerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                               juce::Slider& slider)
{
    // Fully self-contained bipolar rotary — used ONLY by MixerStrip's Pan knob in
    // this app (volumeFader is LinearVertical, handled by drawLinearSlider
    // above), so there's no volume-knob use case here that would want the
    // monotonic "fill accumulates from the minimum" behaviour TheCrateLookAndFeel's
    // own drawRotarySlider (still used elsewhere — Device Chain macro/XY-pad
    // knobs) draws. Deliberately does NOT delegate to that base implementation
    // at all, since its fill arc is the wrong shape for a bipolar control: it
    // always draws from rotaryStartAngle, which would fill Pan's ENTIRE left
    // half even when the value is only slightly negative.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto lineWidth = juce::jmax (2.0f, radius * 0.12f);
    const auto arcRadius = radius - lineWidth * 0.5f;

    // Centre = absolute top dead centre for a symmetric range (Pan -1..1 via
    // setRotaryParameters(1.2pi, 2.8pi, ...) has its midpoint at exactly 2pi,
    // i.e. straight up) — NOT derived from sliderPosProportional, so it never
    // moves regardless of the current value.
    const auto centreAngle = rotaryStartAngle + 0.5f * (rotaryEndAngle - rotaryStartAngle);
    const auto valueAngle  = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // Flat filled body — no gradient/bevel.
    g.setColour (panelLight);
    g.fillEllipse (centre.x - radius * 0.72f, centre.y - radius * 0.72f, radius * 1.44f, radius * 1.44f);

    // Background track (full sweep, both directions).
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (panel);
    g.strokePath (track, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // BIPOLAR fill: starts at the CENTRE (top dead centre / value = 0) and
    // sweeps toward whichever side the current value is on — left for
    // negative, right for positive. addCentredArc handles either direction
    // (centreAngle > valueAngle or vice versa) correctly on its own.
    juce::Path progress;
    progress.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, centreAngle, valueAngle, true);
    g.setColour (accent.withAlpha (slider.isEnabled() ? 1.0f : 0.35f));
    g.strokePath (progress, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Position indicator — short flat tick from centre outward, at the CURRENT
    // value's angle (not the fixed centre angle).
    const juce::Point<float> tip (centre.x + std::sin (valueAngle) * radius * 0.62f,
                                   centre.y - std::cos (valueAngle) * radius * 0.62f);
    g.setColour (slider.isMouseButtonDown() ? accent
                 : slider.isMouseOverOrDragging() ? text.brighter (0.2f)
                 : text);
    g.drawLine (centre.x, centre.y, tip.x, tip.y, lineWidth * 0.8f);

    // Static centre-detent tick — a clear visual anchor for value = 0,
    // independent of the live fill/position indicator above.
    const juce::Point<float> detentInner (centre.x + std::sin (centreAngle) * radius * 0.8f,
                                           centre.y - std::cos (centreAngle) * radius * 0.8f);
    const juce::Point<float> detentOuter (centre.x + std::sin (centreAngle) * radius * 0.98f,
                                           centre.y - std::cos (centreAngle) * radius * 0.98f);
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.drawLine (detentInner.x, detentInner.y, detentOuter.x, detentOuter.y, 1.5f);
}

void CrateMixerLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                           int, int, int, int, juce::ComboBox& box)
{
    const juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);

    g.setColour (panelLight);
    g.fillRoundedRectangle (bounds, (float) height * 0.3f);
    g.setColour (panel);
    g.drawRoundedRectangle (bounds.reduced (0.5f), (float) height * 0.3f, 1.0f);

    // Small flat downward triangle — not the OS/JUCE default combo glyph.
    const juce::Rectangle<float> arrowZone ((float) width - 18.0f, 0.0f, 14.0f, (float) height);
    juce::Path arrow;
    arrow.addTriangle (arrowZone.getX(), arrowZone.getCentreY() - 3.0f,
                        arrowZone.getRight(), arrowZone.getCentreY() - 3.0f,
                        arrowZone.getCentreX(), arrowZone.getCentreY() + 3.0f);

    g.setColour (box.isEnabled() ? textDim : textDim.withAlpha (0.4f));
    g.fillPath (arrow);
}
