#include "MainComponent.h"
#include "UI/CrateColors.h"
#include "CrateStressTest.h"

#if JUCE_DEBUG
 #include "Engine/CrateSandboxBridge.h"
#include "Engine/SandboxManager.h"
 #include <functional>
 #include <memory>
#endif

namespace
{
    // wav/mp3/aiff/flac only, case-insensitive — matches the Lead UX Architect's
    // spec exactly rather than trusting whatever juce::AudioFormatManager happens
    // to have registered.
    bool isSupportedAudioFile (const juce::String& path)
    {
        const auto ext = juce::File (path).getFileExtension().toLowerCase();
        return ext == ".wav" || ext == ".mp3" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
    }
}

//==============================================================================
MainComponent::MainComponent()
    : workflow (std::make_unique<CrateWorkflowManager>()),
      browserDock (*workflow)
{
    // Hybrid Bus/Return Architecture — createAndRouteNewFXChannel() (called
    // from deep inside a MixerStrip/InspectorStrip Sends menu) has no other
    // way to tell ArrangementComponent/MixerComponent a new track now
    // exists. Wired ONCE here, not inside rebuildUIForEdit(): `workflow`
    // itself persists across a project Load (only its internal Edit swaps —
    // see this class's own doc comment), and the lambda reads `arrangement`
    // at CALL time, so it stays correct even after rebuildUIForEdit()
    // replaces that unique_ptr with a fresh instance.
    workflow->onTrackListChanged = [this]
    {
        if (arrangement != nullptr)
            arrangement->rebuildTracks(); // cascades to mixer->rebuildStrips() via its own onTracksChanged
    };

    addAndMakeVisible (browserDock);

    // Hybrid Device & Mixer Paradigm — NOT Edit-bound (see its own doc
    // comment), constructed once here and never touched by rebuildUIForEdit()'s
    // teardown/rebuild, same as browserDock.
    bottomPanelContainer = std::make_unique<BottomPanelContainer>();
    addAndMakeVisible (*bottomPanelContainer);

    rebuildUIForEdit();

    setSize (1100, 650);

    // Global Spacebar Play/Stop — see keyPressed()'s doc comment in the header.
    setWantsKeyboardFocus (true);
    grabKeyboardFocus();

    // Bulletproof Live Mode / Hardware Acceleration directive: attach LAST,
    // once every child is already in place — attachTo() triggers an
    // immediate full repaint of this component and its whole subtree, so
    // there's no benefit (and a real risk of attaching against a
    // half-constructed tree) to doing it any earlier.
    openGLContext.attachTo (*this);
}

MainComponent::~MainComponent()
{
    // MUST detach before this Component (and everything under it) starts
    // being torn down — the GL context holds a live reference to this
    // peer/component tree for its render callback, and detaching here
    // (rather than relying on ~OpenGLContext() during member destruction,
    // which runs AFTER this body, once children are already gone) is the
    // documented-safe teardown order.
    openGLContext.detach();
}

void MainComponent::rebuildUIForEdit()
{
    // A project Load rebuilds everything against the new Edit — the old MIDI
    // clip a piano roll might have been editing is gone, so always land back in
    // Arrangement view rather than an overlay pointing at a freed clip.
    showingMidiEditor = false;

    transportBar = std::make_unique<TransportBar> (*workflow);
    arrangement  = std::make_unique<ArrangementComponent> (workflow->getEdit(), *workflow);
    mixer        = std::make_unique<MixerComponent> (workflow->getEdit(), *workflow);
    deviceChain  = std::make_unique<UniversalDeviceChainComponent> (workflow->getEdit(), *workflow);
    bottomPanelContainer->setDeviceChainComponent (deviceChain.get()); // re-point at the fresh instance this Load produced
    pianoRoll    = std::make_unique<CratePianoRollComponent>();
    midiInspector = std::make_unique<CrateMidiInspectorComponent>();

    // BrowserComponent now hosts midiInspector as a CHILD (under its INSPECTOR
    // tab) rather than MainComponent parenting it directly as a competing Grid
    // sibling — see resized()'s doc comment on leftColumnVisible. Ownership/
    // lifecycle (construction here, setActiveClip() below, reset() on Load)
    // stays entirely in MainComponent; browserDock only holds a display pointer.
    browserDock.setMidiInspector (midiInspector.get());

    // Bug 6 fix superseded: MixerStrip and UniversalDeviceChainComponent each now
    // listen directly to their track's own ValueTree for plugin add/remove (see
    // their valueTreeChildAdded/Removed overrides), which covers plugin loads,
    // deletes, AND Undo/Redo of either — a callback fired only from
    // loadPluginToSelectedTrack() never could have covered the Undo/Redo cases.

    // Bridges ArrangementComponent's selection/deletion UI events into the workflow
    // manager, which is the single authority for currentSelectedTrack and for
    // actually deleting a track (wrapped in an Undo transaction). Also keeps the
    // Universal Device Chain (bottom) in sync with track selection — Dual-
    // Representation System: the "micro view" always reflects whatever track the
    // "macro view" (Arrangement/Mixer) currently has selected.
    arrangement->onTrackSelected = [this] (te::AudioTrack* t)
    {
        workflow->selectTrack (t);
        deviceChain->showTrack (t);

        // Task 4 wiring: this app has no te::SelectionManager instance anywhere
        // (selection is this hand-rolled onTrackSelected callback, already the
        // single real notification path every other selection consumer above
        // uses) — so the Inspector is wired into that SAME existing path rather
        // than bolting on a second, competing selection mechanism nothing else
        // reads from.
        browserDock.setSelectedTrack (t);
    };

    // Task 5: clicking the pinned Master row in the Arrangement pushes
    // edit->getMasterTrack() through the exact same selection/Device Chain/
    // Inspector path a real track's header click uses above — "exactly like a
    // normal track," per spec, just routed through te::Track (Master isn't an
    // AudioTrack) instead of onTrackSelected's AudioTrack-only callback.
    arrangement->onMasterTrackSelected = [this]
    {
        auto* masterTrack = workflow->getEdit().getMasterTrack();
        workflow->selectTrack (masterTrack);
        deviceChain->showTrack (masterTrack);
        browserDock.setSelectedTrack (masterTrack);
    };

    arrangement->onDeleteTrackRequested = [this] (te::AudioTrack* t)
    {
        workflow->selectTrack (t);

        // CRITICAL fix: drop the device chain's references into this track's
        // plugins BEFORE the engine destroys them. deleteSelectedTrack() below
        // frees the track's pluginList; DeviceBlock/ParamRow hold te::Plugin&
        // bound to those exact objects, so if this track is the one currently
        // shown/focused in deviceChain, leaving it untouched means the next
        // repaint or click there (e.g. toggling a bypass button) dereferences
        // freed memory. Mixer doesn't need the same guard — its rebuildStrips()
        // (fired via onTracksChanged below) always runs and fully rebuilds every
        // MixerStrip, so nothing there survives to reference the deleted track's
        // plugins.
        if (deviceChain->getCurrentTrack() == t)
            deviceChain->clearTrack();

        // UAF fix (QA pass): the Inspector needed the EXACT same guard as
        // deviceChain above and never got it. Unlike deviceChain/MixerStrip,
        // CrateTrackInspectorComponent is long-lived (constructed once, just
        // re-pointed on every selection change) and its InspectorStrip
        // instances registered a live te::AutomatableParameter::Listener on
        // this track's volParam/panParam — clearing it unconditionally BEFORE
        // deleteSelectedTrack() runs setTrack(nullptr)'s teardown (listener
        // removal) while the plugin is still alive. Unconditional (not gated
        // on "is this the currently-shown track" like deviceChain's guard
        // above) since BrowserComponent exposes no getSelectedTrack() query
        // and setTrack(nullptr) on an already-empty Inspector is a harmless
        // no-op. (InspectorStrip's track/volumePlugin/meterPlugin were also
        // hardened to reference-counted Ptrs, so even a future path that
        // forgets this call can no longer crash — this just avoids relying
        // on that safety net.)
        browserDock.setSelectedTrack (nullptr);

        workflow->deleteSelectedTrack();
        arrangement->rebuildTracks();
    };

    // Arrangement and Mixer are two independent views onto the same track list —
    // whenever a track is added (+Audio/+MIDI, inside ArrangementComponent) or
    // removed (above), rebuildTracks() fires this so Mixer's strip row doesn't go
    // stale relative to what Arrangement is showing.
    arrangement->onTracksChanged = [this] { mixer->rebuildStrips(); };

    // Phase 4 MIDI Suite — Arrangement reports which MIDI clip to open (double-
    // click existing, or double-click empty lane which creates one first); this
    // bridges that to the Overlay Crossfade. The reverse (Escape) is wired on
    // the piano roll just below.
    arrangement->onMidiClipOpenRequested = [this] (te::MidiClip* clip) { enterMidiEditor (clip); };
    pianoRoll->onExitRequested = [this] { exitMidiEditor(); };

    // The other half of the Dual-Representation sync: clicking an insert slot in a
    // MixerStrip's ChannelStripRack (Pro Tools macro view) brings that exact device
    // into focus in the Universal Device Chain (Ableton micro view) below —
    // eliminating the need for a floating plugin editor window during routine mixing.
    mixer->onPluginSlotSelected = [this] (te::AudioTrack* t, te::Plugin* p)
    {
        workflow->selectTrack (t);
        deviceChain->focusPlugin (t, p);
    };

    // Master Track -> Device Chain: te::MasterTrack isn't an AudioTrack, but
    // showTrack()/loadPluginOntoTrack() were widened to the te::Track base type
    // specifically so this works — clicking Master now lets the user load
    // mastering plugins onto it the same way as any real track.
    mixer->onMasterSelected = [this]
    {
        auto* masterTrack = workflow->getEdit().getMasterTrack();
        workflow->selectTrack (masterTrack);
        deviceChain->showTrack (masterTrack);
        browserDock.setSelectedTrack (masterTrack);
    };

    // Clicking a mastering FX slot in the Mixer's pinned MasterStrip focuses
    // it in the Device Chain — same "select track, then focus this exact
    // plugin" two-step onPluginSlotSelected already does for a real track's
    // inserts, just with getMasterTrack() as the fixed track.
    mixer->onMasterInsertSelected = [this] (te::Plugin* p)
    {
        auto* masterTrack = workflow->getEdit().getMasterTrack();
        workflow->selectTrack (masterTrack);
        deviceChain->focusPlugin (masterTrack, p);
    };

    // Makes the bottom zone actually fit its content: a fold/unfold, track
    // switch, or plugin load/delete changes what deviceChain wants to show, so
    // resized() needs to re-run and re-query getPreferredContentHeight() —
    // nothing else would trigger that on its own.
    deviceChain->onPreferredHeightChanged = [this] { resized(); };

    // onLoadRequested fires and returns immediately (nothing destructive happens
    // yet), so it's safe for THIS lambda to be a TransportBar member. The two
    // lambdas passed to safeLoadProject() below are a different story — they run
    // later, from CrateWorkflowManager's own fileChooser callback (not from
    // anything stored inside transportBar), which is what makes it safe for
    // onBeforeEditSwap to synchronously destroy transportBar. See
    // CrateWorkflowManager::safeLoadProject()'s doc comment for the full ordering
    // contract this relies on.
    transportBar->onLoadRequested = [this]
    {
        workflow->safeLoadProject (
            [this] // onBeforeEditSwap — old Edit still alive; drop every Edit-bound
                   // UI pointer now, before CrateWorkflowManager touches it.
            {
                browserDock.setMidiInspector (nullptr); // clear the display pointer BEFORE the real object dies
                midiInspector.reset(); // hold a raw clip into the old Edit — drop first
                pianoRoll.reset();
                deviceChain.reset();
                mixer.reset();
                arrangement.reset();
                transportBar.reset();
            },
            [this] (te::Edit&) // onAfterEditSwap — new Edit is live; rebuild against it.
            {
                rebuildUIForEdit();
            });
    };

    // Four-Zone Shell progressive-disclosure toggles now live on the real
    // TransportBar (far right) — MainComponent still owns the zone visibility state
    // and the Grid they reflow, it just wires the buttons externally each time
    // TransportBar is reconstructed (Load rebuilds it, so onClick has to be re-set).
    // Toggle state is re-synced from the panels' actual current visibility so a
    // post-Load rebuild doesn't silently reset an already-collapsed zone back open.
    transportBar->toggleBrowserButton.setToggleState (browserDock.isVisible(), juce::dontSendNotification);
    transportBar->toggleBrowserButton.onClick = [this]
    {
        browserDock.setVisible (transportBar->toggleBrowserButton.getToggleState());
        resized();
    };

    // Dynamic Top Toggle Button — text/behaviour now depend on whichever
    // view is active (updateBottomPanelForActiveView() sets the label);
    // clicking it just shows/hides the WHOLE BottomPanelContainer INSTANTLY —
    // Reverted directive: no tween. A DAW's panel toggle must be zero-latency,
    // not a 180ms animation disrupting the workflow.
    transportBar->toggleDeviceChainButton.setToggleState (bottomPanelVisible, juce::dontSendNotification);
    transportBar->toggleDeviceChainButton.onClick = [this]
    {
        bottomPanelVisible = transportBar->toggleDeviceChainButton.getToggleState();
        resized();
    };

    // Segmented TIMELINE/MIXER pair (see TransportBar.h doc comment) — each
    // sets the ABSOLUTE target view (not a toggle) and unconditionally calls
    // exitMidiEditor(), which is always safe to call even when not currently
    // in the MIDI editor (idempotent — see its own doc comment). This is what
    // makes either button a working exit route out of the Zone-4 overlay,
    // fixing the earlier bug where clicking the single toggle button while in
    // the editor silently did nothing visible.
    transportBar->timelineButton.onClick = [this]
    {
        showingMixerView = false;
        exitMidiEditor();
    };

    transportBar->mixerButton.onClick = [this]
    {
        showingMixerView = true;
        exitMidiEditor();
    };

    addAndMakeVisible (*transportBar);
    addAndMakeVisible (*arrangement);
    addAndMakeVisible (*mixer);
    // deviceChain is NOT added here — bottomPanelContainer hosts it (see
    // setDeviceChainComponent() above) and manages its visibility itself.

    // Zone-4 overlay panel starts hidden (addChildComponent, not
    // addAndMakeVisible) — enterMidiEditor() reveals it. Added AFTER
    // arrangement/mixer so it sits above in z-order when shown. midiInspector
    // is NOT added here — it's a child of browserDock now (see
    // browserDock.setMidiInspector() above), which already owns its own
    // addChildComponent/addAndMakeVisible for it.
    addChildComponent (*pianoRoll);

    // Re-applies whichever view was active before this rebuild (Load reconstructs
    // both from scratch, so their fresh isVisible() defaults can't be trusted).
    showingMixerView ? showMixerView() : showArrangementView();
}

void MainComponent::showArrangementView()
{
    showingMixerView = false;
    arrangement->setVisible (true);
    mixer->setVisible (false);

    // Toggle-highlight sync — required here (not just on click) because this
    // function also runs from exitMidiEditor(), Load's rebuildUIForEdit(), and
    // the TIMELINE/MIXER buttons' own onClick, none of which can rely on the
    // radio group's click-only auto-update to keep the OTHER button in sync.
    transportBar->timelineButton.setToggleState (true, juce::dontSendNotification);
    transportBar->mixerButton.setToggleState (false, juce::dontSendNotification);

    // Smart Default (NOT a hard lock — the user can still click any tab
    // afterward): landing on Timeline defaults the Left Panel to INSPECTOR,
    // which shows the track/output dual-strip view since Piano Roll isn't
    // active here.
    browserDock.setActiveTab (3);

    updateBottomPanelForActiveView();
    resized();
}

void MainComponent::showMixerView()
{
    showingMixerView = true;
    mixer->rebuildStrips(); // picks up any track/plugin changes made while Arrangement was showing
    mixer->setVisible (true);
    arrangement->setVisible (false);

    transportBar->timelineButton.setToggleState (false, juce::dontSendNotification);
    transportBar->mixerButton.setToggleState (true, juce::dontSendNotification);

    // Smart Default: the full Mixer is already every track's channel strip —
    // defaulting the Left Panel to a single track's Inspector would be visual
    // duplication, so this lands on PLUGINS instead.
    browserDock.setActiveTab (0);

    updateBottomPanelForActiveView();
    resized();
}

void MainComponent::enterMidiEditor (te::MidiClip* clip)
{
    if (pianoRoll == nullptr || midiInspector == nullptr)
        return;

    showingMidiEditor = true;

    // Hide the Arrangement macro-view; reveal the Zone-4 overlay. transportBar
    // (row 1, spans both Grid columns) is deliberately NEVER touched here — it
    // must stay visible and clickable so the 'Arrange' button remains a
    // working exit route alongside Escape (see its onClick wiring above for
    // the other half of that fix). browserDock is deliberately NOT hidden
    // either anymore — it's the permanent Left Panel host now, including for
    // midiInspector (its INSPECTOR tab content while pianoRollActive).
    arrangement->setVisible (false);
    mixer->setVisible (false);

    pianoRoll->setVisible (true);

    // Point both panels at the clip AFTER making the piano roll visible, so its
    // own grabKeyboardFocus() (in setActiveClip) sees isShowing() == true.
    pianoRoll->setInspectorComponent (midiInspector.get());
    pianoRoll->setActiveClip (clip);
    midiInspector->setActiveClip (clip);

    // Smart Default (not a hard lock): opening the Piano Roll switches the
    // Left Panel to INSPECTOR, and setPianoRollActive(true) is what makes that
    // tab show midiInspector instead of the track/output dual-strip view.
    browserDock.setPianoRollActive (true);
    browserDock.setActiveTab (3);

    updateBottomPanelForActiveView();
    resized();

    // Focus trap (QA requirement): explicit re-grab after layout so Escape
    // registers immediately without the user clicking the piano roll first.
    pianoRoll->grabKeyboardFocus();
}

void MainComponent::exitMidiEditor()
{
    if (pianoRoll == nullptr || midiInspector == nullptr)
        return;

    showingMidiEditor = false;

    pianoRoll->setActiveClip (nullptr);
    midiInspector->setActiveClip (nullptr);
    pianoRoll->setVisible (false);

    // Context switch back — if the Left Panel is still on the INSPECTOR tab,
    // it now shows the track/output dual-strip view instead of midiInspector.
    browserDock.setPianoRollActive (false);

    // Re-show whichever center view (Arrange/Mix) was active before entering —
    // showXxxView() applies its own Smart Default tab AND calls resized() itself.
    showingMixerView ? showMixerView() : showArrangementView();

    // Hand keyboard focus back to the shell so global Spacebar transport works.
    grabKeyboardFocus();
}

void MainComponent::updateBottomPanelForActiveView()
{
    if (bottomPanelContainer == nullptr || transportBar == nullptr)
        return;

    BottomPanelContainer::ActiveView view;
    juce::String buttonText;

    if (showingMidiEditor)
    {
        view = BottomPanelContainer::ActiveView::pianoRoll;
        buttonText = "MIDI FX";
    }
    else if (showingMixerView)
    {
        view = BottomPanelContainer::ActiveView::mixer;
        buttonText = "ANALYZE";
    }
    else
    {
        view = BottomPanelContainer::ActiveView::arrangement;
        buttonText = "DEVICE CHAIN";
    }

    bottomPanelContainer->setActiveView (view);
    transportBar->toggleDeviceChainButton.setButtonText (buttonText);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (CrateColors::DarkBackground);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // Zone-separation frame: strict near-black 2px dividers drawn ON TOP of every
    // child (paintOverChildren runs after all children paint), so they read as
    // real panel seams instead of being silently painted over by whichever zone's
    // own fillAll happens to share that pixel column/row. Bounds are read straight
    // off the children themselves rather than recomputed, so this can never drift
    // from the actual Grid/removeFromBottom layout in resized().
    if (transportBar == nullptr || arrangement == nullptr || mixer == nullptr || deviceChain == nullptr)
        return; // mid-rebuildUIForEdit()

    const auto dividerColour = juce::Colour (0xff000000).withAlpha (0.9f);

    // Transport bottom edge.
    g.setColour (dividerColour);
    g.fillRect (0, transportBar->getBottom(), getWidth(), 2);

    // Browser right edge. BUG FIX: this used to run from transportBar->getBottom()
    // all the way to getHeight() — the full window height — which cut straight
    // through the Device Chain zone at the bottom instead of stopping above it.
    // browserDock's own bounds are exactly right already (Grid lays it out only
    // across the center row, which resized() computes AFTER carving the Device
    // Chain strip off the bottom — see the removeFromBottom() call there), so using
    // browserDock.getY()/getHeight() directly means this line can never drift from
    // that zone boundary, however BottomPanelContainer::preferredHeight changes.
    if (browserDock.isVisible())
        g.fillRect (browserDock.getRight(), browserDock.getY(), 2, browserDock.getHeight());

    // Bottom panel top edge — only meaningful while it's showing.
    if (! bottomPanelBounds.isEmpty())
        g.fillRect (0, bottomPanelBounds.getY(), getWidth(), 2);
}

void MainComponent::resized()
{
    if (transportBar == nullptr || arrangement == nullptr || mixer == nullptr || deviceChain == nullptr
         || pianoRoll == nullptr || midiInspector == nullptr)
        return; // mid-rebuildUIForEdit(), old set already reset, new set not yet built

    constexpr int transportHeight    = 50;
    constexpr int browserWidth       = 280;

    auto bounds = getLocalBounds();

    // Snap-to-bottom fix (still in effect): carve the bottom panel strip off
    // the UN-REDUCED local bounds with plain integer arithmetic BEFORE
    // anything else touches `bounds` — juce::Grid's per-row float rounding is
    // what caused the original 1-2px float; removeFromBottom() has none.
    // Clamps safely to whatever's left if the window is shorter than
    // BottomPanelContainer::preferredHeight (e.g. minimized) — never a
    // negative rect. Reverted directive: toggling is an instant setVisible()-
    // style pop, not a tween — this is the ONLY place that ever sets
    // bottomPanelContainer's bounds, called synchronously from the toggle
    // button's onClick with no animation involved.
    auto bottomBounds = bottomPanelVisible ? bounds.removeFromBottom (BottomPanelContainer::preferredHeight)
                                            : juce::Rectangle<int>();
    bottomPanelContainer->setBounds (bottomBounds);
    bottomPanelBounds = bottomBounds;

    using Track = juce::Grid::TrackInfo;
    using Px    = juce::Grid::Px;
    using Fr    = juce::Grid::Fr;

    // Left column is the Browser/Left-Panel — now the PERMANENT host of both
    // the plugin/sample/favorites browser AND the Context-Aware Inspector (see
    // BrowserComponent's INSPECTOR tab), including while the Piano Roll overlay
    // is open (midiInspector is a CHILD of browserDock now, not a separate Grid
    // sibling competing for this same cell — see setMidiInspector()). So its
    // width only depends on the Browser's own visibility toggle.
    const bool leftColumnVisible = browserDock.isVisible();

    juce::Grid grid;

    grid.templateRows = { Track (Px (transportHeight)), Track (Fr (1)) };

    grid.templateColumns = { Track (Px (leftColumnVisible ? browserWidth : 0)),
                              Track (Fr (1)) };

    // Arrangement, Mixer, and the Piano Roll all share the center cell (2,2).
    // Grid sets bounds on every item each layout regardless of setVisible() —
    // only the visible one paints/receives events (Law I crossfade, same as
    // Arrangement<->Mixer).
    grid.items = {
        juce::GridItem (*transportBar).withArea (1, 1, 2, 3),   // row 1, spans both columns
        juce::GridItem (browserDock).withArea (2, 1),           // row 2, col 1
        juce::GridItem (*arrangement).withArea (2, 2),
        juce::GridItem (*mixer).withArea (2, 2),
        juce::GridItem (*pianoRoll).withArea (2, 2),            // shares center cell
    };

    // Whatever's left after the bottom carve — center Fr(1) row correctly
    // shrinks when the chain is visible and expands to fill it when hidden,
    // same as before, just computed on the post-carve rectangle instead of
    // Grid owning all three rows itself.
    grid.performLayout (bounds);
}

//==============================================================================
// juce::FileDragAndDropTarget — root-level implementation. This class IS the
// DocumentWindow's content component (Main.cpp's setContentOwned()), so there is
// no higher ancestor left to hand this job to. Pure routing: bounds-check
// against arrangement's current bounds, translate coordinates into its local
// space, delegate. All the actual Tracktion Engine work lives in
// ArrangementComponent::processDroppedAudio()/updateDragHover()/clearDragHover().
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    // Confirms whether the JUCE event loop is even reaching this component during
    // a hover — check the Output/debug console if drops still don't register.
    DBG ("MainComponent::isInterestedInFileDrag called");

    for (auto& f : files)
        if (isSupportedAudioFile (f))
            return true;

    return false;
}

void MainComponent::routeDragHover (const juce::StringArray& files, int x, int y)
{
    if (arrangement == nullptr)
        return;

    // Only accept the drop while Arrangement is the actually-visible view — Law I
    // means Mixer shares the exact same Grid cell/bounds, so a bounds-only check
    // would happily "hit" Arrangement even while Mixer is what's on screen.
    if (arrangement->isVisible() && arrangement->getBounds().contains (x, y))
    {
        const auto localPos = arrangement->getLocalPoint (this, juce::Point<int> (x, y));
        arrangement->updateDragHover (files, localPos.x, localPos.y);
    }
    else
    {
        arrangement->clearDragHover();
    }
}

void MainComponent::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    routeDragHover (files, x, y);
}

void MainComponent::fileDragMove (const juce::StringArray& files, int x, int y)
{
    routeDragHover (files, x, y);
}

void MainComponent::fileDragExit (const juce::StringArray&)
{
    if (arrangement != nullptr)
        arrangement->clearDragHover();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
   #if JUCE_DEBUG
    // Debug-Only QA Harness directive: hidden, undocumented, and physically
    // absent from a Release build (CrateStressTest.h's own JUCE_DEBUG guard
    // strips the class itself, not just this call site) — Ctrl+Alt+Shift+S
    // is deliberately obscure and appears nowhere in any menu or shortcut
    // list, so an end user cannot discover or trigger it by accident.
    if (key.getKeyCode() == (int) 'S'
        && key.getModifiers().isCtrlDown()
        && key.getModifiers().isAltDown()
        && key.getModifiers().isShiftDown())
    {
        CrateStressTest::runExtremeLoadTest (workflow->getEdit());
        return true;
    }

    // Step 9 (The Multi-Process Scalability Stress Test) directive: same
    // hidden/debug-only convention as Ctrl+Alt+Shift+S above, a separate
    // key so the 50-plugin scale test never fires by accident in place of
    // the regular 100-track stress test.
    if (key.getKeyCode() == (int) 'X'
        && key.getModifiers().isCtrlDown()
        && key.getModifiers().isAltDown()
        && key.getModifiers().isShiftDown())
    {
        CrateStressTest::runSandboxScaleTest (workflow->getEdit());
        return true;
    }

    // Step 10 (Cross-Process Window Reparenting) directive: same hidden/
    // debug-only convention as the other two above, a separate key again.
    if (key.getKeyCode() == (int) 'G'
        && key.getModifiers().isCtrlDown()
        && key.getModifiers().isAltDown()
        && key.getModifiers().isShiftDown())
    {
        showSandboxEditorTestWindow();
        return true;
    }

    // Step 12.1 (The Violent Crash Test) directive: same hidden/debug-only
    // convention — fires the poison pill at whichever bridge the currently
    // open Step 10 test window is watching. No-op if that window isn't open.
    if (key.getKeyCode() == (int) 'K'
        && key.getModifiers().isCtrlDown()
        && key.getModifiers().isAltDown()
        && key.getModifiers().isShiftDown())
    {
        triggerSandboxCrashTest();
        return true;
    }
   #endif

    if (key == juce::KeyPress::spaceKey)
    {
        if (workflow->getEdit().getTransport().isPlaying())
            workflow->stopAndReturnToStart();
        else
            workflow->startPlayback();

        return true;
    }

    return false;
}

#if JUCE_DEBUG

// Step 10 (Cross-Process Window Reparenting) directive: owns BOTH the
// embedding juce::HWNDComponent AND the te::Plugin::Ptr keeping the
// CrateSandboxBridge (and therefore its whole CrateSandbox.exe child
// process) alive for as long as this test window exists — closing the
// window tears down the entire test cleanly, in one place. A nested class
// (forward-declared in MainComponent.h) rather than an anonymous-namespace
// local type, specifically so MainComponent's own unique_ptr can be typed
// as the REAL derived class and reach hwndComponent/onClosed through it.
class MainComponent::SandboxEditorTestWindow : public juce::DocumentWindow,
                                                private juce::Timer
{
public:
    // The Unmissable Poison Pill directive (Step 12.1 retry): a hidden
    // keyboard shortcut turned out to be a genuine dead end — once the user
    // clicks into the embedded plugin editor, OS keyboard focus moves to
    // THIS window (a separate top-level window from MainComponent), so
    // MainComponent::keyPressed() simply never fires at all. Not a cross-
    // process focus issue as first suspected — a same-process JUCE
    // windowing one. A big, visible button living in THIS exact window
    // sidesteps the whole question of which window currently has OS
    // keyboard focus: a mouse click always goes to whatever's visibly
    // under the cursor, no ambiguity possible.
    //
    // hwndComponent is deliberately NOT stretched to fill whatever space is
    // left below the button — that would silently reintroduce the exact
    // crop/grey-gap problem Step 10.1 fixed. It keeps its own exact native
    // size always; only its POSITION shifts down by crashButtonHeight.
    struct ContentContainer : public juce::Component
    {
        static constexpr int crashButtonHeight = 40;

        ContentContainer()
        {
            crashButton.setButtonText ("FORCE CRASH (Step 12.1 Poison Pill)");
            crashButton.setColour (juce::TextButton::buttonColourId, juce::Colours::red);
            crashButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            addAndMakeVisible (crashButton);
            addAndMakeVisible (hwndComponent);
        }

        void resized() override
        {
            crashButton.setBounds (0, 0, getWidth(), crashButtonHeight);
            hwndComponent.setTopLeftPosition (0, crashButtonHeight);
        }

        juce::TextButton crashButton;
        juce::HWNDComponent hwndComponent;
    };

    explicit SandboxEditorTestWindow (tracktion::engine::Plugin::Ptr bridgePluginToKeep)
        : juce::DocumentWindow ("Sandboxed VST3 Editor (Step 10 Test Harness)",
                                // Near-black rather than JUCE's stock light
                                // Colours::darkgrey (0xff696969) — the plugin
                                // editor's own background reads dark (VST3s
                                // typically use a dark theme), so a bright
                                // grey behind it was exactly what made the
                                // one-frame cross-process resize gap (the
                                // native child HWND catching up to a
                                // SetWindowPos call a moment after this
                                // window's own bounds already changed — see
                                // timerCallback()'s own comment) read as a
                                // harsh flash instead of a barely-visible
                                // sliver. Matches CrateColors::DarkBackground,
                                // the same near-black the rest of the app's
                                // own chrome already uses.
                                CrateColors::DarkBackground, juce::DocumentWindow::closeButton),
          bridgePlugin (std::move (bridgePluginToKeep))
    {
        // Evaluated lazily at CLICK time, not construction time — watchedBridge
        // isn't set until watchBridgeForReconnect() runs, moments after this
        // constructor returns.
        contentContainer.crashButton.onClick = [this]
        {
            if (watchedBridge != nullptr)
                watchedBridge->triggerChildCrash();
        };

        // NOT a native title bar — confirmed empirically that a native
        // frame's resize border is handled directly by Windows (WM_SIZING
        // on the native frame) and bypasses BOTH setResizable(false) AND
        // setResizeLimits() entirely; the window stayed user-resizable
        // through two separate fix attempts. JUCE's OWN title bar routes
        // resize through resizableBorder/resizableCorner components that
        // ResizableWindow::setResizable() actually adds/removes — the
        // reliable mechanism.
        setUsingNativeTitleBar (false);

        // Title Bar Offset directive (Step 10.1 patch 2): setContentNonOwned's
        // SECOND argument is resizeToFitWhenContentChangesSize — passing
        // true here (was false) means: whenever hwndComponent's OWN size
        // changes (via setSize() in showSandboxEditorTestWindow(), once the
        // real editor dimensions are known), ResizableWindow recalculates
        // the OUTER window bounds itself (title bar + borders included) to
        // wrap that content size exactly. Manually computing the outer size
        // via setContentComponentSize() afterward was the source of the
        // grey bar — JUCE's own layout math is the reliable path, not a
        // hand-rolled equivalent of it.
        setContentNonOwned (&contentContainer, true);

        // Step 23 stutter/grey-box root cause: ResizableWindow::setResizable()
        // calls recreateDesktopWindow() whenever the window is already on
        // desktop — i.e. every call DESTROYS AND RECREATES the native OS
        // window. timerCallback() used to call setResizable(true,true) then
        // lockToCurrentSize()'s setResizable(false,false) on EVERY live-
        // resize tick (up to ~33/sec) — that's the low-fps stutter, and
        // almost certainly why the embedded child HWND (reparented into the
        // old, now-destroyed peer) intermittently detached, leaving the grey
        // gap. Turns out the unlock/relock was never needed:
        // childBoundsChanged() (what makes resizeToFitWhenContentChangesSize
        // work, see setContentNonOwned() below) calls plain Component::setSize()
        // directly — completely independent of the resizable flag. So this
        // window is never user-draggable (false, false), permanently, set
        // once here before ever reaching the desktop — and resize limits are
        // a wide-open range (not an exact clamp) so every future programmatic
        // content-driven resize just passes straight through unconstrained.
        setResizable (false, false);
        setResizeLimits (1, ContentContainer::crashButtonHeight + 1, 4000, 4000);
        centreWithSize (600, 500);
    }

    void closeButtonPressed() override
    {
        setVisible (false);
        stopTimer();

        // Step 15.4 (The Teardown Protocol) directive: closing this window
        // used to just drop the WINDOW's own reference to bridgePlugin —
        // the TRACK still held its own reference, so the plugin (and its
        // whole sandbox/tenant machinery) kept running invisibly forever,
        // never actually unloaded.
        //
        // notifyRemovalRequested() is called HERE, directly and
        // immediately — NOT left to fire only from deinitialise() — after
        // real testing proved deinitialise() unreliable to trigger
        // promptly in this codebase (an extra te::Plugin::Ptr reference,
        // traced but not fully resolved, kept the object alive well past
        // an explicit edit.restartPlayback() call). See
        // CrateSandboxBridge::notifyRemovalRequested()'s own doc comment
        // for the full account. Dispatching on clear user intent (closing
        // this window) rather than on uncertain object-destruction timing
        // is the more robust design regardless of what TE's own internals
        // eventually do with the C++ object itself.
        //
        // deleteFromParent() still runs afterward — matches the
        // ALREADY-ESTABLISHED pattern this codebase uses for the same
        // operation (see UniversalDeviceChainComponent.cpp's own
        // delete-plugin button): a clean undo transaction first, deferred
        // via MessageManager::callAsync() so tearing down the plugin
        // doesn't happen while still inside this own click handler's call
        // stack (the exact self-destruction-from-own-click-handler hazard
        // that comment describes). Captures the te::Plugin::Ptr itself,
        // not `this` — valid independent of whether this window survives
        // to the next message-loop iteration.
        if (bridgePlugin != nullptr)
        {
            if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
                bridge->notifyRemovalRequested();

            auto pluginToDelete = bridgePlugin;
            juce::MessageManager::callAsync ([pluginToDelete]
            {
                pluginToDelete->edit.getUndoManager().beginNewTransaction ("Delete Plugin: " + pluginToDelete->getName());
                pluginToDelete->deleteFromParent();
                pluginToDelete->edit.restartPlayback(); // nudge the graph rebuild along — see notifyRemovalRequested()'s own doc comment on why RAM reclaim no longer waits on this
            });
        }

        if (onClosed)
            onClosed(); // resets the owning MainComponent's unique_ptr — see showSandboxEditorTestWindow()
    }

    // Step 12.1 (The Violent Crash Test) directive: lets MainComponent's
    // hidden crash-test hotkey reach the bridge this window is currently
    // watching, without needing its own separate tracking of which bridge
    // is "current."
    CrateSandboxBridge* getWatchedBridge() const noexcept { return watchedBridge; }

    // The Resurrection Test directive (Step 11): after a crash+restart, the
    // NEW child process gets a NEW HWND — the old one died with the old
    // process. Periodically checks whether the bridge's currently-published
    // handle differs from what's embedded right now, and re-embeds if so.
    // This is what makes the Resurrection Test verifiable VISUALLY (the
    // knob snapping to its restored position in a freshly re-created
    // window), not just something to trust from the logs. Windows re-
    // publish automatically after a restart because windowHandleRequested
    // is preserved across CrateSandbox's own per-launch reset (see that
    // process's own initialise() comment) — this class never needs to call
    // requestEditorWindow() a second time.
    void watchBridgeForReconnect (CrateSandboxBridge* bridgeToWatch, void* initiallyEmbeddedHandle)
    {
        watchedBridge = bridgeToWatch;
        currentlyEmbeddedHandle = initiallyEmbeddedHandle;

        // Step 23 (Dynamic Resize) directive: was 300ms, fine for the
        // original post-crash-reconnect-only purpose (a rare event, no
        // perceptible lag acceptable). Now that this SAME timer also
        // follows a LIVE in-progress resize drag (see timerCallback()'s own
        // updated doc comment), 300ms would make the outer window visibly
        // stair-step/catch up rather than track the drag smoothly. 16ms
        // (~60fps, matching typical display refresh) is still a handful of
        // atomic reads and comparisons per tick — negligible cost — and
        // tracks a live drag noticeably smoother than the earlier 30ms
        // (~33fps) pass.
        startTimer (16);

        // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive: a
        // freshly (re)connected CHILD — including a brand new process after
        // a crash restart — always assumes 1.0 (100%) until told otherwise
        // (see ControlBlock::displayScale1000's own doc comment). Rather
        // than waiting for the NEXT monitor-change event to correct that,
        // push THIS window's current, real scale immediately, every time a
        // bridge connects/reconnects — covers both the initial launch (this
        // window might already be sitting on a non-100% monitor when the
        // editor first opens) and post-crash resurrection.
        if (auto* peer = getPeer())
            watchedBridge->publishDisplayScaleFactor ((float) peer->getPlatformScaleFactor());
    }

    ContentContainer contentContainer;
    std::function<void()> onClosed;

private:
    // Step 23 (Geometry Polish & Dynamic Resize) directive: this used to
    // early-return the moment the handle was unchanged, meaning a plugin
    // resizing ITS OWN editor (the same handle, just different dimensions
    // — e.g. a FabFilter-style corner drag) was silently ignored forever.
    // windowWidth/windowHeight are now kept live by the CHILD for as long
    // as its editor exists (see Main.cpp's own matching comment), so this
    // checks size on EVERY tick, independent of whether the handle itself
    // changed, and reacts to either kind of change — or both at once, on a
    // post-crash reconnect that also happens to load a differently-sized
    // plugin.
    void timerCallback() override
    {
        if (watchedBridge == nullptr)
            return;

        auto* newHandle = watchedBridge->getEditorWindowHandle();

        if (newHandle == nullptr)
            return; // no window yet

        const int w = watchedBridge->getEditorWindowWidth();
        const int h = watchedBridge->getEditorWindowHeight();

        if (w <= 0 || h <= 0)
            return;

        const bool handleChanged = (newHandle != currentlyEmbeddedHandle);
        const bool sizeChanged   = (w != lastAppliedWidth || h != lastAppliedHeight);

        if (! handleChanged && ! sizeChanged)
            return; // nothing to do — same child, same size

        currentlyEmbeddedHandle = newHandle;
        lastAppliedWidth  = w;
        lastAppliedHeight = h;

        if (handleChanged)
            contentContainer.hwndComponent.setHWND (newHandle);

        contentContainer.hwndComponent.setSize (w, h); // own native size only — position is fixed up by ContentContainer::resized()
        contentContainer.setSize (w, ContentContainer::crashButtonHeight + h);

        // Step 24 (Cross-Process Repaint Force) directive: JUCE's own
        // HWNDComponent::Pimpl::componentMovedOrResized() (juce_HWNDComponent_windows.cpp)
        // calls EnumChildWindows(hwnd, ...) to invalidate after every
        // resize — but that only reaches actual WIN32 CHILD windows of the
        // embedded HWND. A plain JUCE-rendered editor (ours included, no
        // native child controls of its own) has none, so that call
        // invalidates nothing at all; the embedded top-level window is left
        // to Windows' own default resize-repaint behaviour, which a fast,
        // many-ticks-in-one-second resize burst was observed to outrun
        // (real repro: 400->662px in ~1s left the CHILD's content
        // permanently blank, confirmed NOT a transient one-frame lag —
        // it stayed blank). Forcing the invalidate+repaint ourselves,
        // directly on the raw HWND, closes that gap regardless of why
        // JUCE's own built-in call wasn't enough here.
       #if JUCE_WINDOWS
        if (auto* embeddedHwnd = (HWND) contentContainer.hwndComponent.getHWND())
        {
            ::InvalidateRect (embeddedHwnd, nullptr, TRUE);
            ::UpdateWindow (embeddedHwnd);
        }
       #endif

        // Re-centering makes sense for a fresh post-crash window suddenly
        // appearing, but would feel broken for a live in-place resize — the
        // window should grow/shrink from wherever it currently sits, like
        // any ordinary resizable window, not jump to re-center under the
        // user's cursor mid-drag. setContentNonOwned's own
        // resizeToFitWhenContentChangesSize=true (set in the constructor)
        // already recalculates the outer bounds to fit contentContainer's
        // new size while leaving its position alone — exactly what a
        // size-only change needs, with no extra call here. No setResizable()
        // call here at all anymore — see the constructor's own comment:
        // childBoundsChanged() applies this via plain setSize(), completely
        // independent of the resizable flag, and toggling that flag on a
        // desktop window is what was destroying/recreating the native OS
        // window on every tick (the real cause of the stutter/grey-box bug).
        if (handleChanged)
            centreWithSize (getWidth(), getHeight());

        // Step 24 (Workspace Bounds Clamping) directive: a plugin scaling
        // itself up (e.g. a 150-200% DPI request) can grow this window
        // large enough that its bottom-right corner — where the resize
        // grip actually lives — ends up physically buried under the OS
        // taskbar, making it impossible to grab and shrink back down.
        // childBoundsChanged() (what the content-driven auto-resize above
        // runs through) calls plain setSize(), which — confirmed earlier
        // this same investigation — bypasses any attached
        // ComponentBoundsConstrainer entirely; a constrainer set via
        // setConstrainer()/setResizeLimits() only ever gets enforced
        // through setBoundsConstrained(), which nothing here calls. So
        // rather than relying on a constrainer that would silently never
        // fire, this clamps explicitly, every time a new size is applied:
        // never bigger than the CURRENT display's userArea (screen minus
        // taskbar/dock, exactly what Desktop::Displays already computes),
        // and repositioned so the whole window — corner included — stays
        // fully on-screen.
        {
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto* display = displays.getDisplayForRect (getScreenBounds());

            if (display == nullptr)
                display = displays.getPrimaryDisplay();

            const auto userArea = display->userBounds.toNearestInt();
            const auto bounds = getBounds();

            const int clampedWidth  = juce::jmin (bounds.getWidth(),  userArea.getWidth());
            const int clampedHeight = juce::jmin (bounds.getHeight(), userArea.getHeight());
            const int clampedX = juce::jlimit (userArea.getX(), userArea.getRight()  - clampedWidth,  bounds.getX());
            const int clampedY = juce::jlimit (userArea.getY(), userArea.getBottom() - clampedHeight, bounds.getY());

            if (clampedWidth != bounds.getWidth() || clampedHeight != bounds.getHeight()
                || clampedX != bounds.getX() || clampedY != bounds.getY())
            {
                setBounds (clampedX, clampedY, clampedWidth, clampedHeight);
            }
        }

    }

    tracktion::engine::Plugin::Ptr bridgePlugin;
    CrateSandboxBridge* watchedBridge = nullptr;
    void* currentlyEmbeddedHandle = nullptr;

    // Step 23 directive: tracked explicitly rather than re-reading
    // contentContainer.hwndComponent.getWidth()/getHeight() each tick —
    // avoids comparing against a value that could itself have drifted by a
    // pixel or two through JUCE/DPI-internal rounding after being applied,
    // which would falsely register as "changed" on every subsequent tick.
    int lastAppliedWidth  = 0;
    int lastAppliedHeight = 0;

    // Step 24 (DPI Awareness & Multi-Monitor Scaling) directive: THIS
    // window's peer is the real, on-screen top-level HWND — it genuinely
    // tracks whatever monitor the user drags it to, unlike the CHILD's own
    // (foreign-process-reparented) peer, which does not (see
    // ControlBlock::displayScale1000's own doc comment for the full
    // reasoning). juce::NativeScaleFactorNotifier is JUCE's own reusable
    // listener for exactly this — the SAME class VST3PluginWindow itself
    // uses internally to track ITS peer (see juce_VST3PluginFormat.cpp) —
    // reused here instead of hand-rolling an equivalent poll, since a
    // genuine monitor-DPI change is a rare, event-worthy occurrence, not a
    // continuous-drag one like size (Step 23's own timerCallback-based
    // follow logic stays polling-based deliberately; this doesn't need to
    // be). Declared last so it can safely capture `this` in its
    // constructor's callback — every member it might touch (watchedBridge)
    // already exists by this point in the initialization order.
    juce::NativeScaleFactorNotifier scaleFactorNotifier
    {
        this,
        [this] (float newScale)
        {
            if (watchedBridge != nullptr)
                watchedBridge->publishDisplayScaleFactor (newScale);
        }
    };

    SandboxEditorTestWindow (const SandboxEditorTestWindow&) = delete;
    SandboxEditorTestWindow& operator= (const SandboxEditorTestWindow&) = delete;
};

void MainComponent::triggerSandboxCrashTest()
{
    // Step 15.2 directive: the per-window FORCE CRASH button is the real,
    // proven mechanism (see ContentContainer's own crashButton.onClick) —
    // this hidden hotkey predates it and was already superseded before
    // multi-window support existed. Kept targeting the MOST RECENTLY
    // opened window only, for continuity with its original single-window
    // behavior; not meaningfully used now that every window has its own
    // button.
    if (auto* window = sandboxEditorTestWindows.getLast())
        if (auto* bridge = window->getWatchedBridge())
            bridge->triggerChildCrash();
}

void MainComponent::showSandboxEditorTestWindow()
{
    // Step 15.2 (The Shared Host Engine) directive: the single-instance
    // guard this used to have here is gone — the 3-Plugin Tenancy Test
    // needs multiple concurrent windows, one per tenant bridge (see
    // sandboxEditorTestWindows' own doc comment in MainComponent.h). Every
    // call now creates a genuinely new track + bridge + window.
    auto& edit = workflow->getEdit();

    auto track = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

    if (track == nullptr)
        return;

    // Step 14 (The Crate Brain) directive: this is now the ONLY door a
    // plugin load may come through — SandboxManager consults the Health
    // Registry (and logs its verdict) before dispensing a sandbox, rather
    // than this call going straight to the plugin cache as it did through
    // Step 13.
    auto bridgePlugin = SandboxManager::getInstance().createSandboxPlugin (edit, CrateSandboxBridge::getTestPluginPath());

    if (bridgePlugin == nullptr)
        return;

    // Inserting (not just constructing) is what fires te::Plugin::initialise()
    // through TE's normal graph-build lifecycle, which launches this bridge's
    // own CrateSandbox.exe — same mechanism every earlier sandboxing step's
    // own single-instance test already relies on.
    track->pluginList.insertPlugin (bridgePlugin, -1, nullptr);

    // Same weak_ptr self-reference-avoiding poller pattern CrateStressTest.cpp
    // already established (see its own comment for why) — first poll until
    // CONNECTED, then request the editor window and poll again until READY.
    auto pollForConnection = std::make_shared<std::function<void (int)>> ();
    std::weak_ptr<std::function<void (int)>> weakConnectionPoll = pollForConnection;

    *pollForConnection = [this, bridgePlugin, weakConnectionPoll] (int attemptsLeft)
    {
        auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get());

        if (bridge == nullptr)
            return;

        if (! bridge->isConnected())
        {
            if (attemptsLeft > 0)
                if (auto strongPoll = weakConnectionPoll.lock())
                    juce::Timer::callAfterDelay (250, [strongPoll, attemptsLeft] { (*strongPoll) (attemptsLeft - 1); });

            return;
        }

        bridge->requestEditorWindow();

        auto pollForWindow = std::make_shared<std::function<void (int)>> ();
        std::weak_ptr<std::function<void (int)>> weakWindowPoll = pollForWindow;

        *pollForWindow = [this, bridgePlugin, weakWindowPoll] (int windowAttemptsLeft)
        {
            auto* bridge2 = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get());

            if (bridge2 == nullptr)
                return;

            if (! bridge2->isEditorWindowReady())
            {
                if (windowAttemptsLeft > 0)
                    if (auto strongPoll = weakWindowPoll.lock())
                        juce::Timer::callAfterDelay (250, [strongPoll, windowAttemptsLeft] { (*strongPoll) (windowAttemptsLeft - 1); });

                return;
            }

            auto* hwnd = bridge2->getEditorWindowHandle();
            const int editorWidth  = bridge2->getEditorWindowWidth();
            const int editorHeight = bridge2->getEditorWindowHeight();

            if (hwnd == nullptr || editorWidth <= 0 || editorHeight <= 0)
                return;

            auto* window = sandboxEditorTestWindows.add (new SandboxEditorTestWindow (bridgePlugin));
            window->contentContainer.hwndComponent.setHWND (hwnd);

            // Title Bar Offset directive (Step 10.1 patch 2): resize ONLY
            // the content component, to the EXACT dimensions the CHILD
            // published (its own editor->getWidth()/getHeight(), not
            // resizeToFit()'s own after-the-fact GetWindowRect() query,
            // which could measure a stale/placeholder size from before
            // SetParent() settled). The constructor's setContentNonOwned(...,
            // true) is what makes THIS size change automatically resize the
            // WHOLE window's outer bounds (title bar + borders correctly
            // accounted for by JUCE's own layout math) — no manual
            // setContentComponentSize() call needed, and none of the
            // rounding/ordering issues that caused the grey bar. hwndComponent
            // keeps its own exact native size; ContentContainer's total size
            // adds the crash button's fixed height on top of it (see The
            // Unmissable Poison Pill directive on the class's own doc comment).
            window->contentContainer.hwndComponent.setSize (editorWidth, editorHeight);
            window->contentContainer.setSize (editorWidth, SandboxEditorTestWindow::ContentContainer::crashButtonHeight + editorHeight);

            // Step 15.2 directive: each new window is offset from the last
            // so multiple concurrent tenant windows don't stack exactly on
            // top of each other — cheap, deterministic cascading based on
            // how many windows are already open.
            const int cascadeOffset = 32 * (sandboxEditorTestWindows.size() - 1);
            window->centreWithSize (window->getWidth(), window->getHeight());
            window->setTopLeftPosition (window->getX() + cascadeOffset, window->getY() + cascadeOffset);

            window->onClosed = [this, window] { sandboxEditorTestWindows.removeObject (window); };
            window->setVisible (true);

            // Step 11 directive: start watching for a restart-driven
            // handle change now — see watchBridgeForReconnect()'s own
            // comment.
            window->watchBridgeForReconnect (bridge2, hwnd);
        };

        juce::Timer::callAfterDelay (250, [pollForWindow] { (*pollForWindow) (39); }); // ~10s worth of attempts
    };

    juce::Timer::callAfterDelay (250, [pollForConnection] { (*pollForConnection) (19); }); // ~5s worth of attempts
}

#endif // JUCE_DEBUG

void MainComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (arrangement == nullptr)
        return;

    if (arrangement->isVisible() && arrangement->getBounds().contains (x, y))
    {
        auto localPos = arrangement->getLocalPoint (this, juce::Point<int> (x, y));
        arrangement->processDroppedAudio (files, localPos.x, localPos.y);
    }
}
