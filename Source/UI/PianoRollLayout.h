#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include <cmath>

namespace te = tracktion::engine;

/**
    Shared X-axis (time) layout constants + pixel<->beat mapping for the Piano
    Roll — the Zone 4 counterpart to ArrangementLayout.h's CrateArrangement
    namespace, same "one shared global, not threaded through every nested
    class" reasoning: this app shows exactly one Piano Roll clip open at a time
    (Law I: Strict Single-Window Paradigm), so there is only ever one "current"
    clip anchor/zoom level to track, and PianoRollRuler/PianoRollGridContent
    both need to agree on it without CratePianoRollComponent threading
    getters/setters through both on every access/paint call.

    Y-AXIS (note rows + Keyboard Sidebar) mapping lives here too now — same
    shared-global reasoning as the X-axis section above. Note rendering/editing
    itself is still a later phase; this is row geometry + shading only.

    Coordinates here are LOCAL to the grid content (x = 0 at the active clip's
    own start, y = 0 at MIDI pitch 127), NOT absolute Edit-timeline pixels the
    way CrateArrangement's timeToX()/xToTime() are — a piano roll shows exactly
    one clip's own content, not a scrolling multi-track timeline, so there is
    no equivalent of Arrangement's fixed headerWidth offset to account for.
*/
namespace CratePianoRoll
{
    inline constexpr int rulerHeight    = 24;
    inline constexpr int keyboardWidth  = 48; // PianoRollKeyboard sidebar's fixed width
    inline constexpr int numMidiNotes   = 128;

    // Horizontal zoom — mirrors CrateArrangement::pixelsPerSecond exactly, just
    // beat-based instead of second-based (a MIDI clip's own natural unit, so
    // grid lines stay locked to the beat grid regardless of tempo).
    inline constexpr double defaultPixelsPerBeat = 60.0;
    inline constexpr double minPixelsPerBeat     = 8.0;
    inline constexpr double maxPixelsPerBeat     = 400.0;
    inline double pixelsPerBeat = defaultPixelsPerBeat;

    // Vertical zoom — row height in pixels per MIDI note.
    inline constexpr double defaultPixelsPerNote = 24.0;
    inline constexpr double minPixelsPerNote     = 6.0;
    inline constexpr double maxPixelsPerNote     = 80.0;
    inline double pixelsPerNote = defaultPixelsPerNote;

    // The Edit currently open in the Piano Roll, and the absolute Edit-beat its
    // active clip starts at (x = 0 in every helper below). Both set by
    // CratePianoRollComponent::setActiveClip(); activeEdit is nullptr when no
    // clip is open. activeEdit is what lets the grid query the REAL tempo/
    // time-signature track (tempoSequence) for accurate bar lines, rather than
    // assuming a fixed tempo/4-4 the way ArrangementLayout.h's simpler grid
    // still does (a deliberate accuracy upgrade for THIS view, per spec).
    inline te::Edit* activeEdit = nullptr;
    inline tracktion::BeatPosition clipStartBeat;

    // The clip itself — same shared-global reasoning, needed so
    // PianoRollGridContent's note rendering/hit-testing/mouse physics can
    // reach activeMidiClip->getSequence() directly. Set/cleared by
    // CratePianoRollComponent::setActiveClip() in lockstep with activeEdit/
    // clipStartBeat above (all three always describe the same clip or are all
    // simultaneously cleared — never a partial/inconsistent state).
    //
    // NAMED activeMidiClip, NOT activeClip: CratePianoRollComponent (the
    // enclosing class of every nested class that reads this) has its OWN
    // member literally named activeClip. Nested classes' unqualified name
    // lookup checks the ENCLOSING CLASS's scope before the enclosing
    // namespace — so inside PianoRollGridContent/PianoRollKeyboard, a
    // same-named global would silently resolve to CratePianoRollComponent::
    // activeClip (a non-static member with no accessible instance there)
    // instead of this global, and fail to compile. Confirmed the hard way.
    inline te::MidiClip* activeMidiClip = nullptr;

    // Absolute Edit-beat -> LOCAL pixel x (0 at the clip's own start).
    inline float beatToX (tracktion::BeatPosition beat)
    {
        return (float) ((beat - clipStartBeat).inBeats() * pixelsPerBeat);
    }

    // LOCAL pixel x -> absolute Edit-beat (inverse of beatToX()).
    inline tracktion::BeatPosition xToBeat (double x)
    {
        return clipStartBeat + tracktion::BeatDuration::fromBeats (x / pixelsPerBeat);
    }

    // Which LOCAL beat range (0 = clip start, never negative) intersects the
    // given LOCAL pixel-x window [x1, x2), with a 1-beat margin on each side —
    // callers loop only this range instead of the clip's whole length, same
    // "don't draw/compute what's off-screen" discipline CrateArrangement::
    // visibleBeatRange() already uses for the Arrangement ruler/grid.
    inline void visibleLocalBeatRange (double x1, double x2, double& firstBeatOut, double& lastBeatOut)
    {
        firstBeatOut = juce::jmax (0.0, x1 / pixelsPerBeat - 1.0);
        lastBeatOut  = x2 / pixelsPerBeat + 1.0;
    }

    //==========================================================================
    // Y-AXIS — the inverted mapping. CRITICAL TRAP (spec-mandated, verified by
    // the formula below): screen Y = 0 is the TOP, but MIDI pitch 127 (G8) must
    // be at the TOP and pitch 0 (C-2) at the BOTTOM — the opposite direction to
    // pitch number. Getting this backwards would put the lowest note at the top
    // of the keyboard, immediately and confusingly wrong to any musician.

    // Total scrollable content height (px) needed to show all 128 MIDI notes at
    // the CURRENT vertical zoom level — mirrors contentWidthPx()'s role for X.
    inline int gridContentHeightPx()
    {
        return (int) (numMidiNotes * pixelsPerNote);
    }

    // MIDI note number -> LOCAL pixel y (0 at note 127, increasing downward as
    // note number decreases).
    inline float noteToY (int midiNote)
    {
        return (float) ((127 - midiNote) * pixelsPerNote);
    }

    // LOCAL pixel y -> MIDI note number (inverse of noteToY()), clamped to the
    // valid 0-127 range.
    inline int yToNote (float y)
    {
        return juce::jlimit (0, 127, 127 - (int) std::floor (y / pixelsPerNote));
    }

    // Which MIDI note range intersects the given LOCAL pixel-y window [y1, y2),
    // with a 1-note margin on each side — same "don't draw what's off-screen"
    // discipline as visibleLocalBeatRange() above. Y is inverted relative to
    // note number, so the SMALLER y (y1, the top of the window) maps to the
    // HIGHER note — that's why highestNoteOut reads from y1, not y2.
    inline void visibleNoteRange (float y1, float y2, int& lowestNoteOut, int& highestNoteOut)
    {
        highestNoteOut = juce::jlimit (0, 127, yToNote (y1) + 1);
        lowestNoteOut  = juce::jlimit (0, 127, yToNote (y2) - 1);
    }

    // Standard piano black-key pattern within an octave: C#, D#, F#, G#, A# —
    // semitone offsets {1, 3, 6, 8, 10}. Shared by PianoRollKeyboard's key
    // rendering AND PianoRollGridContent's row shading so the two can never
    // visually disagree about which rows are "black".
    inline bool isBlackKey (int midiNote)
    {
        static constexpr bool blackSemitones[12] = {
            false, true, false, true, false, false, true, false, true, false, true, false
        };
        return blackSemitones[((midiNote % 12) + 12) % 12];
    }

    inline bool isC (int midiNote)
    {
        return ((midiNote % 12) + 12) % 12 == 0;
    }

    // MIDI note -> octave number, per the spec's own convention (pitch 0 =
    // "C-2", pitch 127 = "G8"): octave = (note / 12) - 2, so middle C (60) is
    // "C3" — the widely-used general-MIDI convention.
    inline int octaveOf (int midiNote)
    {
        return midiNote / 12 - 2;
    }
}
