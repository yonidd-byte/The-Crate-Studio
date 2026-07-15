#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Universal Device Chain — Ableton-style "Micro View" (MASTER_ARCHITECTURE.md Law
    IV: "there is exactly one place in this DAW where 'a thing that processes
    signal' lives"). Bottom zone of the Four-Zone Shell.

      +--------------------------------------------------------------------+
      | [4OSC   ] [ EQ (expanded) --------------------------- ] [Comp   ]  |
      |  bypass   | Freq   [======O====]                     |  bypass   |
      |           | Gain   [====O======]                     |           |
      |           | Q      [========O==]                     |           |
      +--------------------------------------------------------------------+

    Listens to track selection (showTrack()) and to plugin-slot clicks bubbled up
    from a MixerStrip's InsertsBlock (focusPlugin()) — clicking an insert slot in
    the Pro Tools-style vertical mixer instantly expands that device here, giving
    "deep tweaking" access without a floating plugin editor window. Only the
    focused device expands; every other device on the track folds to a minimal
    header (Law IV: "folded into minimal headers to reclaim ... space").

    Deliberately does NOT show sends (te::AuxSendPlugin) or the always-present
    VolumeAndPanPlugin/LevelMeterPlugin utility plugins — those are Pro Tools-
    mixer-strip territory (MixerStrip's RoutingBlock/SendsBlock), not "things that
    process signal" a user drags/reorders/generates with.

    Also a juce::DragAndDropTarget (public inheritance — REQUIRED: JUCE's
    DragAndDropContainer discovers drop targets via an EXTERNAL
    dynamic_cast<DragAndDropTarget*>(component) while walking the parent chain
    during a drag, same mechanism/gotcha as juce::FileDragAndDropTarget; private
    inheritance would make the cast well-formed but return nullptr, silently
    breaking drops). Accepts a Browser plugin drag ("plugin_drag|" prefix, see
    BrowserComponent::PluginRow) dropped ANYWHERE on this component and appends
    it to the end of the currently-shown track's chain.
*/
class UniversalDeviceChainComponent : public juce::Component,
                                       public juce::DragAndDropTarget,
                                       private juce::ValueTree::Listener
{
public:
    UniversalDeviceChainComponent (te::Edit& editToShow, CrateWorkflowManager& workflowToUse);
    ~UniversalDeviceChainComponent() override;

    /** Track selection changed elsewhere (Arrangement header click, a MixerStrip
        insert click on a different track, etc.) — rebuilds the chain for that
        track's plugins, with nothing focused/expanded yet. Pass nullptr for "no
        track selected". */
    void showTrack (te::AudioTrack* trackToShow);

    /** A specific plugin should come into focus (from MixerStrip::onPluginSlotSelected).
        Switches track first if pluginOwner differs from the currently shown track,
        then expands that plugin's block and folds every other one. */
    void focusPlugin (te::AudioTrack* pluginOwner, te::Plugin* pluginToFocus);

    /** The track currently displayed here (nullptr if none) — callers check this
        before deleting a track to know whether clearTrack() is actually needed. */
    te::AudioTrack* getCurrentTrack() const noexcept   { return currentTrack.get(); }

    /** Empties the chain and drops every DeviceBlock/ParamRow reference into the
        current track's plugins. MUST be called BEFORE the engine destroys a track
        that's currently displayed/focused here — e.g. right before
        CrateWorkflowManager::deleteSelectedTrack() — since deleting a track frees
        its pluginList, and DeviceBlock holds te::Plugin& bound to those exact
        objects. Equivalent to showTrack(nullptr); named separately so call sites
        state their intent (this isn't a "user navigated away" clear). */
    void clearTrack();

    /** Re-reads the currently-shown track's plugin list and rebuilds the chain —
        call after a plugin loads/unloads on that track so a newly-added device
        appears immediately instead of only after the user happens to switch track
        selection. showTrack(track) alone won't do this: it early-outs when the
        track argument is unchanged from what's already showing. No-op if no
        track is currently shown. */
    void refreshCurrentTrack();

    /** How tall this component's actual content wants to be right now (current
        track's devices, current fold states) — MainComponent uses this instead
        of a fixed constant so the bottom zone shrinks to fit a folded/empty
        chain and grows only as far as an unfolded device's content actually
        needs, instead of always reserving a fixed height regardless of what's
        showing. */
    int getPreferredContentHeight() const;

    /** Fires whenever getPreferredContentHeight() would return something
        different than last time (a fold/unfold, a track switch, a plugin
        load/delete) — MainComponent re-runs its own resized() in response so
        the outer zone height actually tracks this. */
    std::function<void()> onPreferredHeightChanged;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class DeviceBlock;     // one plugin: folded header, or expanded with param sliders
    class ChainRowContent; // horizontal row of DeviceBlock

    void rebuildBlocks();
    void layoutContent();
    void notifyIfPreferredHeightChanged();

    // Centralizes currentTrack reassignment so its te::ValueTree::Listener
    // registration (on track->state, which IS track->pluginList.state — see
    // MixerStrip's equivalent for the same fact) always tracks whichever track is
    // actually being shown. Every currentTrack = ... assignment goes through this.
    void setCurrentTrack (te::AudioTrack* newTrack);

    // te::PluginList::state IS track->state (PluginList::initialise() does
    // state = v, not a child tree), so listening to the track catches plugin
    // add/remove too — including via Undo/Redo, which never goes through
    // refreshCurrentTrack()'s normal call sites. Filtered to IDs::PLUGIN children
    // so a clip add/remove (also a direct child of track->state) doesn't trigger
    // a pointless rebuild.
    void valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree) override;
    void valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int) override;

    te::Edit& edit;
    CrateWorkflowManager& workflow;
    te::AudioTrack::Ptr currentTrack;
    te::Plugin* focusedPlugin = nullptr; // raw: lifetime owned by currentTrack->pluginList, cleared in showTrack()

    juce::Viewport viewport;
    std::unique_ptr<ChainRowContent> content;
    int lastNotifiedHeight = -1;
    bool isDragHovering = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UniversalDeviceChainComponent)
};
