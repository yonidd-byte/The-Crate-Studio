#pragma once

#include <JuceHeader.h>
#include "TheCrateLookAndFeel.h"

/**
    The Pop-out Compressor — Logic-style: channel strips show a single
    "Channel Comp" toggle instead of permanent Threshold/Ratio rotaries, and
    clicking it spawns this lightweight component inside a juce::CallOutBox
    (see InspectorStrip's channelCompButton.onClick / MixerStrip's equivalent).

    Visual scaffold only for this pass (MASTER_ARCHITECTURE.md Zone 5): 5 real
    rotaries (Threshold, Ratio, Attack, Release, Makeup) with a shared look/feel,
    not yet bound to a te::CompressorPlugin — a later DSP pass wires them.
*/
class CrateCompressorPopup : public juce::Component
{
public:
    explicit CrateCompressorPopup (juce::LookAndFeel* sharedLaf)
    {
        setSize (280, 100);

        setupKnob (thresholdKnob, thresholdLabel, "Threshold", sharedLaf, -60.0, 0.0, -18.0);
        setupKnob (ratioKnob,     ratioLabel,     "Ratio",     sharedLaf,   1.0, 20.0,   4.0);
        setupKnob (attackKnob,    attackLabel,    "Attack",    sharedLaf,   0.1, 200.0, 10.0);
        setupKnob (releaseKnob,   releaseLabel,   "Release",   sharedLaf,  10.0, 2000.0, 150.0);
        setupKnob (makeupKnob,    makeupLabel,    "Makeup",    sharedLaf,   0.0, 24.0,   0.0);
    }

    ~CrateCompressorPopup() override
    {
        thresholdKnob.setLookAndFeel (nullptr);
        ratioKnob.setLookAndFeel (nullptr);
        attackKnob.setLookAndFeel (nullptr);
        releaseKnob.setLookAndFeel (nullptr);
        makeupKnob.setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        using LAF = TheCrateLookAndFeel;
        g.fillAll (LAF::panel);

        g.setColour (LAF::textDim);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("CHANNEL COMPRESSOR", getLocalBounds().removeFromTop (16),
                    juce::Justification::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        area.removeFromTop (16); // title

        const int knobWidth = area.getWidth() / 5;

        auto layoutOne = [&] (juce::Slider& knob, juce::Label& label)
        {
            auto col = area.removeFromLeft (knobWidth);
            label.setBounds (col.removeFromBottom (14));
            knob.setBounds (col.reduced (4));
        };

        layoutOne (thresholdKnob, thresholdLabel);
        layoutOne (ratioKnob,     ratioLabel);
        layoutOne (attackKnob,    attackLabel);
        layoutOne (releaseKnob,   releaseLabel);
        layoutOne (makeupKnob,    makeupLabel);
    }

private:
    void setupKnob (juce::Slider& knob, juce::Label& label, const juce::String& caption,
                    juce::LookAndFeel* laf, double min, double max, double def)
    {
        using LAF = TheCrateLookAndFeel;

        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRange (min, max, 0.01);
        knob.setValue (def, juce::dontSendNotification);
        knob.setDoubleClickReturnValue (true, def);
        knob.setLookAndFeel (laf);
        addAndMakeVisible (knob);

        label.setText (caption, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, LAF::textDim);
        label.setFont (juce::FontOptions (9.0f));
        addAndMakeVisible (label);
    }

    juce::Slider thresholdKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider ratioKnob     { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider attackKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider releaseKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider makeupKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label thresholdLabel, ratioLabel, attackLabel, releaseLabel, makeupLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateCompressorPopup)
};
