#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

/**
    Centralized controller sitting above the TE engine/edit. Owns their lifetime and
    is the single place that handles the state-desync-prone intersections of
    Save/Load, Undo/Redo, track selection, plugin routing, and track deletion.

    UI (MainComponent) holds one of these instead of raw engine/edit pointers, and
    rebuilds its child components (TransportBar, ArrangementComponent) whenever
    safeLoadProject()'s callback fires — both hold te::Edit& (a reference, not a
    pointer), so a Load's new Edit instance means they must be reconstructed, not
    patched in place. See safeLoadProject()'s doc comment for the exact ordering
    that keeps that reconstruction safe.

    Plugin format registration: NOT done here. te::Engine's constructor already
    calls PluginManager::initialise() (which registers VST3/AU/etc. formats) exactly
    once, automatically, and engine is never destroyed across a Load — only edit is
    swapped. Calling initialise() again here would be redundant at best; it hasn't
    been verified idempotent, so it isn't called a second time speculatively.
*/
class CrateWorkflowManager
{
public:
    CrateWorkflowManager();
    ~CrateWorkflowManager();

    te::Engine& getEngine() noexcept   { return *engine; }
    te::Edit& getEdit() noexcept       { return *edit; }

    //==============================================================================
    // Track selection / management. Track* (not AudioTrack*) per the request this
    // was designed against — loadPluginToSelectedTrack/deleteSelectedTrack narrow
    // to AudioTrack internally where the underlying TE API requires it.
    void selectTrack (te::Track* track) noexcept   { currentSelectedTrack = track; }
    te::Track* getSelectedTrack() const noexcept   { return currentSelectedTrack; }

    /** Instantiates description onto currentSelectedTrack and pops its editor window.
        No-ops (does nothing, doesn't crash) if nothing is selected or the selection
        isn't an AudioTrack. Wrapped in a single Undo transaction.

        No completion callback here on purpose: MixerStrip and
        UniversalDeviceChainComponent each listen directly to their track's own
        te::ValueTree (track->state, which IS track->pluginList.state) for
        IDs::PLUGIN child add/remove, so they pick up a newly-loaded plugin
        automatically here — AND on Undo/Redo of a load or delete, which a
        one-off completion callback from this method never would have covered. */
    void loadPluginToSelectedTrack (const juce::PluginDescription&);

    /** Same instantiation path as loadPluginToSelectedTrack(), but targets an
        EXPLICIT track and insert index rather than always currentSelectedTrack/
        the end of the list — for drag-and-drop drop targets that already know
        their own track directly (a MixerStrip insert slot, the Universal Device
        Chain), so a drop lands on whichever track/slot it was actually dropped
        on, regardless of what's currently selected elsewhere. insertIndex -1
        appends to the end. Wrapped in a single Undo transaction. Does NOT pop
        the editor window (unlike loadPluginToSelectedTrack) — a drag-drop isn't
        the same "I explicitly asked to add this" gesture the Browser's
        double-click is, so this leaves the window closed until the user opens
        it deliberately (Device Chain's wrench button, or MixerStrip's insert). */
    // te::Track BASE type (not AudioTrack) — the implementation only ever
    // touches targetTrack.pluginList (a member of Track itself), so this works
    // identically for te::MasterTrack, letting the Universal Device Chain load
    // mastering plugins onto Master through this exact same pipeline.
    //
    // Returns false (no-op, nothing instantiated) if description.isInstrument
    // and targetTrack is the Master track — Master is a mastering/effects bus,
    // never an instrument host, in every real DAW. Also returns false if
    // instantiation itself failed. Callers that care why (vs. just silently
    // dropping the drag/menu action) can check description.isInstrument
    // themselves before calling, same as showInstrumentMenu() already does.
    bool loadPluginOntoTrack (const juce::PluginDescription& description, te::Track& targetTrack, int insertIndex);

    /** Deletes currentSelectedTrack and clears the selection. Wrapped in a single
        Undo transaction. No-op if nothing is selected. */
    void deleteSelectedTrack();

    /** Hybrid Bus/Return Architecture — the Sends "+" menu's
        "+ Create New FX Channel" macro. Creates a real return track (a plain
        te::AudioTrack hosting a te::AuxReturnPlugin — see
        Source/UI/TrackUtils.h's isReturnTrack()) and wires a matching
        te::AuxSendPlugin on sourceTrack to the same bus number, all in one
        Undo transaction. Wrapped in a single Undo transaction — one Ctrl+Z
        removes the whole channel, send included.

        Fires onTrackListChanged() at the end — this is called from deep
        inside a MixerStrip/InspectorStrip Sends menu, several component
        layers away from ArrangementComponent/MixerComponent's own track
        lists, which otherwise have no way to learn a new track now exists
        (their rebuild triggers all live in MainComponent's onDeleteTrackRequested/
        addTrack paths, neither of which this call goes through). */
    void createAndRouteNewFXChannel (te::Track& sourceTrack);

    /** Fires whenever a method on this class adds or removes a track
        OUTSIDE the normal ArrangementComponent::addTrack()/onDeleteTrackRequested
        paths (which already call rebuildTracks() themselves) — currently just
        createAndRouteNewFXChannel(). MainComponent wires this once, centrally,
        to arrangement->rebuildTracks() (which already cascades to
        mixer->rebuildStrips() via its own onTracksChanged), rather than
        threading a bespoke callback through every intermediate component
        (MixerStrip -> StripRowContent -> MixerComponent -> MainComponent,
        and separately InspectorStrip -> CrateTrackInspectorComponent ->
        BrowserComponent -> MainComponent) that could ever call
        createAndRouteNewFXChannel(). */
    std::function<void()> onTrackListChanged;

    //==============================================================================
    // Automation persistence (fixes the "~30 baked sub-points become 30 anchors on
    // reload" bug). The real fix is NOT a pre-save/post-load pass: AutomationLaneComponent
    // persists its macro anchor list (time/value/tension/segment-type) into a custom
    // ValueTree property on each parameter's AutomationCurve continuously, every time
    // the curve is re-baked — not just before a save. That's what makes it survive
    // BOTH a save/load round-trip AND an in-session track-list rebuild (e.g. adding a
    // second track), which a save-time-only strip/rebuild pair would not fix.
    //
    // These two functions exist to satisfy that explicit contract and as an
    // extension point / safety net:
    //   - prepareAutomationForSave() does a best-effort validation pass (logs, does
    //     not throw or block) over every AutomatableParameter's curve, flagging any
    //     with baked points but no metadata property — that would indicate a curve
    //     the UI never touched via AutomationLaneComponent (e.g. hand-authored),
    //     which will round-trip as plain points. That's expected, not an error, but
    //     worth knowing about.
    //   - rebuildAutomationAfterLoad() is a no-op by design: AutomationLaneComponent
    //     reads the persisted metadata in its OWN constructor, which runs naturally
    //     when the UI is rebuilt after a load. There's nothing to do here unless a
    //     UI-independent rebake is ever needed (e.g. headless rendering), in which
    //     case this is the place to add it.
    void prepareAutomationForSave();
    void rebuildAutomationAfterLoad();

    //==============================================================================
    // Persistence.
    void saveProject();

    /** Opens a *.crate file chooser and, once the user picks a valid project,
        performs the Edit swap as a single synchronous sequence — no callAsync, no
        gap where a timer or repaint could observe a half-torn-down state:

          1. onBeforeEditSwap() — old Edit still fully alive. Caller MUST destroy
             every UI component that holds a te::Edit&, te::Plugin::Ptr, or raw
             pointer into the current Edit's tracks/plugins (TransportBar,
             ArrangementComponent, MixerComponent, UniversalDeviceChainComponent).
          2. This class destroys the old te::Edit and swaps in the new one.
          3. onAfterEditSwap(newEdit) — caller rebuilds its UI against the new Edit.

        Both callbacks are invoked from inside the FileChooser's async completion
        callback (itself stored in this->fileChooser, not in any caller-owned
        object) — by the time either fires, the original button click that started
        this has long since returned. It is therefore safe for onBeforeEditSwap to
        synchronously destroy the component whose button triggered the load,
        UNLIKE the old single-callback design (which routed the completion through
        a std::function member of that same component and had to defer via
        callAsync to avoid destroying it mid-call).

        Neither callback fires if the chosen file fails to load — current Edit/UI
        stay untouched. */
    void safeLoadProject (std::function<void()> onBeforeEditSwap,
                           std::function<void (te::Edit&)> onAfterEditSwap);

    juce::File getCurrentProjectFile() const   { return currentProjectFile; }

    //==============================================================================
    // Global Play/Stop (Zone 1 spec + spacebar shortcut). Single source of truth
    // for BOTH TransportBar's Play/Stop buttons and MainComponent's spacebar
    // handler, so "last started position" state lives in exactly one place
    // rather than being duplicated (and inevitably drifting) across callers.

    /** Starts playback from the CURRENT position, remembering it as the position
        stopAndReturnToStart() will rewind to. */
    void startPlayback();

    /** Stops playback and rewinds the playhead to wherever the most recent
        startPlayback() call began (0.0 if playback was never started this
        session) — Ableton/Pro Tools convention: Stop rewinds, it doesn't just
        pause in place. */
    void stopAndReturnToStart();

private:
    void initialiseAudioDevice();
    void promptSaveAs();
    void writeCurrentEditToFile();
    void createProjectFolderStructure (const juce::File& projectRoot) const;

    // Shared by loadPluginToSelectedTrack()/loadPluginOntoTrack() — the actual
    // ExternalPlugin::xmlTypeName instantiation + failure alert, factored out so
    // the two call sites (select-then-load vs. drop-onto-explicit-track) can't
    // drift apart on how a plugin actually gets created. Returns nullptr (and
    // has already shown the failure alert) if instantiation failed — callers
    // just need to check for that and stop.
    te::Plugin::Ptr instantiateExternalPlugin (const juce::PluginDescription&);

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<te::Edit> edit;

    double lastPlayStartSeconds = 0.0;

    te::Track* currentSelectedTrack = nullptr;

    juce::File currentProjectFile;              // .crate file; File() until first save
    std::unique_ptr<juce::FileChooser> fileChooser; // must outlive the async chooser callback

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateWorkflowManager)
};
