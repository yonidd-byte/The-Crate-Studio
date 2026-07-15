#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

class PianoRollExpressionLane;
class CrateMidiInspectorComponent;

/**
    Zone 4 (MASTER_ARCHITECTURE.md Phase 4 — The MIDI Suite) — the piano-roll
    editing surface that the Arrangement crossfades TO when a te::MidiClip is
    opened (double-click), and back FROM on Escape (Law I: Strict Single-Window
    Paradigm — this is an in-place overlay swap, never a second window or a
    reload).

    X-axis (time — ruler, bar/beat/subdivision grid, Ctrl/Cmd+scroll zoom) was
    built first and is unchanged. This pass adds the Y-axis: real 128-note
    geometry (noteToY()/yToNote() in PianoRollLayout.h — note 127 at the TOP,
    note 0 at the BOTTOM, the inverse of pitch-number order), the
    PianoRollKeyboard sidebar, black-key row shading in the grid, and
    Alt/Option+scroll vertical zoom-to-cursor. Actual note rendering/editing is
    still a later phase — this is row geometry only.

    Structure (mirrors ArrangementComponent's own Ruler+Viewport+Content split):
      - PianoRollRuler: fixed top strip, bar/beat numbers, offset-synced to the
        viewport's horizontal scroll via ScrollAwareViewport::onViewportMoved
        (same mechanism ArrangementComponent uses for its own ruler).
      - PianoRollKeyboard: fixed left sidebar, white/black key rows + "C3"-style
        octave labels, offset-synced to the viewport's VERTICAL scroll the same
        way the ruler syncs to horizontal.
      - viewport (juce::Viewport): scrolls PianoRollGridContent both axes.
      - PianoRollGridContent: the viewed component — time grid lines + black-key
        row shading, and owns the Ctrl/Cmd (horizontal) + Alt/Option (vertical)
        scroll zoom-to-cursor physics.

    Shared X/Y-axis state (pixelsPerBeat/pixelsPerNote, the active clip's beat
    anchor) lives in PianoRollLayout.h's CratePianoRoll namespace, not as
    members threaded through all four pieces above — same single-shared-global
    reasoning ArrangementLayout.h's CrateArrangement namespace already
    established for the identical ruler/grid/zoom-sync problem (this app shows
    exactly one Piano Roll clip open at a time).

    Focus trap (QA requirement): setWantsKeyboardFocus(true) + an explicit
    grabKeyboardFocus() the moment this becomes visible (see setActiveClip())
    means the Escape key registers immediately, with no stray click needed
    first. keyPressed() consumes Escape and fires onExitRequested; every other
    key falls through (returns false) so it never swallows, e.g., a future
    transport Spacebar handler.
*/
class CratePianoRollComponent : public juce::Component
{
public:
    CratePianoRollComponent();
    ~CratePianoRollComponent() override;

    /** Points the editor at a clip (or nullptr to clear), re-anchors the
        shared X-axis state (CratePianoRoll::activeEdit/clipStartBeat) to it,
        relays out the ruler/grid, and — if non-null and on-screen — grabs
        keyboard focus so Escape works without a preliminary click. Called by
        MainComponent as part of the crossfade INTO this view. */
    void setActiveClip (te::MidiClip* clip);

    te::MidiClip* getActiveClip() const noexcept   { return activeClip; }

    /** Wire the MIDI Inspector so the grid can access scale/chord state for
        dimming, scale-snap, and stamp mode. */
    void setInspectorComponent (CrateMidiInspectorComponent* insp) noexcept;

    /** Fires when the user presses Escape — MainComponent reverses the crossfade
        (restores Arrangement + Browser). This component does NOT hide itself;
        the owner drives visibility, same "view owns the crossfade, not the
        panel" split MainComponent already uses for Arrangement/Mixer. */
    std::function<void()> onExitRequested;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool keyPressed (const juce::KeyPress&) override;

private:
    class PianoRollRuler;       // fixed top bar/beat strip
    class PianoRollKeyboard;    // fixed left white/black-key sidebar
    class PianoRollGridContent; // viewport's viewed component — grid lines/shading + zoom physics

    // juce::Viewport has no public Listener/observer interface — same
    // subclass-and-override-the-protected-virtual approach ArrangementComponent's
    // own ScrollAwareViewport already established for the identical
    // ruler-follows-scroll problem.
    class ScrollAwareViewport : public juce::Viewport
    {
    public:
        std::function<void (int x, int y)> onViewportMoved;

    private:
        void visibleAreaChanged (const juce::Rectangle<int>& newVisibleArea) override
        {
            juce::Viewport::visibleAreaChanged (newVisibleArea);

            if (onViewportMoved)
                onViewportMoved (newVisibleArea.getX(), newVisibleArea.getY());
        }
    };

    // Recomputes gridContent's size from the active clip's length (in beats,
    // via CratePianoRoll::pixelsPerBeat) and the full 128-note height (via
    // CratePianoRoll::pixelsPerNote), and positions ruler/keyboard/viewport
    // against this component's current bounds. Called from resized(),
    // setActiveClip(), and (via PianoRollGridContent::onZoomChanged) after
    // every zoom change, horizontal or vertical.
    void layoutContent();

    te::MidiClip* activeClip = nullptr; // raw: owned by its track's clip list; MainComponent
                                        // clears this (exitMidiEditor) before any teardown that
                                        // could free it, same raw-clip convention ClipComponent uses

    std::unique_ptr<PianoRollRuler> ruler;
    std::unique_ptr<PianoRollKeyboard> keyboard;
    ScrollAwareViewport viewport;
    std::unique_ptr<PianoRollGridContent> gridContent;
    std::unique_ptr<PianoRollExpressionLane> expressionLane; // bottom 120px — velocity/CC rendering

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CratePianoRollComponent)
};
