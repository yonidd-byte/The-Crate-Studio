#include "MainComponent.h"

MainComponent::MainComponent()
{
    engine = std::make_unique<te::Engine> ("The Crate Studio");

    initialiseAudioDevice();
    buildTestToneEdit();

    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (statusLabel);

    statusLabel.setJustificationType (juce::Justification::centred);

    playButton.onClick = [this]
    {
        edit->getTransport().play (false);
        updateStatusLabel();
    };

    stopButton.onClick = [this]
    {
        edit->getTransport().stop (false, false);
        updateStatusLabel();
    };

    updateStatusLabel();
    setSize (500, 300);
}

MainComponent::~MainComponent()
{
    if (edit != nullptr)
        edit->getTransport().stop (false, false);

    edit.reset();
    engine.reset();
}

void MainComponent::initialiseAudioDevice()
{
    auto& deviceManager = engine->getDeviceManager();
    auto& juceDeviceManager = deviceManager.deviceManager;

    for (auto* type : juceDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == "Windows Audio")
        {
            type->scanForDevices();
            juceDeviceManager.setCurrentAudioDeviceType ("Windows Audio", true);
            
            // שולפים את הגדרות הדרייבר ומדליקים בכוח את יציאות 1 ו-2
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            juceDeviceManager.getAudioDeviceSetup (setup);
            setup.outputChannels.setBit (0); 
            setup.outputChannels.setBit (1); 
            juceDeviceManager.setAudioDeviceSetup (setup, true);
            
            break;
        }
    }

    deviceManager.initialise (0, 2);
}

void MainComponent::buildTestToneEdit()
{
    edit = te::createEmptyEdit (*engine, juce::File());
    edit->ensureNumberOfAudioTracks (1);

    if (auto track = te::getAudioTracks (*edit)[0])
    {
        // 1. יצירת הפלאגין עם ההגדרה התקנית של JUCE
        juce::PluginDescription desc;
        desc.pluginFormatName = "TracktionInternal";
        desc.fileOrIdentifier = te::FourOscPlugin::xmlTypeName;
        
        auto synthPlugin = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, desc);
        if (synthPlugin != nullptr)
            track->pluginList.insertPlugin (synthPlugin, 0, nullptr);

        // 2. הגדרת זמנים נוקשה באמצעות המערכת העדכנית של Tracktion Core
        auto start    = tracktion::core::TimePosition::fromSeconds (0.0);
        auto duration = tracktion::core::TimeDuration::fromSeconds (2.0);
        tracktion::core::TimeRange timeRange (start, duration);

        if (auto clip = track->insertMIDIClip ("Test MIDI", timeRange, nullptr))
        {
            auto& sequence = clip->getSequence();
            
            // 3. יצירת התו תוך שימוש באובייקטי מקצבים (Beats) במקום מספרים רגילים
            auto noteStart  = tracktion::core::BeatPosition::fromBeats (0.0);
            auto noteLength = tracktion::core::BeatDuration::fromBeats (2.0);
            
            sequence.addNote (60, noteStart, noteLength, 100, 0, nullptr);
        }

        // 4. ניתוב הערוץ לחומרה
        track->getOutput().setOutputToDefaultDevice (true);
    }

    edit->getTransport().ensureContextAllocated();
    
    // 5. הגדרת אזור הלופ
    auto loopStart    = tracktion::core::TimePosition::fromSeconds (0.0);
    auto loopDuration = tracktion::core::TimeDuration::fromSeconds (2.0);
    edit->getTransport().setLoopRange (tracktion::core::TimeRange (loopStart, loopDuration));
    edit->getTransport().looping = true;
}

void MainComponent::updateStatusLabel()
{
    const bool playing = edit != nullptr && edit->getTransport().isPlaying();
    const auto& deviceManager = engine->getDeviceManager().deviceManager;
    const auto* device = deviceManager.getCurrentAudioDevice();

    juce::String text = "Device: " + (device != nullptr ? device->getName() : "none");
    text << "  |  " << (playing ? "Playing test tone" : "Stopped");
    statusLabel.setText (text, juce::dontSendNotification);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (20);
    statusLabel.setBounds (area.removeFromTop (30));
    area.removeFromTop (10);
    playButton.setBounds (area.removeFromTop (40));
    area.removeFromTop (10);
    stopButton.setBounds (area.removeFromTop (40));
}