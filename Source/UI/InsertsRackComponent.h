#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

class PluginSlotComponent;

/**
    Unified Inserts rack — the "Minimum-10 Scrolling Grid" UI (Pro Tools/Ableton
    hybrid: fixed insertRowHeight rows, real PluginSlotComponents plus ghost
    drop-target padding, scrolling once content overflows the visible rows)
    used by BOTH a real track's MixerStrip AND the pinned MasterStrip. A plugin
    chain is a plugin chain — there is no conceptual UI difference between a
    track's inserts and Master's, so this is ONE class, not two independently
    maintained copies.

    Takes te::Track& (not te::AudioTrack::Ptr, which is what the old
    MixerStrip-only version hardcoded and which cannot hold a MasterTrack* at
    all) — every member this class actually reads (pluginList) or needs
    (edit, for the drag-drop identifier lookup) lives on Track itself, the
    exact interface both AudioTrack and MasterTrack share.

    The ONLY Master-specific rule anywhere in this feature — instruments can
    never load onto Master — lives entirely in
    CrateWorkflowManager::loadPluginOntoTrack()'s own guard. This class has no
    "am I Master" branch for INSTANTIATING plugins. It DOES still have one
    small Master-aware branch for reactivity: which ValueTree actually carries
    a track's PLUGIN children differs between the two track types (see
    rebuild()'s doc comment) — that's a fact about Tracktion Engine's own data
    model, not a UI decision, so it lives here rather than being pushed back
    out onto MixerStrip/MasterStrip to duplicate.

    Owns its own reactivity: rebuild() attaches a juce::ValueTree::Listener to
    whichever tree actually carries targetTrack's plugins, so a plugin loaded/
    removed via ANY path (Device Chain, Mixer drag-drop, Undo/Redo) refreshes
    this rack's slots automatically — the owner does not need its own separate
    plugin-list listener to keep this in sync. Callbacks defer the actual
    rebuild via juce::AsyncUpdater rather than rebuilding inline: ValueTree
    notifications can fire from within another call already in progress
    (e.g. mid-Undo), and rebuilding the whole slot list/UI synchronously from
    inside that isn't safe to assume.
*/
class InsertsRackComponent : public juce::Component,
                              private juce::ValueTree::Listener,
                              private juce::AsyncUpdater
{
public:
    InsertsRackComponent();

    // Declared here but DEFINED in the .cpp (where PluginSlotComponent.h is
    // actually included) — this class is held BY VALUE in both MixerStrip's
    // and MasterStrip's headers, so an implicitly-generated destructor would
    // otherwise get instantiated wherever THEIR destructors are compiled
    // (MixerStrip.cpp/MasterStrip.cpp), which only forward-declare
    // PluginSlotComponent — "can't delete an incomplete type".
    ~InsertsRackComponent() override;

    /** Points this rack at targetTrack and does an immediate, synchronous
        rebuild of every slot — call once when the owner first knows its
        track (constructor), and again if the owner's track ever actually
        changes (a different Track&, not merely a mutation of the same one —
        further changes on the SAME track are caught automatically by the
        listener this attaches, below).

        Also (re-)attaches this rack's own ValueTree::Listener to whichever
        tree actually carries targetTrack's PLUGIN children — for a real
        te::AudioTrack that's track.state itself (PluginList::initialise()
        does state = v, not a child tree), but te::MasterTrack is different:
        MasterTrack::initialise() does pluginList.initialise
        (edit.getMasterPluginList().state) — a SEPARATE ValueTree from the
        MasterTrack's own track.state (verified against
        tracktion_MasterTrack.cpp). Listening to track.state for Master would
        silently never fire on a plugin add/remove, which is exactly the
        "deaf until a forced layout" bug this class must not have. */
    void rebuild (te::Track& targetTrack, CrateWorkflowManager& workflowToUse);

    /** Fires when a real (non-ghost) slot is clicked — owner focuses that
        plugin in the Universal Device Chain. */
    std::function<void (te::Plugin*)> onSlotSelected;

    /** Fixed total height this rack always occupies (caption + the full
        insertMinVisibleRows-tall viewport + padding) — callers reserve
        exactly this much space, same "can't drift apart" discipline
        DeviceBlock's own layout constants use. */
    static int getFixedHeight();

    // Recessed dark container behind the caption/slots — lives HERE (not in
    // MixerStrip/MasterStrip's own paint()) specifically so Track 1's and
    // Master's Inserts racks can never visually diverge depending on
    // whichever parent happens to paint behind this component. Before this
    // override existed, InsertsRackComponent painted nothing at all, so the
    // "container" a user saw was really just whatever background colour its
    // OWN parent filled first (ChannelStripRack fills the darkest
    // colorTheVoid; MasterStrip fills the lighter colorHardware) — the two
    // looked different by accident, not by design.
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Lays out whatever children (real or ghost PluginSlotComponents) have
    // been added, top to bottom, at a fixed row height each — height is
    // driven by child COUNT, not the viewport's visible height, which is
    // what makes scrolling (rather than squishing) happen once more than the
    // minimum visible rows are present.
    struct GridContent : public juce::Component
    {
        void refreshHeight (int width);
        void resized() override;
    };

    void refreshContentLayout();

    // The actual slot-rebuilding work, shared by rebuild()'s synchronous path
    // and handleAsyncUpdate()'s deferred one — reads currentTrack/
    // currentWorkflow rather than taking parameters, since both call sites
    // need the SAME "whatever track we're currently pointed at" state.
    void rebuildSlotsNow();

    // juce::ValueTree::Listener — deliberately just defers to
    // triggerAsyncUpdate() rather than rebuilding inline (see class doc
    // comment). Filtered to IDs::PLUGIN children so a clip add/remove (also a
    // direct child of a real track's own state) doesn't trigger a pointless
    // rebuild.
    void valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree&, int) override;
    void valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged, const juce::Identifier&) override;

    // juce::AsyncUpdater — the safe, deferred half of the three callbacks
    // above.
    void handleAsyncUpdate() override;

    juce::Label caption;

    // Declaration order matters: members are destroyed in REVERSE order, so
    // content (declared first) is destroyed AFTER viewport (declared
    // second) — viewport's destructor runs first while content is still
    // alive, required since viewport holds a raw (non-owning) pointer to it
    // via setViewedComponent (..., false).
    GridContent content;
    juce::Viewport viewport;

    std::vector<std::unique_ptr<PluginSlotComponent>> slots;

    te::Track* currentTrack = nullptr; // not owned — lifetime belongs to the Edit's track list
    CrateWorkflowManager* currentWorkflow = nullptr;
    juce::ValueTree listenedState; // whichever tree rebuild() actually attached the listener to

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertsRackComponent)
};
