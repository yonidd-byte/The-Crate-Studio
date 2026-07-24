#pragma once

#include <JuceHeader.h>
#include "CrateMixerLookAndFeel.h"

class CrateSandboxBridge;

/**
    Step 39 (The Universal Plugin Header) directive, Task 1: a slim, fixed-
    height strip docked above a sandboxed plugin's reparented editor inside
    PluginWindow — surfacing backend capabilities Phases 1-4 (and the Step
    38 Debt Sweep) already built, but never exposed anywhere in the native
    UI: Dry/Wet mix (Task 2), live latency/CPU telemetry (Task 3), Quick
    Bypass and A/B state-swap testing (Task 4).

    ONLY ever constructed for a CrateSandboxBridge-hosted plugin — see
    PluginWindowContent's own doc comment (PluginWindow.cpp) for the
    dynamic_cast gate that decides this. Every control here calls straight
    into CrateSandboxBridge's own public API (setDryWetMix()/setBypassed()/
    storeCurrentStateToSlot()/restoreStateFromSlot()/getPluginLatencySamples()/
    getLastProcessBlockMicros()), which simply doesn't exist for a native,
    non-sandboxed plugin — a native plugin's PluginWindow never constructs
    this at all, so its behaviour is completely unaffected by Step 39.

    Forward-declares CrateSandboxBridge rather than including its header —
    this Component only ever calls a handful of small, already-public
    methods on it, and keeping CrateSandboxBridge.h (a very large file) out
    of every UI translation unit that eventually includes this one is worth
    the forward declaration.
*/
class PluginWindowHeader : public juce::Component,
                           private juce::Timer
{
public:
    explicit PluginWindowHeader (CrateSandboxBridge& bridgeToUse);
    ~PluginWindowHeader() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Step 47 (The Unified Title Bar) directive, Task 1: this header IS
    // the window's title bar now (JUCE's own native one is collapsed to
    // 0 height — see PluginWindow's own constructor) — clicking and
    // dragging any part of the header's own background (not a control)
    // moves the whole window, the same way dragging a native title bar
    // would. Labels below (pluginNameLabel/telemetryLabel) are configured
    // NOT to intercept mouse clicks specifically so a drag started
    // directly on top of the title text or telemetry readout still works,
    // matching how dragging a real title bar's own text works.
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    // Step 41 (Mix Knob UX Rework) directive: bumped again, 56 -> 64 — the
    // knob now fills nearly the WHOLE strip height (no separate "MIX"
    // caption docked underneath it any more; see resized()'s own comment
    // for the new side-by-side layout), so it needed a bit more room to
    // read as genuinely prominent rather than merely "not tiny."
    static constexpr int headerHeight = 64;

    // Step 53 (Defensive Header Geometry) directive: EVERY fixed-width
    // section resized() (the .cpp) lays out, as ONE set of named
    // constants BOTH resized() AND minimumHeaderContentWidth below are
    // built from — replacing Step 50's version, which computed the same
    // total by hand in TWO independent places (this constant's own
    // arithmetic, and resized()'s own sequence of literal numbers) that
    // had already silently drifted apart: Step 41 added the
    // mixNameLabel/mixValueLabel text pair beside the mix knob
    // (mixTextGap + mixTextWidth, 62px total) to resized()'s own layout,
    // but this constant was never updated to account for it, so the
    // enforced floor was 62px SMALLER than what resized() actually
    // needed — confirmed root cause of the QA-reported "header elements
    // draw on top of each other when resized narrow" bug. A single set of
    // named constants, referenced by BOTH sides, makes that drift
    // structurally impossible instead of merely avoidable by careful
    // hand-arithmetic.
    static constexpr int sideMargin = 4;                            // absolute left/right inset (was reduced(4))
    static constexpr int mixClusterWidth = (headerHeight - 8) + 8;  // knob + its own right-hand gap
    static constexpr int mixTextGap = 6, mixTextWidth = 56;
    static constexpr int bypassGap = 14, bypassWidth = 64;
    static constexpr int abGap = 14, abWidth = 150;
    static constexpr int telemetryWidth = 300;

    // Step 54 (Absolute Header Bounds) directive: the two cluster widths
    // as first-class named constants — resized() (the .cpp) places every
    // control at ABSOLUTE x positions summing to exactly these, and the
    // center title's bounds are set EXPLICITLY as
    // (leftClusterWidth, 0, getWidth() - leftClusterWidth -
    // rightClusterWidth, getHeight()) — no removeFromLeft/removeFromRight
    // chains anywhere, so there is no call-order arithmetic left to
    // silently drift.
    static constexpr int leftClusterWidth = sideMargin
                                            + mixClusterWidth + mixTextGap + mixTextWidth
                                            + bypassGap + bypassWidth
                                            + abGap + abWidth;
    static constexpr int rightClusterWidth = telemetryWidth + sideMargin;

    // Step 54 directive: the title's guaranteed breathing room — the host
    // window physically cannot be shrunk to where the center area gets
    // less than this.
    static constexpr int minimumCenterTextWidth = 150;

    // The hard floor — see PluginWindow.cpp's own
    // PluginWindowContent::enforceResizeLimits() for where this is
    // ACTUALLY applied to the host window's real, active
    // ComponentBoundsConstrainer (Step 53 found that the outer
    // DocumentWindow's own setResizeLimits() is a silent no-op once
    // setEditor() has installed a CUSTOM constrainer). Step 54 directive:
    // exactly leftClusterWidth + rightClusterWidth + 150.
    static constexpr int minimumHeaderContentWidth = leftClusterWidth
                                                     + minimumCenterTextWidth
                                                     + rightClusterWidth;

private:
    void timerCallback() override;
    void flashStoreButton (juce::TextButton& button, juce::int64& flashUntilMsOut);
    void updateMixValueLabel();

    CrateSandboxBridge& bridge;

    // Step 47 (The Unified Title Bar) directive, Task 1: the plugin's own
    // display name (bridge.getName() — the SAME name Step 30's "Name
    // Impersonation" already makes authentic, previously shown only in
    // JUCE's now-collapsed native title bar) — centred in whatever middle
    // space remains between the left-side controls and the right-side
    // telemetry readout.
    //
    // Step 50 (Header Layout Squashing) directive: a plain juce::String +
    // Rectangle painted directly in paint() via Graphics::drawFittedText(),
    // NOT a juce::Label — drawFittedText() automatically ellipsis-truncates
    // ("...") a single line of text that doesn't fit its given area, which
    // is the exact behaviour asked for and Label doesn't do by default
    // (Label instead auto-shrinks its font to fit, never truncating).
    // pluginNameArea is always an integer Rectangle (set in resized(),
    // itself built from other integer Rectangles) — text is never drawn
    // against floating-point/sub-pixel coordinates here.
    juce::String pluginName;
    juce::Rectangle<int> pluginNameArea;

    juce::ComponentDragger windowDragger;

    // Step 40 (Mix Knob UX) directive: reuses the SAME photorealistic
    // filmstrip-knob rendering already built for the Hybrid Mixer
    // (CrateMixerLookAndFeel::drawRotarySlider(), loading Assets/pan_knob.png
    // via BinaryData) rather than a new custom LookAndFeel from scratch —
    // one already-polished, already-proven pan-knob look, reused here
    // instead of a second implementation of the same rendering.
    CrateMixerLookAndFeel mixKnobLookAndFeel;

    juce::Slider mixSlider;

    // Step 41 (Mix Knob UX Rework) directive: replaces the old single
    // "MIX" caption docked below the knob — the built-in JUCE text box is
    // gone entirely (NoTextBox), and this pair now sits to the knob's
    // RIGHT instead: mixNameLabel ("MIX", small/bold) stacked above
    // mixValueLabel (the live "100%" readout, larger/prominent), updated
    // from mixSlider's own onValueChange rather than JUCE's built-in box.
    juce::Label mixNameLabel;
    juce::Label mixValueLabel;

    juce::TextButton bypassButton;

    // Step 43 (Store A/B Logic & Button State) directive: momentary
    // triggers, not toggles — storeAButton/storeBButton flash green for
    // storeFlashDurationMs after a click (see flashStoreButton()'s own
    // comment), then revert to this SAME neutral colour regardless of
    // whether that slot holds data — replaces the earlier Step 39 design
    // (permanent green = "has data"), which the user's own QA pass flagged
    // as buttons that "stay highlighted" when a plain click confirmation
    // was what was actually wanted.
    juce::TextButton storeAButton, storeBButton;
    juce::int64 storeAFlashUntilMs = 0, storeBFlashUntilMs = 0;
    static constexpr int storeFlashDurationMs = 350;

    juce::ComboBox abToggle;

    juce::Label telemetryLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindowHeader)
};
