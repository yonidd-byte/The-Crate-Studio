#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

/**
    Phase 1 skeleton: owns the Tracktion Engine instance + a single Edit,
    auto-connects the audio device, and plays a sine test tone through the
    engine's own DSP graph to prove the signal path end-to-end.
*/
class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void initialiseAudioDevice();
    void buildTestToneEdit();

    // Engine must outlive everything else derived from it (Edit, plugins, devices).
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<te::Edit> edit;

    juce::TextButton playButton { "Play Test Tone" };
    juce::TextButton stopButton { "Stop" };
    juce::Label statusLabel;

    void updateStatusLabel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
