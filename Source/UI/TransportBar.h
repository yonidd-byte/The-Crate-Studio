#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/** Play / Stop / Record / Loop / Undo / Redo / Save / Load, bound to
    CrateWorkflowManager. Zone 1 of the shell (MASTER_ARCHITECTURE.md 0.3):
    thin (40-50px), flat, dark, icon-driven — no large text buttons for
    transport actions, a center LCD block (Bars.Beats / Time), and a right-side
    vital-stats cluster (BPM, CPU meter, MIDI activity LED). */
class TransportBar : public juce::Component,
                      private juce::Timer,
                      private te::MidiInputDevice::MidiKeyChangeDispatcher::Listener
{
public:
    explicit TransportBar (CrateWorkflowManager& workflowToUse);
    ~TransportBar() override;

    /** Fired when the Load button is clicked — TransportBar does nothing with
        CrateWorkflowManager::safeLoadProject() itself. MainComponent owns that call
        (and its before/after-swap callbacks) directly, since it's the only thing
        that can safely destroy TransportBar mid-load: routing the destructive
        callback through a std::function member OF TransportBar (as this used to
        do) meant destroying TransportBar from inside its own stored callback — a
        real use-after-free, not just a style preference. This callback fires and
        returns immediately, before anything destructive happens, so there's no
        such hazard here. */
    std::function<void()> onLoadRequested;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Four-Zone Shell progressive-disclosure toggles (MASTER_ARCHITECTURE.md Law II).
    // TransportBar doesn't own the shell's zone visibility state — MainComponent
    // does, since it owns the Grid these buttons' clicks reflow — it just hosts the
    // controls and wires their onClick externally, same pattern as every other
    // button here being wired by its owner rather than doing the work itself.
    juce::TextButton toggleBrowserButton     { "Browser" };
    juce::TextButton toggleDeviceChainButton { "Device Chain" };

    // Center Zone view navigation (Arrangement <-> Mixer, Law I: setVisible() only,
    // never a reload) — two persistent, always-visible segmented buttons (a JUCE
    // radio group, mutually exclusive), NOT a single momentary toggle. That
    // earlier design let a click silently do nothing while the Zone-4 MIDI
    // editor overlay was up (the toggle flipped state UNDERNEATH the
    // still-visible piano roll); MainComponent's onClick for each now
    // explicitly calls exitMidiEditor() too, so either button is always a
    // working "return to Timeline/Mixer" affordance regardless of current
    // state. Toggle-highlight (which one reads as "active") is synced by the
    // owner via setToggleState() every time the center view actually changes —
    // required because view changes can also be driven by Escape or the MIDI
    // Suite crossfade, not just a click on these two buttons.
    //
    // Named TIMELINE/MIXER, not ARRANGE/MIX — sleeker, and "ARRANGE" was
    // truncating in its old 56px slot; see resized() for the fixed-width fix.
    juce::TextButton timelineButton { "TIMELINE" };
    juce::TextButton mixerButton    { "MIXER" };

private:
    // A flat, minimal vector-icon transport button — Play/Stop/Record/Loop/Undo/Redo
    // all draw a juce::Path glyph instead of text (Zone 1 spec: "Kill the Text").
    class IconButton : public juce::Button
    {
    public:
        enum class Icon { play, stop, record, loop, undo, redo };

        explicit IconButton (Icon iconToUse) : juce::Button ({}), icon (iconToUse)
        {
            // Transport buttons are mouse-only controls in this app; keyboard
            // focus stays with the main view so the global Spacebar Play/Stop
            // shortcut (MainComponent::keyPressed) keeps working after clicking
            // one of these. Without this, JUCE's default Button behaviour would
            // let a just-clicked button "steal" focus and then consume Spacebar
            // itself (triggering its own click) instead of letting it bubble up.
            setWantsKeyboardFocus (false);
        }

        void setIconColour (juce::Colour c)   { iconColour = c; repaint(); }

    private:
        void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        Icon icon;
        juce::Colour iconColour = juce::Colours::white;
    };

    // Dark digital-style readout: Bars.Beats on the left, elapsed Time on the right,
    // separated by a thin divider — "LCD Display" per Zone 1 spec.
    class LcdDisplay : public juce::Component
    {
    public:
        void setValues (const juce::String& barsBeats, const juce::String& time);
        void paint (juce::Graphics&) override;

    private:
        juce::String barsBeatsText { "1 . 1 . 1" };
        juce::String timeText { "00:00:00" };
    };

    // Compact horizontal progress-bar CPU meter placeholder.
    class CpuMeter : public juce::Component
    {
    public:
        void setLoad (float proportion0to1);
        void paint (juce::Graphics&) override;

    private:
        float load = 0.0f;
    };

    // 6px circular MIDI activity indicator.
    class MidiLed : public juce::Component
    {
    public:
        void setLit (bool shouldBeLit);
        void paint (juce::Graphics&) override;

    private:
        bool lit = false;
    };

    // Interactive BPM readout — Ableton-style click-drag tempo edit. Drag up/down
    // to raise/lower tempo; the whole gesture (mouseDown through mouseUp) is one
    // Undo transaction, not one transaction per pixel of drag.
    class DraggableBpmLabel : public juce::Component
    {
    public:
        explicit DraggableBpmLabel (CrateWorkflowManager& workflowToUse) : workflow (workflowToUse) {}

        void setDisplayText (const juce::String& text);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;

    private:
        double currentBpm() const;

        CrateWorkflowManager& workflow;
        juce::String displayText { "120.0 BPM" };
        double dragStartBpm = 120.0;
    };

    void timerCallback() override;
    void updateButtonStates();
    void updateLcdAndStats();

    // te::MidiInputDevice::MidiKeyChangeDispatcher::Listener — fires (lazily, on the
    // message thread, per its own doc comment) whenever any MIDI input plays a note
    // for any track. Not an audio-thread callback, so no locking/lock-free queue is
    // needed here; it just flags the LED to flash on the next timer tick.
    void midiKeyStateChanged (te::AudioTrack*, const juce::Array<int>& notesOn,
                               const juce::Array<int>&, const juce::Array<int>&) override;

    CrateWorkflowManager& workflow;

    IconButton playButton     { IconButton::Icon::play };
    IconButton stopButton     { IconButton::Icon::stop };
    IconButton recordButton   { IconButton::Icon::record };
    IconButton loopButton     { IconButton::Icon::loop };
    IconButton undoButton     { IconButton::Icon::undo };
    IconButton redoButton     { IconButton::Icon::redo };

    juce::TextButton saveButton { "Save" };
    juce::TextButton loadButton { "Load" };
    juce::TextButton pluginsButton { "Plugins" };
    juce::TextButton settingsButton { "Settings" };

    LcdDisplay lcd;

    DraggableBpmLabel bpmLabel { workflow };
    CpuMeter cpuMeter;
    MidiLed midiLed;

    juce::SharedResourcePointer<te::MidiInputDevice::MidiKeyChangeDispatcher> midiDispatcher;
    juce::int64 lastMidiActivityMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
