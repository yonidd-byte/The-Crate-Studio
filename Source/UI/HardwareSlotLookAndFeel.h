#pragma once

#include <JuceHeader.h>

#include "CrateTheme.h"

/**
    Premium "hardware slot" chrome for the Channel Strip's Routing controls
    (OUT 1 dropdown, the "No Group" slot, the Read automation-mode button) —
    V2.0 UI/UX Master Manifesto's "SSL Tactile Experience" / Logic Pro
    Micro-Depth reference. These read as physical, milled I/O slots on a
    console, not native-OS flat dropdowns.

    Scoped ONLY to these controls via per-component setLookAndFeel() (see
    MixerStrip's wiring) — every other button in the app (Mute/Solo/Bypass,
    etc.) keeps TheCrateLookAndFeel's "Ghosted Buttons" language untouched.
    This is a deliberately DIFFERENT, more tactile look reserved for controls
    that represent physical console I/O, not toggles.
*/
class HardwareSlotLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HardwareSlotLookAndFeel() = default;

    // No default OS/JUCE dropdown arrow at all — a hardware I/O slot is just
    // clickable text in a milled recess (Logic Pro's own I/O slots), never a
    // combo-box-shaped affordance.
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                        int buttonX, int buttonY, int buttonW, int buttonH,
                        juce::ComboBox&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // The base LookAndFeel_V2 implementation reserves getHeight() px on the
    // RIGHT of the label for a dropdown arrow (box.getWidth() + 3 - box.getHeight()),
    // which we never draw (see drawComboBox — no arrow glyph at all). Left as
    // inherited, that reserved dead zone skews centred text left of the chip's
    // true centre. Override so the label fills the FULL chip and its own
    // Justification::centred (set by the caller) centres across the whole
    // width, not a narrowed sub-box.
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // Custom minimalist scrollbar for SendsSection's internal Viewport — a
    // floating dark thumb only, no track/background fill, no up/down arrow
    // buttons (LookAndFeel_V4 already returns false from
    // areScrollbarButtonsVisible(), inherited unchanged here).
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                         bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                         bool isMouseOver, bool isMouseDown) override;

    // Minimal vector-arc rotary — used for the miniature Send-level knob
    // (CrateSendSlot). Deliberately NOT the pan-knob filmstrip image
    // (CrateMixerLookAndFeel's drawRotarySlider): that asset is a bipolar
    // pan cylinder with a centre detent, semantically wrong for a unipolar
    // 0..1 send level. A monotonic flat arc (fills from the start angle up
    // to the current value, no centre-detent) is the correct shape for this
    // control, and the Manifesto explicitly allows "a minimal vector arc if
    // no image exists" for exactly this case.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                            juce::Slider&) override;

    // Shared with RoutingBlock's own paint() — the "No Group" slot is a plain
    // juce::Label (no Button/ComboBox to route through this LookAndFeel), so
    // it draws the IDENTICAL bevel by hand using these same constants, rather
    // than a second, potentially-drifting copy of the colours/radius.
    // Global Color Palette Standardization directive: every one of these is
    // now DERIVED from CrateColors' exact 4 values via JUCE's own
    // brighter()/darker() (a runtime transform, not a new stored hex literal
    // — still exactly one source of truth) rather than each being its own
    // independent near-miss hex. highlightColour brightens LightBackground so
    // the raised-bevel top-edge highlight line stays visually distinct from
    // fillColour (both being flat-identical would erase the "SSL Tactile"
    // raised-button read entirely).
    static constexpr float cornerRadius     = 2.0f;
    inline static const juce::Colour fillColour       = CrateColors::LightBackground;
    inline static const juce::Colour shadowColour     = CrateColors::DarkBackground; // bottom edge of a RAISED button — see drawButtonBackground
    inline static const juce::Colour highlightColour  = CrateColors::LightBackground.brighter (0.3f); // top edge of a RAISED button
    inline static const juce::Colour dimTextColour    = CrateColors::BrandGray;

    /** The shared "raised hardware button" drawing (drop shadow + fill + top
        highlight + bottom shadow) — public so plain juce::Label-based chips
        that can't route through a LookAndFeel (e.g. "No Group" / "Stereo Out")
        can draw the IDENTICAL bevel by hand, rather than a second copy of
        these draw calls that could drift out of sync with this one. */
    static void drawRaisedChip (juce::Graphics&, juce::Rectangle<float> bounds, juce::Colour fill);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardwareSlotLookAndFeel)
};
