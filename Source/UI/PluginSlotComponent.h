#pragma once

#include <JuceHeader.h>

/**
    Reusable compact plugin slot for the Hybrid Mixer's vertical Inserts/Sends rack
    (MASTER_ARCHITECTURE.md section 9 "Expand Rack" — Pro Tools/Logic-style vertical
    insert visibility, solving the classic Ableton "which devices are on this
    track" problem). One slot = one loaded plugin: a compact dark rounded rect,
    tiny circular bypass toggle on the left, centred crisp small-font name, and a
    hover state that brightens the whole slot slightly.

    Deliberately NOT bound to te::Plugin directly — MixerStrip owns that binding
    and wires onBypassToggle/onClicked/onPluginDropped itself, same "dumb view,
    owner wires everything" pattern as CircularToggleButton in
    UniversalDeviceChainComponent. This keeps the slot reusable for BOTH Inserts
    (bypass = plugin.isEnabled()) and Sends (bypass = the AuxSendPlugin's own
    isEnabled() — sends are te::Plugin too, so this isn't a stretch), and usable
    outside MixerStrip later without dragging a tracktion_engine include along.

    Also a juce::DragAndDropTarget (public inheritance — REQUIRED, not a style
    choice: JUCE's DragAndDropContainer discovers drop targets via an EXTERNAL
    dynamic_cast<DragAndDropTarget*>(component) while walking the parent chain
    during a drag, exactly like juce::FileDragAndDropTarget's OS-level
    equivalent; private inheritance would make that cast well-formed but return
    nullptr, silently breaking drops). Accepts drags whose description starts
    with "plugin_drag|" (BrowserComponent::PluginRow's sourceDescription
    prefix) and shows a bright cyan outline while one is hovering.
*/
class PluginSlotComponent : public juce::Component,
                            public juce::DragAndDropTarget
{
public:
    explicit PluginSlotComponent (juce::String pluginName);
    ~PluginSlotComponent() override;

    void setBypassState (bool isEnabled, juce::NotificationType);

    /** Marks this slot as an empty, recessed "ghost" placeholder (Hybrid Mixer's
        Minimum-10 Scrolling Grid — MixerStrip's InsertsSection): hides the bypass
        toggle and draws a dashed, recessed body instead of the normal filled
        slot look. Still a fully-functional juce::DragAndDropTarget at its own
        exact row position — deliberately NOT toggling setInterceptsMouseClicks,
        since JUCE's drag-and-drop target discovery hit-tests via
        getComponentAt(), which honours that flag; disabling it here would make
        the drag silently fall through to this slot's parent and lose the exact
        numbered-index the ghost row represents. */
    void setGhostState (bool shouldBeGhost);

    /** Extra control shown to the right of the name (e.g. a Send's level slider).
        Not owned — caller keeps the component alive at least as long as this slot;
        same non-owning-raw-pointer convention DeviceBlock uses for its track meter. */
    void setTrailingComponent (juce::Component* trailing);

    std::function<void (bool)> onBypassToggle;
    std::function<void()> onClicked;

    /** Fires when a Browser plugin is dropped onto this slot, with the plugin's
        identifier string (BrowserComponent::PluginRow's "plugin_drag|" prefix
        already stripped) — the owner (MixerStrip's ChannelStripRack) resolves it
        back to a juce::PluginDescription and performs the actual TE instantiation
        + insert-at-index, since this view has no engine access of its own. */
    std::function<void (const juce::String& pluginIdentifier)> onPluginDropped;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    // Tiny circular bypass toggle — same "Component, not Button" reasoning as
    // UniversalDeviceChainComponent's CircularToggleButton (JUCE's Button can't
    // easily be made circular without a full LookAndFeel override): duplicated
    // here rather than shared, since promoting that one to a public/shared type
    // is a bigger refactor than this task asked for.
    class CircularBypassToggle : public juce::Component
    {
    public:
        std::function<void (bool)> onToggle;

        void setState (bool shouldBeOn, juce::NotificationType nt)
        {
            if (on == shouldBeOn)
                return;

            on = shouldBeOn;
            repaint();

            if (nt != juce::dontSendNotification && onToggle)
                onToggle (on);
        }

        bool getState() const noexcept   { return on; }

        void mouseUp (const juce::MouseEvent&) override   { setState (! on, juce::sendNotificationSync); }
        void paint (juce::Graphics&) override;

    private:
        bool on = true;
    };

    CircularBypassToggle bypassToggle;

    // Plugin name is drawn directly in paint() (g.drawText(..., useEllipses =
    // true), not held in a juce::Label — Label has no built-in ellipsis
    // truncation (only setMinimumHorizontalScale()'s font-shrinking, which
    // has no floor and would eventually render illegibly-tiny text for a
    // long name in this fixed-width, fixed-height slot; a Pro Tools-style
    // rack instead truncates with "…" at a fixed, always-legible font size).
    // nameBounds is computed once in resized(), same "shared, can't drift"
    // pattern as MixerStrip's meterBounds.
    juce::String pluginName;
    juce::Rectangle<int> nameBounds;

    juce::Component* trailingComponent = nullptr;
    bool hovering = false;
    bool isDragHovering = false;
    bool isGhost = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginSlotComponent)
};
