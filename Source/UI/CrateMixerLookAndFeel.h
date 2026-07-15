#pragma once

#include <JuceHeader.h>

#include "TheCrateLookAndFeel.h"

/**
    Pro Tools / Logic Pro-grade console chrome for the Hybrid Mixer specifically
    (MASTER_ARCHITECTURE.md section 9). NOT the app-wide default LookAndFeel —
    scoped narrowly to MixerStrip's own fader/pan-knob/routing-combo controls via
    per-component setLookAndFeel(), so every other flat/minimal control in the app
    (transport icons, track header toggle blocks, browser search bar) keeps using
    TheCrateLookAndFeel untouched. Inherits from it rather than LookAndFeel_V4
    directly so button/label/popup-menu colours the mixer ALSO uses (e.g. the
    output-routing ComboBox's popup list) stay in the same palette.
*/
class CrateMixerLookAndFeel : public TheCrateLookAndFeel
{
public:
    CrateMixerLookAndFeel();

    // LinearVertical: full premium fader — deep dark groove, fixed dB tick marks,
    // and a rectangular gradient thumb with grip ridges. Every other style
    // (LinearHorizontal — the send-level mini sliders — and anything else)
    // delegates straight to TheCrateLookAndFeel's existing flat-bar rendering.
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPos, float minSliderPos, float maxSliderPos,
                            juce::Slider::SliderStyle, juce::Slider&) override;

    // Delegates to TheCrateLookAndFeel's flat rotary arc, then adds one static
    // "center-detent" tick at the knob's mid-range angle (e.g. Pan = 0), so a
    // centred value has an obvious visual anchor to snap back to.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                            juce::Slider&) override;

    // Flat pill-shaped dropdown — no default OS/JUCE bevelled rectangle border.
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                        int buttonX, int buttonY, int buttonW, int buttonH,
                        juce::ComboBox&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateMixerLookAndFeel)
};
