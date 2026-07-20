#include "ArrangementComponent.h"
#include "TrackHeaderComponent.h"
#include "AutomationLaneComponent.h"
#include "ArrangementLayout.h"
#include "TheCrateLookAndFeel.h"
#include "CrateColors.h"
#include "CrateDesignSystem.h"
#include "FlatGridComboLookAndFeel.h"
#include "TrackUtils.h"

using namespace CrateArrangement;

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Bus/Return Default Collapsed State directive: same key
    // TrackHeaderComponent/CrateWorkflowManager use to seed/persist the fold
    // micro-state — duplicated here too (rather than a shared header) so
    // TrackRow (a separate Component tree from the header — see its own doc
    // comment) can seed its OWN initial row height correctly for a track
    // that's already folded the moment it's created, without needing to
    // reach across to the header instance for it.
    const juce::Identifier foldedPropertyID ("crateFolded");

    // Content-Driven Dynamic Height directive: same keys TrackHeaderComponent
    // persists — TrackRow needs them too (it has no live header instance to
    // ask) to compute the IDENTICAL height via TrackHeaderComponent::
    // computePreferredHeight().
    const juce::Identifier inputCategoryPropertyID ("crateInputCategory");
    const juce::Identifier outputCategoryPropertyID ("crateOutputCategory");

    // Master Track Fold Parity directive: Master has no te::Track of its own
    // to hang a property on, so this persists straight on the Edit's own
    // root state — same .crate save/load path (MASTER_ARCHITECTURE.md
    // invariant 3) every other persisted property here already uses.
    const juce::Identifier masterFoldedPropertyID ("crateMasterFolded");

    // Zone 3 grid spec: bars slightly brighter, beats very subtle — expressed as
    // true opacity over the lane fill rather than two similarly-dark flat colours,
    // so "low opacity" reads as low opacity regardless of the panel colour beneath.
    const auto gridBarColour  = juce::Colours::white.withAlpha (0.14f);
    const auto gridBeatColour = juce::Colours::white.withAlpha (0.045f);
    const auto playheadColour = CrateColors::PlayheadRed;
    const auto waveformColour = CrateColors::NeonBlue; // Global Color Purge: was an
                                                        // independent near-cyan literal
                                                        // (0xff29e0ff), now strictly the
                                                        // brand accent — architectural note:
                                                        // this is where FL Studio-style
                                                        // spectral coloring eventually
                                                        // replaces a flat colour, without
                                                        // touching the background treatment

    // Only draws the beats that intersect the Graphics context's OWN clip bounds
    // (i.e. whatever's actually dirty/visible right now) — essential now that
    // totalBars is a generous 500 rather than a token 8: looping the whole
    // project length on every repaint regardless of zoom/scroll is exactly the
    // kind of per-frame cost that turns into visible stutter during a zoom
    // gesture. g.getClipBounds() is already in this component's own local
    // (scrolled-with-content) coordinate space, which IS the "absolute" space
    // timeToX()/xToTime() use, so no extra offset math is needed here.
    void paintLaneGrid (juce::Graphics& g, int height)
    {
        const auto clip = g.getClipBounds();
        int firstBeat, lastBeat;
        visibleBeatRange (clip.getX(), clip.getRight(), firstBeat, lastBeat);

        for (int beat = firstBeat; beat <= lastBeat; ++beat)
        {
            const auto x = timeToX (beat * secondsPerBeat);
            g.setColour ((beat % beatsPerBar) == 0 ? gridBarColour : gridBeatColour);
            g.drawVerticalLine ((int) x, 0.0f, (float) height);
        }
    }

    const auto dropHighlightColour = juce::Colours::white.withAlpha (0.08f);

    // wav/mp3/aiff/flac only — matches the Lead UX Architect's spec exactly rather
    // than trusting every format juce::AudioFormatManager happens to have
    // registered (which may include things like ogg/rex the spec didn't ask for).
    bool isSupportedAudioFile (const juce::String& path)
    {
        const auto ext = juce::File (path).getFileExtension().toLowerCase();
        return ext == ".wav" || ext == ".mp3" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
    }

    // TE has no separate "MIDI track" type at the engine level — a MIDI track in
    // this app is just an AudioTrack seeded with an instrument (see addTrack()'s
    // FourOscPlugin seed for freshly-created ones). te::Plugin::isSynth() is the
    // engine-native way to ask "can this plugin turn MIDI into audio" —
    // FourOscPlugin::isSynth() returns true unconditionally, and
    // ExternalPlugin::isSynth() returns desc.isInstrument for a loaded VSTi — so
    // checking it across the track's pluginList is the correct Type Guard
    // (rather than inventing an app-level "track type" flag that could drift
    // from what the engine actually thinks the track can render).
    bool trackHasInstrument (te::AudioTrack& track)
    {
        for (auto* p : track.pluginList)
            if (p != nullptr && p->isSynth())
                return true;

        return false;
    }
}

//==============================================================================
// One clip, as a real Component — clips used to be painted directly by TrackRow,
// which couldn't support either per-clip waveform thumbnails or drag-to-move.
// Waveform generation runs via te::SmartThumbnail, which does its own
// background-thread decoding/proxy generation and calls this component's own
// repaint() as data streams in — nothing here touches file I/O, decoding, or the
// audio thread directly; SmartThumbnail's internal juce::Timer is what turns
// background progress into a UI repaint.
class ClipComponent : public juce::Component
{
public:
    ClipComponent (te::Edit& editToUse, te::Clip* clipToUse)
        : edit (editToUse), clip (clipToUse)
    {
        // Only AudioClipBase-derived clips (WaveAudioClip) have a source file to
        // thumbnail — MIDI/other clip types fall back to the flat block look this
        // app has always used for them.
        if (auto* audioClip = dynamic_cast<te::AudioClipBase*> (clip))
            thumbnail = std::make_unique<te::SmartThumbnail> (edit.engine, audioClip->getAudioFile(), *this, &edit);
    }

    te::Clip* getClip() const noexcept   { return clip; }

    // Set by the owning TrackRow. onDoubleClicked = Condition A (open this clip
    // in the piano roll); onContextMenuRequested = right-click (consolidate).
    std::function<void (te::Clip*)> onDoubleClicked;
    std::function<void (te::Clip*)> onContextMenuRequested;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (clip == nullptr)
            return;

        // Right-click (or ctrl-click on Mac) opens the context menu instead of
        // beginning a move-drag — return before touching the drag state so a
        // menu interaction can never be misread as the start of a drag.
        if (e.mods.isPopupMenu())
        {
            if (onContextMenuRequested)
                onContextMenuRequested (clip);

            return;
        }

        dragStartSeconds  = clip->getPosition().time.getStart().inSeconds();
        clipLengthSeconds = clip->getPosition().time.getLength().inSeconds();

        // Whole drag gesture (mouseDown through mouseUp) is ONE Undo step — every
        // setPosition() call inside mouseDrag() below lands in this same
        // transaction, not one transaction per pixel of movement.
        edit.getUndoManager().beginNewTransaction ("Move Clip: " + clip->getName());
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        // Condition A (Ableton behaviour): double-clicking an existing clip
        // opens it. The MIDI-vs-audio decision is made upstream in
        // ArrangementComponent::openClip() — a double-click on an audio clip
        // there is simply a no-op for now, not this view's concern.
        if (clip != nullptr && onDoubleClicked)
            onDoubleClicked (clip);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (clip == nullptr)
            return;

        // A move-drag never begins from a right-click (that path returned early
        // in mouseDown without setting drag state) — but guard anyway so a
        // right-button drag can't smear the clip around.
        if (e.mods.isPopupMenu())
            return;

        // Delta computed from the ORIGINAL drag-start position each time (not
        // incrementally per-event), so per-event rounding can never accumulate
        // drift over a long drag.
        const double deltaSeconds = (double) e.getDistanceFromDragStartX() / pixelsPerSecond;
        const double newStart = juce::jmax (0.0, dragStartSeconds + deltaSeconds);

        const auto pos = clip->getPosition();
        clip->setPosition ({ { tracktion::TimePosition::fromSeconds (newStart),
                                tracktion::TimePosition::fromSeconds (newStart + clipLengthSeconds) },
                              pos.offset });

        // Reposition immediately rather than waiting for a parent-triggered
        // relayout — this IS the relayout for this one clip, live during the drag.
        setTopLeftPosition ((int) timeToX (newStart), getY());
    }

    void paint (juce::Graphics& g) override
    {
        if (clip == nullptr)
            return;

        auto bounds = getLocalBounds().toFloat();

        // Dark semi-transparent background, per spec — the waveform (or, for a
        // non-audio clip, the flat block) draws on top of this.
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (bounds, 4.0f);

        auto nameStrip = bounds.removeFromTop (14.0f);

        if (thumbnail != nullptr)
        {
            // PERF FIX (deep-zoom stutter): only ask SmartThumbnail to compute
            // paths for the pixels actually on screen — g.getClipBounds() is
            // whatever's really dirty right now, which at a heavy zoom level can
            // be a small fraction of this clip's full (now very wide) width.
            // Drawing the intersected RECT alone isn't enough on its own, though:
            // drawChannels() maps whatever TimeRange it's given across the WHOLE
            // given rect, so the time range passed must shrink to match the same
            // intersected sub-rect too, or the waveform would render squeezed/
            // wrong-scaled into that smaller area instead of showing the correct
            // zoomed-in slice.
            const auto boundsToDraw = g.getClipBounds().getIntersection (getLocalBounds())
                                          .getIntersection (bounds.reduced (2.0f, 1.0f).toNearestInt());

            if (! boundsToDraw.isEmpty())
            {
                g.setColour (waveformColour);
                const auto lengthSeconds = clip->getPosition().time.getLength().inSeconds();

                // Whole-file range, proportionally narrowed to boundsToDraw's own
                // x-extent — every clip in this app is inserted via
                // insertWaveClip() with a zero offset (see processDroppedAudio()),
                // so "pixel x within this clip" and "time into the source file"
                // are a direct pixelsPerSecond mapping, no trim/offset to account
                // for yet.
                const auto startSeconds = juce::jlimit (0.0, lengthSeconds, boundsToDraw.getX() / pixelsPerSecond);
                const auto endSeconds   = juce::jlimit (0.0, lengthSeconds, boundsToDraw.getRight() / pixelsPerSecond);

                thumbnail->drawChannels (g, boundsToDraw,
                                          tracktion::TimeRange (tracktion::TimePosition::fromSeconds (startSeconds),
                                                                 tracktion::TimePosition::fromSeconds (endSeconds)),
                                          1.0f);
            }
        }
        else
        {
            g.setColour (LAF::clip);
            g.fillRoundedRectangle (bounds, 3.0f);
        }

        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (clip->getName(), nameStrip.reduced (4.0f, 0.0f), juce::Justification::centredLeft);

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }

private:
    te::Edit& edit;
    te::Clip* clip = nullptr; // raw: lifetime owned by the track's clip list — TrackRow
                              // rebuilds clipComponents whenever that list changes (its
                              // own ValueTree listener), same convention DeviceBlock/
                              // MixerStrip use for raw te::Plugin*/te::LevelMeterPlugin*.
    std::unique_ptr<te::SmartThumbnail> thumbnail;
    double dragStartSeconds = 0.0;
    double clipLengthSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipComponent)
};

//==============================================================================
// Dedicated playhead overlay — a thin vertical line, repositioned via its own
// 60Hz UI juce::Timer reading transport.getPosition() (a plain double read off
// the engine's already-thread-safe position query, same pattern every other
// timer-driven readout in this app uses — no audio-thread access here).
// Previously drawn inline inside TrackListContent::paintOverChildren(); a real
// Component now so it can be repositioned/repainted independently of a full
// TrackListContent repaint. setInterceptsMouseClicks(false, false) so it never
// blocks clicking/dragging a clip underneath it.
class PlayheadComponent : public juce::Component,
                          private juce::Timer
{
public:
    explicit PlayheadComponent (te::Edit& editToUse) : edit (editToUse)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (60);
    }

    ~PlayheadComponent() override { stopTimer(); }

    void paint (juce::Graphics& g) override   { g.fillAll (playheadColour); }

private:
    void timerCallback() override
    {
        auto* parent = getParentComponent();

        if (parent == nullptr)
            return;

        const auto seconds = edit.getTransport().getPosition().inSeconds();
        const auto x = (int) timeToX (seconds);
        setBounds (x - 1, 0, 2, parent->getHeight());
    }

    te::Edit& edit;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayheadComponent)
};

//==============================================================================
// Corrected Column Geometry: TrackRow is now PURELY the lane (grid + clips +
// automation lane) — no header at all. The header column is a genuinely
// separate, fixed-width component (ArrangementComponent::TrackHeaderColumn,
// docked to the right, vertically synced but never horizontally scrolled) —
// see its own doc comment for why the previous "floating header inside the
// lane's own scrolling row" approach was wrong (Master ended up as one
// full-width row instead of two independently-docked pieces).
class ArrangementComponent::TrackRow : public juce::Component,
                                        private juce::ValueTree::Listener
{
public:
    TrackRow (te::Edit& e, te::AudioTrack::Ptr t)
        : edit (e), track (t)
    {
        // Content-Driven Dynamic Height directive: seed this row's own height
        // from the SAME persisted properties TrackHeaderComponent reads —
        // TrackRow is a separate Component tree with no live header instance
        // to ask, so TrackHeaderComponent::computePreferredHeight() (a static
        // method taking just these facts) is the ONE shared authority both
        // sides compute from, rather than two independent formulas that could
        // silently drift apart.
        if (track != nullptr)
        {
            const bool isReturn = TrackUtils::isReturnTrack (*track);
            const bool isFolded = (bool) track->state.getProperty (foldedPropertyID, false);
            const int inputCategoryId  = (int) track->state.getProperty (inputCategoryPropertyID, 1);
            const int outputCategoryId = (int) track->state.getProperty (outputCategoryPropertyID, 1);

            rowHeight = TrackHeaderComponent::computePreferredHeight (isReturn, isFolded, inputCategoryId, outputCategoryId);
        }

        rebuildClips();

        // Clip add/remove (drag-and-drop file import today; clip delete/paste
        // later) is a direct child of the track's own state — same
        // "PluginList::state IS track->state" fact MixerStrip/
        // UniversalDeviceChainComponent already rely on for plugin add/remove,
        // just watching for clip children here instead.
        if (track != nullptr)
            track->state.addListener (this);
    }

    ~TrackRow() override
    {
        if (track != nullptr)
            track->state.removeListener (this);
    }

    te::AudioTrack* getTrack() const   { return track.get(); }

    // In-Track Automation Overlay (V2.0 UI/UX Manifesto section 4, "The
    // Overlay Method"): automation no longer changes row height — the curve
    // draws ON TOP of the clip lane (see AutomationLaneComponent, sized to this
    // row's full bounds). Content-Driven Dynamic Height directive: height is
    // now governed by whatever TrackHeaderComponent::computePreferredHeight()
    // says the matching header currently needs (fold state, return-ness,
    // input/output category) — see rowHeight's own doc comment.
    int getRowHeight() const           { return rowHeight; }

    /** Bridged from the header (a separate Component tree — see
        TrackHeaderColumn) whenever ANYTHING that changes its preferred
        height changes — fold toggle, or an Input/Output Category combo
        changing which rows are visible. Fires onRowHeightChanged so the
        whole list relayouts and the header column mirrors the new heights.
        The automation overlay (if visible) and clips re-lay against the new
        row height, preserving alignment. */
    void setRowHeight (int newHeight)
    {
        if (rowHeight == newHeight)
            return;

        rowHeight = newHeight;

        if (onRowHeightChanged)
            onRowHeightChanged();
    }

    // Set by the owning content — fires when THIS row's height changes (fold),
    // so TrackListContent can relayout the whole stack + re-sync the headers.
    std::function<void()> onRowHeightChanged;

    /** Forces every clip's bounds to be recomputed against the CURRENT zoom
        level, independent of whether this row's own bounds happened to change —
        JUCE's setBounds() skips calling resized() when the new bounds are
        identical to the old ones, which a zoom change doesn't guarantee here
        (e.g. zooming while the project already fits within the viewport). */
    void refreshLayoutForZoom()   { layoutClips(); }

    /** Shows/hides the automation OVERLAY (curve + dimmed clips) directly over
        this row. Row height never changes, so this is a pure repaint: no
        relayout is triggered, and no other row shifts position. Purge Clutter
        directive removed the header's 'A' toggle that used to call this via
        TrackHeaderColumn — currently unreachable from any UI control (a future
        global automation-view toggle or context-menu is meant to call it
        instead), but left fully intact and working. */
    void setAutomationVisible (bool shouldBeVisible)
    {
        automationVisible = shouldBeVisible;

        if (automationVisible && autoLane == nullptr && track != nullptr)
        {
            // Default the automation lane to the track's Volume parameter, aligned to
            // the same grid window as the clip lane it now overlays.
            if (auto* vp = track->getVolumePlugin())
            {
                autoLane = std::make_unique<AutomationLaneComponent> (edit, track, vp->volParam);
                autoLane->setVisibleLength (totalSeconds);
                autoLane->setBounds (getLocalBounds());
                addAndMakeVisible (*autoLane);
            }
        }

        if (autoLane != nullptr)
        {
            autoLane->setVisible (automationVisible);
            autoLane->toFront (false);
        }

        updateClipDimming();
        repaint();
    }

    // Phase 4 MIDI Suite — forwarded straight up to TrackListContent then
    // ArrangementComponent (which owns the edit + workflow to act on them).
    std::function<void (te::Clip*)> onClipOpenRequested;                       // Condition A
    std::function<void (te::AudioTrack*, double xSeconds)> onEmptyLaneDoubleClicked; // Condition B
    std::function<void (te::Clip*)> onClipContextMenuRequested;                // right-click consolidate

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Condition B (Ableton behaviour): a double-click that reaches the ROW
        // itself (not a child ClipComponent) landed on empty lane space —
        // create a new MIDI clip there. No header to guard against any more —
        // it's not part of this component at all now.
        if (track == nullptr)
            return;

        if (onEmptyLaneDoubleClicked)
            onEmptyLaneDoubleClicked (track.get(), xToTime (e.x));
    }

    void paint (juce::Graphics& g) override
    {
        const auto w = getWidth();

        // Corrected Column Geometry: lane spans the FULL row width — there is
        // no header to reserve space for here at all any more.
        g.setColour (LAF::panelLight);
        g.fillRect (0, 0, w, getHeight());
        paintLaneGrid (g, getHeight());

        // In-Track Automation Overlay: no divider to draw any more — there is
        // only ever one lane now. AutomationLaneComponent (added above, in
        // z-order, when active) draws the curve directly over the clips this
        // paint() just filled; updateClipDimming() is what actually makes
        // the clips underneath read as "background reference" rather than
        // fighting the curve for attention.

        // Row-bottom divider REMOVED from here — TrackListContent::
        // paintOverChildren() now draws ONE clearly-visible line per row,
        // covering this exact same job (CRITICAL FIX: the old LAF::background
        // line here was too subtle against panelLight to actually read as a
        // divider, which is what made the grid look like a featureless void).
    }

    void resized() override
    {
        if (autoLane != nullptr)
            autoLane->setBounds (getLocalBounds());

        layoutClips();
    }

private:
    // In-Track Automation Overlay: clips render at full opacity normally, but
    // dim to a faint background reference the instant automation is active —
    // Component::setAlpha() is a pure compositing property, so this needs no
    // change to ClipComponent's own paint() logic at all.
    void updateClipDimming()
    {
        for (auto& c : clipComponents)
            c->setAlpha (automationVisible ? 0.35f : 1.0f);
    }

    void rebuildClips()
    {
        clipComponents.clear();

        if (track != nullptr)
            for (auto* c : track->getClips())
            {
                auto comp = std::make_unique<ClipComponent> (edit, c);
                comp->onDoubleClicked        = [this] (te::Clip* cl) { if (onClipOpenRequested) onClipOpenRequested (cl); };
                comp->onContextMenuRequested = [this] (te::Clip* cl) { if (onClipContextMenuRequested) onClipContextMenuRequested (cl); };
                addAndMakeVisible (*comp);
                clipComponents.push_back (std::move (comp));
            }

        updateClipDimming();

        // Newly-rebuilt clips are added (and thus z-ordered) AFTER autoLane,
        // which would otherwise bury the overlay behind them the next time a
        // clip is added/removed while automation is showing.
        if (autoLane != nullptr)
            autoLane->toFront (false);

        layoutClips();
    }

    void layoutClips()
    {
        for (auto& c : clipComponents)
        {
            auto* clip = c->getClip();

            if (clip == nullptr)
                continue;

            const auto pos = clip->getPosition();
            const auto x1 = (int) timeToX (pos.time.getStart().inSeconds());
            const auto x2 = (int) timeToX (pos.time.getEnd().inSeconds());
            // Clip height tracks the CURRENT row height (getHeight()), so a folded
            // row's clips shrink with it instead of overflowing past the row edge.
            c->setBounds (x1, 4, juce::jmax (2, x2 - x1), juce::jmax (2, getHeight() - 8));
        }
    }

    // juce::ValueTree::Listener — any child add/remove/reorder on the track's own
    // state MIGHT be a clip (clip ValueTrees use per-type XML tags, not one shared
    // "IDs::CLIP" marker, so this deliberately doesn't try to filter more
    // precisely than "not this track's already-known non-clip children"). Worst
    // case a spurious rebuild recreates identical ClipComponents — harmless,
    // mildly wasteful, never incorrect. Deferred via callAsync + SafePointer:
    // these fire synchronously from wherever the mutation happened (a Load, an
    // Undo/Redo), potentially nested in a call stack this row shouldn't tear
    // itself down inside of.
    void valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree) override      { onPossibleClipListChange (parentTree, childTree); }
    void valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int) override { onPossibleClipListChange (parentTree, childTree); }

    void onPossibleClipListChange (juce::ValueTree& parentTree, juce::ValueTree& childTree)
    {
        if (track == nullptr || parentTree != track->state || childTree.hasType (te::IDs::PLUGIN))
            return;

        juce::Component::SafePointer<TrackRow> safeThis (this);

        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->rebuildClips();
        });
    }

    te::Edit& edit;
    te::AudioTrack::Ptr track;
    std::unique_ptr<AutomationLaneComponent> autoLane;
    std::vector<std::unique_ptr<ClipComponent>> clipComponents;
    bool automationVisible = false;
    int rowHeight = clipLaneHeight; // safe fallback for a null track; the constructor always computes the real value otherwise

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};

//==============================================================================
class ArrangementComponent::TrackListContent : public juce::Component,
                                                private juce::Timer
{
public:
    explicit TrackListContent (te::Edit& e) : edit (e)
    {
        startTimerHz (30);

        addAndMakeVisible (playhead);
    }

    ~TrackListContent() override { stopTimer(); }

    // Phase 4 MIDI Suite — aggregated from every TrackRow, bridged up to
    // ArrangementComponent (which owns edit + workflow to act on them).
    std::function<void (te::Clip*)> onClipOpenExternally;
    std::function<void (te::AudioTrack*, double xSeconds)> onEmptyLaneDoubleClickedExternally;
    std::function<void (te::Clip*)> onClipContextMenuExternally;

    /** Fired at the end of rebuild() — the track LIST itself changed (added/
        removed), not just a row's height. ArrangementComponent's headerColumn
        needs a full rebuild of its own header list in response (see
        getTrackOrder()), not just a relayout. */
    std::function<void()> onRebuilt;

    /** Fired at the end of relayout() (which rebuild() also calls) — the
        track list's Y positions were recomputed (a track added/removed).
        Row heights themselves are now always constant (In-Track Automation
        Overlay replaced the old height-changing "Expandable Lane"), so this
        no longer fires from an automation toggle — only from an actual row
        being added/removed. headerColumn reads getRowHeights() fresh and
        repositions its existing headers to match — no full rebuild needed
        for this, just new Y positions. */
    std::function<void()> onRelayout;

    // Hybrid Bus/Return Architecture: `tracksToShow` is the REGULAR subset
    // only (TrackUtils::splitTracks()'s regularTracks) — return tracks are
    // docked separately (see ArrangementComponent::ReturnLaneDock) and never
    // appear in this scrollable list. ArrangementComponent::rebuildTracks()
    // computes the split ONCE and is the only caller.
    void rebuild (const std::vector<te::AudioTrack*>& tracksToShow)
    {
        rows.clear();

        for (auto* t : tracksToShow)
        {
            auto row = std::make_unique<TrackRow> (edit, t);
            row->onRowHeightChanged = [this] { relayout(); }; // fold changes this row's height -> restack + re-sync headers
            row->onClipOpenRequested        = [this] (te::Clip* cl)                    { if (onClipOpenExternally) onClipOpenExternally (cl); };
            row->onEmptyLaneDoubleClicked   = [this] (te::AudioTrack* tr, double secs) { if (onEmptyLaneDoubleClickedExternally) onEmptyLaneDoubleClickedExternally (tr, secs); };
            row->onClipContextMenuRequested = [this] (te::Clip* cl)                    { if (onClipContextMenuExternally) onClipContextMenuExternally (cl); };

            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        relayout();

        // rebuild() re-adds every row via addAndMakeVisible(), which places each
        // one ABOVE playhead in z-order (added later wins) even though playhead
        // itself was added first, back in the constructor — pull it back to the
        // front so it stays drawn over the rows, not hidden behind them.
        playhead.toFront (false);

        if (onRebuilt)
            onRebuilt();
    }

    void setSelectedTrack (te::AudioTrack* t)
    {
        selectedTrackId = (t != nullptr) ? t->itemID : te::EditItemID();
        // No visual highlight to update here any more — that's entirely
        // ArrangementComponent::TrackHeaderColumn's job now (see its own
        // setSelectedTrack(), driven from the SAME onTrackSelected callback).
    }

    te::AudioTrack::Ptr getSelectedTrack() const
    {
        if (! selectedTrackId.isValid())
            return {};

        for (auto* t : te::getAudioTracks (edit))
            if (t->itemID == selectedTrackId)
                return t;

        return {};
    }

    /** Tracks in the EXACT same order as `rows` — TrackHeaderColumn mirrors
        this order 1:1 so row index N here always corresponds to header index
        N there. */
    std::vector<te::AudioTrack*> getTrackOrder() const
    {
        std::vector<te::AudioTrack*> result;
        result.reserve (rows.size());

        for (auto& row : rows)
            result.push_back (row->getTrack());

        return result;
    }

    /** Current row heights, in the same order as getTrackOrder() — read by
        TrackHeaderColumn::relayoutFromHeights() to mirror each lane row's
        actual height. Every row is the same constant clipLaneHeight today
        (see TrackRow::getRowHeight()) — this stays a per-row query rather
        than a single shared constant in case a future row type genuinely
        needs a different height again. */
    std::vector<int> getRowHeights() const
    {
        std::vector<int> result;
        result.reserve (rows.size());

        for (auto& row : rows)
            result.push_back (row->getRowHeight());

        return result;
    }

    /** Finds the row for this track and shows/hides its automation overlay.
        No current UI caller (see TrackRow::setAutomationVisible()'s doc
        comment) — a future global automation-view toggle or context-menu is
        meant to call this directly. */
    void setTrackAutomationVisible (te::AudioTrack* track, bool visible)
    {
        for (auto& row : rows)
            if (row->getTrack() == track)
                row->setAutomationVisible (visible);
    }

    /** Bridged from TrackHeaderColumn — pushes the header's OWN
        getPreferredHeight() onto the matching lane row (which fires
        onRowHeightChanged -> relayout -> header re-sync) whenever fold OR an
        Input/Output Category combo changes which rows the header needs. */
    void setTrackRowHeight (te::AudioTrack* track, int newHeight)
    {
        for (auto& row : rows)
            if (row->getTrack() == track)
                row->setRowHeight (newHeight);
    }

    void setMinHeight (int h)   { minHeight = h; }

    int preferredHeight() const
    {
        int total = 0;
        for (auto& row : rows)
            total += row->getRowHeight();
        return total;
    }

    /** Timeline zoom changed — force every row to recompute its clips' bounds
        against the new pixelsPerSecond, then repaint the grid. See TrackRow::
        refreshLayoutForZoom()'s doc comment for why this can't just rely on
        resized() firing naturally. */
    void refreshLayoutForZoom()
    {
        for (auto& row : rows)
            row->refreshLayoutForZoom();

        repaint();
    }

    void resized() override
    {
        int y = 0;
        for (auto& row : rows)
        {
            const int h = row->getRowHeight();
            row->setBounds (0, y, getWidth(), h);
            y += h;
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LAF::background);

        // Distinct musical grid drawn across the WHOLE arrangement background — not
        // just inside each track's own lane — so it reads as one continuous canvas
        // (Zone 3 spec) rather than stopping dead below the last track row.
        paintLaneGrid (g, getHeight());

        if (rows.empty())
        {
            g.setColour (LAF::textDim);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText ("No tracks yet  —  use  + Audio  /  + MIDI  to add one",
                         getLocalBounds(), juce::Justification::centred);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // CRITICAL FIX: horizontal divider lines — drawn AFTER every row
        // (paintOverChildren, not paint()) so they're never hidden behind a
        // row's own opaque lane fill. One line per row, at its exact bottom Y —
        // derived from the SAME getRowHeight() each row's own bounds/height
        // come from, so a line can never drift out of sync with where a lane
        // actually ends. Spans the FULL width, matching where each header in
        // headerColumn ends (both sides iterate the same rows in the same
        // order — see ArrangementComponent::TrackHeaderColumn's doc comment).
        {
            g.setColour (juce::Colours::darkgrey);
            int y = 0;

            for (auto& row : rows)
            {
                y += row->getRowHeight();
                g.drawLine (0.0f, (float) y, (float) getWidth(), (float) y, 1.0f);
            }
        }

        if (rows.empty())
            return;

        // Drop-target hover highlight — a subtle lighter overlay over whichever
        // track lane the dragged file is currently over, so the user knows where
        // the clip will land before releasing.
        if (hoverRowIndex >= 0 && hoverRowIndex < (int) rows.size())
        {
            int y = 0;
            for (int i = 0; i < hoverRowIndex; ++i)
                y += rows[(size_t) i]->getRowHeight();

            // Right-Side Headers: lane now spans the full row width.
            g.setColour (dropHighlightColour);
            g.fillRect (0, y, getWidth(), rows[(size_t) hoverRowIndex]->getRowHeight());
        }
    }

    // File drag-and-drop hover highlight + hit-testing. Deliberately NOT a
    // FileDragAndDropTarget itself — see ArrangementComponent.h's doc comment on
    // why that interface lives on ArrangementComponent instead (JUCE's file-drag
    // hit-test stops at the FIRST ancestor implementing it, walking up from the
    // deepest hit component; TrackListContent, being three levels down inside a
    // Viewport, is exactly the kind of nesting that made this worth eliminating
    // rather than debugging blind). This class still owns `rows`, so it exposes
    // the lookups ArrangementComponent needs after translating drag coordinates
    // into this component's local space.
    void setHoverRow (int newIndex)
    {
        if (hoverRowIndex == newIndex)
            return;

        hoverRowIndex = newIndex;
        repaint();
    }

    /** Which row index (into `rows`) the given LOCAL y falls in, or -1 if none
        (empty track list, or y past the last row). Shared by the hover highlight
        and the actual drop logic so they can never disagree about which track a
        given y maps to. */
    int rowIndexForY (int y) const
    {
        int top = 0;

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int h = rows[i]->getRowHeight();

            if (y >= top && y < top + h)
                return (int) i;

            top += h;
        }

        return -1;
    }

    /** The track at the given row index, or nullptr if out of range. */
    te::AudioTrack* trackForRow (int rowIndex) const
    {
        if (rowIndex < 0 || rowIndex >= (int) rows.size())
            return nullptr;

        return rows[(size_t) rowIndex]->getTrack();
    }

private:
    void relayout()
    {
        setSize (getWidth(), juce::jmax (preferredHeight(), minHeight));
        resized();

        if (onRelayout)
            onRelayout();
    }

    void timerCallback() override { repaint(); }

    te::Edit& edit;
    PlayheadComponent playhead { edit };
    std::vector<std::unique_ptr<TrackRow>> rows;
    int hoverRowIndex = -1;
    te::EditItemID selectedTrackId;
    int minHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackListContent)
};

//==============================================================================
// Hybrid Bus/Return Architecture — return tracks (TrackUtils::isReturnTrack())
// are DOCKED here, directly above MasterHeaderRow's lane slot, and never
// scroll — same "pinned, never scrolls" contract Master itself already has
// (see ArrangementComponent::resized()). Deliberately NOT hosted inside a
// juce::Viewport, unlike TrackListContent — reuses TrackRow as-is (identical
// clip/automation-overlay rendering), just without TrackListContent's own
// scroll/playhead/file-drag-drop-hover machinery, none of which a typically
// tiny, fixed return-track dock needs. File-drop-to-import does not reach
// these rows (ArrangementComponent::updateDragHover()/processDroppedAudio()
// only ever query the main `content`) — a disclosed, minor gap, not a
// requirement of this pass.
class ArrangementComponent::ReturnLaneDock : public juce::Component
{
public:
    explicit ReturnLaneDock (te::Edit& e) : edit (e) {}

    std::function<void (te::Clip*)> onClipOpenRequested;
    std::function<void (te::AudioTrack*, double xSeconds)> onEmptyLaneDoubleClicked;
    std::function<void (te::Clip*)> onClipContextMenuRequested;

    void rebuild (const std::vector<te::AudioTrack*>& returnTracks)
    {
        rows.clear();

        for (auto* t : returnTracks)
        {
            auto row = std::make_unique<TrackRow> (edit, t);
            row->onRowHeightChanged = [this] { positionRows(); };
            row->onClipOpenRequested        = [this] (te::Clip* cl)                    { if (onClipOpenRequested) onClipOpenRequested (cl); };
            row->onEmptyLaneDoubleClicked   = [this] (te::AudioTrack* tr, double secs) { if (onEmptyLaneDoubleClicked) onEmptyLaneDoubleClicked (tr, secs); };
            row->onClipContextMenuRequested = [this] (te::Clip* cl)                    { if (onClipContextMenuRequested) onClipContextMenuRequested (cl); };

            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        positionRows();
    }

    /** Bridged from ReturnHeaderDock (header side) — finds the matching row
        by track and pushes the header's own getPreferredHeight() onto it,
        mirroring TrackListContent::setTrackRowHeight()'s identical job for
        regular tracks. Firing this changes getTotalHeight(), so the caller
        (ArrangementComponent) must re-run its own resized() afterward — this
        class has no scroll offset / viewport to keep in sync, so unlike the
        regular list there's no relayoutFromHeights() indirection needed here. */
    void setTrackRowHeight (te::AudioTrack* track, int newHeight)
    {
        for (auto& row : rows)
            if (row->getTrack() == track)
                row->setRowHeight (newHeight);
    }

    /** Total height every current row needs stacked — this IS the dock's
        height in ArrangementComponent::resized(); ReturnHeaderDock's own
        getTotalHeight() must always equal this (same tracks, same order). */
    int getTotalHeight() const
    {
        int total = 0;

        for (auto& row : rows)
            total += row->getRowHeight();

        return total;
    }

    void resized() override   { positionRows(); }

private:
    void positionRows()
    {
        int y = 0;

        for (auto& row : rows)
        {
            const int h = row->getRowHeight();
            row->setBounds (0, y, getWidth(), h);
            y += h;
        }
    }

    te::Edit& edit;
    std::vector<std::unique_ptr<TrackRow>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReturnLaneDock)
};

//==============================================================================
// Corrected Ableton Column Geometry: a genuinely SEPARATE, fixed-width column
// docked to the right — NOT a header floating inside the grid's own scrolling
// row (that earlier approach put Master's header at the wrong place entirely,
// which is the bug this whole pass corrects). Mirrors PianoRollKeyboard's own
// proven pattern in this exact codebase: a plain Component (not a second
// Viewport) whose rows are repositioned by subtracting a vertical scroll
// offset, kept in sync with the grid's viewport via setVerticalOffset() (see
// ArrangementComponent's own viewport.onViewportMoved wiring). Horizontal
// scrolling of the grid never touches this column at all — it has nothing to
// scroll horizontally.
//
// Owns one TrackHeaderComponent per track, in the EXACT same order as
// TrackListContent's own `rows` (both iterate te::getAudioTracks(edit)
// identically) — index N here always corresponds to lane row N there, which
// is what relayoutFromHeights() relies on to stack headers at the same Y as
// their corresponding lane row, including when a track's automation lane
// changes that row's height.
class ArrangementComponent::TrackHeaderColumn : public juce::Component
{
public:
    TrackHeaderColumn (te::Edit& e, CrateWorkflowManager& w) : edit (e), workflow (w) {}

    std::function<void (te::AudioTrack*)> onTrackSelected;
    std::function<void (te::AudioTrack*)> onDeleteTrackRequested;

    /** Bridged to TrackListContent::setTrackRowHeight() by ArrangementComponent —
        the fold arrow / Input-Output Category combos live HERE (header side);
        the height they control lives on the lane row (content side). Carries
        the header's own getPreferredHeight() (Content-Driven Dynamic Height
        directive), not just a fold bool. */
    std::function<void (te::AudioTrack*, int)> onFoldToggled;

    // Hybrid Bus/Return Architecture: `tracksToShow` is the REGULAR subset
    // only, matching TrackListContent::rebuild()'s own list 1:1 — see
    // ArrangementComponent::rebuildTracks(), the only caller.
    void rebuild (const std::vector<te::AudioTrack*>& tracksToShow)
    {
        headers.clear();
        headerTrackIds.clear();

        for (auto* t : tracksToShow)
        {
            auto header = std::make_unique<TrackHeaderComponent> (t, workflow);
            header->onSelect = [this, t] { if (onTrackSelected) onTrackSelected (t); };
            header->onDeleteRequested = [this, t] { if (onDeleteTrackRequested) onDeleteTrackRequested (t); };
            header->onFoldToggle = [this, t, ptr = header.get()]
            {
                if (onFoldToggled)
                    onFoldToggled (t, ptr->getPreferredHeight());
            };

            addAndMakeVisible (*header);
            headers.push_back (std::move (header));
            // Parallel array (not a public accessor on TrackHeaderComponent —
            // it has none) so selection highlighting below can identify which
            // header belongs to which track without modifying that class.
            headerTrackIds.push_back (t != nullptr ? t->itemID : te::EditItemID());
        }

        updateSelectionHighlight();
    }

    /** Repositions every header to match the CURRENT height of its
        corresponding lane row (TrackListContent::getRowHeights(), same index
        order) — call after any relayout of the lane side (row added/removed,
        automation lane toggled, etc.). */
    void relayoutFromHeights (const std::vector<int>& rowHeights)
    {
        int y = 0;

        for (size_t i = 0; i < headers.size(); ++i)
        {
            const int h = (i < rowHeights.size()) ? rowHeights[i] : headers[i]->getPreferredHeight();
            headers[i]->setBounds (0, y - verticalOffset, getWidth(), h);
            y += h;
        }

        contentHeight = y;
    }

    /** Kept in sync with the grid viewport's vertical scroll position (see
        ArrangementComponent's viewport.onViewportMoved) — this column scrolls
        vertically WITH the grid (so header N still lines up with lane row N),
        but never horizontally (there is nothing to scroll horizontally here). */
    void setVerticalOffset (int newOffsetPx, const std::vector<int>& rowHeights)
    {
        if (verticalOffset == newOffsetPx)
            return;

        verticalOffset = newOffsetPx;
        relayoutFromHeights (rowHeights);
    }

    void setSelectedTrack (te::AudioTrack* t)
    {
        selectedTrackId = (t != nullptr) ? t->itemID : te::EditItemID();
        updateSelectionHighlight();
    }

private:
    void updateSelectionHighlight()
    {
        for (size_t i = 0; i < headers.size(); ++i)
            headers[i]->setSelected (selectedTrackId.isValid() && headerTrackIds[i] == selectedTrackId);
    }

    te::Edit& edit;
    CrateWorkflowManager& workflow;
    std::vector<std::unique_ptr<TrackHeaderComponent>> headers;
    std::vector<te::EditItemID> headerTrackIds; // parallel to `headers`
    te::EditItemID selectedTrackId;
    int verticalOffset = 0;
    int contentHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderColumn)
};

//==============================================================================
// Hybrid Bus/Return Architecture — the header-side twin of ReturnLaneDock:
// owns one TrackHeaderComponent per return track, in the EXACT same order
// ReturnLaneDock's own rows use (both are rebuilt from the SAME
// TrackUtils::splitTracks().returnTracks vector — see
// ArrangementComponent::rebuildTracks(), the only place that computes it).
// Unlike TrackHeaderColumn, this never scrolls (no verticalOffset to track —
// it isn't shifted by the grid viewport's scroll position at all, it's
// pinned).
class ArrangementComponent::ReturnHeaderDock : public juce::Component
{
public:
    ReturnHeaderDock (CrateWorkflowManager& w) : workflow (w) {}

    std::function<void (te::AudioTrack*)> onTrackSelected;
    std::function<void (te::AudioTrack*)> onDeleteTrackRequested;

    /** Bridged to ReturnLaneDock::setTrackRowHeight() by ArrangementComponent
        — same header-owns-the-arrow/lane-owns-the-height split
        TrackHeaderColumn::onFoldToggled already uses for regular tracks. Now
        carries the header's own getPreferredHeight() (Content-Driven Dynamic
        Height directive), not just a fold bool. */
    std::function<void (te::AudioTrack*, int)> onFoldToggled;

    void rebuild (const std::vector<te::AudioTrack*>& returnTracks)
    {
        headers.clear();
        headerTrackIds.clear();

        for (auto* t : returnTracks)
        {
            auto header = std::make_unique<TrackHeaderComponent> (t, workflow);
            header->onSelect = [this, t] { if (onTrackSelected) onTrackSelected (t); };
            header->onDeleteRequested = [this, t] { if (onDeleteTrackRequested) onDeleteTrackRequested (t); };
            header->onFoldToggle = [this, t, ptr = header.get()]
            {
                if (onFoldToggled)
                    onFoldToggled (t, ptr->getPreferredHeight());
            };

            addAndMakeVisible (*header);
            headers.push_back (std::move (header));
            headerTrackIds.push_back (t != nullptr ? t->itemID : te::EditItemID());
        }

        updateSelectionHighlight();
        positionHeaders();
    }

    /** Re-lays out (does NOT rebuild) — call after a fold toggle changes one
        header's height, or after ArrangementComponent::resized() gives this
        dock a fresh width. */
    void refreshLayout()   { positionHeaders(); }

    /** Total height every current header needs stacked — MUST always equal
        ReturnLaneDock::getTotalHeight() (same tracks, same order); the two
        sides (this header's own getPreferredHeight() vs. the matching
        TrackRow's rowHeight) are kept in sync purely via the onFoldToggled
        bridge, same proven pattern TrackHeaderColumn/TrackListContent
        already rely on for regular tracks. */
    int getTotalHeight() const
    {
        int total = 0;

        for (auto& h : headers)
            total += h->getPreferredHeight();

        return total;
    }

    void setSelectedTrack (te::AudioTrack* t)
    {
        selectedTrackId = (t != nullptr) ? t->itemID : te::EditItemID();
        updateSelectionHighlight();
    }

    void resized() override   { positionHeaders(); }

private:
    void updateSelectionHighlight()
    {
        for (size_t i = 0; i < headers.size(); ++i)
            headers[i]->setSelected (selectedTrackId.isValid() && headerTrackIds[i] == selectedTrackId);
    }

    void positionHeaders()
    {
        int y = 0;

        for (auto& h : headers)
        {
            const int rowH = h->getPreferredHeight();
            h->setBounds (0, y, getWidth(), rowH);
            y += rowH;
        }
    }

    CrateWorkflowManager& workflow;
    std::vector<std::unique_ptr<TrackHeaderComponent>> headers;
    std::vector<te::EditItemID> headerTrackIds; // parallel to `headers`
    te::EditItemID selectedTrackId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReturnHeaderDock)
};

//==============================================================================
// Master Arrangement UI Overhaul (Lead Architect directive): stripped down to
// exactly what Master has — Name (which IS the Mute toggle, same Ableton
// nameplate paradigm every regular track's MutePlate/name-plate already
// uses), Volume, Pan, Meter. NO input routing (Master has no input — the old
// dummy "Ext. In" combo was actively wrong, not just a harmless placeholder),
// NO dedicated Mute button (the old "M" TextButton — removed entirely, its
// job absorbed into the name plate), NO Solo/Record/Automation (none ever
// applied to Master). Mute is REAL — bound to VolumeAndPanPlugin::
// muteOrUnmute() on edit.getMasterVolumePlugin() (te::Track::isMuted()/
// setMute() are no-op stubs MasterTrack never overrides, so the track-level
// mute path was never usable for Master; the plugin-level one is). Volume AND
// Pan are REAL, bound directly to edit.getMasterVolumePlugin() — the exact
// same object MasterStrip's own Mixer controls read/write, kept in sync
// bidirectionally via te::AutomatableParameter::Listener (same pattern
// TrackHeaderComponent/MixerStrip already use for the identical "two views,
// one Edit" relationship). Meter reads the same te::LevelMeterPlugin
// MasterStrip already ensures exists (idempotent find-or-create).
//
// Disclosed deviation from MixerStrip's own MasterStrip: THAT class (the
// Mixer's Master strip) still keeps a separate dedicated Mute button — this
// class converts to nameplate-mute here specifically, matching the dominant
// pattern everywhere else in this app (every regular track's MutePlate/name-
// plate already works this way) rather than the Mixer's own Master strip,
// which is the one remaining outlier. Not touched in this pass.
class ArrangementComponent::MasterHeaderRow : public juce::Component,
                                               private juce::Timer,
                                               private te::AutomatableParameter::Listener
{
public:
    explicit MasterHeaderRow (te::Edit& editToUse) : edit (editToUse)
    {
        setWantsKeyboardFocus (false);

        // Bulletproof Live Mode / Aggressive Caching directive — same
        // reasoning as TrackHeaderComponent's own setBufferedToImage() call:
        // this row's panel fills/borders only change on user action
        // (select/mute/fold), so JUCE caches them to a bitmap; the level
        // meter stays real-time because timerCallback() below scopes its
        // repaint() to meterBounds only, not the whole component.
        setBufferedToImage (true);

        // Same idempotent find-or-create MasterStrip's constructor uses —
        // reads the SAME underlying te::LevelMeterPlugin either way, so this
        // row's meter and the Mixer's MasterStrip meter show identical levels.
        meterPlugin = edit.getMasterPluginList().findFirstPluginOfType<te::LevelMeterPlugin>();

        if (meterPlugin == nullptr)
        {
            auto plugin = edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, juce::PluginDescription());
            edit.getMasterPluginList().insertPlugin (plugin, -1, nullptr);
            meterPlugin = dynamic_cast<te::LevelMeterPlugin*> (plugin.get());
        }

        if (meterPlugin != nullptr)
            meterPlugin->measurer.addClient (meterClient);

        // The name plate IS the Mute toggle (see mouseDown()) — clicks must
        // fall through to this component's own mouseDown(), same
        // setInterceptsMouseClicks(false, false) MixerStrip's identical
        // trackNameLabel uses. Column 1 Parity directive: the standard track
        // header's OWN name font (13.0f), not a bespoke larger legacy size.
        // Text Contrast Fix directive: colour is hardcoded purely off
        // selected (see refreshNameLabelContrast()), not mute state.
        addAndMakeVisible (nameLabel);
        nameLabel.setText ("MASTER", juce::dontSendNotification);
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (CrateDesignSystem::Typography::headerNameFontSize, juce::Font::bold));
        nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        nameLabel.setInterceptsMouseClicks (false, false);
        refreshNameLabelContrast();

        // Master Track Fold Parity directive: same disclosure glyph a
        // standard track's Column 1 uses, reading isCollapsed straight off
        // this row (no second copy of the state).
        addAndMakeVisible (foldArrow);
        foldArrow.isExpanded = [this] { return ! isCollapsed; };
        foldArrow.onClick = [this] { toggleFold(); };

        isCollapsed = (bool) edit.state.getProperty (masterFoldedPropertyID, false);

        // Redundant Dropdown Removal directive: Master's Column 2 is now a
        // SINGLE "1/2" hardware-output dropdown — the old second "Master
        // Out"/"Stereo Out" combo was redundant (Master only ever has one
        // real destination). Same disclosed-cosmetic treatment every other
        // routing combo in this app already uses (no real hardware-output
        // enumeration exists in this engine).
        addAndMakeVisible (outputCombo);
        outputCombo.setColour (juce::ComboBox::backgroundColourId, CrateColors::DarkBackground);
        outputCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colours::transparentBlack);
        outputCombo.setColour (juce::ComboBox::textColourId,       CrateColors::BrandGray);
        outputCombo.setColour (juce::ComboBox::arrowColourId,      CrateColors::NeonBlue);
        outputCombo.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colours::transparentBlack);
        outputCombo.setJustificationType (juce::Justification::centredLeft);
        outputCombo.setLookAndFeel (&flatGridLookAndFeel);
        outputCombo.addItem ("1/2", 1);
        outputCombo.setSelectedId (1, juce::dontSendNotification);
        outputCombo.setTooltip ("Master's fixed hardware output - display only, not wired to real hardware routing yet.");

        // REAL Volume + Pan — bound directly to edit.getMasterVolumePlugin(),
        // the same object MasterStrip's own Mixer controls use. Column 3
        // Parity directive: CrateVolumeBar/CratePanBar (CrateValueBox.h) —
        // the EXACT same flat 13px drag-boxes a standard track's Column 3
        // Row 2 uses, not a juce::Slider LinearHorizontal bar / rotary
        // pan_knob.png filmstrip. Same class, same paint(), same
        // vertical-drag mouse mechanics — 100% parity, not a lookalike.
        volumePlugin = edit.getMasterVolumePlugin();

        volumeSlider.setRange (-60.0, 6.0);
        addAndMakeVisible (volumeSlider);
        addAndMakeVisible (panBar);

        if (volumePlugin != nullptr)
        {
            volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
            panBar.setValue (volumePlugin->getPan(), juce::dontSendNotification);

            volumeSlider.onDragStart = [this]
            {
                volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction ("Tweak Master Volume");
                volumePlugin->volParam->parameterChangeGestureBegin();
            };
            volumeSlider.onDragEnd = [this] { volumePlugin->volParam->parameterChangeGestureEnd(); };
            volumeSlider.onValueChange = [this] { volumePlugin->setVolumeDb ((float) volumeSlider.getValue()); };

            panBar.onDragStart = [this]
            {
                volumePlugin->panParam->getEdit().getUndoManager().beginNewTransaction ("Tweak Master Pan");
                volumePlugin->panParam->parameterChangeGestureBegin();
            };
            panBar.onDragEnd = [this] { volumePlugin->panParam->parameterChangeGestureEnd(); };
            panBar.onValueChange = [this] { volumePlugin->setPan ((float) panBar.getValue()); };

            // Bidirectional sync — a tweak from MasterStrip's OWN Mixer
            // controls (same underlying plugin) reaches back here too.
            volumePlugin->volParam->addListener (this);
            volumePlugin->panParam->addListener (this);
        }

        startTimerHz (24);
    }

    ~MasterHeaderRow() override
    {
        stopTimer();

        outputCombo.setLookAndFeel (nullptr);

        if (volumePlugin != nullptr)
        {
            volumePlugin->volParam->removeListener (this);
            volumePlugin->panParam->removeListener (this);
        }

        if (meterPlugin != nullptr)
            meterPlugin->measurer.removeClient (meterClient);
    }

    void setSelected (bool shouldBeSelected)
    {
        if (selected == shouldBeSelected)
            return;

        selected = shouldBeSelected;
        refreshNameLabelContrast();
        repaint();
    }

    std::function<void()> onSelected;

    // Master Track Fold Parity directive: the owner (ArrangementComponent)
    // re-runs its own resized() off this whenever Master's fold state
    // changes — same "no lane dock to keep in step" simplicity that let
    // MasterLaneRow be deleted entirely (see the class's own top comment).
    std::function<void()> onFoldToggled;

    // Master Track Fold Parity directive: expanded height stays Master's
    // existing fixed clipLaneHeight (Directive 2 is about ALIGNMENT within
    // that height, not its magnitude); folded height reads the SAME shared
    // computePreferredHeight() a standard folded track uses, not a separate
    // hand-picked literal, so the two can never drift out of sync.
    int getPreferredHeight() const
    {
        namespace DS = CrateDesignSystem::Metrics::TrackHeader;
        return isCollapsed
                   ? TrackHeaderComponent::computePreferredHeight (false, true, DS::noInputCategoryId, DS::masterOutputCategoryId)
                   : CrateArrangement::clipLaneHeight;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        setSelected (true);

        if (onSelected)
            onSelected();

        // The "MASTER" name plate IS the Mute toggle now (Ableton Mute
        // paradigm — same as every regular track's MutePlate/name-plate,
        // replacing the dedicated muteButton this row used to have). Guarded
        // to a single click, same as MixerStrip's identical name-plate-mute
        // pattern.
        if (e.getNumberOfClicks() == 1 && nameLabel.getBounds().contains (e.getPosition()) && volumePlugin != nullptr)
        {
            volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction ("Mute Master");
            volumePlugin->muteOrUnmute();
            // muteOrUnmute() changes volParam's value synchronously, which
            // fires currentValueChanged() below — repaint() here too so the
            // click feels instant rather than waiting on the listener
            // round-trip (paint() reads mute state live, nothing to cache).
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        namespace DS = CrateDesignSystem::Metrics::TrackHeader;

        const auto h = getHeight();

        // Selection Isolation directive: Columns 2+3's LightBackground panel
        // fill NEVER changes with `selected` any more — only Column 1 below
        // reacts to selection, exactly like a standard track's
        // getIdentityFillColour() is the ONLY thing that changes there too.
        g.fillAll (CrateColors::LightBackground);

        // Column 1 identity fill — Master Track Parity directive: a distinct,
        // premium fill (the SAME violet MasterStrip's own nameplate uses in
        // the Mixer — CrateDesignSystem::Colors::masterNameplateViolet),
        // darkened when muted (Master's only "Mute Plate" is this column —
        // see mouseDown() — so mute feedback lives here instead of on the
        // text), brightened toward white when selected — Selection Isolation
        // directive: selection is confined to THIS column, nothing else in
        // the row reacts to it.
        {
            const bool isMuted = volumePlugin != nullptr && volumePlugin->getVolumeDb() <= -90.0f;
            juce::Colour base (CrateDesignSystem::Colors::masterNameplateViolet);
            if (isMuted)
                base = base.darker (0.5f);

            g.setColour (selected ? base.interpolatedWith (juce::Colours::white, 0.4f) : base);
            g.fillRect (0, 0, DS::column1Right, h);
        }

        // Folded State Parity directive: same blank, disabled "dummy"
        // ComboBox-styled placeholder box a standard folded track's Column 2
        // shows in place of its real (now-hidden) dropdown. Just the fill
        // here — its border, like every other stroke, is cloned into
        // paintOverChildren() below (Exact Stroke Parity directive).
        if (isCollapsed && ! collapsedRoutingPlaceholder.isEmpty())
        {
            g.setColour (CrateColors::DarkBackground);
            g.fillRect (collapsedRoutingPlaceholder);
        }

        // Live level meter fill — drawn directly (not a child Slider/Component)
        // in the reserved meterBounds computed by resized(), same "flat fill,
        // no gradient/bevel" language TrackHeaderComponent's own meter uses.
        g.setColour (LAF::lcdBackground);
        g.fillRect (meterBounds);

        constexpr float meterFloorDb = -60.0f;
        constexpr float meterRangeDb = 66.0f;
        const auto normalised = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
        auto fill = meterBounds;
        fill.setWidth (fill.getWidth() * normalised);

        g.setColour (meterLevelDb > -3.0f ? LAF::meterHot : LAF::accent);
        g.fillRect (fill);

        if (meterLevelDb > meterFloorDb + 0.5f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText (juce::String (meterLevelDb, 1), meterBounds.reduced (3.0f, 0.0f),
                        juce::Justification::centredRight);
        }

        // Structural container border (column separators, top/bottom/right
        // cuts) moved to paintOverChildren() — Exact Stroke Parity
        // directive: every one of these is now a byte-for-byte clone of
        // TrackHeaderComponent::paintOverChildren()'s own stroke logic, so
        // it lives in the SAME override, for the SAME "hardware grill drawn
        // after children" reason (see that method's own doc comment).
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        namespace DS = CrateDesignSystem::Metrics::TrackHeader;

        const auto w = getWidth();
        const auto h = getHeight();

        // Exact Stroke Parity directive: byte-for-byte clone of
        // TrackHeaderComponent::paintOverChildren()'s own stroke logic —
        // absolute opaque 2px black column separators (never gate on
        // isCollapsed — the 3-column x boundaries never move), the folded
        // dummy-box border, then the global top/bottom/right-edge cuts that
        // make this row dock visually as the SAME premium hardware grid
        // every standard track's header uses, instead of the old low-alpha
        // "quiet hairline" treatment.
        g.setColour (juce::Colours::black);
        g.fillRect (DS::column1Right, 0, DS::separatorW, h);
        g.fillRect (DS::column2Right, 0, DS::separatorW, h);

        if (isCollapsed && ! collapsedRoutingPlaceholder.isEmpty())
            g.drawRect (collapsedRoutingPlaceholder, 1);

        g.fillRect (0, 0, w, DS::rowBottomBorderH);
        g.fillRect (0, h - DS::rowBottomBorderH, w, DS::rowBottomBorderH);
        g.fillRect (w - DS::separatorW, 0, DS::separatorW, h);
    }

    void resized() override
    {
        namespace DS = CrateDesignSystem::Metrics::TrackHeader;

        // Master's OWN outer bounds (set by ArrangementComponent::resized()
        // via getPreferredHeight()) already reflect isCollapsed — everything
        // below is a literal pixel layout within whatever that height is,
        // not a dynamic content-driven stack.
        const int h = getHeight();
        const int col3X = DS::column2Right + 1;

        outputCombo.setVisible (! isCollapsed);

        if (isCollapsed)
        {
            // Folded State Parity directive: Column 1/2/3 all centre on the
            // EXACT same foldedY = (h - rowH) / 2 a standard folded track
            // uses. Column 2's real dropdown hides in favour of a blank
            // dummy placeholder box (paint() draws it) — same "Ableton never
            // leaves a column visually empty" reasoning. Volume + Pan stay
            // live: they're Master's ONLY Column 3 content (no Box A/B/C to
            // fall back on the way a standard track has).
            const int foldedY = (h - DS::rowH) / 2;

            foldArrow.setBounds (DS::identityPad, foldedY, DS::foldArrowSize, DS::foldArrowSize);
            const int nameX = DS::identityPad + DS::foldArrowSize + DS::identityPad;
            nameLabel.setBounds (nameX, foldedY, DS::column1Right - nameX - DS::identityPad, DS::rowH);

            auto col2 = juce::Rectangle<int> (DS::column1Right, 0, DS::column2Right - DS::column1Right, h)
                            .reduced (DS::col2Inset)
                            .withTrimmedLeft (DS::separatorW);
            collapsedRoutingPlaceholder = { col2.getX(), foldedY, col2.getWidth(), DS::rowH };

            volumeSlider.setBounds (col3X + 2,  foldedY, 46, DS::rowH);
            panBar.setBounds       (col3X + 52, foldedY, 46, DS::rowH);
        }
        else
        {
            collapsedRoutingPlaceholder = {};

            // Expanded State Alignment directive: pinned to the TOP row
            // (y=4), NOT vertically centred — a deliberate departure from a
            // standard track's own expanded Column 1 (which spans its full
            // height), so Master's single Column 2 row / Column 3 boxes
            // line up with the very FIRST routing row every standard
            // track's Column 2 grid starts at, forming one shared
            // horizontal datum line straight across the whole header dock.
            constexpr int topY = 4;

            foldArrow.setBounds (DS::identityPad, topY, DS::foldArrowSize, DS::foldArrowSize);
            const int nameX = DS::identityPad + DS::foldArrowSize + DS::identityPad;
            nameLabel.setBounds (nameX, topY, DS::column1Right - nameX - DS::identityPad, DS::rowH);

            auto col2 = juce::Rectangle<int> (DS::column1Right, 0, DS::column2Right - DS::column1Right, h)
                            .reduced (DS::col2Inset)
                            .withTrimmedLeft (DS::separatorW);
            outputCombo.setBounds (col2.getX(), topY, col2.getWidth(), DS::rowH);

            volumeSlider.setBounds (col3X + 2,  topY, 46, DS::rowH);
            panBar.setBounds       (col3X + 52, topY, 46, DS::rowH);
        }

        // Exact Meter Parity directive: byte-for-byte clone of
        // TrackHeaderComponent::layoutColumn3()'s own meter math — width 7,
        // anchored col3X + col3Width - 10 - 7 from the left (i.e. 10px in
        // from Column 3's own right edge), 3px top/bottom clearance derived
        // from getHeight() so it's automatically correct in BOTH isFolded
        // states without any separate folded-height branch of its own.
        constexpr int meterVerticalPad = 3;
        meterBounds = juce::Rectangle<int> (col3X + DS::col3Width - 10 - 7, meterVerticalPad, 7, h - meterVerticalPad * 2).toFloat();
    }

private:
    // Text Contrast Fix directive: hardcoded purely off `selected` — NOT
    // mute state any more (mute feedback lives in the Column 1 fill's own
    // darken-when-muted in paint() instead, so it isn't silently dropped).
    void refreshNameLabelContrast()
    {
        nameLabel.setColour (juce::Label::textColourId, selected ? juce::Colours::black : juce::Colours::white);
    }

    // Master Track Fold Parity directive: mirrors TrackHeaderComponent's own
    // toggleFold() — persists straight onto the Edit's root state (Master
    // has no te::Track of its own to hang a property on), re-lays out
    // locally, then tells the owner (ArrangementComponent) to re-run ITS
    // OWN resized() so the header dock's reserved height (which reads this
    // row's getPreferredHeight()) catches up.
    void toggleFold()
    {
        isCollapsed = ! isCollapsed;
        edit.state.setProperty (masterFoldedPropertyID, isCollapsed, &edit.getUndoManager());
        resized();
        repaint();

        if (onFoldToggled)
            onFoldToggled();
    }

    void currentValueChanged (te::AutomatableParameter&) override
    {
        if (volumePlugin == nullptr)
            return;

        volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
        panBar.setValue (volumePlugin->getPan(), juce::dontSendNotification);
        repaint(); // mute state (paint()'s Column 1 darken) reads live, nothing to cache
    }

    void curveHasChanged (te::AutomatableParameter&) override {}

    void timerCallback() override
    {
        if (meterPlugin == nullptr)
            return;

        const auto levelL = meterClient.getAndClearAudioLevel (0);
        const auto levelR = meterClient.getAndClearAudioLevel (1);
        meterLevelDb = juce::jmax (levelL.dB, levelR.dB);

        // Aggressive Caching directive: scoped to meterBounds, NOT a bare
        // repaint() — this row is now setBufferedToImage(true) (see the
        // constructor), and a whole-component repaint() would invalidate
        // the ENTIRE cached bitmap on every 24Hz meter tick, making the
        // cache strictly worse than no caching at all. A scoped repaint
        // only re-bakes that thin strip back into the cached image.
        repaint (meterBounds.getSmallestIntegerContainer());
    }

    te::Edit& edit;
    te::VolumeAndPanPlugin::Ptr volumePlugin; // edit.getMasterVolumePlugin() — real, shared with MasterStrip
    te::LevelMeterPlugin* meterPlugin = nullptr; // raw: lifetime owned by edit.getMasterPluginList()
    te::LevelMeasurer::Client meterClient;
    float meterLevelDb = -60.0f; // -INF floor — real level once timerCallback() starts polling

    juce::Label nameLabel; // also the Mute toggle — see mouseDown()/refreshNameLabelContrast()
    CrateFoldArrow foldArrow; // Master Track Fold Parity directive
    juce::ComboBox outputCombo; // Redundant Dropdown Removal directive — Master's ONE real routing dropdown
    FlatGridComboLookAndFeel flatGridLookAndFeel; // Strict I/O Grid directive — same flat chrome TrackHeaderComponent's combos use
    CrateVolumeBar volumeSlider; // Column 3 Parity directive — same flat 13px box a standard track's Column 3 uses
    CratePanBar panBar; // Column 3 Parity directive — same flat 13px box, vertical-drag mechanic
    juce::Rectangle<float> meterBounds; // computed in resized(), read by paint()
    juce::Rectangle<int> collapsedRoutingPlaceholder; // Folded State Parity directive — Column 2's dummy box

    bool isCollapsed = false;
    bool selected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterHeaderRow)
};

//==============================================================================
ArrangementComponent::ArrangementComponent (te::Edit& editToShow, CrateWorkflowManager& workflowToUse)
    : edit (editToShow), workflow (workflowToUse)
{
    addAndMakeVisible (addAudioButton);
    addAndMakeVisible (addMidiButton);
    addAudioButton.onClick = [this] { addTrack (false); };
    addMidiButton.onClick  = [this] { addTrack (true); };

    addAndMakeVisible (ruler);

    content = std::make_unique<TrackListContent> (edit);

    // Phase 4 MIDI Suite double-click / context-menu routing.
    content->onClipOpenExternally = [this] (te::Clip* c) { openClip (c); };
    content->onEmptyLaneDoubleClickedExternally = [this] (te::AudioTrack* t, double secs) { createAndOpenMidiClipAt (t, secs); };
    content->onClipContextMenuExternally = [this] (te::Clip* c) { showClipContextMenu (c); };

    // Hybrid Bus/Return Architecture — the docked return-track lane, same
    // clip/context-menu routing as `content` above, just for the separate
    // non-scrolling track list.
    returnLaneDock = std::make_unique<ReturnLaneDock> (edit);
    returnLaneDock->onClipOpenRequested = [this] (te::Clip* c) { openClip (c); };
    returnLaneDock->onEmptyLaneDoubleClicked = [this] (te::AudioTrack* t, double secs) { createAndOpenMidiClipAt (t, secs); };
    returnLaneDock->onClipContextMenuRequested = [this] (te::Clip* c) { showClipContextMenu (c); };
    addAndMakeVisible (*returnLaneDock);

    // Corrected Column Geometry: whenever the lane side's row list or row
    // heights change, the header column re-syncs from it — see
    // TrackHeaderColumn's own doc comment for why it reads FROM content
    // rather than tracking its own independent state.
    content->onRebuilt = [this]
    {
        headerColumn->rebuild (content->getTrackOrder());
        headerColumn->relayoutFromHeights (content->getRowHeights());
    };
    content->onRelayout = [this] { headerColumn->relayoutFromHeights (content->getRowHeights()); };

    viewport.setViewedComponent (content.get(), false);

    // Both directions now: zooming in can make the content wider than the
    // viewport, and this is what makes the extra width actually reachable.
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    // Keeps the (fixed, non-scrolling) ruler's ticks in sync whenever the user
    // scrolls the viewport horizontally — see ScrollAwareViewport in the header.
    // The Y half now syncs headerColumn vertically (mirrors PianoRollKeyboard's
    // own setVerticalOffset() pattern) — the header column never scrolls
    // horizontally at all, so only y matters there.
    viewport.onViewportMoved = [this] (int x, int y)
    {
        ruler.setHorizontalOffset (x);
        headerColumn->setVerticalOffset (y, content->getRowHeights());
    };

    // A juce::MouseListener registration (NOT the primary event target) so
    // Ctrl/Cmd+wheel zoom is caught regardless of which descendant of `viewport`
    // (a TrackRow, a clip) happens to be directly under the mouse — `true` here
    // means "notify for events on any child too", not just direct hits on the
    // viewport itself. This is a parallel notification path: it does not
    // consume or interfere with Viewport's own normal wheel-scroll handling
    // for plain (non-Ctrl) scrolling.
    viewport.addMouseListener (this, true);

    // Corrected Column Geometry: genuinely separate fixed-width right column —
    // see TrackHeaderColumn's own doc comment.
    headerColumn = std::make_unique<TrackHeaderColumn> (edit, workflow);
    headerColumn->onTrackSelected = [this] (te::AudioTrack* t)
    {
        content->setSelectedTrack (t);
        headerColumn->setSelectedTrack (t);
        returnHeaderDock->setSelectedTrack (nullptr);
        masterHeader->setSelected (false);
        if (onTrackSelected) onTrackSelected (t);
    };
    headerColumn->onDeleteTrackRequested = [this] (te::AudioTrack* t)
    {
        if (onDeleteTrackRequested) onDeleteTrackRequested (t);
    };
    // Purge Clutter directive: the header's 'A' toggle is gone (no UI trigger
    // for the automation overlay right now — see TrackHeaderComponent.h's doc
    // comment), so there is nothing left to bridge here. TrackListContent::
    // setTrackAutomationVisible() and the whole overlay-rendering path are
    // untouched and still fully functional — just unreachable from any control
    // until a global automation-view toggle or context-menu replaces this.
    headerColumn->onFoldToggled = [this] (te::AudioTrack* t, int newHeight)
    {
        content->setTrackRowHeight (t, newHeight);
    };
    addAndMakeVisible (*headerColumn);

    // Hybrid Bus/Return Architecture — the docked return-track header list,
    // wired the same way headerColumn is above, cross-clearing every OTHER
    // selection surface (regular headers, Master) so exactly one thing is
    // ever selected at a time regardless of which dock it lives in.
    returnHeaderDock = std::make_unique<ReturnHeaderDock> (workflow);
    returnHeaderDock->onTrackSelected = [this] (te::AudioTrack* t)
    {
        content->setSelectedTrack (t);
        headerColumn->setSelectedTrack (nullptr);
        returnHeaderDock->setSelectedTrack (t);
        masterHeader->setSelected (false);
        if (onTrackSelected) onTrackSelected (t);
    };
    returnHeaderDock->onDeleteTrackRequested = [this] (te::AudioTrack* t)
    {
        if (onDeleteTrackRequested) onDeleteTrackRequested (t);
    };
    // Unlike headerColumn's identical bridge, a return-dock fold ALSO changes
    // this whole column's total height (it isn't inside a scrolling
    // viewport, so nothing else absorbs the size change) — re-running the
    // top-level resized() is what gives Master/headerColumn/viewport their
    // correct remaining space afterward.
    returnHeaderDock->onFoldToggled = [this] (te::AudioTrack* t, int newHeight)
    {
        returnLaneDock->setTrackRowHeight (t, newHeight);
        resized();
    };
    addAndMakeVisible (*returnHeaderDock);

    // Master — the left-side MasterLaneRow is GONE entirely (Lead Architect
    // correction: it was a dead ghost container blocking the grid). Only
    // MasterHeaderRow remains, pinned to the bottom of the RIGHT header
    // column; the grid itself now extends all the way to the absolute
    // bottom of its own column (see resized()).
    masterHeader = std::make_unique<MasterHeaderRow> (edit);

    masterHeader->onSelected = [this]
    {
        content->setSelectedTrack (nullptr);
        headerColumn->setSelectedTrack (nullptr);
        returnHeaderDock->setSelectedTrack (nullptr);
        if (onMasterTrackSelected) onMasterTrackSelected();
    };

    // Master Track Fold Parity directive: folding Master changes ITS OWN
    // preferred height, which changes how much of the header column is left
    // for headerColumn/viewport above it — same "re-run the top-level
    // resized()" pattern returnHeaderDock->onFoldToggled uses.
    masterHeader->onFoldToggled = [this] { resized(); };

    addAndMakeVisible (*masterHeader);

    rebuildTracks();
}

ArrangementComponent::~ArrangementComponent()
{
    viewport.removeMouseListener (this);
}

te::AudioTrack::Ptr ArrangementComponent::getSelectedTrack() const
{
    return content->getSelectedTrack();
}

void ArrangementComponent::addTrack (bool asMidiTrack)
{
    // Groups track insertion + naming + routing + instrument-seeding into one Undo
    // step rather than several. The underlying calls are already undo-tracked
    // individually (Track/Plugin CachedValue properties bind to edit.getUndoManager()
    // at construction) — this just stops one "Add Track" click from taking 3-4
    // presses of Ctrl+Z to fully reverse.
    edit.getUndoManager().beginNewTransaction (asMidiTrack ? "Add MIDI Track" : "Add Audio Track");

    auto newTrack = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

    if (newTrack != nullptr)
    {
        const int number = te::getAudioTracks (edit).size();
        newTrack->setName ((asMidiTrack ? "MIDI " : "Audio ") + juce::String (number));
        newTrack->getOutput().setOutputToDefaultDevice (false);

        // Fused Identity Block directive (Engine Fix): every new track gets a
        // real colour immediately — te::Track defaults to transparent, which
        // used to read as a flat, uncoloured grey slab in Column 1. Soft
        // Pastel Auto-Colours directive: low saturation (0.3-0.5), high
        // brightness (0.8-0.9) — a calm studio-label look (soft cyan, pale
        // yellow, muted brown), not harsh fully-saturated neon.
        newTrack->setColour (juce::Colour::fromHSV (juce::Random::getSystemRandom().nextFloat(), 0.35f, 0.85f, 1.0f));

        // TE has no separate MIDI-track type — a MIDI track is an AudioTrack with an
        // instrument. Seed the MIDI variant with FourOsc so it makes sound.
        if (asMidiTrack)
            if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, juce::PluginDescription()))
                newTrack->pluginList.insertPlugin (synth, 0, nullptr);

        // Auto-select the freshly added track so plugin loads target it immediately.
        content->setSelectedTrack (newTrack.get());
        headerColumn->setSelectedTrack (newTrack.get()); // header column owns the visual highlight now
        masterHeader->setSelected (false);
    }

    rebuildTracks();
}

void ArrangementComponent::rebuildTracks()
{
    // Hybrid Bus/Return Architecture — split ONCE, hand the SAME two vectors
    // to both the regular pair (content/headerColumn, via getTrackOrder()) and
    // the return pair below, so a track can never end up in both lists or
    // neither regardless of what TrackUtils::isReturnTrack() returns for it
    // at this exact moment.
    const auto split = TrackUtils::splitTracks (edit);

    content->rebuild (split.regularTracks); // fires onRebuilt -> headerColumn->rebuild (content->getTrackOrder())
    returnLaneDock->rebuild (split.returnTracks);
    returnHeaderDock->rebuild (split.returnTracks);

    layoutContent();
    resized(); // the return dock's total height may have just changed (track added/removed)

    if (onTracksChanged)
        onTracksChanged();
}

void ArrangementComponent::layoutContent()
{
    const int visibleW = viewport.getMaximumVisibleWidth();
    const int visibleH = viewport.getMaximumVisibleHeight();

    // Content width now reflects the CURRENT zoom level (contentWidthPx()),
    // clamped up to at least the viewport's visible width — zooming in grows it
    // past visibleW (enabling horizontal scroll via the scrollbar enabled in the
    // constructor); zooming out never shrinks it below what's actually visible.
    const int wantedWidth = juce::jmax (visibleW, contentWidthPx());

    content->setMinHeight (visibleH);
    content->setSize (wantedWidth, juce::jmax (content->preferredHeight(), visibleH));

    // Ruler stays sized to the VISIBLE window width (not the full scrollable
    // content width) — it's a fixed overlay, not part of the horizontally
    // scrolling content; viewportMoved() keeps its drawn offset in sync instead.
    ruler.setBounds (viewport.getX(), toolbarHeight, visibleW, rulerHeight);

    // Primes the header column's vertical position/row-heights on every layout
    // pass (not just actual scroll events) — without this, a fresh load /
    // track add-remove / zoom change would leave headers stuck at their last
    // position until the user happened to scroll.
    headerColumn->setVerticalOffset (viewport.getViewPosition().y, content->getRowHeights());
    headerColumn->relayoutFromHeights (content->getRowHeights()); // setVerticalOffset() no-ops if the y hasn't changed — force a fresh layout regardless
}

void ArrangementComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Ctrl on Windows/Linux, Command on Mac — isCommandDown() is JUCE's
    // cross-platform "the primary modifier" check, matching the spec exactly
    // rather than isCtrlDown() (which would miss Cmd on Mac).
    if (! e.mods.isCommandDown())
        return; // plain scroll: Viewport's own normal handling is untouched by this

    const double factor = std::pow (1.5, (double) wheel.deltaY);
    const double newPixelsPerSecond = juce::jlimit (minPixelsPerSecond, maxPixelsPerSecond,
                                                      pixelsPerSecond * factor);

    if (newPixelsPerSecond == pixelsPerSecond)
        return;

    // Cheap: just a double. The expensive relayout (below, in timerCallback())
    // is deliberately NOT done here — see this method's doc comment in the
    // header for why deferring it is the actual fix for "deep zooming stutters
    // heavily".
    pixelsPerSecond = newPixelsPerSecond;

    zoomRelayoutPending = true;
    constexpr int zoomRelayoutIntervalMs = 40; // ~25 fps worth of coalescing
    startTimer (zoomRelayoutIntervalMs);
}

void ArrangementComponent::timerCallback()
{
    stopTimer();

    if (! zoomRelayoutPending)
        return;

    zoomRelayoutPending = false;

    layoutContent();               // content/ruler sizing — cascades to TrackRow::
                                    // resized() (hence layoutClips()) IF the overall
                                    // size actually changed...
    content->refreshLayoutForZoom(); // ...and unconditionally either way, since it
                                    // might not have (e.g. zoomed while the project
                                    // already fit inside the viewport).
    ruler.repaint();
}

void ArrangementComponent::updateDragHover (const juce::StringArray& files, int x, int y)
{
    bool interested = false;

    for (auto& f : files)
        if (isSupportedAudioFile (f))
        {
            interested = true;
            break;
        }

    if (! interested)
    {
        content->setHoverRow (-1);
        return;
    }

    // x/y arrive already local to THIS component (MainComponent's contract) —
    // translate once more into content's local space, since content sits inside
    // `viewport` (Component::getLocalPoint walks the real transform chain, so
    // this accounts for scroll position automatically).
    const auto pointInContent = content->getLocalPoint (this, juce::Point<int> (x, y));
    content->setHoverRow (content->rowIndexForY (pointInContent.y));
}

void ArrangementComponent::clearDragHover()
{
    content->setHoverRow (-1);
}

void ArrangementComponent::processDroppedAudio (const juce::StringArray& files, int x, int y)
{
    content->setHoverRow (-1);

    const auto pointInContent = content->getLocalPoint (this, juce::Point<int> (x, y));
    auto* track = content->trackForRow (content->rowIndexForY (pointInContent.y));

    if (track == nullptr)
        return;

    // TYPE GUARD, mirror of createAndOpenMidiClipAt()'s: this app treats a
    // track carrying an instrument as a MIDI track (see trackHasInstrument()'s
    // doc comment) — dropping an audio file onto one would create exactly the
    // hybrid Audio+MIDI track the Manifesto's "Strict Track Hierarchy"
    // forbids, even though Tracktion Engine itself has no objection to it.
    if (trackHasInstrument (*track))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Cannot Drop Audio on a MIDI Track",
            "\"" + track->getName() + "\" has an instrument loaded — audio files can only be dropped on an Audio track.");
        return;
    }

    double startSeconds = xToTime (pointInContent.x);

    // One Undo step for the whole drop, however many files landed in it — not
    // one step per file.
    edit.getUndoManager().beginNewTransaction ("Insert Audio File" + juce::String (files.size() > 1 ? "s" : ""));

    for (auto& path : files)
    {
        if (! isSupportedAudioFile (path))
            continue;

        const juce::File audioFile (path);
        const te::AudioFile teAudioFile (edit.engine, audioFile);

        if (! teAudioFile.isValid())
            continue;

        const auto lengthSeconds = teAudioFile.getLength();

        if (lengthSeconds <= 0.0)
            continue;

        // TimePosition lives in tracktion::core (a sibling inline namespace to
        // tracktion::engine, not a member of it) — tracktion:: reaches it since
        // inline namespace members are visible at their enclosing scope, but
        // te:: (== tracktion::engine) doesn't re-export it, so it must be
        // qualified this way rather than via the te:: alias.
        const auto startTime = tracktion::TimePosition::fromSeconds (startSeconds);
        const auto endTime   = tracktion::TimePosition::fromSeconds (startSeconds + lengthSeconds);

        track->insertWaveClip (audioFile.getFileNameWithoutExtension(), audioFile,
                                { { startTime, endTime }, {} }, false);

        // Multi-file drop: subsequent files land back-to-back after the one just
        // placed, not all stacked at the same start time.
        startSeconds += lengthSeconds;
    }
}

void ArrangementComponent::openClip (te::Clip* clip)
{
    // Condition A: only MIDI clips open in the piano roll. A double-click on an
    // audio clip is intentionally a no-op for now (its editor is a later
    // phase) rather than an error.
    if (auto* midiClip = dynamic_cast<te::MidiClip*> (clip))
        if (onMidiClipOpenRequested)
            onMidiClipOpenRequested (midiClip);
}

void ArrangementComponent::createAndOpenMidiClipAt (te::AudioTrack* track, double xSeconds)
{
    if (track == nullptr)
        return;

    // TYPE GUARD: an Audio-only track (no instrument plugin) can't render a MIDI
    // clip into anything audible — double-clicking its empty lane does nothing,
    // rather than silently creating a MIDI clip that will never make sound. See
    // trackHasInstrument()'s doc comment above for why isSynth() is the correct
    // check rather than an app-level "is this a MIDI track" flag.
    if (! trackHasInstrument (*track))
        return;

    // Condition B time selection. The transport loop range is the closest thing
    // this app has today to a "selected time range"/loop brace, so honour it
    // when it's set; otherwise fall back to a 4-bar block starting at the
    // double-clicked position, snapped back to the nearest bar so new clips
    // land on the grid rather than at an arbitrary fractional beat.
    const auto loopRange = edit.getTransport().getLoopRange();

    tracktion::TimeRange range;

    if (loopRange.getLength().inSeconds() > 0.0)
    {
        range = loopRange;
    }
    else
    {
        const double secondsPerBar = beatsPerBar * secondsPerBeat;
        const double snappedStart  = std::floor (xSeconds / secondsPerBar) * secondsPerBar;
        range = { tracktion::TimePosition::fromSeconds (snappedStart),
                  tracktion::TimePosition::fromSeconds (snappedStart + 4.0 * secondsPerBar) };
    }

    edit.getUndoManager().beginNewTransaction ("Create MIDI Clip");

    auto midiClip = track->insertMIDIClip (range, nullptr);

    if (midiClip == nullptr)
        return;

    // Select the owning track (and let the track's own ValueTree listener
    // rebuild its ClipComponents — deferred, same path a file drop uses — so
    // the new block appears without an explicit rebuild here), then open it.
    content->setSelectedTrack (track);

    if (onMidiClipOpenRequested)
        onMidiClipOpenRequested (midiClip.get());
}

void ArrangementComponent::mergeSelectedMidiClips (te::Track* track, const juce::Array<te::MidiClip*>& clips)
{
    // FL Studio-style consolidate: fold several fragmented MIDI blocks on ONE
    // track into a single master clip spanning them all, preserving every
    // note's absolute timeline position. Wrapped in ONE Undo transaction
    // (creation + every note add + every original delete) so a single Ctrl+Z
    // reverses the whole operation — QA requirement.
    auto* clipTrack = dynamic_cast<te::ClipTrack*> (track);

    if (clipTrack == nullptr || clips.size() < 2)
        return;

    // Total spanning bounds across all selected clips.
    auto spanStart = clips.getFirst()->getPosition().time.getStart();
    auto spanEnd   = clips.getFirst()->getPosition().time.getEnd();

    for (auto* c : clips)
    {
        spanStart = juce::jmin (spanStart, c->getPosition().time.getStart());
        spanEnd   = juce::jmax (spanEnd,   c->getPosition().time.getEnd());
    }

    edit.getUndoManager().beginNewTransaction ("Consolidate MIDI Clips");

    auto master = clipTrack->insertMIDIClip ("Merged", { spanStart, spanEnd }, nullptr);

    if (master == nullptr)
        return;

    auto& masterSeq = master->getSequence();

    // Work in the Edit's beat space so note positions survive the merge exactly:
    // a source note at local beat b, in a clip starting at edit-beat E_src, must
    // land at local beat b + (E_src - E_master) in the master (whose content
    // origin is the span start's edit-beat). Every clip this app creates has a
    // zero content offset (freshly inserted MIDI/wave clips do), so local beat
    // maps straight to edit beat with no per-clip offset term to subtract.
    const auto masterStartBeat = edit.tempoSequence.toBeats (spanStart);

    for (auto* c : clips)
    {
        const auto clipStartBeat = edit.tempoSequence.toBeats (c->getPosition().time.getStart());
        const auto deltaBeats    = clipStartBeat - masterStartBeat; // BeatDuration, >= 0

        for (auto* note : c->getSequence().getNotes())
            masterSeq.addNote (note->getNoteNumber(),
                               note->getStartBeat() + deltaBeats,
                               note->getLengthBeats(),
                               note->getVelocity(),
                               note->getColour(),
                               &edit.getUndoManager());
    }

    // Delete the originals — still inside the same transaction. After this the
    // raw pointers in `clips` are dangling; the loop is the last thing to touch
    // them, and nothing below reads the array again.
    for (auto* c : clips)
        c->removeFromParent();

    // Keep the merged track selected; the track's own ValueTree listener
    // rebuilds its ClipComponents (deferred) to show the single new block.
    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track))
        content->setSelectedTrack (audioTrack);
}

void ArrangementComponent::showClipContextMenu (te::Clip* clip)
{
    if (clip == nullptr)
        return;

    auto* clipTrack = clip->getClipTrack();

    if (clipTrack == nullptr)
        return;

    // Gather every MIDI clip on this clip's track — the consolidate target set.
    juce::Array<te::MidiClip*> midiClips;

    for (auto* c : clipTrack->getClips())
        if (auto* mc = dynamic_cast<te::MidiClip*> (c))
            midiClips.add (mc);

    juce::PopupMenu menu;
    menu.addItem (1, "Consolidate MIDI clips on this track", midiClips.size() >= 2);

    // Capturing clipTrack/midiClips raw across the async menu callback is safe
    // here: a desktop PopupMenu is modal, so no track/clip deletion can run
    // between opening the menu and this callback firing. SafePointer still
    // guards THIS component being torn down (a project Load) meanwhile.
    juce::Component::SafePointer<ArrangementComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safeThis, clipTrack, midiClips] (int result)
                        {
                            if (safeThis != nullptr && result == 1)
                                safeThis->mergeSelectedMidiClips (clipTrack, midiClips);
                        });
}

void ArrangementComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::panel);

    // Task 3 (Solid Background): the ENTIRE right column — headerColumn AND
    // masterHeader together — gets one distinct, darker solid fill painted
    // HERE, underneath both children, so there is no visible seam between
    // them regardless of any pixel-level mismatch in their individual
    // bounds — this is what unifies them into a single cohesive Ableton-style
    // dock rather than two coincidentally-adjacent components. A 1px left
    // border separates the whole dock from the timeline.
    const auto rightColumnX = getWidth() - headerWidth;
    g.setColour (CrateColors::DarkBackground); // darker than LAF::panel — a visually distinct dock
    g.fillRect (rightColumnX, 0, headerWidth, getHeight());

    g.setColour (LAF::background);
    g.drawVerticalLine (rightColumnX, 0.0f, (float) getHeight());
}

void ArrangementComponent::paintOverChildren (juce::Graphics& g)
{
    // Master Separator: a strict horizontal line across the ENTIRE grid,
    // aligned with the top of the pinned bottom section — MasterHeaderRow,
    // plus (Hybrid Bus/Return Architecture) the return-track dock now sitting
    // directly above it, both non-scrolling (see resized()'s
    // headersBounds.removeFromBottom() chain: Master first, then the return
    // dock on top of that). MUST be drawn in paintOverChildren(), not
    // paint(): viewport (the scrolling grid) is a CHILD component that fully
    // covers this exact row with its own opaque background fill, so a line
    // drawn in paint() — which runs BEFORE children — was being silently
    // painted over and never actually visible. Full window width, not just
    // the grid: forcing it straight through the header dock column too is
    // what makes it read as one continuous boundary rather than a line that
    // mysteriously stops partway across.
    g.setColour (juce::Colours::darkgrey);
    const auto lineY = (float) (getHeight() - masterHeader->getPreferredHeight() - returnLaneDock->getTotalHeight());
    g.drawLine (0.0f, lineY, (float) getWidth(), lineY, 1.0f);
}

void ArrangementComponent::resized()
{
    auto bounds = getLocalBounds();

    auto toolbar = bounds.removeFromTop (toolbarHeight).reduced (6, 5);
    addAudioButton.setBounds (toolbar.removeFromLeft (80));
    toolbar.removeFromLeft (6);
    addMidiButton.setBounds (toolbar.removeFromLeft (80));

    // Corrected Ableton Column Geometry (Lead Architect fix): split LEFT
    // (grid) / RIGHT (headers) FIRST, before assigning Master's two
    // independent pieces INSIDE each column — this exact order is what makes
    // Master genuinely two pieces pinned to the bottom of their own columns,
    // never one full-width row cutting across both.
    auto headersBounds = bounds.removeFromRight (headerWidth);
    auto gridBounds = bounds;

    // Both columns reserve the SAME ruler-height strip at the top (even
    // though only the grid column actually draws ruler ticks there) — this is
    // what keeps every header row Y-aligned with its corresponding lane row;
    // without it, headerColumn's rows would start rulerHeight px higher than
    // TrackListContent's.
    headersBounds.removeFromTop (rulerHeight);
    gridBounds.removeFromTop (rulerHeight); // ruler itself positioned in layoutContent

    // Master pinned to the bottom of the RIGHT column only — the left-side
    // MasterLaneRow is GONE entirely (Lead Architect correction: it was a
    // dead ghost container).
    masterHeader->setBounds (headersBounds.removeFromBottom (masterHeader->getPreferredHeight()));

    // Hybrid Bus/Return Architecture — return tracks dock directly ABOVE
    // Master, on BOTH sides (header dock above masterHeader; lane dock above
    // where the grid would otherwise end), never scrolling. Zero height (and
    // therefore invisible, consuming no space) when there are no return
    // tracks. returnHeaderDock/returnLaneDock always report the SAME total
    // height (same tracks, same order — see rebuildTracks()), so either one
    // is a valid source for the shared height; returnLaneDock is used here
    // since gridBounds needs it too.
    const int returnDockHeight = returnLaneDock->getTotalHeight();
    returnHeaderDock->setBounds (headersBounds.removeFromBottom (returnDockHeight));
    returnLaneDock->setBounds (gridBounds.removeFromBottom (returnDockHeight));

    // The rest of each column. gridBounds now correctly stops at the top of
    // the return dock (or Master, if there are no return tracks) instead of
    // extending all the way to the absolute bottom.
    headerColumn->setBounds (headersBounds);
    viewport.setBounds (gridBounds);

    layoutContent();
}
