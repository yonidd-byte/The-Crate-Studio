#pragma once

#include <JuceHeader.h>

#include "CrateColors.h"

/**
    THE CRATE LOOK AND FEEL ENGINE — V2.0 UI/UX Master Manifesto, "Premium Dark
    Room" palette + SSL console tactile language. This IS the app's single
    globally-applied juce::LookAndFeel_V4 (see Main.cpp's
    TheCrateStudioApplication::initialise(), which calls
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel) with an instance of
    this exact class) — the Manifesto's "CrateLookAndFeel Engine" directive
    asked for a new class of that name, but this class already IS that engine,
    already wired globally, already read by name (`LAF::colour`) from ~30 other
    UI files. Renaming it and standing up a second, parallel LookAndFeel_V4
    would either orphan one of the two (MASTER_ARCHITECTURE.md invariant 6:
    "No orphaned components") or require rewriting every one of those call
    sites in the same change — so this evolves the existing engine in place to
    the V2.0 spec instead of duplicating it under a new name.

    Charcoal/void backgrounds, a single strictly-rationed neon cyan accent,
    matte saturated Mute/Solo reds and yellows — no default JUCE gradients,
    bevels, or 3D knobs on any FLAT control. The one deliberate exception is
    CrateMixerLookAndFeel (which inherits from this class) — the Mixer's own
    volume faders get the full "SSL console" metallic-gradient treatment the
    Manifesto's section 3 describes, scoped there rather than here because
    every OTHER slider in the app (Device Chain XY pad axis combos aside —
    those aren't sliders at all) is deliberately flat, per this class's own
    remit.
*/
class TheCrateLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TheCrateLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPos, float minSliderPos, float maxSliderPos,
                            juce::Slider::SliderStyle, juce::Slider&) override;

    // Rotary knobs (MixerStrip's pan knob, Device Chain macro knobs) were falling
    // through to LookAndFeel_V4's default beveled/gradient rotary — contradicting
    // this class's own "no default JUCE gradients, bevels, or 3D knobs" mandate
    // above. Flat filled circle + accent arc, matching drawLinearSlider's language.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                            juce::Slider&) override;

    // "Ghosted Buttons" (Manifesto section 3): OFF blends into the surrounding
    // panel almost invisibly; ON fills with whichever saturated role colour the
    // button itself was given via buttonOnColourId (colorMuteRed for Mute,
    // colorSoloYellow for Solo, colorNeonCyan for a generic toggle — this class
    // has no notion of "which button is this," so the SATURATED colour is
    // always whatever the caller set on that specific button; this override
    // only supplies the shared shape/ghosting language). 3.0f corner radius,
    // exactly matching the SSL console aesthetic the Manifesto specifies.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // No juce::ToggleButton exists anywhere in this codebase today (every
    // Mute/Solo/Bypass control is a juce::TextButton with
    // setClickingTogglesState(true), which routes through
    // drawButtonBackground above, NOT this) — grepped to confirm before
    // relying on it. Implemented anyway, with the identical ghosted-button
    // language, so it draws correctly the moment anything DOES use a real
    // ToggleButton, rather than being a silently-dead override.
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // ============================================================
    // Global Color Palette Standardization directive: every one of these now
    // resolves to one of CrateColors' exact FOUR ARGB values (CrateColors.h)
    // rather than its own independent hex literal — this is the ONE place in
    // the app the mapping happens, so the ~30 files that read the palette
    // through these names (`using LAF = TheCrateLookAndFeel;`) all pick up
    // the strict new palette with zero call-site changes. This DOES flatten
    // the previous "micro-depth" distinction between panel/panelLight/
    // colorFaderGroove (each used to be its own slightly-different near-black/
    // grey) — intentional per the directive's "immutable, do not deviate"
    // mandate; colorMuteRed/colorSoloYellow are semantic STATUS colours
    // (armed/muted/soloed), not brand chrome, so they deliberately stay
    // outside the 4-colour hierarchy (see CrateColors.h's own doc comment).
    // ============================================================
    inline static const juce::Colour colorTheVoid       = CrateColors::DarkBackground;  // absolute background — Arrangement Grid
    inline static const juce::Colour colorHardware      = CrateColors::LightBackground; // Mixer / Inspector / Device Chain panels
    inline static const juce::Colour colorNeonCyan      = CrateColors::NeonBlue;        // the strictly-rationed accent
    inline static const juce::Colour colorTextPrimary   { 0xffffffff }; // pure white — not brand chrome, universal text-on-dark
    inline static const juce::Colour colorTextSecondary = CrateColors::BrandGray;
    inline static const juce::Colour colorMuteRed       { 0xffff3b30 }; // matte, non-blinding — semantic status colour, see above
    inline static const juce::Colour colorSoloYellow    { 0xffffcc00 }; // matte, non-blinding — semantic status colour, see above

    // Ghosted-button OFF fill (section 3) and the recessed fader groove —
    // both now flattened to the strict 4-colour hierarchy (see above).
    inline static const juce::Colour colorGhostedOff = CrateColors::LightBackground;
    inline static const juce::Colour colorFaderGroove = CrateColors::DarkBackground;

    // Legacy aliases — every existing UI file (~30 of them) reads the
    // palette through these names via `using LAF = TheCrateLookAndFeel;`.
    // panelLight previously kept a deliberate micro-depth step lighter than
    // panel; that distinction is GONE now (strict 4-colour hierarchy has no
    // room for a fifth "slightly lighter panel" shade) — both are
    // LightBackground.
    inline static const juce::Colour background { colorTheVoid };
    inline static const juce::Colour panel      { colorHardware };
    inline static const juce::Colour panelLight = CrateColors::LightBackground;
    inline static const juce::Colour accent     { colorNeonCyan };
    inline static const juce::Colour text       { colorTextPrimary };
    inline static const juce::Colour textDim    { colorTextSecondary };

    // Matte pastel for clip blocks (Ableton-style) — desaturated, not a glaring
    // primary. Clip name labels should use textOnClip (dark), not the main `text`
    // colour, for contrast against the lighter pastel fill.
    inline static const juce::Colour clip       { 0xff6fb3c2 };
    inline static const juce::Colour textOnClip { 0xff10181a };

    // Transport LCD block + vital-stats cluster (Zone 1) — shared so any other
    // component that ever needs a "digital readout" look matches exactly.
    inline static const juce::Colour lcdBackground { 0xff0a0a0c };
    inline static const juce::Colour lcdText       { 0xffffb000 }; // amber digital digits
    inline static const juce::Colour ledOff        { 0xff3a3a3e };
    inline static const juce::Colour ledOn         { 0xff2fe08a }; // MIDI activity green
    inline static const juce::Colour meterHot      { colorMuteRed }; // already the exact same hex the Manifesto specifies

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheCrateLookAndFeel)
};
