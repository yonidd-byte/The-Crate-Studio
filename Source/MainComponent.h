#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CrateWorkflowManager.h"
#include "UI/TransportBar.h"
#include "UI/BrowserComponent.h"
#include "UI/ArrangementComponent.h"
#include "UI/MixerComponent.h"
#include "UI/UniversalDeviceChainComponent.h"
#include "UI/CratePianoRollComponent.h"
#include "UI/CrateMidiInspectorComponent.h"
#include "UI/BottomPanelContainer.h"

namespace te = tracktion::engine;

/**
    The Four-Zone Shell (MASTER_ARCHITECTURE.md Law I / Law II), now wired to the
    real Tracktion Engine stack.

      Top    -> real TransportBar (bound to CrateWorkflowManager), plus the
                Browser/Device Chain progressive-disclosure toggles and the
                Mix/Arrange view toggle, all at its far right.
      Left   -> ShellPlaceholderPanel (Crate Browser not built yet).
      Center -> real ArrangementComponent AND real MixerComponent, both bound to
                workflow->getEdit(), sharing the same Grid cell. Only one is
                setVisible() at a time — Law I: this is a crossfade, never a reload.
      Bottom -> BottomPanelContainer, a context-aware swap host: the real
                UniversalDeviceChainComponent while Arrangement is active, a
                MasterAnalyzerComponent while Mixer is active, and a MIDI FX
                placeholder while the Piano Roll overlay is active (Hybrid
                Device & Mixer Paradigm) — see BottomPanelContainer's own doc
                comment for the ownership split between this class and it.

    Layout is entirely juce::Grid-driven, rebuilt every resized() call from current
    zone visibility — a hidden zone's track collapses to Grid::Px(0) and the center
    Fr(1) track absorbs the freed space. No manual bounds math for the shell itself.
    Grid::performLayout() sets bounds on every GridItem regardless of setVisible()
    state, so giving arrangement and mixer the same cell area is safe: both get
    identical bounds every resize, but only the visible one paints or receives
    mouse events.

    TransportBar, ArrangementComponent, MixerComponent, and
    UniversalDeviceChainComponent all hold te::Edit& (not a pointer), so none can
    survive CrateWorkflowManager swapping the Edit out from under them on Load —
    rebuildUIForEdit() tears all four down and reconstructs them against the new
    Edit. browserDock is NOT part of that teardown: it holds no Edit reference, so
    it's constructed once in the constructor and simply persists across reloads.

    Dual-Representation sync (Pro Tools macro view <-> Ableton micro view): a track
    selection anywhere (Arrangement header click) calls deviceChain->showTrack();
    clicking a plugin slot inside a MixerStrip's InsertsBlock calls
    deviceChain->focusPlugin(), expanding that exact device below without opening a
    floating plugin editor window.

    Also a juce::DragAndDropContainer (public inheritance, same requirement as
    FileDragAndDropTarget above — internal JUCE drags are found via
    DragAndDropContainer::findParentDragContainerFor(), which walks the parent
    chain doing an external dynamic_cast<DragAndDropContainer*> at each step;
    private inheritance would make that well-formed but return nullptr).
    BrowserComponent::PluginRow is the drag SOURCE (Browser plugin list);
    PluginSlotComponent and UniversalDeviceChainComponent are the drop targets —
    MainComponent itself has no DragAndDropTarget/Listener code of its own, it's
    purely the required container root the drag mechanism looks for.
*/
class MainComponent : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       public juce::DragAndDropContainer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    /** Global Spacebar Play/Stop (Ableton/Pro Tools convention). Routed through
        CrateWorkflowManager::startPlayback()/stopAndReturnToStart() — the SAME
        methods TransportBar's Play/Stop buttons call — so both entry points
        share one "where did playback start" state instead of each tracking it
        separately. setWantsKeyboardFocus(true) + grabKeyboardFocus() in the
        constructor give this component focus by default; TransportBar's
        IconButtons additionally opt out of keyboard focus themselves so a
        button click can't silently steal Spacebar afterwards (see IconButton's
        constructor comment) — this alone doesn't make every control in the app
        Spacebar-safe, just the transport controls most likely to be clicked
        right before reaching for it. */
    bool keyPressed (const juce::KeyPress& key) override;

    // juce::FileDragAndDropTarget — MUST be public (or protected), not private.
    // JUCE's native peer discovers this interface via an EXTERNAL
    // dynamic_cast<FileDragAndDropTarget*>(component) while walking the parent
    // chain during an OS drag-enter — that cast runs from library code with no
    // friend/member access to MainComponent, so if the inheritance is private,
    // the cast is well-formed but returns nullptr (checked-access rule for
    // dynamic_cast to an inaccessible base), and the OS peer reports zero valid
    // drop targets before isInterestedInFileDrag() ever runs. This is different
    // from Timer/AutomatableParameter::Listener elsewhere in this codebase (safe
    // to keep private there): those register via `this` converted to the
    // listener pointer INSIDE this class's own member function (e.g.
    // addListener(this)), where private inheritance IS accessible — no later
    // external cast is ever needed. FileDragAndDropTarget has no such
    // registration step; it's discovered cold, from outside, every time.
    //
    // Pure routing: bounds-check against arrangement's bounds, translate
    // coordinates, delegate to ArrangementComponent::processDroppedAudio() (or
    // updateDragHover()/clearDragHover() for the live highlight) — the actual
    // Tracktion Engine insertion logic lives there, not here, since this class
    // has no business reasoning about clip time/track lookups.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void rebuildUIForEdit();
    void showArrangementView();
    void showMixerView();

    // Hybrid Device & Mixer Paradigm — recomputes bottomPanelContainer's
    // ActiveView from the current showingMixerView/showingMidiEditor state,
    // updates the toggle button's text ("DEVICE CHAIN" / "ANALYZE" / "MIDI
    // FX"), and re-points it at whichever real deviceChain instance is alive.
    // Called from every place that changes view state (Load, Arrange<->Mixer,
    // MIDI editor enter/exit) so the bottom panel can never show stale content
    // for the view you're actually looking at.
    void updateBottomPanelForActiveView();

    // Phase 4 (The MIDI Suite) — the Overlay Crossfade (Law I: Strict
    // Single-Window Paradigm). enterMidiEditor() hides Arrangement + Browser and
    // reveals the Piano Roll (center) + Inspector (left column), pointing both at
    // `clip`; exitMidiEditor() reverses it, restoring whichever center view and
    // browser state were active before. Neither destroys anything — it's a pure
    // setVisible() swap over shared Grid cells, same crossfade mechanism
    // Arrangement<->Mixer already uses.
    void enterMidiEditor (te::MidiClip* clip);
    void exitMidiEditor();

    // Shared by fileDragEnter/fileDragMove: bounds-checks against arrangement's
    // current bounds and routes to ArrangementComponent::updateDragHover() (local
    // coordinates) or clearDragHover() if the point falls outside it. Not part of
    // the FileDragAndDropTarget interface itself, so this one stays private.
    void routeDragHover (const juce::StringArray& files, int x, int y);

    // Declaration order matters: members are destroyed in reverse declaration order.
    // transportBar/arrangement/mixer/deviceChain hold te::Edit& into workflow's
    // Edit, so they must be declared AFTER workflow to be destroyed BEFORE it
    // (their references would otherwise dangle for however long the intervening
    // destructors take to run).
    std::unique_ptr<CrateWorkflowManager> workflow;
    std::unique_ptr<TransportBar> transportBar;
    std::unique_ptr<ArrangementComponent> arrangement;
    std::unique_ptr<MixerComponent> mixer;
    std::unique_ptr<UniversalDeviceChainComponent> deviceChain;

    // Hybrid Device & Mixer Paradigm — NOT Edit-bound itself (see its own doc
    // comment), so unlike arrangement/mixer/deviceChain this is constructed
    // ONCE in the constructor and survives every project Load untouched, the
    // same way browserDock does; rebuildUIForEdit() just re-points it at
    // whichever fresh deviceChain instance that Load produced.
    std::unique_ptr<BottomPanelContainer> bottomPanelContainer;

    // Zone 4 (MIDI Suite) overlay panels. Hold a raw te::MidiClip* into the
    // current Edit, so — like the four above — they're reconstructed by
    // rebuildUIForEdit() on a project Load (and reset first in onBeforeEditSwap),
    // never left pointing at a freed clip. Declared after `workflow` for the same
    // reverse-destruction-order reason.
    std::unique_ptr<CratePianoRollComponent> pianoRoll;
    std::unique_ptr<CrateMidiInspectorComponent> midiInspector;

    // Not Edit-bound (only knownPluginList + track-selection state, both of which
    // outlive a Load) — safe to construct once and leave alone across Load/rebuild.
    BrowserComponent browserDock;

    // arrangement/mixer/deviceChain are destroyed and reconstructed by
    // rebuildUIForEdit() (Load), so their own isVisible() can't survive that as the
    // source of truth for view/zone state — these do, and rebuildUIForEdit()
    // re-applies them every time.
    bool showingMixerView = false;
    bool bottomPanelVisible = true; // renamed from deviceChainVisible — now gates the WHOLE BottomPanelContainer, not just the Device Chain specifically

    // bottomPanelContainer's own bounds, cached so paintOverChildren()'s
    // top-edge divider line can read the CURRENT layout without recomputing it.
    juce::Rectangle<int> bottomPanelBounds;

    // Whether the Zone-4 MIDI editor overlay is currently up (Piano Roll +
    // Inspector shown, Arrangement + Browser hidden). Reset to false by
    // rebuildUIForEdit() so a project Load always lands back in Arrangement.
    bool showingMidiEditor = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
