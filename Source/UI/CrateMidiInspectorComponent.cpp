#include "CrateMidiInspectorComponent.h"
#include "TheCrateLookAndFeel.h"

using LAF = TheCrateLookAndFeel;

CrateMidiInspectorComponent::CrateMidiInspectorComponent()
{
    setLookAndFeel (&lookAndFeel);

    // TIME QUANTIZE.
    gridResolutionCombo.addItemList ({ "1/4", "1/8", "1/16", "1/32" }, 1);
    gridResolutionCombo.setSelectedId (3); // 1/16
    gridResolutionCombo.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (gridResolutionCombo);

    strengthSlider.setRange (0.0, 100.0);
    strengthSlider.setValue (50.0);
    strengthSlider.setSliderStyle (juce::Slider::LinearBar);
    strengthSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    strengthSlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (strengthSlider);

    strengthLabel.attachToComponent (&strengthSlider, true);
    strengthLabel.setFont (11.0f);
    strengthLabel.setColour (juce::Label::textColourId, LAF::textDim);
    addAndMakeVisible (strengthLabel);

    swingSlider.setRange (0.0, 100.0);
    swingSlider.setValue (50.0);
    swingSlider.setSliderStyle (juce::Slider::LinearBar);
    swingSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    swingSlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (swingSlider);

    swingLabel.attachToComponent (&swingSlider, true);
    swingLabel.setFont (11.0f);
    swingLabel.setColour (juce::Label::textColourId, LAF::textDim);
    addAndMakeVisible (swingLabel);

    // SCALE QUANTIZE.
    rootNoteCombo.addItemList ({ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
    rootNoteCombo.setSelectedId (1); // C
    rootNoteCombo.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (rootNoteCombo);

    scaleTypeCombo.addItemList ({ "Major", "Minor", "Pentatonic", "Blues", "Chromatic" }, 1);
    scaleTypeCombo.setSelectedId (1); // Major
    scaleTypeCombo.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (scaleTypeCombo);

    snapToScaleToggle.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (snapToScaleToggle);

    // COLLAPSE MODE.
    collapseEmptyToggle.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (collapseEmptyToggle);

    // NOTE PROPERTIES.
    velocitySlider.setRange (1.0, 127.0);
    velocitySlider.setValue (100.0);
    velocitySlider.setSliderStyle (juce::Slider::LinearBar);
    velocitySlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    velocitySlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (velocitySlider);

    velocityLabel.attachToComponent (&velocitySlider, true);
    velocityLabel.setFont (11.0f);
    velocityLabel.setColour (juce::Label::textColourId, LAF::textDim);
    addAndMakeVisible (velocityLabel);

    lengthSlider.setRange (0.25, 16.0);
    lengthSlider.setValue (1.0);
    lengthSlider.setSliderStyle (juce::Slider::LinearBar);
    lengthSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    lengthSlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (lengthSlider);

    lengthLabel.attachToComponent (&lengthSlider, true);
    lengthLabel.setFont (11.0f);
    lengthLabel.setColour (juce::Label::textColourId, LAF::textDim);
    addAndMakeVisible (lengthLabel);

    // STAMP TOOL.
    stampModeToggle.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (stampModeToggle);

    chordTypeCombo.addItemList ({ "Triad", "Seventh", "Ninth", "Custom" }, 1);
    chordTypeCombo.setSelectedId (1); // Triad
    chordTypeCombo.setLookAndFeel (&lookAndFeel);
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
    auto area = getLocalBounds().reduced (8);
    constexpr int controlHeight = 22;
    constexpr int spacing = 8;
    constexpr int sectionGap = 12;

    // TIME QUANTIZE.
    area.removeFromTop (14); // section label
    gridResolutionCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    strengthSlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    swingSlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (sectionGap);

    // SCALE QUANTIZE.
    area.removeFromTop (14); // section label
    rootNoteCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    scaleTypeCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    snapToScaleToggle.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (sectionGap);

    // COLLAPSE MODE.
    collapseEmptyToggle.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (sectionGap);

    // NOTE PROPERTIES.
    area.removeFromTop (14); // section label
    velocitySlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    lengthSlider.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (sectionGap);

    // STAMP TOOL.
    stampModeToggle.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (spacing);
    chordTypeCombo.setBounds (area.removeFromTop (controlHeight));
}
