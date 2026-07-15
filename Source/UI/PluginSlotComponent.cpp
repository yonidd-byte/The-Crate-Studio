#include "PluginSlotComponent.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    constexpr int bypassDiameter = 10;
    constexpr float slotCornerSize = 4.0f;

    const auto slotBodyColour   = juce::Colour (0xff232328);
    const auto slotHoverColour  = juce::Colour (0xff2c2c32);
    const auto bypassOffColour  = juce::Colour (0xff5a5a60);

    const juce::String pluginDragPrefix = "plugin_drag|";
}

void PluginSlotComponent::CircularBypassToggle::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (on ? LAF::accent : bypassOffColour);
    g.fillEllipse (bounds);
    g.setColour (LAF::background);
    g.drawEllipse (bounds, 1.0f);
}

PluginSlotComponent::PluginSlotComponent (juce::String pluginNameToShow)
    : pluginName (std::move (pluginNameToShow))
{
    setInterceptsMouseClicks (true, true); // true for children too — bypassToggle needs its own clicks

    addAndMakeVisible (bypassToggle);
    bypassToggle.onToggle = [this] (bool isOn) { if (onBypassToggle) onBypassToggle (isOn); };
}

PluginSlotComponent::~PluginSlotComponent() = default;

void PluginSlotComponent::setBypassState (bool isEnabled, juce::NotificationType nt)
{
    bypassToggle.setState (isEnabled, nt);
}

void PluginSlotComponent::setGhostState (bool shouldBeGhost)
{
    if (isGhost == shouldBeGhost)
        return;

    isGhost = shouldBeGhost;
    bypassToggle.setVisible (! isGhost); // recessed rows show no toggle — nothing to bypass yet
    repaint();
}

void PluginSlotComponent::setTrailingComponent (juce::Component* trailing)
{
    trailingComponent = trailing;

    if (trailingComponent != nullptr)
        addAndMakeVisible (*trailingComponent);

    resized();
}

void PluginSlotComponent::mouseEnter (const juce::MouseEvent&)
{
    hovering = true;
    repaint();
}

void PluginSlotComponent::mouseExit (const juce::MouseEvent&)
{
    hovering = false;
    repaint();
}

void PluginSlotComponent::mouseUp (const juce::MouseEvent& e)
{
    // Only the slot body counts as "select this plugin" — a click that started
    // and ended on the bypass toggle already handled itself via that child
    // Component's own mouseUp, and JUCE won't have routed it here in that case
    // (getEventRelativeTo would be inside bypassToggle's own bounds, a separate
    // Component with its own mouse handling) — this guard is defensive only.
    if (! bypassToggle.getBounds().contains (e.getPosition()) && onClicked)
        onClicked();
}

void PluginSlotComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    if (isGhost)
    {
        // Recessed, dashed-outline empty row — an explicit, numbered drop
        // target in its own right, not just leftover background.
        g.setColour (juce::Colour (0xff18181c));
        g.fillRoundedRectangle (bounds, slotCornerSize);

        juce::Path outline;
        outline.addRoundedRectangle (bounds.reduced (0.5f), slotCornerSize);
        juce::Path dashedOutline;
        const float dashLengths[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashedOutline, outline, dashLengths, 2);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillPath (dashedOutline);
    }
    else
    {
        g.setColour (hovering ? slotHoverColour : slotBodyColour);
        g.fillRoundedRectangle (bounds, slotCornerSize);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), slotCornerSize, 1.0f);
    }

    // Premium drop-target glow — a distinct bright cyan outline (+ a faint fill)
    // so it reads clearly against the flat, otherwise-unlit slot body.
    if (isDragHovering)
    {
        g.setColour (LAF::accent.withAlpha (0.16f));
        g.fillRoundedRectangle (bounds.reduced (0.5f), slotCornerSize);
        g.setColour (LAF::accent);
        g.drawRoundedRectangle (bounds.reduced (1.0f), slotCornerSize, 1.5f);
    }

    // Pro Tools-style strict truncation: a long name (e.g.
    // "TheCrateConnect_Mastering") gets a trailing "…" at a fixed, always
    // legible 11pt rather than shrinking to fit or overflowing the slot.
    // reduced (4, 0) keeps the "…" off the slot's own rounded edge.
    if (pluginName.isNotEmpty())
    {
        g.setColour (LAF::text);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (pluginName, nameBounds.reduced (4, 0), juce::Justification::centred, true);
    }
}

bool PluginSlotComponent::isInterestedInDragSource (const SourceDetails& details)
{
    return details.description.toString().startsWith (pluginDragPrefix);
}

void PluginSlotComponent::itemDragEnter (const SourceDetails&)
{
    isDragHovering = true;
    repaint();
}

void PluginSlotComponent::itemDragExit (const SourceDetails&)
{
    isDragHovering = false;
    repaint();
}

void PluginSlotComponent::itemDropped (const SourceDetails& details)
{
    isDragHovering = false;
    repaint();

    const auto identifier = details.description.toString().fromFirstOccurrenceOf (pluginDragPrefix, false, false);

    if (identifier.isNotEmpty() && onPluginDropped)
        onPluginDropped (identifier);
}

void PluginSlotComponent::resized()
{
    auto area = getLocalBounds().reduced (3, 0);

    bypassToggle.setBounds (area.removeFromLeft (bypassDiameter).withSizeKeepingCentre (bypassDiameter, bypassDiameter));
    area.removeFromLeft (3);

    if (trailingComponent != nullptr)
    {
        constexpr int trailingWidth = 34;
        trailingComponent->setBounds (area.removeFromRight (trailingWidth));
        area.removeFromRight (3);
    }

    nameBounds = area;
}
