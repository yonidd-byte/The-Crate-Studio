#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

/**
    Native editor host window for a te::Plugin. TE deliberately leaves window
    creation to the host app (PluginWindowState::showWindow() calls out to
    Engine::getUIBehaviour().createPluginWindow(), whose default implementation
    returns nullptr) — this is that implementation, adapted from TE's own
    examples/common/PluginWindow.h reference.
*/
class PluginWindow : public juce::DocumentWindow
{
public:
    explicit PluginWindow (te::Plugin&);
    ~PluginWindow() override;

    static std::unique_ptr<juce::Component> create (te::Plugin&);

    void show();

private:
    void moved() override;
    void userTriedToCloseWindow() override         { plugin.windowState->closeWindowExplicitly(); }
    void closeButtonPressed() override              { userTriedToCloseWindow(); }
    float getDesktopScaleFactor() const override    { return 1.0f; }

    void setEditor (std::unique_ptr<te::Plugin::EditorComponent>);

    std::unique_ptr<te::Plugin::EditorComponent> editor;
    te::Plugin& plugin;
    bool updateStoredBounds = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

/** Wires TE's plugin-window extension point to PluginWindow above. Pass an instance
    of this to the te::Engine constructor. */
class CrateUIBehaviour : public te::UIBehaviour
{
public:
    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& pws) override
    {
        if (auto ws = dynamic_cast<te::Plugin::WindowState*> (&pws))
            return PluginWindow::create (ws->plugin);

        return {};
    }
};
