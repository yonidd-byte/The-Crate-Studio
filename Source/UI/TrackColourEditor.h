#pragma once

#include <JuceHeader.h>

/**
    Shared "rename + track-colour" editor used by BOTH the Mixer name-plate
    (MixerStrip) and the Arrangement timeline header (TrackHeaderComponent), so
    the two entry points are literally the same code (not two drifting copies).

    The picker itself is a real juce::ColourSelector (palette grid + hue slider,
    no alpha) hosted in a CallOutBox — pro-DAW standard, not a fixed swatch list.
    All track mutation is left to the CALLER via callbacks it guards with its own
    SafePointer, so this helper never holds a raw te::Track pointer across the
    async menu/callout lifetime.
*/
namespace CrateTrackEditor
{
    class TrackColourPicker : public juce::Component,
                              private juce::ChangeListener
    {
    public:
        std::function<void (juce::Colour)> onColour;

        explicit TrackColourPicker (juce::Colour initial)
            : selector (juce::ColourSelector::showColourspace | juce::ColourSelector::showColourAtTop) // grid + hue, NO alpha
        {
            selector.setCurrentColour (initial, juce::dontSendNotification);
            selector.addChangeListener (this);
            addAndMakeVisible (selector);
            setSize (260, 300);
        }

        ~TrackColourPicker() override   { selector.removeChangeListener (this); }

        void resized() override         { selector.setBounds (getLocalBounds().reduced (4)); }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            if (onColour)
                onColour (selector.getCurrentColour());
        }

        juce::ColourSelector selector;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackColourPicker)
    };

    /** Pops the Rename... / Colour... menu at the mouse, then routes the user's
        choice back through the caller's callbacks:
          - onRename(newName)       : apply a validated, non-empty new name.
          - onColourGestureBegin()  : called ONCE right before the colour
                                      callout opens — the caller opens a single
                                      Undo transaction here so the live drag
                                      coalesces into one step.
          - onColour(colour)        : live colour on every selector change. */
    inline void showNameColourMenu (juce::Component* target,
                                    juce::Rectangle<int> anchorScreenBounds,
                                    juce::String currentName,
                                    juce::Colour currentColour,
                                    std::function<void (juce::String)> onRename,
                                    std::function<void()> onColourGestureBegin,
                                    std::function<void (juce::Colour)> onColour)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Rename...");
        menu.addItem (2, "Colour...");

        juce::Component::SafePointer<juce::Component> safeTarget (target);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
            [safeTarget, anchorScreenBounds, currentName, currentColour, onRename, onColourGestureBegin, onColour] (int result)
            {
                if (result == 1)
                {
                    auto* aw = new juce::AlertWindow ("Rename Track", "Enter a new track name:",
                                                      juce::MessageBoxIconType::NoIcon);
                    aw->addTextEditor ("name", currentName);
                    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                    aw->enterModalState (true, juce::ModalCallbackFunction::create (
                        [aw, onRename] (int r)
                        {
                            if (r == 1 && onRename)
                            {
                                const auto newName = aw->getTextEditorContents ("name").trim();
                                if (newName.isNotEmpty())
                                    onRename (newName);
                            }
                        }), true); // deleteWhenDismissed — freed after this callback
                    return;
                }

                if (result == 2)
                {
                    if (onColourGestureBegin)
                        onColourGestureBegin();

                    const auto initial = currentColour.isTransparent() ? juce::Colour (0xff30506a) : currentColour;

                    auto picker = std::make_unique<TrackColourPicker> (initial);
                    picker->onColour = onColour;

                    juce::CallOutBox::launchAsynchronously (std::move (picker), anchorScreenBounds,
                                                            safeTarget != nullptr ? safeTarget->getTopLevelComponent() : nullptr);
                }
            });
    }
}
