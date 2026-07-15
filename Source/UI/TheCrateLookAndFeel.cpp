#include "TheCrateLookAndFeel.h"

TheCrateLookAndFeel::TheCrateLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId, background);

    setColour (juce::TextButton::buttonColourId, panelLight);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::TextButton::textColourOffId, text);

    setColour (juce::Slider::trackColourId, panelLight);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::Slider::backgroundColourId, panel);

    setColour (juce::Label::textColourId, text);

    setColour (juce::ComboBox::backgroundColourId, panelLight);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, panel);
    setColour (juce::ComboBox::arrowColourId, textDim);

    setColour (juce::ListBox::backgroundColourId, background);
    setColour (juce::TableListBox::backgroundColourId, background);
    setColour (juce::TableHeaderComponent::backgroundColourId, panel);
    setColour (juce::TableHeaderComponent::textColourId, text);

    setColour (juce::ScrollBar::thumbColourId, panelLight.brighter (0.3f));

    setColour (juce::AlertWindow::backgroundColourId, background);
    setColour (juce::AlertWindow::textColourId, text);

    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);

    setColour (juce::TextEditor::backgroundColourId, panel);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, panelLight);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
}

void TheCrateLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPos, float minSliderPos, float maxSliderPos,
                                             juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style == juce::Slider::LinearHorizontal)
    {
        // Ableton-style flat horizontal fader: thin track, accent-filled left
        // portion, plain rectangular thumb — no 3D bevel/knob.
        const auto trackHeight = 3.0f;
        const auto centreY = (float) y + (float) height * 0.5f;
        const juce::Rectangle<float> track ((float) x, centreY - trackHeight * 0.5f, (float) width, trackHeight);

        g.setColour (panel);
        g.fillRect (track);

        g.setColour (accent.withAlpha (slider.isEnabled() ? 0.7f : 0.3f));
        g.fillRect (juce::Rectangle<float> ((float) x, centreY - trackHeight * 0.5f, sliderPos - (float) x, trackHeight));

        const float thumbWidth = 6.0f;
        const juce::Rectangle<float> thumb (sliderPos - thumbWidth * 0.5f, (float) y + 1.0f,
                                             thumbWidth, (float) height - 2.0f);

        g.setColour (slider.isMouseButtonDown() ? accent
                     : slider.isMouseOverOrDragging() ? text.brighter (0.2f)
                     : text);
        g.fillRect (thumb);
        return;
    }

    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto trackWidth = 4.0f;
    const auto trackX = (float) x + (float) width * 0.5f - trackWidth * 0.5f;
    const juce::Rectangle<float> track (trackX, (float) y, trackWidth, (float) height);

    g.setColour (panel);
    g.fillRect (track);

    // Filled bar from the bottom of the track up to the current position — reads
    // as "how much signal," matching Ableton/FL fader convention.
    const auto filledTop = juce::jlimit ((float) y, (float) (y + height), sliderPos);
    g.setColour (accent.withAlpha (slider.isEnabled() ? 0.7f : 0.3f));
    g.fillRect (juce::Rectangle<float> (trackX, filledTop, trackWidth, (float) (y + height) - filledTop));

    const float thumbHeight = 8.0f;
    const juce::Rectangle<float> thumb ((float) x + 1.0f, sliderPos - thumbHeight * 0.5f,
                                         (float) width - 2.0f, thumbHeight);

    g.setColour (slider.isMouseButtonDown() ? accent
                 : slider.isMouseOverOrDragging() ? text.brighter (0.2f)
                 : text);
    g.fillRect (thumb);
}

void TheCrateLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                             juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto lineWidth = juce::jmax (2.0f, radius * 0.12f);
    const auto arcRadius = radius - lineWidth * 0.5f;
    const auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // Flat filled body — no gradient/bevel.
    g.setColour (panelLight);
    g.fillEllipse (centre.x - radius * 0.72f, centre.y - radius * 0.72f, radius * 1.44f, radius * 1.44f);

    // Background track (full sweep) then accent-filled progress arc on top —
    // same "how much signal" language as drawLinearSlider's fader fill.
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (panel);
    g.strokePath (track, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path progress;
    progress.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (accent.withAlpha (slider.isEnabled() ? 1.0f : 0.35f));
    g.strokePath (progress, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Position indicator — a short flat tick from centre outward, not a 3D pointer.
    const juce::Point<float> tip (centre.x + std::sin (angle) * radius * 0.62f,
                                   centre.y - std::cos (angle) * radius * 0.62f);
    g.setColour (slider.isMouseButtonDown() ? accent
                 : slider.isMouseOverOrDragging() ? text.brighter (0.2f)
                 : text);
    g.drawLine (centre.x, centre.y, tip.x, tip.y, lineWidth * 0.8f);
}

void TheCrateLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                                 bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    auto fill = backgroundColour;

    if (shouldDrawButtonAsDown)
        fill = accent;
    else if (shouldDrawButtonAsHighlighted)
        fill = backgroundColour.brighter (0.15f);

    if (! button.isEnabled())
        fill = fill.withMultipliedAlpha (0.4f);

    g.setColour (fill);
    g.fillRect (bounds);

    if (button.hasKeyboardFocus (true))
    {
        g.setColour (accent);
        g.drawRect (bounds, 1.0f);
    }
}

juce::Font TheCrateLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (15.0f, (float) buttonHeight * 0.55f));
}
