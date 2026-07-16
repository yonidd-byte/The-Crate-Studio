#include "CrateMidiInspectorComponent.h"
#include "TheCrateLookAndFeel.h"

using LAF = TheCrateLookAndFeel;

CrateMidiInspectorComponent::CrateMidiInspectorComponent()
{
    setLookAndFeel (&lookAndFeel);

    // TIME QUANTIZE.
    gridResolutionCombo.addItemList ({ "Free", "1/4", "1/8", "1/16", "1/32" }, 1);
    gridResolutionCombo.setSelectedId (4); // 1/16 (index 3, id 4)
    gridResolutionCombo.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (gridResolutionCombo);

    strengthSlider.setRange (0.0, 100.0);
    strengthSlider.setValue (50.0);
    strengthSlider.setSliderStyle (juce::Slider::LinearBar);
    strengthSlider.setTextBoxStyle (juce::Slider::TextBoxRight, true, 40, 20);
    strengthSlider.setNumDecimalPlacesToDisplay (0);
    strengthSlider.setTextValueSuffix ("%");
    strengthSlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (strengthSlider);

    strengthLabel.setFont (11.0f);
    strengthLabel.setColour (juce::Label::textColourId, LAF::textDim);
    strengthLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (strengthLabel);

    swingSlider.setRange (0.0, 100.0);
    swingSlider.setValue (50.0);
    swingSlider.setSliderStyle (juce::Slider::LinearBar);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxRight, true, 40, 20);
    swingSlider.setNumDecimalPlacesToDisplay (0);
    swingSlider.setTextValueSuffix ("%");
    swingSlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (swingSlider);

    swingLabel.setFont (11.0f);
    swingLabel.setColour (juce::Label::textColourId, LAF::textDim);
    swingLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (swingLabel);

    // SCALE QUANTIZE.
    rootNoteCombo.addItemList ({ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
    rootNoteCombo.setSelectedId (1); // C
    rootNoteCombo.setLookAndFeel (&lookAndFeel);
    rootNoteCombo.onChange = [this] { if (onScaleChanged) onScaleChanged(); broadcastScaleState(); };
    addAndMakeVisible (rootNoteCombo);

    scaleTypeCombo.addItemList ({
        "Ionian (Major)",
        "Dorian",
        "Phrygian",
        "Lydian",
        "Mixolydian",
        "Aeolian (Minor)",
        "Locrian",
        "Harmonic Minor",
        "Melodic Minor"
    }, 1);
    scaleTypeCombo.setSelectedId (1); // Ionian (Major)
    scaleTypeCombo.setLookAndFeel (&lookAndFeel);
    scaleTypeCombo.onChange = [this] { if (onScaleChanged) onScaleChanged(); broadcastScaleState(); };
    addAndMakeVisible (scaleTypeCombo);

    snapToScaleToggle.setLookAndFeel (&lookAndFeel);
    snapToScaleToggle.onStateChange = [this] { if (onScaleChanged) onScaleChanged(); broadcastScaleState(); };
    addAndMakeVisible (snapToScaleToggle);

    // COLLAPSE MODE.
    collapseEmptyToggle.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (collapseEmptyToggle);

    // NOTE PROPERTIES.
    velocitySlider.setRange (1.0, 127.0);
    velocitySlider.setValue (100.0);
    velocitySlider.setSliderStyle (juce::Slider::LinearBar);
    velocitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, true, 40, 20);
    velocitySlider.setNumDecimalPlacesToDisplay (0);
    velocitySlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (velocitySlider);

    velocityLabel.setFont (11.0f);
    velocityLabel.setColour (juce::Label::textColourId, LAF::textDim);
    velocityLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (velocityLabel);

    lengthSlider.setRange (0.25, 16.0);
    lengthSlider.setValue (1.0);
    lengthSlider.setSliderStyle (juce::Slider::LinearBar);
    lengthSlider.setTextBoxStyle (juce::Slider::TextBoxRight, true, 40, 20);
    lengthSlider.setNumDecimalPlacesToDisplay (2);
    lengthSlider.setTextValueSuffix (" Beats");
    lengthSlider.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (lengthSlider);

    lengthLabel.setFont (11.0f);
    lengthLabel.setColour (juce::Label::textColourId, LAF::textDim);
    lengthLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (lengthLabel);

    // Wire slider callbacks to grid content.
    velocitySlider.onDragEnd = [this]
    {
        if (onVelocitySliderChanged)
            onVelocitySliderChanged ((int) std::round (velocitySlider.getValue()));
    };

    lengthSlider.onDragEnd = [this]
    {
        if (onLengthSliderChanged)
            onLengthSliderChanged (lengthSlider.getValue());
    };

    strengthSlider.onDragEnd = [this]
    {
        if (onHumanizeApplied)
            onHumanizeApplied ((int) std::round (strengthSlider.getValue()));
    };

    swingSlider.onDragEnd = [this]
    {
        if (onSwingApplied)
            onSwingApplied ((int) std::round (swingSlider.getValue()));
    };

    // STAMP TOOL.
    stampModeToggle.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (stampModeToggle);

    chordTypeCombo.addItemList ({
        "Major Triad",
        "Minor Triad",
        "Maj7",
        "Min7",
        "Dom7",
        "Min9",
        "Sus2",
        "Sus4",
        "Diminished"
    }, 1);
    chordTypeCombo.setSelectedId (1); // Major Triad
    chordTypeCombo.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (chordTypeCombo);
}

CrateMidiInspectorComponent::~CrateMidiInspectorComponent() = default;

juce::Array<int> CrateMidiInspectorComponent::getScaleIntervals (int scaleType)
{
    switch (scaleType)
    {
        case 0: return { 0, 2, 4, 5, 7, 9, 11 };  // Ionian (Major)
        case 1: return { 0, 2, 3, 5, 7, 9, 10 };  // Dorian
        case 2: return { 0, 1, 3, 5, 7, 8, 10 };  // Phrygian
        case 3: return { 0, 2, 4, 6, 7, 9, 11 };  // Lydian
        case 4: return { 0, 2, 4, 5, 7, 9, 10 };  // Mixolydian
        case 5: return { 0, 2, 3, 5, 7, 8, 10 };  // Aeolian (Minor)
        case 6: return { 0, 1, 3, 5, 6, 8, 10 };  // Locrian
        case 7: return { 0, 2, 3, 5, 7, 8, 11 };  // Harmonic Minor
        case 8: return { 0, 2, 3, 5, 7, 9, 11 };  // Melodic Minor
        default: return {};
    }
}

void CrateMidiInspectorComponent::broadcastScaleState()
{
    if (onScaleStateChanged)
        onScaleStateChanged (isSnapToScaleEnabled(),
                             getRootNote(),
                             getScaleIntervals (getScaleType()));
}

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
    else
    {
        // Draw section headers for visual grouping.
        auto drawHeader = [&g] (const juce::String& text, int y)
        {
            g.setColour (LAF::textDim);
            g.setFont (9.0f);
            g.drawText (text, juce::Rectangle<int> (8, y, 100, 12), juce::Justification::topLeft);
        };

        constexpr int padding = 10;
        constexpr int labelHeight = 20;
        constexpr int controlHeight = 24;
        constexpr int smallGap = 5;
        constexpr int bigGap = 20;

        // Section headers (positioned to match resized() layout exactly).
        int yPos = padding;

        // TIME section: label + grid + strength + swing
        drawHeader ("TIME", yPos);
        yPos += labelHeight + controlHeight + smallGap + controlHeight + smallGap + controlHeight + bigGap;

        // SCALE section: label + root + scale + snap + hide
        drawHeader ("SCALE", yPos);
        yPos += labelHeight + controlHeight + smallGap + controlHeight + smallGap + controlHeight + controlHeight + bigGap;

        // TOOLS section: label + velocity + length + stamp + type
        drawHeader ("TOOLS", yPos);
    }
}

void CrateMidiInspectorComponent::resized()
{
    juce::Rectangle<int> area = getLocalBounds().reduced (10);
    constexpr int labelHeight = 20;
    constexpr int controlHeight = 24;
    constexpr int labelWidth = 70;
    constexpr int smallGap = 5;

    // TIME SECTION
    area.removeFromTop (labelHeight); // Reserve space for "TIME" label (drawn in paint())
    gridResolutionCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (smallGap);

    {
        auto sliderArea = area.removeFromTop (controlHeight);
        strengthLabel.setBounds (sliderArea.removeFromLeft (labelWidth));
        strengthSlider.setBounds (sliderArea);
    }
    area.removeFromTop (smallGap);

    {
        auto sliderArea = area.removeFromTop (controlHeight);
        swingLabel.setBounds (sliderArea.removeFromLeft (labelWidth));
        swingSlider.setBounds (sliderArea);
    }

    // SCALE SECTION (with big gap)
    area.removeFromTop (20); // BIG GAP before next section
    area.removeFromTop (labelHeight); // Reserve space for "SCALE" label
    rootNoteCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (smallGap);
    scaleTypeCombo.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (smallGap);
    snapToScaleToggle.setBounds (area.removeFromTop (controlHeight));
    collapseEmptyToggle.setBounds (area.removeFromTop (controlHeight));

    // TOOLS SECTION (with big gap)
    area.removeFromTop (20); // BIG GAP before TOOLS
    area.removeFromTop (labelHeight); // Reserve space for "TOOLS" label

    {
        auto sliderArea = area.removeFromTop (controlHeight);
        velocityLabel.setBounds (sliderArea.removeFromLeft (labelWidth));
        velocitySlider.setBounds (sliderArea);
    }
    area.removeFromTop (smallGap);

    {
        auto sliderArea = area.removeFromTop (controlHeight);
        lengthLabel.setBounds (sliderArea.removeFromLeft (labelWidth));
        lengthSlider.setBounds (sliderArea);
    }
    area.removeFromTop (smallGap);

    stampModeToggle.setBounds (area.removeFromTop (controlHeight));
    chordTypeCombo.setBounds (area.removeFromTop (controlHeight));
}
