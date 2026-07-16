#include "PianoRollArticulationLane.h"
#include "TheCrateLookAndFeel.h"

using LAF = TheCrateLookAndFeel;
using namespace CratePianoRoll;

PianoRollArticulationLane::PianoRollArticulationLane()
{
}

PianoRollArticulationLane::~PianoRollArticulationLane() = default;

void PianoRollArticulationLane::setActiveClip (te::MidiClip* clip)
{
    activeMidiClip = clip;
    articulations.clear();
    repaint();
}

void PianoRollArticulationLane::setScrollOffset (int x, int y)
{
    if (horizontalOffset != x || verticalOffset != y)
    {
        horizontalOffset = x;
        verticalOffset = y;
        repaint();
    }
}

void PianoRollArticulationLane::setZoom (double ppb, double ppn)
{
    if (pixelsPerBeat != ppb || pixelsPerNote != ppn)
    {
        pixelsPerBeat = ppb;
        pixelsPerNote = ppn;
        repaint();
    }
}

double PianoRollArticulationLane::xToBeat (float screenX) const
{
    const float headerW = (float) headerWidth;
    const float contentX = screenX - headerW;
    return (contentX + horizontalOffset) / pixelsPerBeat;
}

juce::Colour PianoRollArticulationLane::getArticulationColour (int articulationID) const
{
    // Simple hue-based colour based on articulation ID (0-127 maps to hue range).
    const float hue = (float) articulationID / 127.0f;
    return juce::Colour::fromHSV (hue, 0.5f, 0.8f, 1.0f);
}

void PianoRollArticulationLane::paint (juce::Graphics& g)
{
    // Distinct background color (slightly different from expression lane above).
    g.setColour (juce::Colour (0xff1a1c20));
    g.fillAll();

    // Header area: dark background.
    g.setColour (LAF::panel);
    g.fillRect (0.0f, 0.0f, (float) headerWidth, (float) getHeight());

    // Header text: "Articulations".
    g.setColour (LAF::textDim);
    g.setFont (10.0f);
    g.drawText ("Articulations",
                juce::Rectangle<int> (4, 2, headerWidth - 8, 20),
                juce::Justification::centredLeft);

    if (activeMidiClip == nullptr)
        return;

    const float headerW = (float) headerWidth;
    const float contentW = (float) getWidth() - headerW;
    const float contentH = (float) getHeight();

    // Clip region: data canvas on the right.
    g.reduceClipRegion ((int) headerW, 0, (int) contentW, (int) contentH);

    g.setColour (LAF::panelLight);
    g.drawVerticalLine ((int) headerW, 0.0f, contentH); // divider

    // Draw articulation blocks.
    for (const auto& block : articulations)
    {
        const float blockStartX = (float) (block.startBeat * pixelsPerBeat) - (float) horizontalOffset + headerW;
        const float blockEndX = (float) (block.endBeat * pixelsPerBeat) - (float) horizontalOffset + headerW;
        const float blockWidth = blockEndX - blockStartX;

        if (blockWidth > 0.5f && blockStartX < getWidth() && blockEndX > headerW)
        {
            const juce::Rectangle<float> blockBounds { blockStartX, 6.0f, blockWidth, contentH - 12.0f };

            g.setColour (block.colour.withAlpha (0.6f));
            g.fillRect (blockBounds);

            g.setColour (block.colour);
            g.drawRect (blockBounds, 1.0f);

            // Draw ID label if space permits.
            if (blockWidth > 30.0f)
            {
                g.setColour (LAF::text);
                g.setFont (9.0f);
                g.drawText (juce::String (block.articulationID),
                           blockBounds.reduced (2.0f),
                           juce::Justification::centredLeft);
            }
        }
    }

    // Draw drag preview if currently drawing.
    if (isDrawing)
    {
        const float drawStartX = (float) (drawStartBeat * pixelsPerBeat) - (float) horizontalOffset + headerW;
        const float drawEndX = (float) (drawEndBeat * pixelsPerBeat) - (float) horizontalOffset + headerW;
        const float drawWidth = drawEndX - drawStartX;

        if (drawWidth != 0.0f)
        {
            const juce::Rectangle<float> previewBounds { juce::jmin (drawStartX, drawEndX), 6.0f,
                                                         std::abs (drawWidth), contentH - 12.0f };

            g.setColour (juce::Colours::white.withAlpha (0.2f));
            g.fillRect (previewBounds);

            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawRect (previewBounds, 1.0f);
        }
    }

    // Draw top border to separate from expression lane above.
    g.setColour (juce::Colours::black);
    g.drawLine (0.0f, 0.0f, (float) getWidth(), 0.0f, 2.0f);
}

void PianoRollArticulationLane::resized()
{
}

void PianoRollArticulationLane::mouseDown (const juce::MouseEvent& e)
{
    if (activeMidiClip == nullptr || e.position.x < headerWidth)
        return;

    isDrawing = true;
    drawStartBeat = xToBeat (e.position.x);
    drawEndBeat = drawStartBeat;
    repaint();
}

void PianoRollArticulationLane::mouseDrag (const juce::MouseEvent& e)
{
    if (!isDrawing || activeMidiClip == nullptr)
        return;

    drawEndBeat = xToBeat (e.position.x);
    repaint();
}

void PianoRollArticulationLane::mouseUp (const juce::MouseEvent& e)
{
    if (!isDrawing || activeMidiClip == nullptr)
    {
        isDrawing = false;
        repaint();
        return;
    }

    isDrawing = false;

    // Create articulation block if width > 0.
    const double minBeat = juce::jmin (drawStartBeat, drawEndBeat);
    const double maxBeat = juce::jmax (drawStartBeat, drawEndBeat);

    if (maxBeat - minBeat > 0.01)
    {
        ArticulationBlock newBlock;
        newBlock.startBeat = minBeat;
        newBlock.endBeat = maxBeat;
        newBlock.articulationID = 0; // Default keyswitch
        newBlock.colour = getArticulationColour (0);

        articulations.push_back (newBlock);
    }

    repaint();
}
