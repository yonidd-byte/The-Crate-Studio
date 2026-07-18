#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "TimeRulerComponent.h"
#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Ableton-style arrangement view — Right-Side Header Column geometry.

      +--------------------------------------------------------------+
      | [+ Audio] [+ MIDI]                          (toolbar)        |
      +---------------------------------------------------+----------+
      | 1     2     3   ...                     (ruler)   |          |
      +---------------------------------------------------+----------+
      | clip lane (grid + clips)                           | header  |  <- 80px, or 180 if 'A' on
      +----------------------------------------------------+ (M/S/A) |
      | automation lane (when expanded)                     |         |
      +---------------------------------------------------+----------+
      | lane ...                                            | ...     |
      +---------------------------------------------------+----------+
      | (grid extends all the way to the absolute bottom)  | MASTER  |  <- pinned to the
      +---------------------------------------------------+----------+     bottom of the
       (vertically scrollable grid, LEFT — touches deviceChain)  RIGHT       header column only

    Corrected Column Geometry: the grid (viewport + TrackListContent) and the
    header column (TrackHeaderColumn) are genuinely SEPARATE Components split
    left/right FIRST — not a header floating inside the grid's own scrolling
    row. The header column scrolls VERTICALLY in lockstep with the grid
    (mirrors PianoRollKeyboard's proven pattern) but never horizontally — there
    is nothing to scroll horizontally in a fixed column. Master is a SINGLE
    piece now (MasterHeaderRow, right column only) — the left-side
    MasterLaneRow was removed entirely (Lead Architect correction: it was a
    dead ghost container blocking the grid) — the grid itself extends all the
    way to the absolute bottom of its own column with no Master strip at all.

    Track selection is tracked here; the selected track's header highlights and is the
    target for plugin instantiation. The 'A' toggle on a header (in the header column)
    expands that track's automation lane (in the grid column) via a bridge callback —
    see TrackListContent::setTrackAutomationVisible().
*/
class ArrangementComponent : public juce::Component,
                              private juce::Timer
{
public:
    ArrangementComponent (te::Edit& editToShow, CrateWorkflowManager& workflowToUse);
    ~ArrangementComponent() override;

    /** The currently selected track (plugin-load target), or nullptr if none. */
    te::AudioTrack::Ptr getSelectedTrack() const;

    /** Fires whenever a header is clicked (or one of its controls is dragged, which
        also selects that row). Bridge point for an external owner (e.g.
        CrateWorkflowManager via MainComponent) that wants to track selection itself. */
    std::function<void (te::AudioTrack*)> onTrackSelected;

    /** Task 5 (Ableton-Style Arrangement Master Track): fires when the pinned
        Master row (header + lane, anchored to the absolute bottom of this view,
        outside the scrolling track viewport) is clicked. Separate from
        onTrackSelected above since te::MasterTrack does not derive from
        te::AudioTrack — MainComponent pushes edit->getMasterTrack() into
        selection/Device Chain in response, same as clicking a real track. */
    std::function<void()> onMasterTrackSelected;

    /** Fires when a header's delete button is clicked, AFTER onTrackSelected has
        already fired for the same track (so an external selection model is
        up to date by the time this runs). This class does not delete the track
        itself — deletion + undo-transaction wrapping is CrateWorkflowManager's job;
        the owner is expected to call that and then rebuildTracks() below. */
    std::function<void (te::AudioTrack*)> onDeleteTrackRequested;

    /** Re-reads the Edit's current track list and rebuilds the row UI. Call after
        any external mutation (e.g. CrateWorkflowManager::deleteSelectedTrack()).
        Also fires onTracksChanged at the end, so it's the one place both the
        internal (+Audio/+MIDI buttons) and external (delete) mutation paths funnel
        through to notify anything else that mirrors the track list (MixerComponent). */
    void rebuildTracks();

    /** Fires at the end of every rebuildTracks() — i.e. whenever a track was added
        or removed, regardless of which side triggered it. MixerComponent is not
        aware of track add/delete on its own; MainComponent bridges this to
        MixerComponent::rebuildStrips() so the two views of the same Edit never
        drift out of sync. */
    std::function<void()> onTracksChanged;

    /** Phase 4 (The MIDI Suite): fires when a te::MidiClip should be opened in
        the piano-roll editor — either because the user double-clicked an
        existing MIDI clip, or because they double-clicked an empty lane (which
        creates a new clip first, then opens it). MainComponent bridges this to
        the Overlay Crossfade (hide Arrangement + Browser, show Piano Roll +
        Inspector — Law I). ArrangementComponent does NOT drive the crossfade
        itself; it only owns the clip creation/selection and reports which clip
        to open, same split as onTrackSelected/onDeleteTrackRequested above. */
    std::function<void (te::MidiClip*)> onMidiClipOpenRequested;

    // Drag-and-drop file import (MASTER_ARCHITECTURE.md Zone 3) — the
    // FileDragAndDropTarget interface itself now lives on MainComponent (the
    // window's root content component), NOT here. Two levels of elevation were
    // tried and both still reported a blocked OS cursor (TrackListContent, then
    // ArrangementComponent itself), so MainComponent is the last place left to
    // put it before there's no component left to blame. ArrangementComponent's
    // job is now purely: given coordinates ALREADY confirmed to be inside its
    // bounds and translated to its local space, do the actual work. It has no
    // opinion on whether it's the thing that caught the OS drag event at all.

    /** Performs the real Tracktion Engine insertion for a drop: track lookup by
        y, clip start time by x, insertWaveClip() per supported file (skipping any
        unsupported ones in a mixed drop), the whole thing wrapped in one Undo
        transaction. x/y must already be local to THIS component (MainComponent
        translates from its own coordinate space via getLocalPoint() before
        calling this). */
    void processDroppedAudio (const juce::StringArray& files, int x, int y);

    /** Live hover feedback while a file is being dragged over (before it's
        dropped) — same "x/y already local to this component" contract as
        processDroppedAudio(). No-ops (clears any existing highlight) if none of
        the dragged files are a supported audio format. */
    void updateDragHover (const juce::StringArray& files, int x, int y);

    /** Clears any active hover highlight — call when the drag leaves this
        component's bounds entirely. */
    void clearDragHover();

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    /** Ctrl/Cmd + scroll wheel = zoom (MASTER_ARCHITECTURE.md Zone 3). Plain
        scroll (no modifier) is deliberately left alone here — this is a
        juce::MouseListener override registered on `viewport` (see the
        constructor), which is a PARALLEL notification path, not the primary
        event target, so Viewport's own normal wheel-scroll handling is
        completely unaffected either way.

        THROTTLED: updates pixelsPerSecond immediately (cheap — a single
        double), but the expensive part (relayout of every clip in every track,
        grid/ruler repaint) is deferred to timerCallback() below, coalescing a
        rapid-fire scroll-wheel/trackpad zoom gesture (which can deliver dozens
        of events per second) down to one actual relayout roughly every
        zoomRelayoutIntervalMs, instead of doing the full relayout synchronously
        on every single wheel event — this is the concrete fix for "deep
        zooming stutters heavily". */
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    class TrackListContent;  // stacked LANE rows (grid + clips + automation) + playhead — NO header
    class TrackRow;          // one lane: clip lane + optional automation lane, no header at all

    // Corrected Column Geometry: genuinely separate fixed-width column, docked
    // right, vertically synced to the grid viewport but never horizontally
    // scrolled — see its own doc comment for why this replaced the earlier
    // "header floats inside the grid's scrolling row" approach.
    class TrackHeaderColumn;

    // Master is a SINGLE piece — pinned to the bottom of the RIGHT header
    // column only. The left-side MasterLaneRow was removed entirely (Lead
    // Architect correction: it was a dead ghost container blocking the grid).
    class MasterHeaderRow;

    void addTrack (bool asMidiTrack);
    void layoutContent();

    // Phase 4 MIDI Suite. openClip() fires onMidiClipOpenRequested if the clip
    // is a te::MidiClip (Condition A: double-click existing clip).
    // createAndOpenMidiClipAt() is Condition B: double-click an empty lane ->
    // insert a new MIDI clip matching the current time selection (transport
    // loop range if set, else a default 4-bar block at the click), select it,
    // then open it. mergeSelectedMidiClips() is the FL-style consolidate
    // utility, wrapped in one Undo transaction. showClipContextMenu() is the
    // right-click entry point that offers the consolidate action.
    void openClip (te::Clip* clip);
    void createAndOpenMidiClipAt (te::AudioTrack* track, double xSeconds);
    void mergeSelectedMidiClips (te::Track* track, const juce::Array<te::MidiClip*>& clips);
    void showClipContextMenu (te::Clip* clip);

    // The throttled other half of mouseWheelMove() — see its doc comment above.
    void timerCallback() override;

    // juce::Viewport has no public Listener/observer interface in this JUCE
    // version — keeping the (fixed, non-scrolling) ruler's ticks in sync with the
    // viewport's horizontal scroll position (once zooming makes the content wider
    // than the visible area) requires SUBCLASSING Viewport and overriding its
    // protected virtual visibleAreaChanged(), which is all this tiny type does.
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

    te::Edit& edit;
    CrateWorkflowManager& workflow; // threaded to TrackListContent -> TrackRow -> TrackHeaderComponent,
                                    // for CrateWorkflowManager::loadPluginOntoTrack() on a Browser plugin drop

    juce::TextButton addAudioButton { "+ Audio" };
    juce::TextButton addMidiButton  { "+ MIDI" };

    TimeRulerComponent ruler { edit };
    ScrollAwareViewport viewport;
    std::unique_ptr<TrackListContent> content;
    bool zoomRelayoutPending = false;

    // Corrected Column Geometry — genuinely separate right column, not part
    // of the grid's own scrolling content. See TrackHeaderColumn's doc comment.
    std::unique_ptr<TrackHeaderColumn> headerColumn;

    // Master — a single piece, pinned to the bottom of headerColumn's own
    // column (see resized()). No left-side lane piece any more.
    std::unique_ptr<MasterHeaderRow> masterHeader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangementComponent)
};
