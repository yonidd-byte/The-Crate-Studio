#include "CrateMidiInspectorComponent.h"
#include "TheCrateLookAndFeel.h"

using LAF = TheCrateLookAndFeel;

CrateMidiInspectorComponent::CrateMidiInspectorComponent()
{
    // TIME QUANTIZE.
    gridResolutionCombo.addItemList ({ "1/4", "1/8", "1/16", "1/32" }, 1);
    gridResolutionCombo.setSelectedId (3); // 1/16
    addAndMakeVisible (gridResolutionCombo);

    strengthSlider.setRange (0.0, 100.0);
    strengthSlider.setValue (50.0);
    strengthSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    addAndMakeVisible (strengthSlider);

    swingSlider.setRange (0.0, 100.0);
    swingSlider.setValue (50.0);
    swingSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    addAndMakeVisible (swingSlider);

    // SCALE QUANTIZE.
    rootNoteCombo.addItemList ({ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
    rootNoteCombo.setSelectedId (1); // C
    addAndMakeVisible (rootNoteCombo);

    scaleTypeCombo.addItemList ({ "Major", "Minor", "Pentatonic", "Blues", "Chromatic" }, 1);
    scaleTypeCombo.setSelectedId (1); // Major
    addAndMakeVisible (scaleTypeCombo);

    addAndMakeVisible (snapToScaleToggle);

    // COLLAPSE MODE.
    addAndMakeVisible (collapseEmptyToggle);

    // NOTE PROPERTIES.
    velocitySlider.setRange (1.0, 127.0);
    velocitySlider.setValue (100.0);
    velocitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    addAndMakeVisible (velocitySlider);

    lengthSlider.setRange (0.25, 16.0);
    lengthSlider.setValue (1.0);
    lengthSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    addAndMakeVisible (lengthSlider);

    // STAMP TOOL.
    addAndMakeVisible (stampModeToggle);

    chordTypeCombo.addItemList ({ "Triad", "Seventh", "Ninth", "Custom" }, 1);
    chordTypeCombo.setSelectedId (1); // Triad
    addAndMakeVisible (chordTypeCombo);
}

CrateMidiInspectorComponent::~CrateMidiInspectorComponent() = default;

void CrateMidiInspectorComponent::setActiveClip (te::MidiClip* clip)
{
    activeClip = clip;
    repaint();
}

void CrateMidiInspectorComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);

    if (activeClip == nullptr)
    {
        g.setColour (LAF::textDim);
        g.setFont (11.0f);
        g.drawText ("Open clip to edit", getLocalBounds(), juce::Justification::centred);
    }
}

void CrateMidiInspectorComponent::resized()
{
    auto area = getLocalBounds().reduced (6);
    constexpr int controlHeight = 20;
    constexpr int spacing = 6;

    // TIME QUANTIZE.
    area.removeFromTop (12); // label
    gridResolutionCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    strengthSlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    swingSlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing * 2);

    // SCALE QUANTIZE.
    area.removeFromTop (12); // label
    rootNoteCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    scaleTypeCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    snapToScaleToggle.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing * 2);

    // COLLAPSE MODE.
    collapseEmptyToggle.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing * 2);

    // NOTE PROPERTIES.
    area.removeFromTop (12); // label
    velocitySlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    lengthSlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing * 2);

    // STAMP TOOL.
    stampModeToggle.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    chordTypeCombo.setBounds (area.removeFromTop (controlHeight));
}
