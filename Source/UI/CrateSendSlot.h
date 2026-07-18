#pragma once

#include <JuceHeader.h>
#include "TheCrateLookAndFeel.h"
#include "HardwareSlotLookAndFeel.h"

/**
    Logic Pro-style inline Send slot — one compact horizontal row combining a
    destination chip ("Bus 1") with a miniature rotary knob controlling the
    send amount, both living INSIDE this single slot's bounds (Logic draws the
    send knob right in the channel strip's send row, not in a separate popover).

    V2.0 Manifesto "Hardware Unification": the destination chip is now a real
    hardware-slot bevel (same fill/shadow/highlight/corner-radius family as
    OUT 1 / Read / No Group — see HardwareSlotLookAndFeel), split strictly
    70% destination / 30% knob. This is the ONE shared Send-row component used
    by BOTH the full Mixer channel strip (MixerStrip) and the collapsed Dual-
    Strip Inspector (CrateTrackInspectorComponent) — upgrading it here upgrades
    both call sites at once, rather than drifting two copies of this look.

    The knob's LookAndFeel is caller-supplied (setLookAndFeelForKnob()) —
    pass a HardwareSlotLookAndFeel instance for its minimal vector-arc rotary
    (correct: send level is unipolar, not a bipolar pan). onAmountChanged is
    exposed so a DSP pass can wire it; onBypassToggle is OPTIONAL (only set by
    callers that track a real te::AuxSendPlugin::isEnabled() — clicking the
    destination chip toggles it, dimming the chip when off. Leave it unset for
    a purely visual/scaffold usage).
*/
class CrateSendSlot : public juce::Component
{
public:
    explicit CrateSendSlot (const juce::String& busName)
        : busName_ (busName)
    {
        // Miniature rotary — send amount, unipolar. No text box (there's no
        // room in a compact send row); the knob position IS the readout.
        amountKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        amountKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        amountKnob.setRange (0.0, 1.0, 0.01);
        amountKnob.setValue (0.0, juce::dontSendNotification);
        amountKnob.setDoubleClickReturnValue (true, 0.0);
        // Global UX rule: scroll wheel is disabled on Send knobs — a user
        // scrolling down a busy channel strip can otherwise accidentally
        // change a send level. Click-and-drag only. Fixed HERE (not in each
        // caller) since this is the ONE shared knob both MixerStrip and the
        // Inspector's Channel Strip use.
        amountKnob.setScrollWheelEnabled (false);
        amountKnob.setTooltip ("Send amount to " + busName);
        amountKnob.onValueChange = [this]
        {
            if (onAmountChanged)
                onAmountChanged ((float) amountKnob.getValue());
        };
        addAndMakeVisible (amountKnob);
    }

    /** Fired on knob drag — bound to the real te::AuxSendPlugin gain parameter
        by the caller. */
    std::function<void (float amount)> onAmountChanged;

    /** OPTIONAL — set by a caller that tracks a real te::AuxSendPlugin's
        isEnabled() state. When set, clicking the destination chip toggles
        bypass and the chip dims to show the off state. */
    std::function<void (bool isEnabled)> onBypassToggle;

    /** Reflects external bypass-state changes (e.g. the send was disabled
        from elsewhere) back onto the chip — does NOT fire onBypassToggle. */
    void setBypassState (bool isEnabled, juce::NotificationType)
    {
        bypassed = ! isEnabled;
        repaint();
    }

    juce::Slider& getAmountKnob() noexcept   { return amountKnob; }

    void setLookAndFeelForKnob (juce::LookAndFeel* laf)   { amountKnob.setLookAndFeel (laf); }

    ~CrateSendSlot() override   { amountKnob.setLookAndFeel (nullptr); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Only the destination chip (left zone) is clickable for bypass —
        // the knob has its own drag handling and shouldn't also toggle bypass
        // on a stray click-release.
        if (onBypassToggle != nullptr && chipBounds.contains (e.getPosition()))
        {
            bypassed = ! bypassed;
            onBypassToggle (! bypassed);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using SlotLAF = HardwareSlotLookAndFeel;

        if (chipBounds.isEmpty())
            return;

        const auto bounds = chipBounds.toFloat();

        SlotLAF::drawRaisedChip (g, bounds, bypassed ? SlotLAF::fillColour.darker (0.3f) : SlotLAF::fillColour);

        g.setColour (bypassed ? SlotLAF::dimTextColour.withAlpha (0.4f) : SlotLAF::dimTextColour);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (busName_, chipBounds, juce::Justification::centred);
    }

    void resized() override
    {
        // No horizontal reduce — the chip's left edge must land EXACTLY at
        // this component's own X, so it shares the same starting X as every
        // other rack-family control (OUT 1+2, No Group, Read) whose parent
        // container is positioned with the identical rackMargin+rackButtonPadding
        // offset in MixerStrip::resized().
        auto area = getLocalBounds().reduced (0, 1);

        // Strict 70/30 split — destination chip left, knob right.
        const int knobWidth = juce::roundToInt ((float) area.getWidth() * 0.30f);
        amountKnob.setBounds (area.removeFromRight (juce::jmax (knobWidth, area.getHeight())));
        area.removeFromRight (3);
        chipBounds = area; // painted by hand in paint() — see class doc comment
    }

private:
    juce::String busName_;
    juce::Slider amountKnob;
    juce::Rectangle<int> chipBounds; // destination chip zone, computed in resized(), painted in paint()
    bool bypassed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateSendSlot)
};
