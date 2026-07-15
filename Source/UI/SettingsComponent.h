#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

/**
    Audio & MIDI hardware settings — wraps JUCE's AudioDeviceSelectorComponent bound
    directly to the engine's underlying juce::AudioDeviceManager, so driver/sample-rate/
    buffer-size/MIDI-input choices flow straight into Tracktion Engine.
*/
class SettingsComponent : public juce::Component,
                           private juce::ChangeListener
{
public:
    explicit SettingsComponent (te::Engine& engineToConfigure);
    ~SettingsComponent() override;

    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    te::Engine& engine;
    juce::AudioDeviceSelectorComponent selector;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsComponent)
};
