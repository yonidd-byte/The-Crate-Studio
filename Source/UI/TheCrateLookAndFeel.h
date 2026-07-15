#pragma once

#include <JuceHeader.h>

/**
    Global dark/flat DAW theme. Charcoal backgrounds, cyan accent, solid fills —
    no default JUCE gradients, bevels, or 3D knobs. Faders draw as flat vertical
    track bars (Ableton/FL style), buttons as flat rectangles.
*/
class TheCrateLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TheCrateLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPos, float minSliderPos, float maxSliderPos,
                            juce::Slider::SliderStyle, juce::Slider&) override;

    // Rotary knobs (MixerStrip's pan knob, Device Chain macro knobs) were falling
    // through to LookAndFeel_V4's default beveled/gradient rotary — contradicting
    // this class's own "no default JUCE gradients, bevels, or 3D knobs" mandate
    // above. Flat filled circle + accent arc, matching drawLinearSlider's language.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                            juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // Shared palette — reused directly by components that paint their own
    // backgrounds/meters (MixerComponent etc.) so everything stays in sync.
    inline static const juce::Colour background { 0xff121214 };
    inline static const juce::Colour panel      { 0xff1a1a1e };
    inline static const juce::Colour panelLight { 0xff232328 };
    inline static const juce::Colour accent     { 0xff00d9ff };
    inline static const juce::Colour text       { 0xffe8e8ea };
    inline static const juce::Colour textDim    { 0xff8a8a90 };

    // Matte pastel for clip blocks (Ableton-style) — desaturated, not a glaring
    // primary. Clip name labels should use textOnClip (dark), not the main `text`
    // colour, for contrast against the lighter pastel fill.
    inline static const juce::Colour clip       { 0xff6fb3c2 };
    inline static const juce::Colour textOnClip { 0xff10181a };

    // Transport LCD block + vital-stats cluster (Zone 1) — shared so any other
    // component that ever needs a "digital readout" look matches exactly.
    inline static const juce::Colour lcdBackground { 0xff0a0a0c };
    inline static const juce::Colour lcdText       { 0xffffb000 }; // amber digital digits
    inline static const juce::Colour ledOff        { 0xff3a3a3e };
    inline static const juce::Colour ledOn         { 0xff2fe08a }; // MIDI activity green
    inline static const juce::Colour meterHot      { 0xffff3b30 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheCrateLookAndFeel)
};
