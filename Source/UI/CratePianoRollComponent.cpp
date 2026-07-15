#include "CratePianoRollComponent.h"
#include "PianoRollLayout.h"
#include "TheCrateLookAndFeel.h"
#include "PianoRollExpressionLane.h"
#include "CrateMidiInspectorComponent.h"

using namespace CratePianoRoll;

namespace
{
    using LAF = TheCrateLookAndFeel;

    // THEME FIX: the deep-indigo debug panel is gone — the piano roll now uses
    // the SAME shared charcoal palette every other zone already draws from,
    // rather than a one-off hex. Rule: whatever LAF colour occupied a given
    // Grid cell before this overlay took it over, the overlay uses that same
    // colour in that same cell — pianoRollBackground matches
    // ArrangementComponent::paint()'s LAF::panel (this replaces Arrangement in
    // the center cell); CrateMidiInspectorComponent.cpp separately matches
    // LAF::background (it replaces the Browser in the left cell).
    const auto pianoRollBackground = LAF::panel;

    // Subtle transparent white per spec (~0.05) — toned down from an earlier
    // pass's much stronger 0.30/0.14/0.045, which read as heavy/"debug" rather
    // than a professional, quiet grid. Bar > Beat (1/4 note) > Subdivision
    // (1/16 note) tiering is preserved (still needed for readability at a
    // glance), just at a genuinely subtle level now.
    const auto gridBarColour       = juce::Colours::white.withAlpha (0.10f);
    const auto gridBeatColour      = juce::Colours::white.withAlpha (0.05f);
    const auto gridSixteenthColour = juce::Colours::white.withAlpha (0.025f);

    // FL Studio-style black-key row guide, drawn across the WHOLE timeline
    // (not just under the keyboard) so a note dragged far to the right still
    // reads against the same pitch reference. Toned down alongside the grid
    // lines above, same "subtle, not heavy" theme fix.
    const auto blackKeyRowColour = juce::Colours::black.withAlpha (0.10f);

    // Keyboard sidebar's own key colours — deliberately distinct from the
    // grid's row-shading colour above (a translucent overlay vs. the sidebar's
    // own opaque key faces), same two-different-jobs split TrackHeaderComponent
    // and MixerStrip already keep for the "same fact, two views" pattern. A
    // near-white/near-black pair, not literal juce::Colours::white/black, so
    // they still sit comfortably in the app's dark theme rather than glaring.
    const auto keyboardWhiteKeyColour  = juce::Colour (0xffd8d8dc);
    const auto keyboardBlackKeyColour  = juce::Colour (0xff1a1a1e);
    const auto keyboardSeparatorColour = juce::Colours::black.withAlpha (0.4f);

    // Vibrant, distinct MIDI-note colour — deliberately NOT LAF::clip (the
    // muted pastel Arrangement uses for audio-clip blocks); notes need to read
    // clearly against the grid at a glance, closer to FL Studio's own vivid
    // note-block green.
    const auto midiNoteColour = juce::Colour (0xff2ecc8f);
}

//==============================================================================
// Fixed top strip — bar/beat numbers only (no note-name gutter yet, that's a
// Y-axis concern). horizontalOffset is kept in sync with the viewport's scroll
// position via CratePianoRollComponent's ScrollAwareViewport::onViewportMoved,
// same mechanism/contract as TimeRulerComponent::setHorizontalOffset().
class CratePianoRollComponent::PianoRollRuler : public juce::Component
{
public:
    PianoRollRuler() = default;

    void setHorizontalOffset (int newOffsetPx)
    {
        if (horizontalOffset == newOffsetPx)
            return;

        horizontalOffset = newOffsetPx;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LAF::background);

        if (activeEdit == nullptr)
            return;

        const auto w = getWidth();
        const auto h = (float) getHeight();

        double firstLocalBeat, lastLocalBeat;
        visibleLocalBeatRange (horizontalOffset, horizontalOffset + w, firstLocalBeat, lastLocalBeat);

        for (int beat = (int) std::floor (firstLocalBeat); beat <= (int) std::ceil (lastLocalBeat); ++beat)
        {
            if (beat < 0)
                continue;

            // Real tempo/time-signature lookup (via the Edit's own
            // tempoSequence), not an assumed fixed 4/4 — toBarsAndBeats()
            // authoritatively answers "is this beat a bar boundary, and which
            // bar", correctly even across a time-signature change.
            const auto absoluteBeat = clipStartBeat + tracktion::BeatDuration::fromBeats ((double) beat);
            const auto barsAndBeats = activeEdit->tempoSequence.toBarsAndBeats (activeEdit->tempoSequence.toTime (absoluteBeat));
            const bool isBarLine = std::abs (barsAndBeats.beats.inBeats()) < 1.0e-6;

            const float x = (float) (beat * pixelsPerBeat) - (float) horizontalOffset;

            g.setColour (isBarLine ? LAF::textDim : LAF::panelLight);
            g.drawVerticalLine ((int) x, isBarLine ? 2.0f : h * 0.55f, h);

            if (isBarLine)
            {
                g.setColour (LAF::text);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (juce::String (barsAndBeats.bars + 1),
                            juce::Rectangle<float> (x + 3.0f, 1.0f, 30.0f, h - 2.0f),
                            juce::Justification::centredLeft);
            }
        }

        g.setColour (LAF::panelLight);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) w);
    }

private:
    int horizontalOffset = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollRuler)
};

//==============================================================================
// Fixed left sidebar — one row per MIDI note (0-127), white/black key faces
// plus a "C3"-style label at each C. verticalOffset is kept in sync with the
// viewport's VERTICAL scroll via CratePianoRollComponent's ScrollAwareViewport::
// onViewportMoved, the exact same offset-tracking contract PianoRollRuler uses
// for horizontal scroll — this sidebar is that same idea, rotated 90 degrees.
class CratePianoRollComponent::PianoRollKeyboard : public juce::Component
{
public:
    PianoRollKeyboard() = default;

    void setVerticalOffset (int newOffsetPx)
    {
        if (verticalOffset == newOffsetPx)
            return;

        verticalOffset = newOffsetPx;
        repaint();
    }

    void setInspectorComponent (CrateMidiInspectorComponent* insp)
    {
        if (inspector == insp)
            return;

        inspector = insp;
        repaint();
    }

    // ABLETON-STYLE 12 EQUAL ROWS. The interlocking-piano geometry (previous
    // pass) gave visually uniform white keys but violated the Golden Rule this
    // view actually needs: EVERY note — grid row, MIDI note, keyboard key —
    // must be the exact same height h = pixelsPerNote, with zero exceptions,
    // so a note block placed in the grid lines up 1:1 with its key. A real
    // piano's unequal white-key heights look nice on their own but misalign
    // against a note grid that (correctly) gives every semitone equal space.
    // One pass, one row height, no group math: black keys get a 12px left
    // indent (a visual hint, not a geometry change) but stay exactly h tall
    // like everything else.
    void paint (juce::Graphics& g) override
    {
        g.fillAll (LAF::background);

        const auto w = (float) getWidth();
        const auto hTotal = getHeight();
        const float h = (float) pixelsPerNote;

        int lowestNote, highestNote;
        visibleNoteRange ((float) verticalOffset, (float) (verticalOffset + hTotal), lowestNote, highestNote);

        const bool drawLabels = pixelsPerNote >= 12.0f;

        for (int note = lowestNote; note <= highestNote; ++note)
        {
            const float y = noteToY (note) - (float) verticalOffset; // exact float math — the Golden Rule

            if (isBlackKey (note))
            {
                g.setColour (keyboardBlackKeyColour);
                g.fillRect (juce::Rectangle<float> (12.0f, y, w - 12.0f, h)); // 12px indent hints "piano"
                                                                              // without changing row height
            }
            else
            {
                g.setColour (keyboardWhiteKeyColour);
                g.fillRect (juce::Rectangle<float> (0.0f, y, w, h));
            }

            // Crisp 1px separator at the exact float bottom of EVERY key
            // (black and white alike) — equal rows means equal treatment,
            // no per-pitch-class special case needed anywhere in this pass.
            g.setColour (keyboardSeparatorColour);
            g.fillRect (juce::Rectangle<float> (0.0f, y + h, w, 1.0f));

            if (drawLabels && ! isBlackKey (note))
            {
                g.setColour (LAF::textOnClip); // dark text — white keys only, over their own face
                g.setFont (juce::FontOptions (9.5f));
                g.drawText (juce::MidiMessage::getMidiNoteName (note, true, true, 3),
                            juce::Rectangle<float> (0.0f, y, w, h).reduced (4.0f, 0.0f),
                            juce::Justification::centredRight);
            }
        }

        // KEYBOARD DIMMING: overlay dark rectangles on keys not in the active scale.
        if (inspector != nullptr && inspector->isSnapToScaleEnabled())
        {
            const int rootNote = inspector->getRootNote();
            const int scaleType = inspector->getScaleType();

            static const juce::Array<int> majorIntervals   { 0, 2, 4, 5, 7, 9, 11 };
            static const juce::Array<int> minorIntervals   { 0, 2, 3, 5, 7, 8, 10 };
            static const juce::Array<int> pentatonicIntervals { 0, 2, 4, 7, 9 };
            static const juce::Array<int> bluesIntervals   { 0, 3, 5, 6, 7, 10 };

            const juce::Array<int>* activeScale = nullptr;
            switch (scaleType)
            {
                case 0: activeScale = &majorIntervals; break;
                case 1: activeScale = &minorIntervals; break;
                case 2: activeScale = &pentatonicIntervals; break;
                case 3: activeScale = &bluesIntervals; break;
                case 4: activeScale = nullptr; break; // Chromatic
            }

            if (activeScale != nullptr)
            {
                g.setColour (juce::Colours::black.withAlpha (0.3f));

                for (int note = lowestNote; note <= highestNote; ++note)
                {
                    const int pitchClass = note % 12;
                    const int adjustedPitchClass = (pitchClass - rootNote + 12) % 12;

                    if (!activeScale->contains (adjustedPitchClass))
                    {
                        const float y = noteToY (note) - (float) verticalOffset;
                        g.fillRect (juce::Rectangle<float> (0.0f, y, w, h));
                    }
                }
            }
        }

        g.setColour (LAF::background);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) hTotal); // divider against the ruler/grid
    }

private:
    int verticalOffset = 0;
    CrateMidiInspectorComponent* inspector = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollKeyboard)
};

//==============================================================================
// The Viewport's viewed component — time grid lines + black-key row shading,
// plus the Ctrl/Cmd (horizontal) and Alt/Option (vertical) scroll
// zoom-to-cursor physics. Plain scroll
// (no modifier) is deliberately left to the base Component::mouseWheelMove()
// implementation, whose default behaviour forwards the event up to the nearest
// enabled ancestor — the owning juce::Viewport — which performs its own normal
// two-axis scroll. This is a real, load-bearing pass-through, not a missing
// override: verified against JUCE's own Component::mouseWheelMove() default
// body (juce_Component.cpp), which exists specifically for this purpose.
class CratePianoRollComponent::PianoRollGridContent : public juce::Component,
                                                       private juce::ValueTree::Listener
{
public:
    PianoRollGridContent() = default; // JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR below
                                       // deletes the copy ctor, which suppresses the implicit
                                       // default one too — make_unique<>() needs this explicit

    // Defensive teardown: if this component is destroyed while still
    // registered on a clip's sequence (e.g. a project Load happens while the
    // MIDI editor is open, WITHOUT the user pressing Escape/Arrange first —
    // CratePianoRollComponent::setActiveClip (nullptr) never runs in that
    // path), an un-removed juce::ValueTree::Listener would leave a dangling
    // pointer inside that ValueTree's listener list, which the old Edit's own
    // teardown could then call into after this object is gone. Always safe to
    // call unconditionally — removeListener() on a tree this was never added
    // to is a documented no-op.
    ~PianoRollGridContent() override
    {
        if (activeMidiClip != nullptr)
            activeMidiClip->getSequence().state.removeListener (this);
    }

    // Called by CratePianoRollComponent::setActiveClip() whenever the open
    // clip changes (including to/from nullptr) — keeps this component's
    // ValueTree::Listener registration pointed at exactly the current clip's
    // MidiList, so external mutations (Undo/Redo of a note add/delete/resize,
    // in particular — this component's OWN mouseDown/mouseDrag mutations
    // already repaint() directly and don't depend on this) still trigger a
    // repaint. Takes explicit old/new pointers rather than reading the shared
    // CratePianoRoll::activeMidiClip global, so detach-then-attach ordering can
    // never depend on when the caller updates that global relative to this call.
    void setSequenceListenerTarget (te::MidiClip* oldClip, te::MidiClip* newClip)
    {
        if (oldClip != nullptr)
            oldClip->getSequence().state.removeListener (this);

        if (newClip != nullptr)
            newClip->getSequence().state.addListener (this);
    }

    void setInspectorComponent (CrateMidiInspectorComponent* insp) noexcept { inspector = insp; }

    // Fired after pixelsPerBeat changes (Ctrl/Cmd+scroll) — the owner resizes
    // THIS component to the new content width, synchronously, before the zoom
    // handler below repositions the viewport's scroll to re-anchor the cursor.
    std::function<void()> onZoomChanged;

    void paint (juce::Graphics& g) override
    {
        // Darker canvas than the outer panel — matches ArrangementComponent::
        // TrackListContent's identical LAF::background-over-LAF::panel
        // convention for its own scrollable grid-vs-frame relationship.
        g.fillAll (LAF::background);

        if (activeEdit == nullptr)
            return;

        const auto clipBounds = g.getClipBounds();

        // Row shading FIRST, underneath the time grid lines drawn below — FL
        // Studio-style black-key guide across the whole visible timeline. Only
        // the visible note range is touched (g.getClipBounds() already gives
        // that in this component's own local Y, same "don't draw off-screen"
        // discipline the X-axis loop below already uses).
        int lowestNote, highestNote;
        visibleNoteRange ((float) clipBounds.getY(), (float) clipBounds.getBottom(), lowestNote, highestNote);

        for (int note = lowestNote; note <= highestNote; ++note)
        {
            if (! isBlackKey (note))
                continue;

            g.setColour (blackKeyRowColour);
            g.fillRect (0.0f, noteToY (note), (float) getWidth(), (float) pixelsPerNote);
        }

        // SCALE DIMMING: darken rows that don't belong to the active scale.
        if (inspector != nullptr && inspector->isSnapToScaleEnabled())
        {
            const int rootNote = inspector->getRootNote(); // 0-11 (C to B)
            const int scaleType = inspector->getScaleType();

            // Scale interval arrays (semitone offsets from root within octave).
            static const juce::Array<int> majorIntervals   { 0, 2, 4, 5, 7, 9, 11 };
            static const juce::Array<int> minorIntervals   { 0, 2, 3, 5, 7, 8, 10 };
            static const juce::Array<int> pentatonicIntervals { 0, 2, 4, 7, 9 };
            static const juce::Array<int> bluesIntervals   { 0, 3, 5, 6, 7, 10 };

            const juce::Array<int>* activeScale = nullptr;
            switch (scaleType)
            {
                case 0: activeScale = &majorIntervals; break;      // Major
                case 1: activeScale = &minorIntervals; break;      // Minor
                case 2: activeScale = &pentatonicIntervals; break; // Pentatonic
                case 3: activeScale = &bluesIntervals; break;      // Blues
                case 4: activeScale = nullptr; break;              // Chromatic (no dimming)
            }

            if (activeScale != nullptr)
            {
                // Highlight scale lanes: root note brighter, in-scale notes subtle.
                for (int note = 0; note < 128; ++note)
                {
                    const int pitchClass = note % 12;
                    const int adjustedPitchClass = (pitchClass - rootNote + 12) % 12;

                    if (activeScale->contains (adjustedPitchClass))
                    {
                        const float y = noteToY (note);
                        // Root note gets brighter highlight.
                        if (adjustedPitchClass == 0)
                        {
                            g.setColour (juce::Colours::white.withAlpha (0.06f));
                        }
                        else
                        {
                            g.setColour (juce::Colours::white.withAlpha (0.03f));
                        }
                        g.fillRect (0.0f, y, (float) getWidth(), (float) pixelsPerNote);
                    }
                    else
                    {
                        // Out-of-scale rows stay dimmed (optional, comment out if not needed).
                        const float y = noteToY (note);
                        g.setColour (juce::Colours::black.withAlpha (0.06f));
                        g.fillRect (0.0f, y, (float) getWidth(), (float) pixelsPerNote);
                    }
                }
            }
        }

        double firstLocalBeat, lastLocalBeat;
        visibleLocalBeatRange (clipBounds.getX(), clipBounds.getRight(), firstLocalBeat, lastLocalBeat);

        // 1/16-note subdivisions only once they're actually legible on screen —
        // below that pixel-spacing threshold they're just visual noise, so fall
        // back to beat-level (1/4 note) granularity instead.
        const double pixelsPerSixteenth = pixelsPerBeat * 0.25;
        const bool drawSixteenths = pixelsPerSixteenth >= 6.0;
        const double step = drawSixteenths ? 0.25 : 1.0;

        const double startBeat = std::floor (firstLocalBeat / step) * step;
        const auto height = (float) getHeight();

        for (double lb = startBeat; lb <= lastLocalBeat; lb += step)
        {
            if (lb < 0.0)
                continue;

            const auto absoluteBeat = clipStartBeat + tracktion::BeatDuration::fromBeats (lb);
            const auto barsAndBeats = activeEdit->tempoSequence.toBarsAndBeats (activeEdit->tempoSequence.toTime (absoluteBeat));
            const bool isBarLine  = std::abs (barsAndBeats.beats.inBeats()) < 1.0e-6;
            const bool isBeatLine = std::abs (lb - std::round (lb)) < 1.0e-6;

            g.setColour (isBarLine ? gridBarColour : (isBeatLine ? gridBeatColour : gridSixteenthColour));
            g.drawVerticalLine ((int) std::round (lb * pixelsPerBeat), 0.0f, height);
        }

        // TODO: Ghost channels disabled — needs TE API research for clip iteration

        // Notes drawn LAST, on top of the grid, for visibility. Only the
        // notes that actually intersect the dirty rect are touched — same
        // "don't draw off-screen" discipline as the grid lines above.
        if (activeMidiClip != nullptr)
        {
            const auto clipBoundsF = clipBounds.toFloat();

            for (auto* note : activeMidiClip->getSequence().getNotes())
            {
                const auto bounds = noteBounds (*note);

                if (! clipBoundsF.intersects (bounds))
                    continue;

                // Logic Pro velocity coloring: green (1) -> yellow (64) -> red (127).
                const int velocity = note->getVelocity();
                const float normVel = velocity / 127.0f;
                juce::Colour noteColour;

                if (normVel < 0.5f)
                {
                    // Green to Yellow.
                    const float t = normVel * 2.0f;
                    noteColour = juce::Colour (
                        (uint8) (255 * t),      // R: 0 -> 255
                        255,                    // G: 255
                        0                       // B: 0
                    );
                }
                else
                {
                    // Yellow to Red.
                    const float t = (normVel - 0.5f) * 2.0f;
                    noteColour = juce::Colour (
                        255,                    // R: 255
                        (uint8) (255 * (1.0f - t)), // G: 255 -> 0
                        0                       // B: 0
                    );
                }

                g.setColour (noteColour);
                g.fillRoundedRectangle (bounds.reduced (0.5f), 2.0f);
                g.setColour (noteColour.darker (0.6f));
                g.drawRoundedRectangle (bounds.reduced (0.5f), 2.0f, 1.0f);

                // Selection highlight: white 2px border if in selection.
                if (selectedNotes.contains (note))
                {
                    g.setColour (juce::Colours::white);
                    g.drawRoundedRectangle (bounds.reduced (0.5f), 2.0f, 2.0f);
                }

                // Note label: always draw name with auto-clipping. Dynamic text contrast.
                const juce::String noteName = juce::MidiMessage::getMidiNoteName (note->getNoteNumber(), true, true, 4);
                const auto textColour = noteColour.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white;
                g.setColour (textColour);
                g.setFont (8.0f);
                const auto textBounds = juce::Rectangle<int> ((int) bounds.getX() + 2, (int) bounds.getY() + 2,
                                                               (int) bounds.getWidth() - 4, (int) bounds.getHeight() - 4);
                g.drawText (noteName, textBounds, juce::Justification::centredLeft, true);
            }
        }

        // Draw marquee selection rectangle if active.
        if (isSelecting && selectionRect.getWidth() > 0.0f && selectionRect.getHeight() > 0.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.1f));
            g.fillRect (selectionRect);
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.drawRect (selectionRect, 1.0f);
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (e.mods.isCommandDown())
        {
            zoomHorizontal (e, wheel);
            return;
        }

        if (e.mods.isAltDown())
        {
            zoomVertical (e, wheel);
            return;
        }

        // Deliberate pass-through — see class doc comment above.
        Component::mouseWheelMove (e, wheel);
    }

private:
    void zoomHorizontal (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        auto* vp = findParentComponentOfClass<juce::Viewport>();

        if (vp == nullptr || activeEdit == nullptr)
            return;

        // Ableton/FL zoom-to-cursor physics: capture which beat is under the
        // cursor, and how far the cursor sits from the viewport's own left
        // edge (a SCREEN-fixed quantity, invariant across the zoom change
        // below), BEFORE touching pixelsPerBeat — both depend on the OLD zoom
        // level. e.x is already local to THIS component (0 = clip start), the
        // exact coordinate space xToBeat()/beatToX() expect.
        const auto beatUnderCursor = xToBeat ((double) e.x);
        const int screenOffsetX = e.x - vp->getViewPositionX();

        const double factor = std::pow (1.5, (double) wheel.deltaY);
        pixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, pixelsPerBeat * factor);

        if (onZoomChanged)
            onZoomChanged(); // resizes this component to the new content bounds — MUST run
                              // before the setViewPosition() call below, so the new scroll
                              // position is clamped against valid (already-resized) content bounds

        // Re-anchor: put the SAME beat back at the SAME screen position.
        const int newContentX = (int) beatToX (beatUnderCursor);
        vp->setViewPosition (juce::jmax (0, newContentX - screenOffsetX), vp->getViewPositionY());

        repaint();
    }

    void zoomVertical (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        auto* vp = findParentComponentOfClass<juce::Viewport>();

        if (vp == nullptr)
            return;

        // Same zoom-to-cursor physics as zoomHorizontal(), mirrored onto Y.
        // Deliberately NOT round-tripped through the public int-snapping
        // yToNote()/noteToY(int) pair — those quantise to a whole MIDI note,
        // which would lose the cursor's sub-row position and make the
        // "stationary under the cursor" note visibly drift by up to one row
        // per zoom step. This raw-pixel formula is the same linear mapping
        // (noteToY() is affine in pixelsPerNote through the fixed y=0 <->
        // note-127 anchor), just evaluated at full float precision so zoom
        // stays pixel-exact — same reasoning xToBeat()'s continuous
        // BeatPosition already gives zoomHorizontal() for free.
        const float notePositionUnderCursor = 127.0f - (float) e.y / (float) pixelsPerNote;
        const int screenOffsetY = e.y - vp->getViewPositionY();

        const double factor = std::pow (1.5, (double) wheel.deltaY);
        pixelsPerNote = juce::jlimit (minPixelsPerNote, maxPixelsPerNote, pixelsPerNote * factor);

        if (onZoomChanged)
            onZoomChanged(); // resizes this component to the new content bounds first

        const int newContentY = (int) std::round ((127.0f - notePositionUnderCursor) * (float) pixelsPerNote);
        vp->setViewPosition (vp->getViewPositionX(), juce::jmax (0, newContentY - screenOffsetY));

        repaint();
    }

    //==========================================================================
    // FL Studio mouse physics: add (left-click empty), delete (right-click a
    // note), resize (left-drag a note's rightmost 8px), move (left-drag a
    // note's body — pitch AND time). Each drag gesture is exactly one Undo
    // transaction, opened on mouseDown; the two drag modes (resize/move) are
    // mutually exclusive, only one ever set per gesture.
public:
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (activeMidiClip == nullptr)
            return;

        auto* hitNote = hitTestNote (e.position);

        if (e.mods.isRightButtonDown())
        {
            if (hitNote != nullptr)
            {
                // Right-click on note: delete it.
                activeMidiClip->edit.getUndoManager().beginNewTransaction ("Delete MIDI Note");
                activeMidiClip->getSequence().removeNote (*hitNote, &activeMidiClip->edit.getUndoManager());
                repaint();
            }
            else
            {
                // Right-click on empty space: start continuous eraser.
                activeMidiClip->edit.getUndoManager().beginNewTransaction ("Erase Notes");
                isErasing = true;
            }
            return;
        }

        if (hitNote != nullptr)
        {
            const auto bounds = noteBounds (*hitNote);

            if (e.position.x >= bounds.getRight() - 8.0f)
            {
                // Begin a resize GESTURE — one Undo transaction for the whole
                // drag, opened here (not per mouseDrag tick), same discipline
                // ClipComponent's move-drag and DraggableBpmLabel's tempo-drag
                // already use elsewhere in this app.
                activeMidiClip->edit.getUndoManager().beginNewTransaction ("Resize MIDI Note");
                isResizing = true;
                draggedNote = hitNote;
            }
            else
            {
                // Begin a MOVE gesture (pitch + time). Capture the anchor state
                // NOW so mouseDrag can compute deltas against a fixed origin
                // rather than accumulating per-event (which would drift). One
                // Undo transaction for the whole drag, same as resize above.
                activeMidiClip->edit.getUndoManager().beginNewTransaction ("Move MIDI Note(s)");
                isMoving = true;
                draggedNote = hitNote;
                dragStartX    = e.position.x;
                dragStartY    = e.position.y;
                dragStartBeat = hitNote->getStartBeat().inBeats();
                dragStartNote = hitNote->getNoteNumber();

                // If the clicked note is part of selectedNotes, store initial state of all selected.
                if (selectedNotes.contains (hitNote))
                {
                    // Store each selected note's initial beat and pitch for group move.
                    for (auto* note : selectedNotes)
                    {
                        selectedNotesInitialState.add ({ note->getStartBeat().inBeats(), note->getNoteNumber() });
                    }
                }
                else
                {
                    // Single note move — store only this note.
                    selectedNotesInitialState.clear();
                    selectedNotesInitialState.add ({ dragStartBeat, dragStartNote });
                    selectedNotes.clear();
                    selectedNotes.add (hitNote);
                }
            }

            return;
        }

        // Left-click on empty space: start marquee selection only if Ctrl is held.
        if (e.mods.isCtrlDown())
        {
            isSelecting = true;
            dragStartX = e.position.x;
            dragStartY = e.position.y;
            selectionRect = { e.position.x, e.position.y, 0.0f, 0.0f };
            selectedNotes.clear();
            return;
        }

        // Left-click on empty space without Ctrl: add a note immediately.
        const double rawLocalBeat = (double) e.position.x / pixelsPerBeat;
        const double snappedBeat  = std::round (rawLocalBeat / 0.25) * 0.25;
        int rootNoteNumber = yToNote (e.position.y);

        // SCALE-SNAP on note creation.
        if (inspector != nullptr && inspector->isSnapToScaleEnabled())
        {
            const int rootNote = inspector->getRootNote();
            const int scaleType = inspector->getScaleType();

            static const juce::Array<int> majorIntervals   { 0, 2, 4, 5, 7, 9, 11 };
            static const juce::Array<int> minorIntervals   { 0, 2, 3, 5, 7, 8, 10 };
            static const juce::Array<int> pentatonicIntervals { 0, 2, 4, 7, 9 };
            static const juce::Array<int> bluesIntervals   { 0, 3, 5, 6, 7, 10 };

            const juce::Array<int>* activeScale = nullptr;
            switch (scaleType)
            {
                case 0: activeScale = &majorIntervals; break;
                case 1: activeScale = &minorIntervals; break;
                case 2: activeScale = &pentatonicIntervals; break;
                case 3: activeScale = &bluesIntervals; break;
                case 4: activeScale = nullptr; break;
            }

            if (activeScale != nullptr)
            {
                // Find nearest valid pitch in the scale.
                int pitchClass = ((rootNoteNumber % 12) + 12) % 12;
                int scaleIdx = (pitchClass - rootNote + 12) % 12;

                // Find nearest interval in the scale.
                int nearest = 0;
                int minDelta = 12;
                for (int interval : *activeScale)
                {
                    int delta = std::abs (interval - scaleIdx);
                    if (delta < minDelta)
                    {
                        minDelta = delta;
                        nearest = interval;
                    }
                }

                // Snap the note.
                rootNoteNumber = ((rootNote + nearest) % 12) + (rootNoteNumber / 12) * 12;
                rootNoteNumber = juce::jlimit (0, 127, rootNoteNumber);
            }
        }

        // STAMP MODE: chord insertion.
        if (inspector != nullptr && inspector->isStampModeEnabled())
        {
            activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add Chord");

            static const juce::Array<int> triadIntervals     { 0, 4, 7 };
            static const juce::Array<int> seventhIntervals   { 0, 4, 7, 11 };
            static const juce::Array<int> ninthIntervals     { 0, 4, 7, 11, 14 };
            static const juce::Array<int> customIntervals    { 0 };

            const juce::Array<int>* chordIntervals = nullptr;
            switch (inspector->getChordType())
            {
                case 0: chordIntervals = &triadIntervals; break;
                case 1: chordIntervals = &seventhIntervals; break;
                case 2: chordIntervals = &ninthIntervals; break;
                case 3: chordIntervals = &customIntervals; break;
            }

            if (chordIntervals != nullptr)
            {
                for (int interval : *chordIntervals)
                {
                    const int chordNoteNumber = juce::jlimit (0, 127, rootNoteNumber + interval);
                    activeMidiClip->getSequence().addNote (chordNoteNumber,
                                                           tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                           tracktion::BeatDuration::fromBeats (0.25),
                                                           100, 0,
                                                           &activeMidiClip->edit.getUndoManager());
                }
            }

            repaint();
        }
        else
        {
            // Normal single-note insertion.
            activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add MIDI Note");
            activeMidiClip->getSequence().addNote (rootNoteNumber,
                                                   tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                   tracktion::BeatDuration::fromBeats (0.25),
                                                   100, 0,
                                                   &activeMidiClip->edit.getUndoManager());
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (activeMidiClip == nullptr)
            return;

        auto& undoManager = activeMidiClip->edit.getUndoManager();

        // Continuous right-click eraser: hit-test all notes at current position and erase any that intersect.
        if (isErasing)
        {
            auto* hitNote = hitTestNote (e.position);
            if (hitNote != nullptr)
            {
                activeMidiClip->getSequence().removeNote (*hitNote, &undoManager);
                repaint();
            }
            return;
        }

        // Marquee selection: update selection rectangle and check for intersecting notes.
        if (isSelecting)
        {
            const float minX = juce::jmin (dragStartX, e.position.x);
            const float maxX = juce::jmax (dragStartX, e.position.x);
            const float minY = juce::jmin (dragStartY, e.position.y);
            const float maxY = juce::jmax (dragStartY, e.position.y);

            selectionRect = { minX, minY, maxX - minX, maxY - minY };

            // Check which notes intersect the selection rect.
            selectedNotes.clear();
            for (auto* note : activeMidiClip->getSequence().getNotes())
            {
                const auto bounds = noteBounds (*note);
                if (selectionRect.toFloat().intersects (bounds))
                    selectedNotes.add (note);
            }

            repaint();
            return;
        }

        if (draggedNote == nullptr)
            return;

        if (isResizing)
        {
            // Snap to grid resolution (Free = no snap, 1/4 = 0.25, 1/8 = 0.125, etc).
            const double rawEndBeat = (double) e.position.x / pixelsPerBeat;
            double snappedEndBeat = rawEndBeat;

            if (inspector != nullptr)
            {
                const int gridRes = inspector->getGridResolution();
                if (gridRes == 0) // Free
                {
                    snappedEndBeat = rawEndBeat; // no snapping
                }
                else if (gridRes == 1) // 1/4
                {
                    snappedEndBeat = std::round (rawEndBeat / 0.25) * 0.25;
                }
                else if (gridRes == 2) // 1/8
                {
                    snappedEndBeat = std::round (rawEndBeat / 0.125) * 0.125;
                }
                else if (gridRes == 3) // 1/16
                {
                    snappedEndBeat = std::round (rawEndBeat / 0.0625) * 0.0625;
                }
                else if (gridRes == 4) // 1/32
                {
                    snappedEndBeat = std::round (rawEndBeat / 0.03125) * 0.03125;
                }
            }
            else
            {
                snappedEndBeat = std::round (rawEndBeat / 0.25) * 0.25; // default to 1/16
            }
            const double startBeat      = draggedNote->getStartBeat().inBeats();
            constexpr double minLengthBeats = 1.0 / 16.0; // a 1/64th note, in quarter-note beats
            const double newLength = juce::jmax (minLengthBeats, snappedEndBeat - startBeat);

            // te::MidiNote has no standalone setLength() — setStartAndLength()
            // is the real API (verified against tracktion_MidiNote.h); start is
            // passed through unchanged since this gesture only resizes the end.
            draggedNote->setStartAndLength (draggedNote->getStartBeat(),
                                            tracktion::BeatDuration::fromBeats (newLength),
                                            &undoManager);
            repaint();
            return;
        }

        if (isMoving)
        {
            // TIME: delta from the fixed drag origin. Snap to grid resolution.
            const double deltaBeat = (double) (e.position.x - dragStartX) / pixelsPerBeat;
            double rawNewBeat = dragStartBeat + deltaBeat;
            double newBeat = juce::jmax (0.0, rawNewBeat);

            if (inspector != nullptr)
            {
                const int gridRes = inspector->getGridResolution();
                if (gridRes == 0) // Free
                {
                    // no snapping
                }
                else if (gridRes == 1) // 1/4
                {
                    newBeat = std::round (newBeat / 0.25) * 0.25;
                }
                else if (gridRes == 2) // 1/8
                {
                    newBeat = std::round (newBeat / 0.125) * 0.125;
                }
                else if (gridRes == 3) // 1/16
                {
                    newBeat = std::round (newBeat / 0.0625) * 0.0625;
                }
                else if (gridRes == 4) // 1/32
                {
                    newBeat = std::round (newBeat / 0.03125) * 0.03125;
                }
            }
            else
            {
                newBeat = std::round (newBeat / 0.0625) * 0.0625; // default 1/16
            }

            // PITCH: integer note-row delta (yToNote already snaps to a whole
            // note each side, so the difference is a clean integer offset — no
            // sub-row jitter as the cursor wanders within a row), applied to
            // the anchor pitch and clamped to the valid 0-127 range.
            const int deltaNote = yToNote (e.position.y) - yToNote (dragStartY);
            int newNote   = juce::jlimit (0, 127, dragStartNote + deltaNote);

            // SCALE-SNAP during move: enforce snapping if enabled.
            if (inspector != nullptr && inspector->isSnapToScaleEnabled())
            {
                const int rootNote = inspector->getRootNote();
                const int scaleType = inspector->getScaleType();

                static const juce::Array<int> majorIntervals   { 0, 2, 4, 5, 7, 9, 11 };
                static const juce::Array<int> minorIntervals   { 0, 2, 3, 5, 7, 8, 10 };
                static const juce::Array<int> pentatonicIntervals { 0, 2, 4, 7, 9 };
                static const juce::Array<int> bluesIntervals   { 0, 3, 5, 6, 7, 10 };

                const juce::Array<int>* activeScale = nullptr;
                switch (scaleType)
                {
                    case 0: activeScale = &majorIntervals; break;
                    case 1: activeScale = &minorIntervals; break;
                    case 2: activeScale = &pentatonicIntervals; break;
                    case 3: activeScale = &bluesIntervals; break;
                    case 4: activeScale = nullptr; break;
                }

                if (activeScale != nullptr)
                {
                    // Strict enforcement: find the nearest note that IS in the scale.
                    const int octave = newNote / 12;
                    const int pitchClass = newNote % 12;
                    int bestNote = newNote;
                    int bestDist = 128;

                    // Check up to ±1 octave around the target note.
                    for (int oct = octave - 1; oct <= octave + 1; ++oct)
                    {
                        for (int interval : *activeScale)
                        {
                            const int candidate = oct * 12 + ((rootNote + interval) % 12);
                            if (candidate >= 0 && candidate <= 127)
                            {
                                const int dist = std::abs (candidate - newNote);
                                if (dist < bestDist)
                                {
                                    bestDist = dist;
                                    bestNote = candidate;
                                }
                            }
                        }
                    }

                    newNote = juce::jlimit (0, 127, bestNote);
                }
            }

            // Multi-note move: apply the same delta to all selected notes.
            if (selectedNotesInitialState.size() > 1)
            {
                // Move all selected notes with the same delta.
                const double groupDeltaBeat = newBeat - dragStartBeat;
                const int groupDeltaNote = newNote - dragStartNote;

                for (int i = 0; i < selectedNotes.size() && i < selectedNotesInitialState.size(); ++i)
                {
                    auto* note = selectedNotes[i];
                    const auto& initialState = selectedNotesInitialState[i];

                    double noteBeat = initialState.beat + groupDeltaBeat;
                    int noteNum = juce::jlimit (0, 127, initialState.noteNumber + groupDeltaNote);

                    // Apply scale snap if enabled (same logic as single note).
                    if (inspector != nullptr && inspector->isSnapToScaleEnabled())
                    {
                        const int rootNote = inspector->getRootNote();
                        const int scaleType = inspector->getScaleType();

                        static const juce::Array<int> majorIntervals   { 0, 2, 4, 5, 7, 9, 11 };
                        static const juce::Array<int> minorIntervals   { 0, 2, 3, 5, 7, 8, 10 };
                        static const juce::Array<int> pentatonicIntervals { 0, 2, 4, 7, 9 };
                        static const juce::Array<int> bluesIntervals   { 0, 3, 5, 6, 7, 10 };

                        const juce::Array<int>* activeScale = nullptr;
                        switch (scaleType)
                        {
                            case 0: activeScale = &majorIntervals; break;
                            case 1: activeScale = &minorIntervals; break;
                            case 2: activeScale = &pentatonicIntervals; break;
                            case 3: activeScale = &bluesIntervals; break;
                            case 4: activeScale = nullptr; break;
                        }

                        if (activeScale != nullptr)
                        {
                            const int octave = noteNum / 12;
                            int bestNote = noteNum;
                            int bestDist = 128;

                            for (int oct = octave - 1; oct <= octave + 1; ++oct)
                            {
                                for (int interval : *activeScale)
                                {
                                    const int candidate = oct * 12 + ((rootNote + interval) % 12);
                                    if (candidate >= 0 && candidate <= 127)
                                    {
                                        const int dist = std::abs (candidate - noteNum);
                                        if (dist < bestDist)
                                        {
                                            bestDist = dist;
                                            bestNote = candidate;
                                        }
                                    }
                                }
                            }
                            noteNum = juce::jlimit (0, 127, bestNote);
                        }
                    }

                    note->setStartAndLength (tracktion::BeatPosition::fromBeats (noteBeat),
                                            note->getLengthBeats(),
                                            &undoManager);
                    note->setNoteNumber (noteNum, &undoManager);
                }
            }
            else
            {
                // Single note move.
                draggedNote->setStartAndLength (tracktion::BeatPosition::fromBeats (newBeat),
                                                draggedNote->getLengthBeats(),
                                                &undoManager);
                draggedNote->setNoteNumber (newNote, &undoManager);
            }

            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isErasing)
        {
            isErasing = false;
            repaint();
            return;
        }

        if (isSelecting)
        {
            isSelecting = false;

            // If the selection rect is tiny (just a click), treat it as note insertion instead.
            if (selectionRect.getWidth() < 3.0f && selectionRect.getHeight() < 3.0f && activeMidiClip != nullptr)
            {
                // Click on empty space — add a note.
                const double rawLocalBeat = (double) e.position.x / pixelsPerBeat;
                const double snappedBeat  = std::round (rawLocalBeat / 0.25) * 0.25;
                int rootNoteNumber = yToNote (e.position.y);

                // Apply scale snap if enabled.
                if (inspector != nullptr && inspector->isSnapToScaleEnabled())
                {
                    const int rootNote = inspector->getRootNote();
                    const int scaleType = inspector->getScaleType();

                    static const juce::Array<int> majorIntervals   { 0, 2, 4, 5, 7, 9, 11 };
                    static const juce::Array<int> minorIntervals   { 0, 2, 3, 5, 7, 8, 10 };
                    static const juce::Array<int> pentatonicIntervals { 0, 2, 4, 7, 9 };
                    static const juce::Array<int> bluesIntervals   { 0, 3, 5, 6, 7, 10 };

                    const juce::Array<int>* activeScale = nullptr;
                    switch (scaleType)
                    {
                        case 0: activeScale = &majorIntervals; break;
                        case 1: activeScale = &minorIntervals; break;
                        case 2: activeScale = &pentatonicIntervals; break;
                        case 3: activeScale = &bluesIntervals; break;
                        case 4: activeScale = nullptr; break;
                    }

                    if (activeScale != nullptr)
                    {
                        int pitchClass = ((rootNoteNumber % 12) + 12) % 12;
                        int scaleIdx = (pitchClass - rootNote + 12) % 12;

                        int nearest = 0;
                        int minDelta = 12;
                        for (int interval : *activeScale)
                        {
                            int delta = std::abs (interval - scaleIdx);
                            if (delta < minDelta)
                            {
                                minDelta = delta;
                                nearest = interval;
                            }
                        }

                        rootNoteNumber = ((rootNote + nearest) % 12) + (rootNoteNumber / 12) * 12;
                        rootNoteNumber = juce::jlimit (0, 127, rootNoteNumber);
                    }
                }

                // Add note or chord.
                if (inspector != nullptr && inspector->isStampModeEnabled())
                {
                    activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add Chord");

                    static const juce::Array<int> triadIntervals     { 0, 4, 7 };
                    static const juce::Array<int> seventhIntervals   { 0, 4, 7, 11 };
                    static const juce::Array<int> ninthIntervals     { 0, 4, 7, 11, 14 };
                    static const juce::Array<int> customIntervals    { 0 };

                    const juce::Array<int>* chordIntervals = nullptr;
                    switch (inspector->getChordType())
                    {
                        case 0: chordIntervals = &triadIntervals; break;
                        case 1: chordIntervals = &seventhIntervals; break;
                        case 2: chordIntervals = &ninthIntervals; break;
                        case 3: chordIntervals = &customIntervals; break;
                    }

                    if (chordIntervals != nullptr)
                    {
                        for (int interval : *chordIntervals)
                        {
                            const int chordNoteNumber = juce::jlimit (0, 127, rootNoteNumber + interval);
                            activeMidiClip->getSequence().addNote (chordNoteNumber,
                                                                   tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                                   tracktion::BeatDuration::fromBeats (0.25),
                                                                   100, 0,
                                                                   &activeMidiClip->edit.getUndoManager());
                        }
                    }
                }
                else
                {
                    activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add MIDI Note");
                    activeMidiClip->getSequence().addNote (rootNoteNumber,
                                                           tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                           tracktion::BeatDuration::fromBeats (0.25),
                                                           100, 0,
                                                           &activeMidiClip->edit.getUndoManager());
                }
            }

            repaint();
            return;
        }

        // Required, not optional: without this, isResizing/isMoving/draggedNote
        // would stay set after the gesture ends, and the NEXT unrelated
        // mouseDrag (or a stale draggedNote left pointing at a note deleted by
        // an intervening Undo) would silently resume editing through a dangling
        // pointer. Every gesture must end its own state cleanly.
        isResizing = false;
        isMoving = false;
        draggedNote = nullptr;
        selectedNotesInitialState.clear();
    }

private:
    // Exact pixel bounds of one note — shared by paint()'s note-drawing loop
    // and hitTestNote()/mouseDown()'s resize-edge check, so the two can never
    // visually disagree about where a note actually is. note.getStartBeat()/
    // getLengthBeats() are CLIP-LOCAL beats (0 = this clip's own content
    // start) — deliberately NOT run through beatToX()/xToBeat(), which are
    // for ABSOLUTE Edit-beat contexts (the ruler/grid's tempoSequence
    // look-ups); a clip-local beat is already exactly the "beat" this
    // component's own local-beat loop above uses, so it maps to pixels with a
    // direct multiply, no clipStartBeat translation involved.
    juce::Rectangle<float> noteBounds (const te::MidiNote& note) const
    {
        const float x = (float) (note.getStartBeat().inBeats() * pixelsPerBeat);
        const float y = noteToY (note.getNoteNumber());
        const float width = juce::jmax (3.0f, (float) (note.getLengthBeats().inBeats() * pixelsPerBeat));
        return { x, y, width, (float) pixelsPerNote };
    }

    // Reverse iteration (highest array index first) hit-tests the TOP-MOST
    // note first for any pixel where two notes visually overlap — the last
    // note added is drawn last (on top), so it should also be the first one
    // a click resolves to, matching what the user actually sees.
    te::MidiNote* hitTestNote (juce::Point<float> pos) const
    {
        if (activeMidiClip == nullptr)
            return nullptr;

        auto& notes = activeMidiClip->getSequence().getNotes();

        for (int i = notes.size() - 1; i >= 0; --i)
        {
            auto* note = notes.getUnchecked (i);

            if (noteBounds (*note).contains (pos))
                return note;
        }

        return nullptr;
    }

    // juce::ValueTree::Listener — fires for ANY mutation of the active clip's
    // MidiList state, which includes ones THIS component didn't itself cause
    // (Undo/Redo of a note add/delete/resize via the global Undo/Redo
    // buttons, in particular). Deferred via callAsync + SafePointer: the
    // clip's UndoManager can fire this synchronously from deep inside an
    // undo()/redo() call this component has no business repainting itself in
    // the middle of — same "defer to a clean top-level call" reasoning every
    // other ValueTree::Listener in this app already follows (MixerStrip,
    // TrackRow, UniversalDeviceChainComponent). Mutations THIS component
    // causes directly (mouseDown/mouseDrag above) already call repaint()
    // synchronously themselves — that path doesn't depend on this listener at
    // all, it's purely for externally-triggered changes.
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override        { scheduleRepaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { scheduleRepaint(); }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { scheduleRepaint(); }

    void scheduleRepaint()
    {
        juce::Component::SafePointer<PianoRollGridContent> safeThis (this);

        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->repaint();
        });
    }

    CrateMidiInspectorComponent* inspector = nullptr; // reference to scale/chord state

    te::MidiNote* draggedNote = nullptr;
    bool isResizing = false;

    // Move-gesture state. dragStart* are the fixed origin captured on mouseDown
    // (cursor position + the note's pitch/start at that instant), so mouseDrag
    // computes deltas against a stable anchor rather than accumulating them.
    bool isMoving = false;
    double dragStartBeat = 0.0;
    int dragStartNote = 0;
    float dragStartX = 0.0f;
    float dragStartY = 0.0f;

    // Right-click eraser state.
    bool isErasing = false;

    // Marquee selection state.
    bool isSelecting = false;
    juce::Rectangle<float> selectionRect;
    juce::Array<te::MidiNote*> selectedNotes;

    // Multi-note drag state: store initial beat/note for each selected note.
    struct InitialNoteState { double beat; int noteNumber; };
    juce::Array<InitialNoteState> selectedNotesInitialState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollGridContent)
};

//==============================================================================
CratePianoRollComponent::CratePianoRollComponent()
{
    setWantsKeyboardFocus (true); // required for keyPressed() (Escape) to ever be delivered

    ruler = std::make_unique<PianoRollRuler>();
    addAndMakeVisible (*ruler);

    keyboard = std::make_unique<PianoRollKeyboard>();
    addAndMakeVisible (*keyboard);

    gridContent = std::make_unique<PianoRollGridContent>();
    gridContent->onZoomChanged = [this] { layoutContent(); };

    viewport.setViewedComponent (gridContent.get(), false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    expressionLane = std::make_unique<PianoRollExpressionLane>();
    addAndMakeVisible (expressionLane.get());

    // Keeps the (fixed, non-scrolling) ruler/keyboard in sync whenever the user
    // scrolls the viewport — horizontal offset to the ruler, vertical to the
    // keyboard, identical contract to ArrangementComponent's own
    // viewport.onViewportMoved wiring. ALSO syncs the expression lane's
    // horizontal offset and zoom so its stems align exactly with the grid.
    viewport.onViewportMoved = [this] (int x, int y)
    {
        ruler->setHorizontalOffset (x);
        keyboard->setVerticalOffset (y);
        expressionLane->setScrollOffset (x, y);
        expressionLane->setZoom (pixelsPerBeat, pixelsPerNote);
    };
}

CratePianoRollComponent::~CratePianoRollComponent() = default;

void CratePianoRollComponent::setInspectorComponent (CrateMidiInspectorComponent* insp) noexcept
{
    gridContent->setInspectorComponent (insp);
    keyboard->setInspectorComponent (insp);

    // Wire scale change callback to keyboard repaint.
    if (insp != nullptr)
    {
        insp->onScaleChanged = [this]
        {
            if (keyboard != nullptr)
                keyboard->repaint();
        };
    }
}

void CratePianoRollComponent::setActiveClip (te::MidiClip* clip)
{
    auto* previousClip = activeClip; // this component's OWN member (see header)
    activeClip = clip;

    // CratePianoRoll::activeMidiClip is a SEPARATE global (deliberately NOT
    // named activeClip — see its own doc comment in PianoRollLayout.h for why
    // that name specifically would silently fail to compile inside the nested
    // classes below). PianoRollGridContent reads this global directly to
    // reach the clip's MidiList for rendering/hit-testing/mouse physics.
    CratePianoRoll::activeMidiClip = activeClip;

    if (activeClip != nullptr)
    {
        activeEdit = &activeClip->edit;

        // TrackItem::getStartBeat() already gives the clip's absolute Edit-beat
        // start directly (accounting for the Edit's real tempo track) — no need
        // to manually round-trip through tempoSequence.toBeats() ourselves.
        clipStartBeat = activeClip->getStartBeat();
    }
    else
    {
        activeEdit = nullptr;
        clipStartBeat = {};
    }

    // Keeps the grid's ValueTree::Listener pointed at whichever clip is
    // actually open now, so Undo/Redo of a note edit still triggers a repaint
    // — see PianoRollGridContent::setSequenceListenerTarget()'s doc comment.
    gridContent->setSequenceListenerTarget (previousClip, activeClip);

    // Expression lane needs the clip too for velocity rendering.
    expressionLane->setActiveClip (activeClip);

    layoutContent();

    // Focus trap (QA requirement): grab focus the instant we're pointed at a
    // clip AND actually on-screen, so Escape works with no preliminary click.
    // isShowing() guards the case where MainComponent sets the clip a beat
    // before making us visible — the grab would no-op there; MainComponent's
    // enterMidiEditor() re-grabs after setVisible(true) to cover it.
    if (activeClip != nullptr && isShowing())
        grabKeyboardFocus();
}

void CratePianoRollComponent::layoutContent()
{
    const int visibleW = viewport.getMaximumVisibleWidth();
    const int visibleH = viewport.getMaximumVisibleHeight();

    // Content width reflects the clip's own length at the CURRENT horizontal
    // zoom level, clamped up to at least the viewport's visible width —
    // zooming in grows it past visibleW (enabling horizontal scroll), zooming
    // out never shrinks it below what's actually visible. Height is all 128
    // MIDI notes at the CURRENT vertical zoom level, same clamp-up logic.
    int contentWidth = visibleW;

    if (activeClip != nullptr && activeEdit != nullptr)
        contentWidth = juce::jmax (visibleW, (int) beatToX (activeClip->getEndBeat()));

    const int contentHeight = juce::jmax (visibleH, gridContentHeightPx());

    gridContent->setSize (contentWidth, contentHeight);

    // Ruler/keyboard stay sized to the VISIBLE window (not the full scrollable
    // content) — fixed overlays, not part of the scrolling content;
    // viewport.onViewportMoved keeps their drawn offsets in sync instead.
    ruler->setBounds (viewport.getX(), 0, visibleW, rulerHeight);
    keyboard->setBounds (0, viewport.getY(), keyboardWidth, visibleH);

    ruler->repaint();
    keyboard->repaint();
    gridContent->repaint();
}

void CratePianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (pianoRollBackground);

    // Top-left dead zone (above keyboard, left of ruler) — fill with panel colour.
    g.setColour (LAF::panel);
    g.fillRect (0, 0, keyboardWidth, rulerHeight);

    // Thin accent top-rule so the zone edge is legible against the shell's
    // near-black dividers, matching the other zones' framed look.
    g.setColour (LAF::accent.withAlpha (0.6f));
    g.fillRect (0, 0, getWidth(), 2);
}

void CratePianoRollComponent::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (rulerHeight);      // ruler occupies this row — positioned in layoutContent()
    area.removeFromLeft (keyboardWidth);   // keyboard occupies this column — positioned in layoutContent()

    // Bottom 120px for expression lane (velocity/CC rendering).
    constexpr int expressionLaneHeight = 120;
    expressionLane->setBounds (getLocalBounds().withTop (getHeight() - expressionLaneHeight));

    // Remaining area for the viewport/grid.
    auto viewportArea = area.removeFromBottom (area.getHeight() - expressionLaneHeight);
    viewport.setBounds (viewportArea);

    layoutContent();
}

bool CratePianoRollComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onExitRequested)
            onExitRequested();

        return true;
    }

    return false; // let every other key fall through — never swallow (e.g. a transport Spacebar)
}
