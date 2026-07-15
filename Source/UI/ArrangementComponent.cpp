#include "ArrangementComponent.h"
#include "TrackHeaderComponent.h"
#include "AutomationLaneComponent.h"
#include "ArrangementLayout.h"
#include "TheCrateLookAndFeel.h"

using namespace CrateArrangement;

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Zone 3 grid spec: bars slightly brighter, beats very subtle — expressed as
    // true opacity over the lane fill rather than two similarly-dark flat colours,
    // so "low opacity" reads as low opacity regardless of the panel colour beneath.
    const auto gridBarColour  = juce::Colours::white.withAlpha (0.14f);
    const auto gridBeatColour = juce::Colours::white.withAlpha (0.045f);
    const auto playheadColour = juce::Colour (0xffff3b30);
    const auto waveformColour = juce::Colour (0xff29e0ff); // bright cyan — architectural note:
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
class ArrangementComponent::TrackRow : public juce::Component,
                                        private juce::ValueTree::Listener
{
public:
    TrackRow (te::Edit& e, CrateWorkflowManager& w, te::AudioTrack::Ptr t)
        : edit (e), track (t), header (t, w)
    {
        addAndMakeVisible (header);

        header.onSelect = [this] { if (onSelected) onSelected (track.get()); };
        header.onAutomationToggle = [this] { toggleAutomation(); };
        header.onDeleteRequested = [this] { if (onDeleteRequested) onDeleteRequested (track.get()); };

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

    int getRowHeight() const           { return automationVisible ? clipLaneHeight + autoLaneHeight : clipLaneHeight; }

    void setSelected (bool s)          { header.setSelected (s); }

    /** Forces every clip's bounds to be recomputed against the CURRENT zoom
        level, independent of whether this row's own bounds happened to change —
        JUCE's setBounds() skips calling resized() when the new bounds are
        identical to the old ones, which a zoom change doesn't guarantee here
        (e.g. zooming while the project already fits within the viewport). */
    void refreshLayoutForZoom()   { layoutClips(); }

    // Set by the owning content.
    std::function<void (te::AudioTrack*)> onSelected;
    std::function<void (te::AudioTrack*)> onDeleteRequested;
    std::function<void()> onHeightChanged;

    // Phase 4 MIDI Suite — forwarded straight up to TrackListContent then
    // ArrangementComponent (which owns the edit + workflow to act on them).
    std::function<void (te::Clip*)> onClipOpenRequested;                       // Condition A
    std::function<void (te::AudioTrack*, double xSeconds)> onEmptyLaneDoubleClicked; // Condition B
    std::function<void (te::Clip*)> onClipContextMenuRequested;                // right-click consolidate

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Condition B (Ableton behaviour): a double-click that reaches the ROW
        // itself (not a child ClipComponent, not the header) landed on empty
        // lane space — create a new MIDI clip there. The header is a child
        // covering [0, headerWidth) and eats its own clicks, so anything
        // arriving here is already in the lane; the guard is belt-and-braces.
        if (track == nullptr || e.x < headerWidth)
            return;

        if (onEmptyLaneDoubleClicked)
            onEmptyLaneDoubleClicked (track.get(), xToTime (e.x));
    }

    void paint (juce::Graphics& g) override
    {
        const auto w = getWidth();

        juce::Rectangle<int> lane (headerWidth, 0, w - headerWidth, getHeight());
        g.setColour (LAF::panelLight);
        g.fillRect (lane);
        paintLaneGrid (g, getHeight());

        // Divider between clip lane and automation lane.
        if (automationVisible)
        {
            g.setColour (LAF::background);
            g.drawHorizontalLine (clipLaneHeight, (float) headerWidth, (float) w);
        }

        g.setColour (LAF::background);
        g.drawHorizontalLine (getHeight() - 1, (float) headerWidth, (float) w);
    }

    void resized() override
    {
        header.setBounds (0, 0, headerWidth, getHeight());

        if (autoLane != nullptr)
            autoLane->setBounds (headerWidth, clipLaneHeight, getWidth() - headerWidth, autoLaneHeight);

        layoutClips();
    }

private:
    void toggleAutomation()
    {
        automationVisible = header.getAutomationVisible();

        if (automationVisible && autoLane == nullptr && track != nullptr)
        {
            // Default the automation lane to the track's Volume parameter, aligned to
            // the same grid window as the clip lane above.
            if (auto* vp = track->getVolumePlugin())
            {
                autoLane = std::make_unique<AutomationLaneComponent> (edit, track, vp->volParam);
                autoLane->setVisibleLength (totalSeconds);
                addAndMakeVisible (*autoLane);
            }
        }

        if (autoLane != nullptr)
            autoLane->setVisible (automationVisible);

        resized();

        if (onHeightChanged)
            onHeightChanged();
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
            c->setBounds (x1, 4, juce::jmax (2, x2 - x1), clipLaneHeight - 8);
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
    TrackHeaderComponent header;
    std::unique_ptr<AutomationLaneComponent> autoLane;
    std::vector<std::unique_ptr<ClipComponent>> clipComponents;
    bool automationVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};

//==============================================================================
class ArrangementComponent::TrackListContent : public juce::Component,
                                                private juce::Timer
{
public:
    TrackListContent (te::Edit& e, CrateWorkflowManager& w) : edit (e), workflow (w)
    {
        startTimerHz (30);

        addAndMakeVisible (playhead);
    }

    ~TrackListContent() override { stopTimer(); }

    // Bridges to ArrangementComponent's public callbacks — set once by the owner,
    // not per-rebuild (this object itself is long-lived; only `rows` gets rebuilt).
    std::function<void (te::AudioTrack*)> onTrackSelectedExternally;
    std::function<void (te::AudioTrack*)> onDeleteTrackRequestedExternally;

    // Phase 4 MIDI Suite — aggregated from every TrackRow, bridged up to
    // ArrangementComponent (which owns edit + workflow to act on them).
    std::function<void (te::Clip*)> onClipOpenExternally;
    std::function<void (te::AudioTrack*, double xSeconds)> onEmptyLaneDoubleClickedExternally;
    std::function<void (te::Clip*)> onClipContextMenuExternally;

    void rebuild()
    {
        rows.clear();

        for (auto* t : te::getAudioTracks (edit))
        {
            auto row = std::make_unique<TrackRow> (edit, workflow, t);
            row->onSelected = [this] (te::AudioTrack* tr)
            {
                setSelectedTrack (tr);
                if (onTrackSelectedExternally) onTrackSelectedExternally (tr);
            };
            row->onDeleteRequested = [this] (te::AudioTrack* tr)
            {
                if (onDeleteTrackRequestedExternally) onDeleteTrackRequestedExternally (tr);
            };
            row->onHeightChanged = [this] { relayout(); };
            row->onClipOpenRequested        = [this] (te::Clip* cl)                    { if (onClipOpenExternally) onClipOpenExternally (cl); };
            row->onEmptyLaneDoubleClicked   = [this] (te::AudioTrack* tr, double secs) { if (onEmptyLaneDoubleClickedExternally) onEmptyLaneDoubleClickedExternally (tr, secs); };
            row->onClipContextMenuRequested = [this] (te::Clip* cl)                    { if (onClipContextMenuExternally) onClipContextMenuExternally (cl); };
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        updateSelectionHighlight();
        relayout();

        // rebuild() re-adds every row via addAndMakeVisible(), which places each
        // one ABOVE playhead in z-order (added later wins) even though playhead
        // itself was added first, back in the constructor — pull it back to the
        // front so it stays drawn over the rows, not hidden behind them.
        playhead.toFront (false);
    }

    void setSelectedTrack (te::AudioTrack* t)
    {
        selectedTrackId = (t != nullptr) ? t->itemID : te::EditItemID();
        updateSelectionHighlight();
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

            g.setColour (dropHighlightColour);
            g.fillRect (headerWidth, y, getWidth() - headerWidth, rows[(size_t) hoverRowIndex]->getRowHeight());
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
    }

    void updateSelectionHighlight()
    {
        for (auto& row : rows)
            row->setSelected (row->getTrack() != nullptr
                               && selectedTrackId.isValid()
                               && row->getTrack()->itemID == selectedTrackId);
    }

    void timerCallback() override { repaint(); }

    te::Edit& edit;
    CrateWorkflowManager& workflow;
    PlayheadComponent playhead { edit };
    std::vector<std::unique_ptr<TrackRow>> rows;
    int hoverRowIndex = -1;
    te::EditItemID selectedTrackId;
    int minHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackListContent)
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

    content = std::make_unique<TrackListContent> (edit, workflow);
    content->onTrackSelectedExternally = [this] (te::AudioTrack* t) { if (onTrackSelected) onTrackSelected (t); };
    content->onDeleteTrackRequestedExternally = [this] (te::AudioTrack* t) { if (onDeleteTrackRequested) onDeleteTrackRequested (t); };

    // Phase 4 MIDI Suite double-click / context-menu routing.
    content->onClipOpenExternally = [this] (te::Clip* c) { openClip (c); };
    content->onEmptyLaneDoubleClickedExternally = [this] (te::AudioTrack* t, double secs) { createAndOpenMidiClipAt (t, secs); };
    content->onClipContextMenuExternally = [this] (te::Clip* c) { showClipContextMenu (c); };

    viewport.setViewedComponent (content.get(), false);

    // Both directions now: zooming in can make the content wider than the
    // viewport, and this is what makes the extra width actually reachable.
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    // Keeps the (fixed, non-scrolling) ruler's ticks in sync whenever the user
    // scrolls the viewport horizontally — see ScrollAwareViewport in the header.
    viewport.onViewportMoved = [this] (int x, int) { ruler.setHorizontalOffset (x); };

    // A juce::MouseListener registration (NOT the primary event target) so
    // Ctrl/Cmd+wheel zoom is caught regardless of which descendant of `viewport`
    // (a TrackRow, its header buttons, a clip) happens to be directly under the
    // mouse — `true` here means "notify for events on any child too", not just
    // direct hits on the viewport itself. This is a parallel notification path:
    // it does not consume or interfere with Viewport's own normal wheel-scroll
    // handling for plain (non-Ctrl) scrolling.
    viewport.addMouseListener (this, true);

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

        // TE has no separate MIDI-track type — a MIDI track is an AudioTrack with an
        // instrument. Seed the MIDI variant with FourOsc so it makes sound.
        if (asMidiTrack)
            if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, juce::PluginDescription()))
                newTrack->pluginList.insertPlugin (synth, 0, nullptr);

        // Auto-select the freshly added track so plugin loads target it immediately.
        content->setSelectedTrack (newTrack.get());
    }

    rebuildTracks();
}

void ArrangementComponent::rebuildTracks()
{
    content->rebuild();
    layoutContent();

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
}

void ArrangementComponent::resized()
{
    auto area = getLocalBounds();

    auto toolbar = area.removeFromTop (toolbarHeight).reduced (6, 5);
    addAudioButton.setBounds (toolbar.removeFromLeft (80));
    toolbar.removeFromLeft (6);
    addMidiButton.setBounds (toolbar.removeFromLeft (80));

    area.removeFromTop (rulerHeight); // ruler positioned in layoutContent
    viewport.setBounds (area);

    layoutContent();
}
