#include "MainComponent.h"
#include "UI/CrateColors.h"

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
}

MainComponent::~MainComponent() = default;

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
