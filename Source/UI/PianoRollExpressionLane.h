#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "PianoRollLayout.h"

namespace te = tracktion::engine;

class PianoRollExpressionLane : public juce::Component
{
public:
    enum ExpressionMode { Velocity, ContinuousCC };

    PianoRollExpressionLane();
    ~PianoRollExpressionLane() override;

    void setActiveClip (te::MidiClip* clip);
    void setScrollOffset (int x, int y);
    void setZoom (double pixelsPerBeat, double pixelsPerNote);

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    // LEFT HEADER: ComboBox for expression type.
    juce::ComboBox expressionTypeCombo;

    ExpressionMode currentMode = Velocity;

    // State.
    te::MidiClip* activeMidiClip = nullptr;
    int horizontalOffset = 0;
    int verticalOffset = 0;
    double pixelsPerBeat = 60.0;
    double pixelsPerNote = 24.0;

    // Brush state.
    bool isBrushing = false;
    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;

    juce::Colour velocityToColour (int velocity) const;
    double xToBeat (float screenX) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollExpressionLane)
};
