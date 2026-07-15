#include "PianoRollExpressionLane.h"
#include "TheCrateLookAndFeel.h"

using LAF = TheCrateLookAndFeel;
using namespace CratePianoRoll;

PianoRollExpressionLane::PianoRollExpressionLane()
{
    expressionTypeCombo.addItemList ({ "Velocity", "Pitch Bend", "Modulation" }, 1);
    expressionTypeCombo.setSelectedId (1); // Velocity
    expressionTypeCombo.setLookAndFeel (&lookAndFeel);
    expressionTypeCombo.onChange = [this]
    {
        if (expressionTypeCombo.getSelectedItemIndex() == 0)
            currentMode = Velocity;
        repaint();
    };
    addAndMakeVisible (expressionTypeCombo);
}

PianoRollExpressionLane::~PianoRollExpressionLane() = default;

void PianoRollExpressionLane::setActiveClip (te::MidiClip* clip)
{
    activeMidiClip = clip;
    repaint();
}

void PianoRollExpressionLane::setScrollOffset (int x, int y)
{
    if (horizontalOffset != x)
    {
        horizontalOffset = x;
        repaint();
    }
}

void PianoRollExpressionLane::setZoom (double ppb, double ppn)
{
    if (pixelsPerBeat != ppb || pixelsPerNote != ppn)
    {
        pixelsPerBeat = ppb;
        pixelsPerNote = ppn;
        repaint();
    }
}

double PianoRollExpressionLane::xToBeat (float screenX) const
{
    const float headerW = (float) keyboardWidth;
    const float contentX = screenX - headerW;
    return (contentX + horizontalOffset) / pixelsPerBeat;
}

juce::Colour PianoRollExpressionLane::velocityToColour (int velocity) const
{
    // Logic Pro gradient: low (green) -> mid (yellow) -> high (red).
    const float norm = velocity / 127.0f; // 0 to 1
    if (norm < 0.5f)
    {
        // Green to Yellow: [0, 0.5]
        const float t = norm * 2.0f; // 0 to 1 within this range
        return juce::Colour (
            (uint8) (255 * t),           // R: 0 -> 255
            255,                         // G: stays 255
            0                            // B: 0
        );
    }
    else
    {
        // Yellow to Red: [0.5, 1]
        const float t = (norm - 0.5f) * 2.0f; // 0 to 1 within this range
        return juce::Colour (
            255,                         // R: stays 255
            (uint8) (255 * (1.0f - t)),  // G: 255 -> 0
            0                            // B: 0
        );
    }
}

void PianoRollExpressionLane::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);

    if (activeMidiClip == nullptr || currentMode != Velocity)
        return;

    const auto w = (float) getWidth();
    const float headerW = (float) keyboardWidth;
    const float contentW = w - headerW;
    const float contentH = (float) getHeight();

    g.setColour (LAF::panelLight);
    g.drawVerticalLine ((int) headerW, 0.0f, contentH); // divider

    // Velocity stems + caps for each note.
    for (auto* note : activeMidiClip->getSequence().getNotes())
    {
        const float xCenter = (float) ((note->getStartBeat().inBeats() + note->getLengthBeats().inBeats() * 0.5) * pixelsPerBeat) - (float) horizontalOffset;

        // Clamp to visible content area.
        if (xCenter < 0.0f || xCenter >= contentW)
            continue;

        const float screenX = xCenter + headerW;
        const int velocity = note->getVelocity();
        const float stemHeight = (velocity / 127.0f) * (contentH - 4.0f);

        g.setColour (velocityToColour (velocity));

        // 2px stem.
        g.fillRect (screenX - 1.0f, contentH - stemHeight, 2.0f, stemHeight);

        // 5x5px cap.
        g.fillRect (screenX - 2.5f, contentH - stemHeight - 5.0f, 5.0f, 5.0f);
    }
}

void PianoRollExpressionLane::resized()
{
    // Fixed-size dropdown in header: 24px tall, 80px wide, centered vertically.
    constexpr int comboHeight = 24;
    constexpr int comboWidth = 80;
    const int comboY = (getHeight() - comboHeight) / 2;
    expressionTypeCombo.setBounds (4, comboY, comboWidth, comboHeight);
}

void PianoRollExpressionLane::mouseDown (const juce::MouseEvent& e)
{
    if (activeMidiClip == nullptr || currentMode != Velocity)
        return;

    // Brush scaffold: track start position, begin transaction.
    activeMidiClip->edit.getUndoManager().beginNewTransaction ("Draw Velocity");
    isBrushing = true;
    lastMouseX = e.position.x;
    lastMouseY = e.position.y;
}

void PianoRollExpressionLane::mouseDrag (const juce::MouseEvent& e)
{
    if (!isBrushing || activeMidiClip == nullptr || currentMode != Velocity)
        return;

    const float contentH = (float) getHeight();
    const float x0 = lastMouseX;
    const float x1 = e.position.x;
    const float y0 = lastMouseY;
    const float y1 = e.position.y;

    // Convert pixel X positions to beat space.
    const double minBeat = xToBeat (juce::jmin (x0, x1));
    const double maxBeat = xToBeat (juce::jmax (x0, x1));

    // Sweep: all notes with center beat in [minBeat, maxBeat] get interpolated Y -> velocity.
    for (auto* note : activeMidiClip->getSequence().getNotes())
    {
        const double noteCenterBeat = note->getStartBeat().inBeats() + (note->getLengthBeats().inBeats() * 0.5);

        if (noteCenterBeat >= minBeat && noteCenterBeat <= maxBeat)
        {
            // Linear interpolation factor in beat space.
            const double t = (maxBeat != minBeat) ? (noteCenterBeat - minBeat) / (maxBeat - minBeat) : 0.5;
            const float interpolatedY = (float) (y0 + t * (y1 - y0));

            // Y coordinate to velocity: contentH (bottom) = 0 vel, top = 127 vel.
            const float normalizedY = juce::jlimit (0.0f, contentH - 4.0f, contentH - interpolatedY);
            const int newVelocity = (int) ((normalizedY / (contentH - 4.0f)) * 127.0f);

            note->setVelocity (juce::jlimit (1, 127, newVelocity), &activeMidiClip->edit.getUndoManager());
        }
    }

    lastMouseX = e.position.x;
    lastMouseY = e.position.y;
    repaint();
}

void PianoRollExpressionLane::mouseUp (const juce::MouseEvent&)
{
    isBrushing = false;
    repaint();
}
