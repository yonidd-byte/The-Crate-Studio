#pragma once

#include <JuceHeader.h>

/**
    Shared Column 3 "value box" family — Ableton-style flat 13px drag boxes,
    NOT juce::Slider (see CrateVolumeBar's own doc comment for why). Originally
    private nested classes of TrackHeaderComponent (VolumeBar/PanBar); pulled
    out here, unchanged, so ArrangementComponent's MasterHeaderRow can reach
    100% visual + interaction parity with a standard track's Column 3 without
    a second hand-maintained copy of this paint()/mouseDrag() logic drifting
    out of sync with the original.
*/
class CrateVolumeBar : public juce::Component
{
public:
    void setRange (double newMin, double newMax)   { rangeMin = newMin; rangeMax = newMax; }
    void setValue (double newValue, juce::NotificationType);
    double getValue() const                         { return value; }

    std::function<void()> onDragStart;
    std::function<void()> onValueChange;
    std::function<void()> onDragEnd;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override; // resets to 0 dB (unity)

private:
    double rangeMin = -60.0, rangeMax = 6.0;
    double value = 0.0;
    double valueOnDragStart = 0.0;
};

// Column 3's Pan control — a flat horizontal-LOOKING drag-box (Ableton
// Geometry / PNG Pivot directive: a rotary knob breaks the strict 13px
// row-height grid), but driven by VERTICAL mouse drag (Vertical Drag Axis
// directive — Track Headers sit against the screen's right edge, so a
// horizontal drag metaphor runs the mouse off the monitor almost
// immediately). Same plain-Component drag mechanics as CrateVolumeBar above
// (bipolar -1..1 range, "C" / "50L" / "50R" readout instead of a dB string).
class CratePanBar : public juce::Component
{
public:
    void setValue (double newValue, juce::NotificationType);
    double getValue() const                         { return value; }

    std::function<void()> onDragStart;
    std::function<void()> onValueChange;
    std::function<void()> onDragEnd;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override; // resets to centre ("C")

private:
    double value = 0.0; // -1 (full left) .. +1 (full right), 0 == centre
    double valueOnDragStart = 0.0;
};
