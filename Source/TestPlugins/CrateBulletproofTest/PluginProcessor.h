#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// Step 20 (Plugin Isolation & Thread Sanitization) directive: a deliberately
// trivial, guaranteed-inert VST3 -- JUCE's own bundled AudioPlugin example
// template (modules/JUCE/examples/CMake/AudioPlugin), copied verbatim rather
// than modified, so its processBlock() stays a true no-op passthrough with
// zero DSP of any kind. Used ONLY to isolate whether the CrateSandbox child
// process hang (triggered the moment LookaheadWorkerThread feeds real,
// non-silent extracted clip audio into a real VST3's processBlock()) is
// specific to Rift Filter Lite's own internal DSP, or a bug in this
// codebase's own IPC/threading plumbing. If this plugin also hangs under
// the identical Time-Slip test, the bug is ours; if it doesn't, Rift Filter
// Lite's own denormal/state handling is implicated.
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    //==============================================================================
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
