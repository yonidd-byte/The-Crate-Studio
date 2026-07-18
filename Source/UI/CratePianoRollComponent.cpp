#include "CratePianoRollComponent.h"
#include "PianoRollLayout.h"
#include "TheCrateLookAndFeel.h"
#include "PianoRollExpressionLane.h"
#include "PianoRollArticulationLane.h"
#include "CrateMidiInspectorComponent.h"

using namespace CratePianoRoll;

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Utility: Get chord intervals (semitones from root) for a given chord type.
    static juce::Array<int> getChordIntervals (int chordType)
    {
        switch (chordType)
        {
            case 0: return juce::Array<int> { 0, 4, 7 };           // Major Triad
            case 1: return juce::Array<int> { 0, 3, 7 };           // Minor Triad
            case 2: return juce::Array<int> { 0, 4, 7, 11 };       // Maj7
            case 3: return juce::Array<int> { 0, 3, 7, 10 };       // Min7
            case 4: return juce::Array<int> { 0, 4, 7, 10 };       // Dom7
            case 5: return juce::Array<int> { 0, 3, 7, 10, 14 };   // Min9
            case 6: return juce::Array<int> { 0, 2, 7 };           // Sus2
            case 7: return juce::Array<int> { 0, 5, 7 };           // Sus4
            case 8: return juce::Array<int> { 0, 3, 6 };           // Diminished
            default: return juce::Array<int> { 0 };
        }
    }

    // Utility: Snap a MIDI note to the nearest note in the active scale.
    // Uses correct root note offset: normalizedPitch = (noteNumber - rootNote + 120) % 12
    static int snapNoteToScale (int noteNumber, int rootNote, const juce::Array<int>& scale)
    {
        if (scale.isEmpty())
            return noteNumber;

        const int normalizedPitch = (noteNumber - rootNote + 120) % 12;

        // If already in scale, don't snap.
        if (scale.contains (normalizedPitch))
            return noteNumber;

        // Check ±1 semitone for nearest scale note.
        if (scale.contains ((normalizedPitch + 1) % 12))
            return juce::jlimit (0, 127, noteNumber + 1);

        if (scale.contains ((normalizedPitch - 1 + 12) % 12))
            return juce::jlimit (0, 127, noteNumber - 1);

        return noteNumber;
    }

    // MIDI AUDITIONING: injects a live Note-On/Off straight into the clip's
    // owning AudioTrack via TE's injectLiveMidiMessage() — the exact same
    // real-time-safe path a physical MIDI input device uses (AudioTrack.cpp
    // calls it from PhysicalMidiInputDevice's own thread; here it's called
    // from the message thread instead, which is equally valid — it's a
    // listener-broadcast hook, not a raw audio-thread queue write). Deliberately
    // bypasses the clip's MidiList entirely: auditioning must never touch the
    // actual note data or the Undo stack, only make sound.
    void auditionNoteOn (te::MidiClip* clip, int noteNumber, int velocity)
    {
        if (clip == nullptr)
            return;

        if (auto* track = dynamic_cast<te::AudioTrack*> (clip->getClipTrack()))
            track->injectLiveMidiMessage (juce::MidiMessage::noteOn (1, noteNumber, (juce::uint8) juce::jlimit (1, 127, velocity)),
                                          tracktion::engine::MPESourceID {}); // 0 == not MPE (notMPE is deprecated)
    }

    void auditionNoteOff (te::MidiClip* clip, int noteNumber)
    {
        if (clip == nullptr || noteNumber < 0)
            return;

        if (auto* track = dynamic_cast<te::AudioTrack*> (clip->getClipTrack()))
            track->injectLiveMidiMessage (juce::MidiMessage::noteOff (1, noteNumber),
                                          tracktion::engine::MPESourceID {}); // 0 == not MPE (notMPE is deprecated)
    }

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

        // KEYBOARD DIMMING: overlay dark rectangles on keys not in the active
        // scale. Reads PUSHED state (isSnapToScale/rootNote/activeScaleIntervals),
        // not the inspector pointer — see setScaleState().
        if (isSnapToScale && ! activeScaleIntervals.isEmpty())
        {
            g.setColour (juce::Colours::black.withAlpha (0.3f));

            for (int note = lowestNote; note <= highestNote; ++note)
            {
                // Correct root note offset: normalize pitch to 0-11 range relative to root.
                const int normalizedPitch = (note - rootNote + 120) % 12;

                if (! activeScaleIntervals.contains (normalizedPitch))
                {
                    const float y = noteToY (note) - (float) verticalOffset;
                    g.fillRect (juce::Rectangle<float> (0.0f, y, w, h));
                }
            }
        }

        g.setColour (LAF::background);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) hTotal); // divider against the ruler/grid
    }

    // Pushed scale state (Inspector -> Parent -> here). Mirrors the grid's own
    // setScaleState so keyboard key-dimming and grid row-dimming stay in lockstep.
    void setScaleState (bool snapEnabled, int root, const juce::Array<int>& scaleIntervals)
    {
        isSnapToScale = snapEnabled;
        rootNote = root;
        activeScaleIntervals = scaleIntervals;
        repaint();
    }

    // MIDI AUDITIONING: click a key to hear its pitch, held for as long as the
    // mouse is down — same injectLiveMidiMessage() path PianoRollGridContent
    // uses (see the free functions above), so both input surfaces sound
    // through the exact same live-MIDI mechanism.
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int note = yToNote (e.position.y + (float) verticalOffset);
        auditionNoteOn (activeMidiClip, note, 100);
        auditionedNote = note;
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        auditionNoteOff (activeMidiClip, auditionedNote);
        auditionedNote = -1;
    }

private:
    int verticalOffset = 0;
    CrateMidiInspectorComponent* inspector = nullptr;

    // Scale state pushed down via setScaleState() (no longer read from inspector in paint()).
    bool isSnapToScale = false;
    int rootNote = 0;
    juce::Array<int> activeScaleIntervals;

    int auditionedNote = -1; // -1 == not currently auditioning

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
    PianoRollGridContent()
    {
        setWantsKeyboardFocus (true);
    }

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

    // Push scale state down from the Inspector (via the parent). The grid no
    // longer reaches back into the inspector pointer for scale/root/snap in
    // paint() or the mouse handlers — it renders and snaps purely from these
    // members, so a stale or not-yet-wired inspector can't silently blank the
    // dimming. DBG line acts as a console "print screen" of the pushed state.
    void setScaleState (bool snapEnabled, int root, const juce::Array<int>& scaleIntervals)
    {
        isSnapToScale = snapEnabled;
        rootNote = root;
        activeScaleIntervals = scaleIntervals;

        DBG ("Grid State Updated -> Snap: " << (int) snapEnabled
             << ", Root: " << root
             << ", Intervals Count: " << scaleIntervals.size());

        repaint();
    }

    // Callback fired whenever note data changes (add/delete/modify), so dependent
    // UI (expression lane, articulation lane) can repaint and stay in sync.
    std::function<void()> onNoteDataChanged;

    void updateLastNoteVelocity (int vel) noexcept { lastNoteVelocity = juce::jlimit (1, 127, vel); }

    // Safe access: validate selectedNotes against active clip; remove any stale pointers.
    void validateSelectedNotes() noexcept
    {
        if (activeMidiClip == nullptr)
        {
            selectedNotes.clear();
            return;
        }

        auto& notes = activeMidiClip->getSequence().getNotes();
        for (int i = selectedNotes.size() - 1; i >= 0; --i)
        {
            if (!notes.contains (selectedNotes[i]))
                selectedNotes.remove (i);
        }
    }

    // Apply velocity to all selected notes.
    void applyVelocityToSelection (int newVelocity) noexcept
    {
        validateSelectedNotes();
        if (selectedNotes.isEmpty() || activeMidiClip == nullptr)
            return;

        auto& um = activeMidiClip->edit.getUndoManager();
        um.beginNewTransaction ("Set Velocity");

        for (auto* note : selectedNotes)
            if (note != nullptr)
                note->setVelocity (juce::jlimit (1, 127, newVelocity), &um);

        repaint();
        if (onNoteDataChanged) onNoteDataChanged();
    }

    // Apply length to all selected notes.
    void applyLengthToSelection (double newLengthBeats) noexcept
    {
        validateSelectedNotes();
        if (selectedNotes.isEmpty() || activeMidiClip == nullptr)
            return;

        auto& um = activeMidiClip->edit.getUndoManager();
        um.beginNewTransaction ("Set Length");

        for (auto* note : selectedNotes)
        {
            if (note != nullptr)
            {
                const double clampedLength = juce::jmax (1.0 / 16.0, newLengthBeats);
                note->setStartAndLength (note->getStartBeat(),
                                        tracktion::BeatDuration::fromBeats (clampedLength),
                                        &um);
            }
        }

        repaint();
        if (onNoteDataChanged) onNoteDataChanged();
    }

    // Apply humanize to all selected notes (randomize velocity and timing).
    void applyHumanizeToSelection (int strength) noexcept
    {
        validateSelectedNotes();
        if (selectedNotes.isEmpty() || activeMidiClip == nullptr || strength <= 0)
            return;

        auto& um = activeMidiClip->edit.getUndoManager();
        um.beginNewTransaction ("Humanize");

        juce::Random rng;
        const float strengthFactor = strength / 100.0f;

        for (auto* note : selectedNotes)
        {
            if (note != nullptr)
            {
                // Randomize velocity by ±strength%
                const int vel = note->getVelocity();
                const float velDelta = (rng.nextFloat() - 0.5f) * 2.0f * vel * strengthFactor;
                const int newVel = juce::jlimit (1, 127, (int) std::round (vel + velDelta));
                note->setVelocity (newVel, &um);

                // Randomize start time by ±strength% of 1/16 beat
                const double timingJitter = (rng.nextFloat() - 0.5f) * 2.0f * (1.0 / 16.0) * strengthFactor;
                const double newBeat = juce::jmax (0.0, note->getStartBeat().inBeats() + timingJitter);
                note->setStartAndLength (tracktion::BeatPosition::fromBeats (newBeat),
                                        note->getLengthBeats(),
                                        &um);
            }
        }

        repaint();
        if (onNoteDataChanged) onNoteDataChanged();
    }

    // Apply swing to all selected notes.
    void applySwingToSelection (int swingAmount) noexcept
    {
        validateSelectedNotes();
        if (selectedNotes.isEmpty() || activeMidiClip == nullptr || swingAmount <= 0 || inspector == nullptr)
            return;

        auto& um = activeMidiClip->edit.getUndoManager();
        um.beginNewTransaction ("Apply Swing");

        const int gridRes = inspector->getGridResolution();
        double subdivision = 1.0; // default: whole beat
        if (gridRes == 0) subdivision = 1.0;      // Free
        else if (gridRes == 1) subdivision = 0.25; // 1/4
        else if (gridRes == 2) subdivision = 0.125; // 1/8
        else if (gridRes == 3) subdivision = 0.0625; // 1/16
        else if (gridRes == 4) subdivision = 0.03125; // 1/32

        const double swingFactor = swingAmount / 100.0 * 0.5 * subdivision;

        for (auto* note : selectedNotes)
        {
            if (note != nullptr)
            {
                const double beat = note->getStartBeat().inBeats();
                const double beatInSubdivision = std::fmod (beat / subdivision, 2.0);

                // Apply swing to off-beat notes (those that fall on odd subdivisions).
                if (beatInSubdivision >= 1.0)
                {
                    const double newBeat = beat + swingFactor;
                    note->setStartAndLength (tracktion::BeatPosition::fromBeats (juce::jmax (0.0, newBeat)),
                                            note->getLengthBeats(),
                                            &um);
                }
            }
        }

        repaint();
        if (onNoteDataChanged) onNoteDataChanged();
    }

    // Trigger fold/scale update (called when fold toggle or scale changes).
    void refreshFoldMapping() noexcept
    {
        updateVisibleNotes();
        repaint();
    }

    // Fired after pixelsPerBeat changes (Ctrl/Cmd+scroll) — the owner resizes
    // THIS component to the new content width, synchronously, before the zoom
    // handler below repositions the viewport's scroll to re-anchor the cursor.
    std::function<void()> onZoomChanged;

    void paint (juce::Graphics& g) override
    {
        validateSelectedNotes(); // thread-safety: remove stale pointers before rendering
        updateVisibleNotes(); // fold/scale mapping for Y-axis

        // 1. FILL ABSOLUTE BACKGROUND
        g.fillAll (LAF::background);

        if (activeEdit == nullptr)
            return;

        const auto clipBounds = g.getClipBounds();
        const auto clipBoundsF = clipBounds.toFloat();

        // 2. PAINT KEY BACKGROUND + OUT-OF-SCALE DIMMING.
        // RAW 128-row iteration — deliberately NOT folded/visibleNotes-based.
        // Note rectangles (noteBounds(), below) also use RAW noteToY(), and
        // mouse hit-testing (mouseDown/mouseDrag) uses RAW yToNote()/noteToY()
        // from PianoRollLayout.h. Using folded indices here while everything
        // else stays raw is exactly what caused the "ghost note" desync — a
        // dragged note's rectangle rendering at its compressed fold-index row
        // while the mouse and note-name label agreed on the real MIDI pitch.
        // One coordinate system, used everywhere, fixes both at once.
        for (int note = 0; note < 128; ++note)
        {
            const float y = noteToY (note);
            if (y + pixelsPerNote < clipBounds.getY() || y > clipBounds.getBottom())
                continue; // off-screen — same "don't draw what's not visible" discipline as before

            // 1. Base piano key colour (black/white row shading).
            g.setColour (isBlackKey (note) ? blackKeyRowColour : LAF::background);
            g.fillRect (0.0f, y, (float) getWidth(), (float) pixelsPerNote);

            // 2. Scale dimming overlay, only for rows NOT in the active scale.
            if (isSnapToScale && ! activeScaleIntervals.isEmpty())
            {
                const int normalizedPitch = (note - rootNote + 120) % 12;
                if (! activeScaleIntervals.contains (normalizedPitch))
                {
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    g.fillRect (0.0f, y, (float) getWidth(), (float) pixelsPerNote);
                }
            }
        }

        // 3. PAINT GRID LINES (vertical/horizontal).
        double firstLocalBeat, lastLocalBeat;
        visibleLocalBeatRange (clipBounds.getX(), clipBounds.getRight(), firstLocalBeat, lastLocalBeat);

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

        // Black-key row guide (drawn after grid lines for proper layering).
        for (int note : visibleNotes)
        {
            if (! isBlackKey (note))
                continue;

            const float y = noteToYFolded (note);
            if (y >= clipBounds.getY() && y < clipBounds.getBottom())
            {
                g.setColour (blackKeyRowColour);
                g.fillRect (0.0f, y, (float) getWidth(), (float) pixelsPerNote);
            }
        }

        // 4. PAINT GHOST CHANNELS (background notes).
        // FL STUDIO GHOST CHANNELS: render semi-transparent notes from overlapping clips.
        // (Requires TE API iteration; simplified visual echo mode pending full implementation.)
        // TODO: Full implementation requires iterating edit->getAllTracks() and checking time overlap.

        // 5. PAINT ACTIVE MIDI NOTES (foreground).
        if (activeMidiClip != nullptr)
        {
            for (auto* note : activeMidiClip->getSequence().getNotes())
            {
                const auto bounds = noteBounds (*note);

                if (! clipBoundsF.intersects (bounds))
                    continue;

                // HSV velocity coloring: pastel professional palette (anti-eye-strain).
                const int velocity = note->getVelocity();
                const float hue = juce::jmap (float(velocity), 1.0f, 127.0f, 0.65f, 0.0f);
                juce::Colour noteColour = juce::Colour::fromHSV (hue, 0.65f, 0.85f, 1.0f);

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

                // Note label: always draw name with auto-clipping. Dynamic text contrast. Crisp bold font.
                const juce::String noteName = juce::MidiMessage::getMidiNoteName (note->getNoteNumber(), true, true, 4);
                const auto textColour = noteColour.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white;
                g.setColour (textColour);
                g.setFont (juce::Font (10.5f, juce::Font::bold));
                const auto textBounds = juce::Rectangle<int> ((int) bounds.getX() + 2, (int) bounds.getY() + 2,
                                                               (int) bounds.getWidth() - 4, (int) bounds.getHeight() - 4);
                g.drawText (noteName, textBounds, juce::Justification::centredLeft, true);
            }
        }

        // 6. PAINT PLAYHEAD / SELECTION LASSO.
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

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            if (activeMidiClip != nullptr && selectedNotes.size() > 0)
            {
                activeMidiClip->edit.getUndoManager().beginNewTransaction ("Delete Notes");
                for (auto* note : selectedNotes)
                {
                    activeMidiClip->getSequence().removeNote (*note, &activeMidiClip->edit.getUndoManager());
                }
                selectedNotes.clear();
                repaint();
                return true;
            }
        }
        return false;
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
                // Right-click on empty space: deselect all notes.
                selectedNotes.clear();
                // Right-click drag: start continuous eraser.
                activeMidiClip->edit.getUndoManager().beginNewTransaction ("Erase Notes");
                isErasing = true;
            }
            return;
        }

        if (hitNote != nullptr)
        {
            lastNoteLengthBeats = hitNote->getLengthBeats().inBeats(); // remember for pencil tool
            lastNoteVelocity = hitNote->getVelocity(); // remember for pencil tool
            const auto bounds = noteBounds (*hitNote);

            // MIDI AUDITIONING: grabbing an existing note (resize OR move) sounds
            // its pitch for as long as the gesture lasts — mouseUp always sends
            // the matching Note-Off (see mouseUp below).
            auditionNoteOn (activeMidiClip, hitNote->getNoteNumber(), hitNote->getVelocity());
            auditionedNoteNumber = hitNote->getNoteNumber();

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
                    repaint(); // instant visual feedback: show selection border
                }
            }

            return;
        }

        // Left-click on empty space: start marquee selection (Ableton-style, no Ctrl needed).
        // mouseUp will add a note if rect is tiny (just a click), or keep selection if dragged.
        // MIDI AUDITIONING: sound the pitch under the cursor immediately — if this
        // turns into a click-to-add-note (mouseUp, tiny rect), the user hears the
        // note they're about to draw; if it turns into a real marquee drag, mouseUp
        // still cleanly sends the matching Note-Off either way.
        {
            const int previewNote = yToNote (e.position.y);
            auditionNoteOn (activeMidiClip, previewNote, lastNoteVelocity);
            auditionedNoteNumber = previewNote;
        }
        isSelecting = true;
        dragStartX = e.position.x;
        dragStartY = e.position.y;
        selectionRect = { e.position.x, e.position.y, 0.0f, 0.0f };
        selectedNotes.clear();
        return;

        // NOTE: The code below is unreachable due to the return above.
        // In Ableton-style, clicking on empty space always starts marquee.
        // If the drag is tiny (< 3px), mouseUp treats it as a note-add instead.
        const double rawLocalBeat = (double) e.position.x / pixelsPerBeat;
        const double snappedBeat  = std::round (rawLocalBeat / 0.25) * 0.25;
        int rootNoteNumber = yToNote (e.position.y);

        // SCALE-SNAP on note creation — uses pushed member state.
        if (isSnapToScale)
            rootNoteNumber = snapNoteToScale (rootNoteNumber, rootNote, activeScaleIntervals);

        // STAMP MODE: chord insertion.
        if (inspector != nullptr && inspector->isStampModeEnabled())
        {
            activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add Chord");

            const juce::Array<int> chordIntervals = getChordIntervals (inspector->getChordType());

            for (int interval : chordIntervals)
            {
                const int chordNoteNumber = juce::jlimit (0, 127, rootNoteNumber + interval);
                activeMidiClip->getSequence().addNote (chordNoteNumber,
                                                       tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                       tracktion::BeatDuration::fromBeats (lastNoteLengthBeats),
                                                       lastNoteVelocity, 0,
                                                       &activeMidiClip->edit.getUndoManager());
            }

            repaint();
        }
        else
        {
            // Normal single-note insertion.
            activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add MIDI Note");
            activeMidiClip->getSequence().addNote (rootNoteNumber,
                                                   tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                   tracktion::BeatDuration::fromBeats (lastNoteLengthBeats),
                                                   lastNoteVelocity, 0,
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
            lastNoteLengthBeats = newLength; // remember for pencil tool
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

            // SCALE-SNAP during move: uses pushed member state.
            if (isSnapToScale)
                newNote = snapNoteToScale (newNote, rootNote, activeScaleIntervals);

            // MIDI AUDITIONING: retrigger only when the pitch actually changes
            // row — re-injecting Note-On every drag tick at the SAME pitch would
            // just re-attack/click the same note dozens of times a second.
            if (newNote != auditionedNoteNumber)
            {
                auditionNoteOff (activeMidiClip, auditionedNoteNumber);
                auditionNoteOn (activeMidiClip, newNote, lastNoteVelocity);
                auditionedNoteNumber = newNote;
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

                    // Apply scale snap if enabled (same pushed member state).
                    if (isSnapToScale)
                        noteNum = snapNoteToScale (noteNum, rootNote, activeScaleIntervals);

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
        // MIDI AUDITIONING: unconditionally end whatever's sounding — covers
        // every gesture branch below (erase/marquee/resize/move) with one call,
        // so a stuck note can never survive past mouseUp regardless of path.
        auditionNoteOff (activeMidiClip, auditionedNoteNumber);
        auditionedNoteNumber = -1;

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

                // Apply scale snap if enabled — uses pushed member state.
                if (isSnapToScale)
                    rootNoteNumber = snapNoteToScale (rootNoteNumber, rootNote, activeScaleIntervals);

                // Add note or chord.
                if (inspector != nullptr && inspector->isStampModeEnabled())
                {
                    activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add Chord");

                    const juce::Array<int> chordIntervals = getChordIntervals (inspector->getChordType());

                    for (int interval : chordIntervals)
                    {
                        const int chordNoteNumber = juce::jlimit (0, 127, rootNoteNumber + interval);
                        activeMidiClip->getSequence().addNote (chordNoteNumber,
                                                               tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                               tracktion::BeatDuration::fromBeats (lastNoteLengthBeats),
                                                               lastNoteVelocity, 0,
                                                               &activeMidiClip->edit.getUndoManager());
                    }
                }
                else
                {
                    activeMidiClip->edit.getUndoManager().beginNewTransaction ("Add MIDI Note");
                    activeMidiClip->getSequence().addNote (rootNoteNumber,
                                                           tracktion::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat)),
                                                           tracktion::BeatDuration::fromBeats (lastNoteLengthBeats),
                                                           lastNoteVelocity, 0,
                                                           &activeMidiClip->edit.getUndoManager());
                }
            }

            repaint();
            if (onNoteDataChanged) onNoteDataChanged();
            return;
        }

        // Required, not optional: without this, isResizing/isMoving/draggedNote
        // would stay set after the gesture ends, and the NEXT unrelated
        // mouseDrag (or a stale draggedNote left pointing at a note deleted by
        // an intervening Undo) would silently resume editing through a dangling
        // pointer. Every gesture must end its own state cleanly.
        if (isResizing || isMoving)
            if (onNoteDataChanged) onNoteDataChanged();

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
        // RAW Y — same coordinate system as the background paint loop and the
        // mouse hit-testing/drag math below. Using the folded Y here (while
        // everything else stayed raw) was the exact cause of the ghost-note
        // desync: a note's rectangle would render at its compressed fold-index
        // row while its own label already showed the correct (raw) MIDI pitch.
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
            {
                safeThis->repaint();
                if (safeThis->onNoteDataChanged) safeThis->onNoteDataChanged();
            }
        });
    }

    CrateMidiInspectorComponent* inspector = nullptr; // reference to grid-res/stamp/chord state
    double lastNoteLengthBeats = 1.0; // remembers pencil tool's last used note length
    int lastNoteVelocity = 100; // remembers pencil tool's last used velocity

    // Scale state pushed down via setScaleState() — the single source of truth for
    // paint() dimming AND mouseDown/mouseDrag physical snapping (no inspector read).
    bool isSnapToScale = false;
    int rootNote = 0;
    juce::Array<int> activeScaleIntervals;

    // Hide Empty Rows: folded Y-axis mapping (visibleNotes maps screen index -> MIDI note).
    std::vector<int> visibleNotes; // indices into 0-127, sorted descending (127 at index 0)

    // Update visibleNotes based on fold state and active scale.
    void updateVisibleNotes() noexcept
    {
        visibleNotes.clear();

        // If snap/fold is disabled (or no scale pushed), show all 128 notes.
        // Reads PUSHED member state, not the inspector — same single source of
        // truth as paint() and the mouse handlers.
        if (! isSnapToScale || activeScaleIntervals.isEmpty())
        {
            for (int note = 127; note >= 0; --note)
                visibleNotes.push_back (note);
            return;
        }

        // Fold enabled: only show notes that have content OR are in the active scale.
        std::set<int> notesWithContent;
        if (activeMidiClip != nullptr)
        {
            for (auto* note : activeMidiClip->getSequence().getNotes())
                notesWithContent.insert (note->getNoteNumber());
        }

        // Add notes in descending order (127 first).
        for (int note = 127; note >= 0; --note)
        {
            const bool hasContent = notesWithContent.find (note) != notesWithContent.end();
            const int normalizedPitch = (note - rootNote + 120) % 12; // correct root offset
            const bool inScale = activeScaleIntervals.contains (normalizedPitch);

            if (hasContent || inScale)
                visibleNotes.push_back (note);
        }
    }

    // Folded Y-axis mapping: pixel Y -> visible note index -> MIDI note number.
    int yToNoteFolded (float y) const noexcept
    {
        if (visibleNotes.empty())
            return 60; // fallback

        const int index = juce::jlimit (0, (int) visibleNotes.size() - 1,
                                       (int) std::floor (y / pixelsPerNote));
        return visibleNotes[index];
    }

    // Folded Y-axis mapping: MIDI note number -> visible note index -> pixel Y.
    float noteToYFolded (int midiNote) const noexcept
    {
        if (visibleNotes.empty())
            return 0.0f;

        // Find this note's index in visibleNotes.
        for (int i = 0; i < (int) visibleNotes.size(); ++i)
        {
            if (visibleNotes[i] == midiNote)
                return (float) (i * pixelsPerNote);
        }

        // Note not in visible list (shouldn't happen if updateVisibleNotes ran).
        return 0.0f;
    }

    te::MidiNote* draggedNote = nullptr;
    bool isResizing = false;

    int auditionedNoteNumber = -1; // -1 == not currently auditioning (see auditionNoteOn/Off above)

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
    gridContent->onNoteDataChanged = [this]
    {
        if (expressionLane != nullptr) expressionLane->repaint();
        if (articulationLane != nullptr) articulationLane->repaint();
    };

    viewport.setViewedComponent (gridContent.get(), false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    expressionLane = std::make_unique<PianoRollExpressionLane>();
    expressionLane->setKeyboardWidth (keyboardWidth);
    expressionLane->onVelocityChanged = [this] (int vel)
    {
        if (gridContent != nullptr)
            gridContent->updateLastNoteVelocity (vel);
    };
    addAndMakeVisible (expressionLane.get());

    articulationLane = std::make_unique<PianoRollArticulationLane>();
    articulationLane->setKeyboardWidth (keyboardWidth);
    addAndMakeVisible (articulationLane.get());

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
        articulationLane->setScrollOffset (x, y);
        articulationLane->setZoom (pixelsPerBeat, pixelsPerNote);
    };
}

CratePianoRollComponent::~CratePianoRollComponent() = default;

void CratePianoRollComponent::setInspectorComponent (CrateMidiInspectorComponent* insp) noexcept
{
    gridContent->setInspectorComponent (insp);
    keyboard->setInspectorComponent (insp);

    // Wire scale change callback to refresh fold mapping and repaint.
    if (insp != nullptr)
    {
        insp->onScaleChanged = [this]
        {
            if (gridContent != nullptr)
            {
                gridContent->refreshFoldMapping();
                gridContent->repaint(); // Update grid dimming when scale changes
            }
            if (keyboard != nullptr)
                keyboard->repaint();
        };

        // DATA PLUMBING: push resolved scale state (snap/root/intervals) down to
        // BOTH the grid (row dimming + physical snapping) and the keyboard (key
        // dimming). This is the single source of truth — neither reads back into
        // the inspector for scale state anymore.
        insp->onScaleStateChanged = [this] (bool snap, int root, juce::Array<int> intervals)
        {
            if (gridContent != nullptr)
                gridContent->setScaleState (snap, root, intervals);
            if (keyboard != nullptr)
                keyboard->setScaleState (snap, root, intervals);
        };

        // Prime the initial state immediately, so the grid/keyboard aren't blind
        // until the user first touches a control.
        insp->broadcastScaleState();

        // Wire slider callbacks to apply changes to selected notes.
        insp->onVelocitySliderChanged = [this] (int vel)
        {
            if (gridContent != nullptr)
                gridContent->applyVelocityToSelection (vel);
        };

        insp->onLengthSliderChanged = [this] (double length)
        {
            if (gridContent != nullptr)
                gridContent->applyLengthToSelection (length);
        };

        insp->onHumanizeApplied = [this] (int strength)
        {
            if (gridContent != nullptr)
                gridContent->applyHumanizeToSelection (strength);
        };

        insp->onSwingApplied = [this] (int swing)
        {
            if (gridContent != nullptr)
                gridContent->applySwingToSelection (swing);
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

    // Articulation lane needs the clip for rendering articulation blocks.
    articulationLane->setActiveClip (activeClip);

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
    constexpr int expressionLaneHeight = 120;
    constexpr int articulationLaneHeight = 80;
    constexpr int totalLaneHeight = expressionLaneHeight + articulationLaneHeight;

    const int w = getWidth();
    const int h = getHeight();

    // Position ruler at top (full width, fixed height).
    // Ruler is positioned in layoutContent() but starts at y=0.

    // Position keyboard on left (fixed width, full height).
    // Keyboard is positioned in layoutContent() but starts at x=0.

    // Position lanes at bottom (full width).
    expressionLane->setBounds (0, h - totalLaneHeight, w, expressionLaneHeight);
    articulationLane->setBounds (0, h - articulationLaneHeight, w, articulationLaneHeight);

    // Viewport: starts at y=rulerHeight, ends at y=(h-totalLaneHeight), starts at x=keyboardWidth.
    // NO PADDING — attach directly to ruler and keyboard with 0-pixel gaps.
    const int viewportX = keyboardWidth;
    const int viewportY = rulerHeight;
    const int viewportW = w - keyboardWidth;
    const int viewportH = h - rulerHeight - totalLaneHeight;

    viewport.setBounds (viewportX, viewportY, viewportW, viewportH);

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
