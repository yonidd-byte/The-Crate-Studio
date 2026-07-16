#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "PianoRollLayout.h"

namespace te = tracktion::engine;

class PianoRollArticulationLane : public juce::Component
{
public:
    PianoRollArticulationLane();
    ~PianoRollArticulationLane() override;

    void setActiveClip (te::MidiClip* clip);
    void setScrollOffset (int x, int y);
    void setZoom (double pixelsPerBeat, double pixelsPerNote);
    void setKeyboardWidth (int width) noexcept { headerWidth = width; }

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    struct ArticulationBlock
    {
        double startBeat = 0.0;
        double endBeat = 1.0;
        int articulationID = 0; // keyswitch ID (0-127)
        juce::Colour colour;
    };

    te::MidiClip* activeMidiClip = nullptr;
    int horizontalOffset = 0;
    int verticalOffset = 0;
    int headerWidth = 100;
    double pixelsPerBeat = 60.0;
    double pixelsPerNote = 24.0;

    std::vector<ArticulationBlock> articulations;

    // Drawing state.
    bool isDrawing = false;
    double drawStartBeat = 0.0;
    double drawEndBeat = 0.0;

    double xToBeat (float screenX) const;
    juce::Colour getArticulationColour (int articulationID) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollArticulationLane)
};
