#pragma once

#include <JuceHeader.h>
#include <cmath>

/**
    Shared layout constants + time->x mapping for the arrangement view, so the ruler,
    track rows, clip components, and playhead overlay all agree on the grid without
    each redefining it.
*/
namespace CrateArrangement
{
    inline constexpr int headerWidth    = 200;
    inline constexpr int clipLaneHeight = 104;  // main clip lane row height — fits the
                                                 // Ableton-style header's IO text, R/S/M/A
                                                 // row, volume fader, and live meter bar
    inline constexpr int autoLaneHeight = 100;  // extra height added when automation is expanded
    inline constexpr int rulerHeight    = 24;
    inline constexpr int toolbarHeight  = 34;

    // Generous scrollable canvas length: 500 bars of 4/4 at 120 BPM — NOT a real
    // enforced project-length limit (TE's Edit has none), just "as far as this
    // app's ruler/grid currently extend". BUG FIX: this used to be a token 8
    // bars, which made the ruler/grid go visibly blank ("stuck at 9") the moment
    // you scrolled or zoomed in far enough to look past bar 9 — there was
    // nothing there to draw. What VARIES with zoom is how many PIXELS the whole
    // thing occupies (pixelsPerSecond, below), not this length.
    inline constexpr double tempoBpm       = 120.0;
    inline constexpr int    beatsPerBar    = 4;
    inline constexpr int    totalBars      = 500;
    inline constexpr double secondsPerBeat = 60.0 / tempoBpm;
    inline constexpr double totalSeconds   = totalBars * beatsPerBar * secondsPerBeat;

    // Timeline zoom (MASTER_ARCHITECTURE.md Zone 3 — Ctrl/Cmd+scroll in
    // ArrangementComponent). A single shared value, not per-Edit state: this app
    // shows exactly one Edit's arrangement at a time, so there is only ever one
    // "current" zoom level to track.
    inline constexpr double defaultPixelsPerSecond = 60.0;
    inline constexpr double minPixelsPerSecond      = 8.0;
    inline constexpr double maxPixelsPerSecond       = 500.0;
    inline double pixelsPerSecond = defaultPixelsPerSecond;

    // Maps time (seconds) to an absolute x position: headerWidth is the fixed
    // left column, everything after it is time * pixelsPerSecond — NOT stretched
    // to fit whatever width happens to be available (that was the old, pre-zoom
    // behaviour). Content wider than the visible viewport just scrolls.
    inline float timeToX (double seconds)
    {
        return (float) headerWidth + (float) (seconds * pixelsPerSecond);
    }

    // Inverse of timeToX() — maps an x pixel back to a time (seconds), clamped to
    // never go negative. Used by drag-and-drop file import and clip dragging to
    // turn an x-coordinate into a time.
    inline double xToTime (int x)
    {
        return juce::jmax (0.0, (double) (x - headerWidth) / pixelsPerSecond);
    }

    // Total scrollable content width (px) needed to show the full project length
    // at the CURRENT zoom level — TrackListContent/TrackRow size themselves to
    // this (clamped up to at least the viewport's visible width), so scrolling
    // and zooming stay consistent everywhere that draws against the grid.
    inline int contentWidthPx()
    {
        return headerWidth + (int) (totalSeconds * pixelsPerSecond);
    }

    // Which beat indices intersect the ABSOLUTE (unscrolled — i.e. straight out
    // of timeToX()) x-range [absoluteX1, absoluteX2), with a 1-beat margin on
    // each side. Callers loop ONLY this range instead of the whole (now
    // generous, 2000-beat) project length — essential once totalBars is large:
    // without it, every grid/ruler repaint would recompute and draw thousands
    // of off-screen ticks regardless of zoom, which is exactly the kind of
    // per-frame cost that turns into visible stutter during a zoom gesture.
    inline void visibleBeatRange (int absoluteX1, int absoluteX2, int& firstBeatOut, int& lastBeatOut)
    {
        const double t1 = xToTime (absoluteX1);
        const double t2 = xToTime (absoluteX2);

        firstBeatOut = juce::jmax (0, (int) std::floor (t1 / secondsPerBeat) - 1);
        lastBeatOut  = juce::jmin (totalBars * beatsPerBar, (int) std::ceil (t2 / secondsPerBeat) + 1);
    }
}
