#include "PluginWindowHeader.h"
#include "../Engine/CrateSandboxBridge.h"

PluginWindowHeader::PluginWindowHeader (CrateSandboxBridge& bridgeToUse)
    : bridge (bridgeToUse)
{
    // Task 2: Universal Dry/Wet Mix. Rotary, 0%-100%, defaulting to
    // whatever the bridge's own current value is (100% wet unless a
    // previous header instance for this SAME bridge already changed it —
    // the bridge, not the header, is the source of truth, so a header
    // recreated for any reason always shows the real current state).
    //
    // Step 41 (Mix Knob UX Rework) directive: styled with the SAME
    // photorealistic filmstrip LookAndFeel the Hybrid Mixer's own pan knob
    // already uses (see mixKnobLookAndFeel's own doc comment) instead of
    // the default JUCE rotary — set BEFORE any other slider configuration
    // so nothing paints with the default LookAndFeel even for one frame.
    // NoTextBox: the built-in JUCE value box is gone entirely, replaced by
    // mixNameLabel/mixValueLabel sitting to the knob's RIGHT (see
    // resized()'s own comment) — a custom display the user can style and
    // position independently of JUCE's own fixed text-box layout options.
    mixSlider.setLookAndFeel (&mixKnobLookAndFeel);
    mixSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    mixSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    mixSlider.setRange (0.0, 100.0, 1.0);
    mixSlider.setValue (bridge.getDryWetMix() * 100.0, juce::dontSendNotification);
    mixSlider.setDoubleClickReturnValue (true, 100.0); // double-click resets to 100% wet (normal, unmixed behaviour)
    mixSlider.onValueChange = [this]
    {
        bridge.setDryWetMix ((float) (mixSlider.getValue() / 100.0));
        updateMixValueLabel();
    };
    addAndMakeVisible (mixSlider);

    mixNameLabel.setText ("MIX", juce::dontSendNotification);
    mixNameLabel.setJustificationType (juce::Justification::centredLeft);
    mixNameLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    mixNameLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (mixNameLabel);

    mixValueLabel.setJustificationType (juce::Justification::centredLeft);
    mixValueLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    addAndMakeVisible (mixValueLabel);
    updateMixValueLabel();

    // Task 4: Quick Bypass. See CrateSandboxBridge::applyDryWetMix()'s own
    // doc comment for why this ramps the SAME mix value to 0.0 (a disclosed
    // design choice — a host-side dry pass-through, not the hosted
    // plugin's own internal bypass logic, which isn't generically reachable
    // across an arbitrary remote VST3 process) rather than a separate
    // mechanism.
    //
    // Step 41 (Bypass Button Layout) directive: buttonColourId set
    // EXPLICITLY (not just buttonOnColourId, which only painted a visible
    // background once already toggled ON) — matches storeAButton/
    // storeBButton's own explicit background colour scheme exactly, rather
    // than inheriting whatever the app-wide default LookAndFeel renders an
    // unstyled TextButton as (which, at rest, apparently reads as "a naked
    // text link" — this button now always has a solid, deliberate
    // background regardless of that).
    //
    // Step 43 (Bypass Neon Focus Ring) directive: setWantsKeyboardFocus(false)
    // — the standard JUCE idiom for "never draw the keyboard-focus outline
    // on this component." A mouse click still normally grants keyboard
    // focus to whatever was clicked, and JUCE's default LookAndFeel draws
    // a focus outline around any focused component that wants one; this
    // is what produced the persistent thin outline after clicking Bypass.
    bypassButton.setButtonText ("Bypass");
    bypassButton.setClickingTogglesState (true);
    bypassButton.setToggleState (bridge.isBypassed(), juce::dontSendNotification);
    bypassButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkgrey);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange.darker (0.2f));
    bypassButton.setWantsKeyboardFocus (false);
    bypassButton.onClick = [this] { bridge.setBypassed (bypassButton.getToggleState()); };
    addAndMakeVisible (bypassButton);

    // Task 4 (Store A/B Logic & Button State), confirmed already wired to
    // the real backend — storeCurrentStateToSlot()/restoreStateFromSlot()
    // are NOT stubs: they call straight into the SAME liveRestoreRequested
    // IPC channel and Continuous State Sync (Step 11 "Muscle Memory")
    // chunk this whole session built and Step 37 hardened the thread
    // safety of. What Step 43 changes here is purely the BUTTON'S OWN
    // visual state (see flashStoreButton()'s own comment) — momentary
    // flash instead of a persistent highlight — not the underlying logic,
    // which was already real.
    // Step 44 (Focus Ring, Round 2) directive: setWantsKeyboardFocus(false)
    // on BOTH — TheCrateLookAndFeel::drawButtonBackground() draws an
    // accent-coloured outline around ANY button that still hasKeyboardFocus(),
    // regardless of buttonColourId (confirmed by reading its own source) —
    // Step 43 only applied this to bypassButton/gearButton, missing these
    // two, which is exactly what left Store A ringed after being clicked.
    storeAButton.setButtonText ("Store A");
    storeAButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkgrey);
    storeAButton.setWantsKeyboardFocus (false);
    storeAButton.onClick = [this]
    {
        // Step 48 (Crash Isolation) directive: logged BEFORE touching
        // anything else — if a crash happens inside this handler with no
        // corresponding "stored slot" line following it in the log, the
        // crash is inside storeCurrentStateToSlot() itself, not
        // flashStoreButton() or anything downstream.
        CrateSandboxBridge::logToSharedLog ("DIAG A/B CLICK: Store A pressed.");
        bridge.storeCurrentStateToSlot ('A'); // real IPC: snapshots lastKnownState into the bridge's own RAM slot A
        flashStoreButton (storeAButton, storeAFlashUntilMs);
    };
    addAndMakeVisible (storeAButton);

    storeBButton.setButtonText ("Store B");
    storeBButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkgrey);
    storeBButton.setWantsKeyboardFocus (false);
    storeBButton.onClick = [this]
    {
        CrateSandboxBridge::logToSharedLog ("DIAG A/B CLICK: Store B pressed.");
        bridge.storeCurrentStateToSlot ('B'); // real IPC: snapshots lastKnownState into the bridge's own RAM slot B
        flashStoreButton (storeBButton, storeBFlashUntilMs);
    };
    addAndMakeVisible (storeBButton);

    // The A/B combo box's onChange is the OTHER real IPC call — pushes the
    // selected slot's bytes over ControlBlock::liveRestoreRequested, which
    // the CHILD applies via hostedPlugin->setStateInformation() on its own
    // next message-thread-equivalent tick (Main.cpp's timerCallback()/
    // serviceTenant(), guarded by pluginAccessLock — see Step 39's own doc
    // comment on that channel for why it's deliberately separate from the
    // crash-resurrection payload).
    abToggle.addItem ("A", 1);
    abToggle.addItem ("B", 2);
    abToggle.setSelectedId (1, juce::dontSendNotification);
    abToggle.onChange = [this]
    {
        const char which = (abToggle.getSelectedId() == 2) ? 'B' : 'A';
        CrateSandboxBridge::logToSharedLog (juce::String ("DIAG A/B CLICK: toggle -> ") + which);
        bridge.restoreStateFromSlot (which); // real IPC: instant, disk-free live state swap
    };
    addAndMakeVisible (abToggle);

    // Task 3: Live Telemetry — latency (samples/ms) and CPU footprint
    // (the hosted plugin's own real processBlock() time, published by
    // AudioBridgeThread — see ControlBlock::lastProcessBlockMicros' own
    // doc comment for why this is deliberately NOT the same figure as the
    // full IPC round-trip time).
    //
    // Step 43 (Telemetry Text Jitter) directive: monospaced, so every
    // digit occupies identical pixel width — see timerCallback()'s own
    // comment for why a monospace font ALONE isn't quite enough and this
    // is paired with fixed-width padding.
    telemetryLabel.setJustificationType (juce::Justification::centredRight);
    // Step 48 (Telemetry Font Pixelation) directive: getDefaultMonospacedFontName()
    // resolves to Courier New on Windows — legitimately blockier/lower-quality
    // at small sizes than a ClearType-optimized face. "Consolas" is a real,
    // genuine improvement; juce::Font falls back to the platform's own
    // default monospace automatically if a named font isn't installed, so
    // this degrades safely on a machine without it rather than failing.
    // (Note: juce::Graphics has no setAntiAliasingEnabled() method in this
    // JUCE version — text anti-aliasing is handled by the OS text renderer,
    // not a per-draw-call toggle, so there's nothing to explicitly enable
    // here; the font swap is the actual fix.)
    telemetryLabel.setFont (juce::Font (juce::FontOptions (13.0f).withName ("Consolas")));
    telemetryLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    telemetryLabel.setInterceptsMouseClicks (false, false); // Step 47: lets a drag started on the telemetry text still move the window
    addAndMakeVisible (telemetryLabel);

    // Step 47/50 (The Unified Title Bar / Header Layout) directive:
    // bridge.getName() is te::Plugin's own name accessor — the SAME
    // authentic, impersonated name (Step 30) previously shown only in
    // JUCE's now-collapsed native title bar (see PluginWindow's own
    // constructor). Painted directly in paint() rather than a Label — see
    // pluginName's own doc comment in the header for why (drawFittedText's
    // automatic ellipsis truncation).
    pluginName = bridge.getName();

    startTimerHz (10); // cheap atomic reads — 10Hz is already faster than the eye can usefully read a changing number
}

PluginWindowHeader::~PluginWindowHeader()
{
    stopTimer();
    mixSlider.setLookAndFeel (nullptr); // standard JUCE ordering discipline — never leave a Component pointing at a LookAndFeel that's about to be destroyed
}

void PluginWindowHeader::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2a2a2e));
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawLine (0.0f, (float) getHeight() - 1.0f, (float) getWidth(), (float) getHeight() - 1.0f, 1.0f);

    // Step 50 (Header Layout Squashing / Text Pixelation) directive:
    // drawFittedText() with maxNumberOfLines=1 automatically truncates
    // with an ellipsis when the text is too wide for pluginNameArea,
    // instead of JUCE's Label default (auto-shrinking the font). Both
    // pluginNameArea (set in resized(), itself built from integer
    // Rectangle arithmetic throughout) and this call's own signature take
    // Rectangle<int> — never a floating-point rect — so this text is
    // always drawn on whole-pixel boundaries.
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawFittedText (pluginName, pluginNameArea, juce::Justification::centred, 1);
}

void PluginWindowHeader::mouseDown (const juce::MouseEvent& e)
{
    if (auto* window = getTopLevelComponent())
        windowDragger.startDraggingComponent (window, e);
}

void PluginWindowHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (auto* window = getTopLevelComponent())
        windowDragger.dragComponent (window, e, nullptr);
}

void PluginWindowHeader::resized()
{
    // Step 54 (Absolute Header Bounds) directive: NO removeFromLeft/
    // removeFromRight chains, NO FlexBox — every control is placed at an
    // ABSOLUTE, explicitly-computed x position via plain setBounds(),
    // with `x` advancing by exactly the same named constants
    // leftClusterWidth (the header's own static constexpr) is summed
    // from. The center title's bounds are then set EXPLICITLY as
    // (leftClusterWidth, 0, getWidth() - leftClusterWidth -
    // rightClusterWidth, getHeight()) — the exact formula the directive
    // specifies — so the title provably receives 100% of whatever
    // horizontal space the two clusters don't occupy, as a direct
    // subtraction, not as the residue of a mutation chain.
    const int h = getHeight();
    const int mixKnobSize = h - 8;
    const int controlHeight = 28;
    const int controlY = (h - controlHeight) / 2;

    int x = sideMargin;

    // Mix knob: centred inside its own fixed-width cluster column.
    juce::Rectangle<int> mixArea (x, 0, mixClusterWidth, h);
    mixSlider.setBounds (mixArea.withSizeKeepingCentre (mixKnobSize, mixKnobSize));
    x += mixClusterWidth + mixTextGap;

    // "MIX" over the live "100%" readout: two stacked halves of the same
    // fixed-width column.
    mixNameLabel.setBounds  (x, 4,         mixTextWidth, (h - 8) / 2);
    mixValueLabel.setBounds (x, 4 + (h - 8) / 2, mixTextWidth, (h - 8) / 2);
    x += mixTextWidth + bypassGap;

    bypassButton.setBounds (x, controlY, bypassWidth, controlHeight);
    x += bypassWidth + abGap;

    // A/B group: Store A (48px column, 2px side insets), Store B (same),
    // then the A/B toggle in the group's remaining width — identical
    // pixel results to the old removeFromLeft(48).reduced(2,0) sequence,
    // just written as direct positions.
    storeAButton.setBounds (x + 2,  controlY, 44, controlHeight);
    storeBButton.setBounds (x + 50, controlY, 44, controlHeight);
    abToggle.setBounds     (x + 98, controlY, abWidth - 100, controlHeight);
    x += abWidth;

    // x now equals leftClusterWidth by construction — both are the sum of
    // the SAME named constants, asserted here so any future edit that
    // breaks that equivalence fails loudly in a debug build instead of
    // silently mis-sizing the title.
    jassert (x == leftClusterWidth);

    // Right cluster: telemetry, pinned to the right edge at its own
    // absolute position.
    telemetryLabel.setBounds (getWidth() - rightClusterWidth, 0, telemetryWidth, h);

    // The center title — the directive's exact formula. jmax guards the
    // degenerate case of a width below the enforced minimum (only ever
    // reachable for one frame during construction, before
    // enforceResizeLimits() lands): a zero-width rectangle, never a
    // negative-width one.
    pluginNameArea = { leftClusterWidth, 0,
                       juce::jmax (0, getWidth() - leftClusterWidth - rightClusterWidth), h };
    repaint(); // painted directly in paint() — see pluginName's own doc comment for why this isn't a Label
}

void PluginWindowHeader::updateMixValueLabel()
{
    mixValueLabel.setText (juce::String (juce::roundToInt (mixSlider.getValue())) + "%", juce::dontSendNotification);
}

void PluginWindowHeader::flashStoreButton (juce::TextButton& button, juce::int64& flashUntilMsOut)
{
    // Step 43 (Store A/B Logic & Button State) directive: a momentary
    // confirmation flash, not a persistent "this slot has data" indicator
    // (the earlier Step 39 design) — the user's own QA pass wants a plain
    // push-button feel: flash green briefly, then ALWAYS settle back to
    // the same neutral colour, whether or not the slot holds data.
    // Reverted from timerCallback() (already ticking at 10Hz) once
    // flashUntilMsOut elapses — no separate one-shot timer mechanism
    // needed for two buttons that can never flash at overlapping,
    // different-duration windows anyway.
    button.setColour (juce::TextButton::buttonColourId, juce::Colours::limegreen);
    flashUntilMsOut = juce::Time::currentTimeMillis() + storeFlashDurationMs;
}

void PluginWindowHeader::timerCallback()
{
    const auto nowMs = juce::Time::currentTimeMillis();

    if (storeAFlashUntilMs != 0 && nowMs >= storeAFlashUntilMs)
    {
        storeAButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkgrey);
        storeAFlashUntilMs = 0;
    }

    if (storeBFlashUntilMs != 0 && nowMs >= storeBFlashUntilMs)
    {
        storeBButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkgrey);
        storeBFlashUntilMs = 0;
    }

    const int latencySamples = bridge.getPluginLatencySamples();
    const double sampleRate = bridge.getCachedSampleRate();
    const double latencyMs = sampleRate > 0.0 ? ((double) latencySamples / sampleRate) * 1000.0 : 0.0;
    const uint32_t cpuMicros = bridge.getLastProcessBlockMicros();

    const juce::String microSign (juce::CharPointer_UTF8 ("\xc2\xb5"));

    // Step 43 (Telemetry Text Jitter) directive: monospace font ALONE only
    // guarantees each individual DIGIT is the same width — the overall
    // string still shifts if the NUMBER OF digits changes (94 -> 104 is
    // still 2 chars -> 3 chars even in a monospace face). Padding every
    // numeric field to a fixed character width (paddedLeft with spaces,
    // right-justified — matches how the eye reads a changing number) means
    // the ENTIRE composed string is a fixed length regardless of value, so
    // combined with the monospace font (set once, in the constructor) the
    // whole label's layout is now genuinely static.
    juce::String text;
    text << "Latency: " << juce::String (latencySamples).paddedLeft (' ', 5) << " smp ("
         << juce::String ((int) latencyMs).paddedLeft (' ', 4) << " ms)"
         << "   |   "
         << "CPU: " << juce::String ((int) cpuMicros).paddedLeft (' ', 4) << " " << microSign << "s";

    telemetryLabel.setText (text, juce::dontSendNotification);
}
