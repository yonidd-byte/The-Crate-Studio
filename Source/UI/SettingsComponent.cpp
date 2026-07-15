#include "SettingsComponent.h"

SettingsComponent::SettingsComponent (te::Engine& engineToConfigure)
    : engine (engineToConfigure),
      selector (engine.getDeviceManager().deviceManager,
                0, 2,     // audio input channels min/max
                0, 2,     // audio output channels min/max
                true,     // show MIDI input options
                false,    // show MIDI output selector (no MIDI out devices yet)
                true,     // show channels as stereo pairs
                false)    // don't hide advanced options behind a button
{
    addAndMakeVisible (selector);

    // AudioDeviceSelectorComponent only touches JUCE's AudioDeviceManager. TE keeps a
    // separate MIDI input device list that needs an explicit rescan to notice toggles
    // made here (e.g. enabling a new MIDI controller).
    engine.getDeviceManager().deviceManager.addChangeListener (this);

    setSize (600, 500);
}

SettingsComponent::~SettingsComponent()
{
    engine.getDeviceManager().deviceManager.removeChangeListener (this);
}

void SettingsComponent::resized()
{
    selector.setBounds (getLocalBounds());
}

void SettingsComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    engine.getDeviceManager().rescanMidiDeviceList();
}
