#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "MainComponent.h"
#include "UI/TheCrateLookAndFeel.h"

namespace te = tracktion::engine;

class TheCrateStudioApplication : public juce::JUCEApplication
{
public:
    TheCrateStudioApplication() = default;

    const juce::String getApplicationName() override       { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override              { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // Plugin Sandboxing directive: CrateEngineBehaviour::
        // canScanPluginsOutOfProcess() (CrateWorkflowManager.cpp) opts in to
        // out-of-process plugin scanning — this is the other half of that
        // contract (see EngineBehaviour::canScanPluginsOutOfProcess()'s own
        // doc comment): re-launching with the scan-worker command line MUST
        // short-circuit straight into the child-process scan helper before
        // any of our own window/Engine/CrateWorkflowManager construction
        // runs, or a relaunched scan process would open a second full DAW
        // window instead of just scanning and exiting.
        if (te::PluginManager::startChildProcessPluginScan (commandLine))
            return;

        // Must be set before MainWindow constructs — it reads the default LookAndFeel's
        // background colour at construction time.
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                               juce::Desktop::getInstance().getDefaultLookAndFeel()
                                   .findColour (juce::ResizableWindow::backgroundColourId),
                               DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);

           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
           #endif

            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    TheCrateLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (TheCrateStudioApplication)
