#include "PluginWindow.h"

namespace
{
   #if JUCE_LINUX
    constexpr bool shouldAddPluginWindowToDesktop = false;
   #else
    constexpr bool shouldAddPluginWindowToDesktop = true;
   #endif
}

PluginWindow::PluginWindow (te::Plugin& plug)
    : DocumentWindow (plug.getName(), juce::Colours::black, DocumentWindow::closeButton, shouldAddPluginWindowToDesktop),
      plugin (plug)
{
    getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
    setResizeLimits (100, 50, 4000, 4000);

    setEditor (plugin.createEditor());
    setBoundsConstrained (getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());

   #if JUCE_LINUX
    setAlwaysOnTop (true);
    addToDesktop();
   #endif

    updateStoredBounds = true;
}

PluginWindow::~PluginWindow()
{
    updateStoredBounds = false;
    plugin.edit.flushPluginStateIfNeeded (plugin);
    setEditor (nullptr);
}

void PluginWindow::show()
{
    setVisible (true);
    toFront (false);
    setBoundsConstrained (getBounds());
}

void PluginWindow::setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
{
    setConstrainer (nullptr);
    editor.reset();

    if (newEditor != nullptr)
    {
        editor = std::move (newEditor);
        setContentNonOwned (editor.get(), true);
    }

    setResizable (editor == nullptr || editor->allowWindowResizing(), false);

    if (editor != nullptr && editor->allowWindowResizing())
        setConstrainer (editor->getBoundsConstrainer());
}

std::unique_ptr<juce::Component> PluginWindow::create (te::Plugin& plugin)
{
    if (auto externalPlugin = dynamic_cast<te::ExternalPlugin*> (&plugin))
        if (externalPlugin->getAudioPluginInstance() == nullptr)
            return nullptr;

    std::unique_ptr<PluginWindow> w;

    {
        // Blocks input to the rest of the app for the brief moment the plugin's
        // editor is being created, matching TE's own reference implementation.
        struct Blocker : public juce::Component { void inputAttemptWhenModal() override {} };
        Blocker blocker;
        blocker.enterModalState (false);

        w = std::make_unique<PluginWindow> (plugin);
    }

    if (w == nullptr || w->editor == nullptr)
        return {};

    w->show();
    return w;
}

void PluginWindow::moved()
{
    if (updateStoredBounds)
    {
        plugin.windowState->lastWindowBounds = getBounds();
        plugin.edit.pluginChanged (plugin);
    }
}
