#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    juce::ignoreUnused (processorRef);

    // Step 23 (Geometry Polish & Dynamic Resize) directive: genuinely
    // resizable, with a real JUCE corner-drag grip (setResizable's own
    // addToDesktop-time behaviour) — this is the concrete, draggable
    // stand-in for "a FabFilter-style resizable VST3" the Step 23
    // verification test drags to prove the Parent's outer window follows.
    setResizable (true, true);
    setResizeLimits (200, 150, 1000, 800);
    setSize (400, 300);
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white);
    g.setFont (15.0f);
    g.drawFittedText ("Crate Bulletproof Test", getLocalBounds(), juce::Justification::centred, 1);
}

void AudioPluginAudioProcessorEditor::resized()
{
}
