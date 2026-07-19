#include "TrackHeaderComponent.h"
#include "TheCrateLookAndFeel.h"
#include "TrackColourEditor.h"
#include "CrateColors.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Global Color Centralization & Purge: these were independent near-black/
    // grey literals (0xff252525/0xff3a3a3a) — now the exact same
    // CrateColors::DarkBackground/LightBackground pair MasterHeaderRow's
    // identical container styling uses (see ArrangementComponent.cpp's
    // "literal copy" comment), so the two headers stay pixel-identical.
    const auto headerDefault  = CrateColors::DarkBackground;
    const auto headerSelected = CrateColors::LightBackground;

    // Role colours (soloOnColour/recordOnColour) now live as TrackHeaderComponent's
    // own inline static members in the header, reading straight from CrateColors —
    // both this file and the default member initializers that construct
    // recordArmButton/soloButton read the exact same constants, so the glyph a
    // button is built with can never drift from the colour its click logic reasons
    // about here.

    // Design System Centralization: thin aliases into
    // CrateDesignSystem::Metrics::TrackHeader (see that namespace's own
    // layout for the full 300x90 geometry this class now reads from).
    namespace DS = CrateDesignSystem::Metrics::TrackHeader;

    constexpr float meterFloorDb = DS::meterFloorDb;
    constexpr float meterRangeDb = DS::meterRangeDb; // floor to +6 dB headroom

    // Expanded-state geometry is now EXACT HARDCODED juce::Rectangle constants
    // in layoutExpanded() itself (Lead Architect directive — dynamic flexbox
    // math produced a scattered layout), so col1Width/col2Width/colGap no
    // longer exist as separate constants. These two remain: the collapsed
    // micro-state (layoutCollapsed(), untouched by this directive) still uses
    // its own dynamic math and needs them.
    constexpr int colourStripW   = DS::colourStripW;    // 3px vertical track-colour strip (collapsed state only)
    constexpr int foldArrowW     = DS::foldArrowCollapsedW;   // standard crisp size — same FoldArrow as the expanded header, kept in step
    constexpr int meterStripW    = DS::meterStripW;    // slim vertical LED meter (collapsed state only — see
                                         // layoutExpanded()'s own doc comment for why the expanded
                                         // state's meter is temporarily not painted)

    // Persisted as a plain property on the track's own ValueTree — round-trips
    // through the same .crate save/load path as every other track property
    // (MASTER_ARCHITECTURE.md invariant 3: "Everything persists"), no separate
    // serialization step needed.
    const juce::Identifier armedPropertyID ("crateRecordArmed");

    const juce::String pluginDragPrefix = "plugin_drag|";
}

void TrackHeaderComponent::ToggleBlock::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Flat solid-filled square — no rounding, no gradient/bevel, no default JUCE
    // TextButton chrome — this IS the whole "premium Ableton-style toggle block"
    // look; there is nothing else drawn on top except the single glyph letter.
    // Contrast fix: the off-state used to be plain LightBackground — identical
    // to the track header's own selected-row fill, so an off button's bounding
    // box was invisible against it ("blob" complaint). Brightened so every
    // button always reads as its own distinct box.
    auto bg = getToggleState() ? onColour : CrateColors::LightBackground.brighter (DS::offStateBrighten);

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (DS::pressedBrighten);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (DS::hoverBrighten);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    g.setColour (getToggleState() ? juce::Colours::black : LAF::textDim);
    g.setFont (juce::FontOptions (fontSize, juce::Font::bold));
    g.drawText (glyph, getLocalBounds(), juce::Justification::centred);
}

void TrackHeaderComponent::MutePlate::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Toggle state == "is muted". Lit CrateColors::NeonBlue when AUDIBLE (the
    // common resting state reads as "signal alive"); dim DarkBackground when
    // muted, so a muted track visibly recedes. The track number stays legible
    // in both states (dark text on the neon plate, grey text on the dim one).
    const bool muted = getToggleState();
    auto bg = muted ? CrateColors::DarkBackground : CrateColors::NeonBlue;

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (DS::pressedBrighten);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (DS::muteHoverBrighten);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    g.setColour (muted ? CrateColors::BrandGray : juce::Colours::black);
    g.setFont (juce::FontOptions (CrateDesignSystem::Typography::mutePlateFontSize, juce::Font::bold));
    g.drawText (number, getLocalBounds(), juce::Justification::centred);
}

void TrackHeaderComponent::FoldArrow::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool)
{
    const bool expanded = (isExpanded != nullptr) && isExpanded();
    auto b = getLocalBounds().toFloat().reduced (DS::foldArrowInset);

    juce::Path tri;
    if (expanded)
    {
        // Down-pointing (disclosure open).
        tri.addTriangle (b.getX(), b.getY(), b.getRight(), b.getY(), b.getCentreX(), b.getBottom());
    }
    else
    {
        // Right-pointing (disclosure closed).
        tri.addTriangle (b.getX(), b.getY(), b.getRight(), b.getCentreY(), b.getX(), b.getBottom());
    }

    g.setColour (shouldDrawButtonAsHighlighted ? CrateColors::NeonBlue : CrateColors::BrandGray);
    g.fillPath (tri);
}

void TrackHeaderComponent::MonitorButton::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Flat box, no JUCE chrome — same language as ToggleBlock/MutePlate.
    // Active state comes from the OWNER's monitorMode via isActive(), not a
    // per-button toggle state (all three buttons share one group state).
    // Contrast fix — see ToggleBlock::paintButton's identical comment.
    const bool active = (isActive != nullptr) && isActive();
    auto bg = active ? CrateColors::SoloYellow : CrateColors::LightBackground.brighter (DS::offStateBrighten);

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (DS::pressedBrighten);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (DS::hoverBrighten);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    g.setColour (active ? juce::Colours::black : CrateColors::BrandGray);
    g.setFont (juce::FontOptions (CrateDesignSystem::Typography::monitorLabelFontSize, juce::Font::bold));
    g.drawText (label, getLocalBounds(), juce::Justification::centred);
}

void TrackHeaderComponent::VolumeBar::setValue (double newValue, juce::NotificationType nt)
{
    const auto clamped = juce::jlimit (rangeMin, rangeMax, newValue);

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (nt == juce::sendNotification && onValueChange)
        onValueChange();
}

void TrackHeaderComponent::VolumeBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    constexpr float corner = DS::volumeBarCornerRadius;

    // Flat dark well.
    g.setColour (CrateColors::DarkBackground);
    g.fillRoundedRectangle (b, corner);

    // NeonBlue fill growing left -> right with the value — plain linear
    // proportion (no juce::Slider skew to account for; this class has none).
    const auto prop = (rangeMax > rangeMin)
                         ? juce::jlimit (0.0, 1.0, (value - rangeMin) / (rangeMax - rangeMin))
                         : 0.0;
    if (prop > 0.0)
    {
        juce::Graphics::ScopedSaveState clip (g);
        juce::Path well;
        well.addRoundedRectangle (b, corner);
        g.reduceClipRegion (well);

        auto fill = b.withWidth (b.getWidth() * (float) prop);
        g.setColour (CrateColors::NeonBlue.withAlpha (DS::volumeBarFillAlpha));
        g.fillRect (fill);
    }

    // Centred dB readout — white over the fill, still legible over the dark well.
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (CrateDesignSystem::Typography::volumeBarFontSize, juce::Font::bold));
    g.drawText (juce::String (value, 1) + " dB", getLocalBounds(), juce::Justification::centred);
}

void TrackHeaderComponent::VolumeBar::mouseDown (const juce::MouseEvent&)
{
    valueOnDragStart = value;

    if (onDragStart)
        onDragStart();
}

void TrackHeaderComponent::VolumeBar::mouseDrag (const juce::MouseEvent& e)
{
    // Horizontal relative drag (matches the fill bar's own left->right visual
    // metaphor): drag the full width of the box to sweep the full range.
    // getDistanceFromDragStartX() is JUCE's OWN pixel-delta-since-mouseDown
    // helper (juce_MouseEvent.h) — using it instead of manually tracking a
    // start-X member removes any chance of a hand-rolled delta-tracking bug.
    constexpr float pixelsForFullRange = DS::volumeBarDragPixelsForFullRange;
    const double delta = (double) e.getDistanceFromDragStartX() / (double) pixelsForFullRange * (rangeMax - rangeMin);
    setValue (valueOnDragStart + delta, juce::sendNotification);
}

void TrackHeaderComponent::VolumeBar::mouseUp (const juce::MouseEvent&)
{
    if (onDragEnd)
        onDragEnd();
}

void TrackHeaderComponent::VolumeBar::mouseDoubleClick (const juce::MouseEvent&)
{
    setValue (0.0, juce::sendNotification); // reset to unity gain (0 dB)
}

TrackHeaderComponent::TrackHeaderComponent (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse)
    : track (trackToControl), workflow (workflowToUse)
{
    if (track != nullptr)
    {
        isReturnTrackFlag = TrackUtils::isReturnTrack (*track);

        volumePlugin = track->getVolumePlugin();

        // Reuse the track's single shared level meter (inserted by whichever of
        // MixerStrip / TrackHeaderComponent gets constructed first for this track)
        // rather than creating a second one — same pattern MixerStrip itself uses.
        meterPlugin = track->pluginList.findFirstPluginOfType<te::LevelMeterPlugin>();

        if (meterPlugin == nullptr)
        {
            auto plugin = track->edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, juce::PluginDescription());
            track->pluginList.insertPlugin (plugin, -1, nullptr);
            meterPlugin = dynamic_cast<te::LevelMeterPlugin*> (plugin.get());
        }

        if (meterPlugin != nullptr)
            meterPlugin->measurer.addClient (meterClient);
    }

    // Column 1: fold/collapse arrow (Cubase/Ableton disclosure). Reads the
    // header's own isCollapsed as its single source of truth; clicking toggles
    // the fold micro-state and bridges the height change out to the lane row.
    addAndMakeVisible (foldArrow);
    foldArrow.isExpanded = [this] { return ! isCollapsed; };
    foldArrow.onClick = [this] { toggleFold(); };

    // Column 3: Mute Plate — the track-number plate IS the Mute toggle. Number
    // read once from the engine's own 1-based ordering (getAudioTrackNumber());
    // every add/delete rebuilds every header from scratch, so renumbering falls
    // out for free. Toggle state == isMuted (true polarity, no inversion).
    addAndMakeVisible (mutePlate);
    mutePlate.setNumberText (track != nullptr ? juce::String (track->getAudioTrackNumber()) : juce::String ("-"));
    mutePlate.setClickingTogglesState (true);
    mutePlate.setToggleState (track != nullptr && track->isMuted (false), juce::dontSendNotification);
    mutePlate.onClick = [this]
    {
        if (onSelect) onSelect();
        if (track != nullptr) track->setMute (mutePlate.getToggleState());
    };

    addAndMakeVisible (nameLabel);
    nameLabel.setText (track != nullptr ? track->getName() : juce::String ("—"), juce::dontSendNotification);
    nameLabel.setFont (juce::FontOptions (CrateDesignSystem::Typography::headerNameFontSize, juce::Font::bold));
    nameLabel.setJustificationType (juce::Justification::centredLeft); // Column 1: left-aligned, bold
    refreshNameLabelContrast(); // sets textColourId — Column 1 is a solid track-colour fill now, not always dark
    nameLabel.setEditable (false, true, false); // double-click to rename
    nameLabel.onTextChange = [this]
    {
        if (track != nullptr)
            track->setName (nameLabel.getText());
    };

    // Column 2: Two-Tier Ableton-style I/O (Hybrid Bus/Return Architecture
    // directive). Flat + dark — strip the V4 combo's outline (transparent)
    // and bevel; fill with DarkBackground, text BrandGray, arrow NeonBlue.
    // Display-only: no input-device enumeration or output-bus routing exists
    // in this engine (grepped — no AuxReturnPlugin, no input-device-category
    // model anywhere), so none of these carry an onChange that mutates real
    // routing. The ONE exception is outputSpecificCombo's "Master" item,
    // which still shows the track's real destination name, same as the
    // single output combo this replaces used to.
    const auto styleFlatCombo = [] (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, CrateColors::DarkBackground);
        c.setColour (juce::ComboBox::outlineColourId,    juce::Colours::transparentBlack);
        c.setColour (juce::ComboBox::textColourId,       CrateColors::BrandGray);
        c.setColour (juce::ComboBox::arrowColourId,      CrateColors::NeonBlue);
        c.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colours::transparentBlack);
        c.setJustificationType (juce::Justification::centredLeft);
    };

    // ---- Input Category / Specific ----
    // "No Input" Logic (Ableton Accuracy): "No Input" is the default, first
    // option — it's the clean, no-source resting state real Ableton tracks
    // start in. Ext. In / Resampling are shifted down to ids 2/3 to make room.
    addAndMakeVisible (inputCategoryCombo);
    styleFlatCombo (inputCategoryCombo);
    inputCategoryCombo.addItem ("No Input", 1);
    inputCategoryCombo.addItem ("Ext. In", 2);
    inputCategoryCombo.addItem ("Resampling", 3);
    inputCategoryCombo.setSelectedId (1, juce::dontSendNotification);
    inputCategoryCombo.setTooltip ("Input routing isn't wired to the engine yet - display only.");

    addAndMakeVisible (inputSpecificCombo);
    styleFlatCombo (inputSpecificCombo);
    inputSpecificCombo.setTooltip ("Input routing isn't wired to the engine yet - display only.");

    // Cosmetic only (no real device enumeration behind either list) — but a
    // real category change SHOULD offer different specific channels, so this
    // stays wired rather than static, matching "intelligent" menu behaviour
    // without claiming a backend that doesn't exist.
    const auto refreshInputSpecificItems = [this]
    {
        inputSpecificCombo.clear (juce::dontSendNotification);

        constexpr int noInputCategoryId = 1;
        constexpr int resamplingCategoryId = 3;
        const auto categoryId = inputCategoryCombo.getSelectedId();

        if (categoryId == noInputCategoryId)
        {
            // "No Input" Logic: nothing to pick — updateInputSpecificVisibility()
            // hides the combo entirely rather than showing an empty dropdown.
        }
        else if (categoryId == resamplingCategoryId)
        {
            inputSpecificCombo.addItem ("Stereo Out", 1);
            inputSpecificCombo.setSelectedId (1, juce::dontSendNotification);
        }
        else // Ext. In
        {
            inputSpecificCombo.addItem ("1/2", 1);
            inputSpecificCombo.addItem ("1", 2);
            inputSpecificCombo.addItem ("2", 3);
            inputSpecificCombo.setSelectedId (1, juce::dontSendNotification);
        }

        updateInputSpecificVisibility();
    };
    refreshInputSpecificItems();
    inputCategoryCombo.onChange = refreshInputSpecificItems;

    // ---- Monitoring (IN / AUTO / OFF) ----
    // AUTO is the default (Ableton convention: auto-monitor on record arm).
    // Purely local UI state — no hardware zero-latency monitoring path exists
    // in this engine, so clicking these does not touch any engine object;
    // see monitorMode's own doc comment on the header.
    monitorInButton.isActive   = [this] { return monitorMode == MonitorMode::in; };
    monitorAutoButton.isActive = [this] { return monitorMode == MonitorMode::autoMode; };
    monitorOffButton.isActive  = [this] { return monitorMode == MonitorMode::off; };
    monitorInButton.onClick   = [this] { if (onSelect) onSelect(); setMonitorMode (MonitorMode::in); };
    monitorAutoButton.onClick = [this] { if (onSelect) onSelect(); setMonitorMode (MonitorMode::autoMode); };
    monitorOffButton.onClick  = [this] { if (onSelect) onSelect(); setMonitorMode (MonitorMode::off); };
    monitorInButton.setTooltip ("Input Monitoring: Always On");
    monitorAutoButton.setTooltip ("Input Monitoring: Auto (on Record Arm)");
    monitorOffButton.setTooltip ("Input Monitoring: Off (hardware zero-latency monitoring)");
    addAndMakeVisible (monitorInButton);
    addAndMakeVisible (monitorAutoButton);
    addAndMakeVisible (monitorOffButton);

    // ---- Output Category / Specific ----
    addAndMakeVisible (outputCategoryCombo);
    styleFlatCombo (outputCategoryCombo);
    outputCategoryCombo.addItem ("Master", 1);
    outputCategoryCombo.addItem ("Ext. Out", 2);
    outputCategoryCombo.addItem ("Sends Only", 3);
    outputCategoryCombo.setSelectedId (1, juce::dontSendNotification);
    outputCategoryCombo.setTooltip ("Output reflects the track's real destination when set to Master.");

    addAndMakeVisible (outputSpecificCombo);
    styleFlatCombo (outputSpecificCombo);
    outputSpecificCombo.setTooltip ("Output reflects the track's real destination when set to Master.");

    const auto refreshOutputSpecificItems = [this]
    {
        outputSpecificCombo.clear (juce::dontSendNotification);

        switch (outputCategoryCombo.getSelectedId())
        {
            case 1: // Master — the ONE real value in this whole two-tier system
                outputSpecificCombo.addItem (track != nullptr ? track->getOutput().getDescriptiveOutputName()
                                                                : juce::String ("Master"), 1);
                break;
            case 2: // Ext. Out — no real hardware-output enumeration exists yet
                outputSpecificCombo.addItem ("Out 1/2", 1);
                break;
            case 3: // Sends Only — no output destination at all by definition
                outputSpecificCombo.addItem ("-", 1);
                break;
            default:
                break;
        }

        outputSpecificCombo.setSelectedId (1, juce::dontSendNotification);
        updateOutputSpecificVisibility();
    };
    refreshOutputSpecificItems();
    outputCategoryCombo.onChange = refreshOutputSpecificItems;

    recordArmButton.setClickingTogglesState (true);
    soloButton.setClickingTogglesState (true);

    addAndMakeVisible (recordArmButton);

    if (isReturnTrackFlag)
    {
        // Return Track Button directive: return tracks don't record — this
        // slot becomes a Pre/Post toggle instead (Pre/Post fader tap point
        // for the return's own signal), purely cosmetic local UI state (no
        // engine concept exists for it, same disclosed-placeholder category
        // as the I/O combos/monitor triad). POST is the default, matching
        // real-console convention (most sends are post-fader).
        recordArmButton.setGlyph ("POST");
        recordArmButton.setFontSize (CrateDesignSystem::Typography::togglePrePostFontSize); // "POST"/"PRE" don't fit at the single-letter "R"/"S" size
        recordArmButton.setToggleState (true, juce::dontSendNotification);
        recordArmButton.onClick = [this]
        {
            if (onSelect) onSelect();

            const bool isPost = recordArmButton.getToggleState();
            recordArmButton.setGlyph (isPost ? "POST" : "PRE");
        };
    }
    else
    {
        recordArmButton.setToggleState (track != nullptr && (bool) track->state.getProperty (armedPropertyID, false), juce::dontSendNotification);
        recordArmButton.onClick = [this]
        {
            if (onSelect) onSelect();

            if (track == nullptr)
                return;

            const bool armed = recordArmButton.getToggleState();
            track->state.setProperty (armedPropertyID, armed, &track->edit.getUndoManager());

            // Best-effort: only actually arms something if an input device is already
            // routed to this track. No-ops safely otherwise — full input-routing UI is
            // a later phase (MASTER_ARCHITECTURE.md roadmap), this just makes the arm
            // toggle real wherever a route already exists rather than purely cosmetic.
            for (auto* inputInstance : track->edit.getEditInputDevices().getDevicesForTargetTrack (*track))
                inputInstance->setRecordingEnabled (track->itemID, armed);
        };
    }

    addAndMakeVisible (soloButton);
    soloButton.setToggleState (track != nullptr && track->isSolo (false), juce::dontSendNotification);
    soloButton.onClick = [this]
    {
        if (onSelect) onSelect();
        if (track != nullptr) track->setSolo (soloButton.getToggleState());
    };

    addAndMakeVisible (volumeSlider);
    volumeSlider.setRange (-60.0, 6.0);
    volumeSlider.onDragStart = [this] { if (onSelect) onSelect(); };

    // Column 3: the tactile Pan knob — a rotary juce::Slider rendered by
    // CrateMixerLookAndFeel's pan_knob.png filmstrip (+ its touch-gated neon
    // glow), the SAME premium bipolar console feel as the Mixer's own pan knob.
    // Bound strictly via te::AutomatableParameter (panParam) — no
    // AudioProcessorParameterAttachment (wrong framework for Tracktion Engine).
    addAndMakeVisible (panKnob);
    panKnob.setLookAndFeel (&mixerLookAndFeel);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                  juce::MathConstants<float>::pi * 2.8f, true);

    if (volumePlugin != nullptr)
    {
        volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
        volumeSlider.onValueChange = [this]
        {
            if (volumePlugin != nullptr)
                volumePlugin->setVolumeDb ((float) volumeSlider.getValue());
        };

        panKnob.setValue (volumePlugin->getPan(), juce::dontSendNotification);
        // Pan-knob lifecycle mirrors MixerStrip's: an explicit undo transaction +
        // parameterChangeGesture pair around the drag, so a pan tweak is one
        // undoable step and TE's automation sees a proper gesture, not a stream
        // of unrelated value writes.
        panKnob.onDragStart = [this]
        {
            if (onSelect) onSelect();
            if (volumePlugin == nullptr) return;
            volumePlugin->panParam->getEdit().getUndoManager().beginNewTransaction (
                "Tweak Pan: " + (track != nullptr ? track->getName() : juce::String()));
            volumePlugin->panParam->parameterChangeGestureBegin();
        };
        panKnob.onDragEnd = [this]
        {
            if (volumePlugin != nullptr) volumePlugin->panParam->parameterChangeGestureEnd();
        };
        panKnob.onValueChange = [this]
        {
            if (volumePlugin != nullptr)
                volumePlugin->setPan ((float) panKnob.getValue());
        };

        // Bidirectional sync with MixerStrip: fires when Volume/Pan change from
        // that strip's fader/pan knob (or automation, or a script), not just from
        // this header's own controls. currentValueChanged() refreshes BOTH the
        // volume slider and the pan knob now that the header owns a real pan control.
        volumePlugin->volParam->addListener (this);
        volumePlugin->panParam->addListener (this);
    }

    // Mute/Solo/Armed aren't AutomatableParameters — plain CachedValue<bool> /
    // ValueTree properties on the track's own state — so they need a
    // juce::ValueTree::Listener instead of te::AutomatableParameter::Listener to
    // catch changes made from MixerStrip or from Undo/Redo.
    if (track != nullptr)
        track->state.addListener (this);

    // Purge Clutter: Delete/Backspace now deletes this track (no more 'x'
    // button) — needs keyboard focus to ever receive the key, grabbed on
    // click/select in mouseDown() below, same pattern
    // UniversalDeviceChainComponent's MiniParamSlider already established.
    setWantsKeyboardFocus (true);

    startTimerHz (24); // live meter poll only — see timerCallback()
}

TrackHeaderComponent::~TrackHeaderComponent()
{
    stopTimer();

    // Detach the pan knob's LookAndFeel BEFORE mixerLookAndFeel is destroyed —
    // a live Slider pointing at a freed LookAndFeel would dangle. Belt-and-braces
    // on top of the declaration order (mixerLookAndFeel declared before panKnob).
    panKnob.setLookAndFeel (nullptr);

    if (volumePlugin != nullptr)
    {
        volumePlugin->volParam->removeListener (this);
        volumePlugin->panParam->removeListener (this);
    }

    if (track != nullptr)
        track->state.removeListener (this);

    if (meterPlugin != nullptr)
        meterPlugin->measurer.removeClient (meterClient);
}

void TrackHeaderComponent::currentValueChanged (te::AutomatableParameter&)
{
    // TE itself asserts this fires message-thread-only (verified against
    // tracktion_AutomatableParameter.cpp during the Phase 1 audit), so this
    // callAsync isn't working around an audio-thread hazard — it's here per the
    // Lead Architect's explicit instruction, and it buys real safety of a
    // different kind: SafePointer means that IF a track deletion (or any other
    // synchronous teardown) destroys this row between the listener firing and the
    // deferred lambda running, the lambda no-ops instead of touching freed memory.
    // Refreshes BOTH volume and pan — the single listener fires for either param,
    // and re-reading both is cheaper than tracking which one changed.
    juce::Component::SafePointer<TrackHeaderComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
        {
            safeThis->refreshVolumeFromEngine();
            safeThis->refreshPanFromEngine();
        }
    });
}

void TrackHeaderComponent::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& property)
{
    if (track == nullptr || v != track->state)
        return;

    juce::Component::SafePointer<TrackHeaderComponent> safeThis (this);

    if (property == te::IDs::mute || property == te::IDs::solo || property == armedPropertyID)
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->refreshToggleStatesFromEngine();
        });
        return;
    }

    // Name / colour changed from the Mixer (or Undo/Redo) — reflect it here so
    // the two views stay in lockstep. Both mutate the SAME track ValueTree and
    // both listen to it, so this is fully bidirectional.
    if (property == te::IDs::name || property == te::IDs::colour)
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis == nullptr || safeThis->track == nullptr)
                return;

            safeThis->nameLabel.setText (safeThis->track->getName(), juce::dontSendNotification);
            safeThis->refreshNameLabelContrast(); // Column 1's fill colour may have just changed
            safeThis->repaint(); // Column 1's solid fill is painted from track->getColour()
        });
    }
}

void TrackHeaderComponent::refreshVolumeFromEngine()
{
    if (volumePlugin == nullptr)
        return;

    volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
}

void TrackHeaderComponent::refreshPanFromEngine()
{
    if (volumePlugin == nullptr)
        return;

    panKnob.setValue (volumePlugin->getPan(), juce::dontSendNotification);
}

void TrackHeaderComponent::refreshToggleStatesFromEngine()
{
    if (track == nullptr)
        return;

    // The Mute Plate is the mute toggle now — its toggle state IS isMuted.
    mutePlate.setToggleState (track->isMuted (false), juce::dontSendNotification);
    soloButton.setToggleState (track->isSolo (false), juce::dontSendNotification);

    // Return Track Button directive: recordArmButton is a purely local
    // Pre/Post toggle for a return track (no armedPropertyID concept applies
    // to it at all) — syncing it from armedPropertyID here would silently
    // stomp the user's Pre/Post choice back to PRE every time THIS function
    // runs for an unrelated reason (e.g. Mute/Solo changed elsewhere).
    if (! isReturnTrackFlag)
        recordArmButton.setToggleState ((bool) track->state.getProperty (armedPropertyID, false), juce::dontSendNotification);
}

void TrackHeaderComponent::refreshNameLabelContrast()
{
    const auto bg = (track != nullptr && ! track->getColour().isTransparent())
                        ? track->getColour()
                        : headerDefault;

    // Standard relative-luminance formula (ITU-R BT.601 weights) — perceptual
    // brightness, not JUCE's HSB getBrightness() (which is just max(R,G,B) and
    // reads saturated colours as "bright" even when they're dark for contrast
    // purposes, e.g. a pure deep blue). Threshold at 0.5.
    const float luminance = 0.299f * bg.getFloatRed() + 0.587f * bg.getFloatGreen() + 0.114f * bg.getFloatBlue();
    nameLabel.setColour (juce::Label::textColourId, luminance > 0.5f ? juce::Colours::black : juce::Colours::white);
}

void TrackHeaderComponent::timerCallback()
{
    // getAndClearAudioLevel() drains a lock-free value the audio thread only ever
    // writes to (LevelMeasurer::Client) — this timer is what turns that into a
    // repaint on the message thread; the audio thread never touches this
    // Component or its UI state directly.
    if (meterPlugin == nullptr)
        return;

    const auto levelL = meterClient.getAndClearAudioLevel (0);
    const auto levelR = meterClient.getAndClearAudioLevel (1);
    meterLevelDb = juce::jmax (levelL.dB, levelR.dB);
    repaint (meterBounds);
}

void TrackHeaderComponent::setSelected (bool shouldBeSelected)
{
    if (selected != shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }
}

void TrackHeaderComponent::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus(); // click-to-select-and-focus, so Delete/Backspace reaches keyPressed() below

    if (onSelect)
        onSelect();

    if (e.mods.isPopupMenu())
        showNameColourEditor();
}

bool TrackHeaderComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        // Deferred: onDeleteRequested ends in a full track-list rebuild that
        // destroys this row (and this very keyPressed() call still on the
        // stack). Running that synchronously would free it mid-callback —
        // defer to the next message-loop iteration so this call stack unwinds
        // first, same discipline the old delete BUTTON's onClick used.
        if (onDeleteRequested)
        {
            auto callback = onDeleteRequested;
            juce::MessageManager::callAsync (callback);
        }

        return true;
    }

    return false;
}

void TrackHeaderComponent::showNameColourEditor()
{
    if (track == nullptr)
        return;

    juce::Component::SafePointer<TrackHeaderComponent> safeThis (this);

    CrateTrackEditor::showNameColourMenu (
        this,
        nameLabel.getScreenBounds(),
        track->getName(),
        track->getColour(),
        [safeThis] (juce::String newName)
        {
            if (safeThis != nullptr && safeThis->track != nullptr)
            {
                safeThis->track->edit.getUndoManager().beginNewTransaction ("Rename Track");
                safeThis->track->setName (newName);
            }
        },
        [safeThis]
        {
            if (safeThis != nullptr && safeThis->track != nullptr)
                safeThis->track->edit.getUndoManager().beginNewTransaction ("Set Track Colour");
        },
        [safeThis] (juce::Colour c)
        {
            if (safeThis != nullptr && safeThis->track != nullptr)
                safeThis->track->setColour (c);
        });
}

void TrackHeaderComponent::paint (juce::Graphics& g)
{
    // Base fill for Columns 2 + 3 (Column 1 gets its own solid track-colour
    // fill below, drawn over whatever's here — this base never shows through
    // Column 1 once that's painted).
    g.fillAll (selected ? headerSelected : headerDefault);

    // Column 1 (Identity): the ENTIRE column is filled SOLID with the track's
    // colour — exactly like Ableton, not a translucent tint over the whole
    // header. Falls back to the neutral header fill's own colour when no
    // track colour is set (te::Track's default is transparent), so an
    // uncoloured track's Column 1 just reads as part of the same flat panel.
    if (! column1Bounds.isEmpty())
    {
        // Return & Master Track Colors directive: a return track never shows
        // its own (possibly randomly-assigned) te::Track colour in Column 1 —
        // forced to a fixed neutral grey so it reads as visually distinct
        // from a regular Audio/MIDI track, same as Master's own row.
        const auto fillColour = isReturnTrackFlag
                                    ? CrateColors::BrandGray
                                    : (track != nullptr && ! track->getColour().isTransparent())
                                        ? track->getColour()
                                        : (selected ? headerSelected : headerDefault);
        g.setColour (fillColour);
        g.fillRect (column1Bounds);
    }

    // Left accent stripe when selected — quick visual anchor for the active track.
    if (selected)
    {
        g.setColour (LAF::accent);
        g.fillRect (juce::Rectangle<int> (0, 0, DS::accentStripeW, getHeight()));
    }

    // Ableton-style column separators — EXACT hardcoded 1px fills at the fixed
    // Column 1/2 (x=90) and Column 2/3 (x=180) boundaries of the 300x90
    // expanded layout (Lead Architect directive — no dynamic position
    // tracking any more, these ARE the boundaries). Collapsed has no columns
    // to separate.
    if (! isCollapsed)
    {
        g.setColour (CrateColors::DarkBackground.darker (0.6f));
        g.fillRect (DS::column1Right, 0, DS::separatorW, DS::headerHeight);
        g.fillRect (DS::column2Right, 0, DS::separatorW, DS::headerHeight);
    }

    // Column 3 far-right edge: slim VERTICAL LED meter — flat fill, fills bottom
    // -> up with level, hot red near clipping (matching the Mixer's meter language).
    if (! meterBounds.isEmpty())
    {
        g.setColour (LAF::lcdBackground);
        g.fillRect (meterBounds);

        const auto normalised = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
        auto fill = meterBounds.toFloat();
        const auto lit = fill.getHeight() * normalised;
        fill.removeFromTop (fill.getHeight() - lit); // grow from the bottom edge upward

        g.setColour (meterLevelDb > -3.0f ? LAF::meterHot : LAF::accent);
        g.fillRect (fill);
    }

    // "Giant Blob" contrast fix: LAF::background (== CrateColors::DarkBackground)
    // was IDENTICAL to this row's own unselected background fill, so the old
    // drawHorizontalLine() border vanished against it whenever the track
    // wasn't selected — exactly the "tracks bleed into each other" symptom.
    // A literal near-black fillRect guarantees contrast regardless of
    // selection or track-colour state. The right-edge separator (timeline
    // boundary, not row-to-row separation) is unaffected by that complaint,
    // so it keeps its original colour/line.
    g.setColour (juce::Colours::black);
    g.fillRect (0, getHeight() - DS::rowBottomBorderH, getWidth(), DS::rowBottomBorderH);

    g.setColour (LAF::background);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    // Premium drop-target glow for a Browser plugin drag — drawn last (on top of
    // everything above) so it reads clearly regardless of selection/meter state.
    if (isDragHovering)
    {
        g.setColour (LAF::accent.withAlpha (0.12f));
        g.fillRect (getLocalBounds());
        g.setColour (LAF::accent);
        g.drawRect (getLocalBounds(), 2);
    }
}

bool TrackHeaderComponent::isInterestedInDragSource (const SourceDetails& details)
{
    return track != nullptr && details.description.toString().startsWith (pluginDragPrefix);
}

void TrackHeaderComponent::itemDragEnter (const SourceDetails&)
{
    isDragHovering = true;
    repaint();
}

void TrackHeaderComponent::itemDragExit (const SourceDetails&)
{
    isDragHovering = false;
    repaint();
}

void TrackHeaderComponent::itemDropped (const SourceDetails& details)
{
    isDragHovering = false;
    repaint();

    if (track == nullptr)
        return;

    const auto identifier = details.description.toString().fromFirstOccurrenceOf (pluginDragPrefix, false, false);

    if (identifier.isEmpty())
        return;

    // track->edit.engine (not a workflow.getEngine() call) since this component
    // doesn't otherwise need engine-level access — the track itself already
    // knows its own Edit/Engine, same pattern MixerStrip's ChannelStripRack uses.
    if (auto desc = track->edit.engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
        workflow.loadPluginOntoTrack (*desc, *track, -1);
}

void TrackHeaderComponent::toggleFold()
{
    isCollapsed = ! isCollapsed;

    // Snappy local feedback first (re-lay this header for the new micro-state),
    // then bridge the height change out so the lane row matches — that round-trip
    // (relayoutFromHeights) re-sets this header's bounds and calls resized() again
    // with the correct final height, but doing it locally too avoids a one-frame
    // stale layout in the interim.
    resized();
    repaint();

    if (onFoldToggle)
        onFoldToggle();
}

void TrackHeaderComponent::applyCollapsedVisibility()
{
    // Column 2 (Routing, now 5 rows) + the pan knob + volume drag-box are the
    // controls that only exist in the expanded state.
    const bool showInputControls = ! isCollapsed && ! isReturnTrackFlag;

    // Hybrid Bus/Return Architecture (Ableton Accuracy): return tracks don't
    // record from external inputs — Input Category/Specific and the
    // IN/AUTO/OFF monitor triad never show for one, expanded or not.
    inputCategoryCombo.setVisible (showInputControls);
    updateInputSpecificVisibility(); // "No Input" Logic: also hides when category == No Input
    monitorInButton.setVisible (showInputControls);
    monitorAutoButton.setVisible (showInputControls);
    monitorOffButton.setVisible (showInputControls);
    outputCategoryCombo.setVisible (! isCollapsed);
    updateOutputSpecificVisibility(); // Output Combo Box Logic: also hides when category == Master
    volumeSlider.setVisible (! isCollapsed);
    panKnob.setVisible (! isCollapsed);
}

void TrackHeaderComponent::updateInputSpecificVisibility()
{
    constexpr int noInputCategoryId = 1;
    const bool showInputControls = ! isCollapsed && ! isReturnTrackFlag;
    inputSpecificCombo.setVisible (showInputControls && inputCategoryCombo.getSelectedId() != noInputCategoryId);
}

void TrackHeaderComponent::updateOutputSpecificVisibility()
{
    constexpr int masterCategoryId = 1;
    outputSpecificCombo.setVisible (! isCollapsed && outputCategoryCombo.getSelectedId() != masterCategoryId);
}

void TrackHeaderComponent::resized()
{
    applyCollapsedVisibility();

    if (isCollapsed)
        layoutCollapsed (getLocalBounds());
    else
        layoutExpanded (getLocalBounds());
}

void TrackHeaderComponent::layoutExpanded (juce::Rectangle<int>)
{
    // EXACT HARDCODED BOUNDS (Lead Architect directive) — no removeFromLeft(),
    // no withSizeKeepingCentre(), no derived math. Every number below is a
    // literal pixel position/size for the 300x90 expanded header. The
    // parameter is ignored (always getLocalBounds() == {0,0,300,90} for this
    // row) since nothing here is computed FROM it any more.

    // ---- Column 1 (Identity): x=[0,90) ----------------------------------------
    column1Bounds = { 0, 0, DS::column1Right, DS::headerHeight }; // solid track-colour fill, strictly bounded — see paint()
    foldArrow.setBounds (DS::foldArrowX, DS::foldArrowY, DS::foldArrowW, DS::foldArrowH); // standard crisp, centred disclosure glyph — NOT the squashed 10x10
    nameLabel.setBounds (DS::nameLabelX, DS::nameLabelY, DS::nameLabelW, DS::nameLabelH); // vertically centred by the label's own Justification::centredLeft

    // ---- Column 2 (Routing): x=[90,180) ----------------------------------
    if (isReturnTrackFlag)
    {
        // Ableton Accuracy: return tracks don't record from external inputs —
        // Input Category/Specific + the monitor triad are hidden entirely
        // (applyCollapsedVisibility()), so ONLY the two Output combos need
        // bounds here, centred as a 2-row (36px) block in the full 90px
        // column height: (90-36)/2 = 27.
        outputCategoryCombo.setBounds (DS::col2X, DS::col2ReturnRow1Y, DS::col2W, DS::col2RowH);
        outputSpecificCombo.setBounds (DS::col2X, DS::col2ReturnRow2Y, DS::col2W, DS::col2RowH);
    }
    else
    {
        // 5 rows of exactly 18px, tiling the full 90px column height with
        // ZERO dead space (Hybrid Bus/Return Architecture directive). x=94/
        // width=82 insets each row 4px from the Column 1/2 and Column 2/3
        // separator lines (x=90, x=180).
        inputCategoryCombo.setBounds  (DS::col2X, 0, DS::col2W, DS::col2RowH);
        inputSpecificCombo.setBounds  (DS::col2X, DS::inputSpecificY, DS::col2W, DS::col2RowH);
        {
            // IN | AUTO | OFF — 3 equal buttons, 2px gaps, within the same 82px row width.
            constexpr int monitorBtnW = (DS::col2W - 2 * DS::monitorGap) / 3;
            monitorInButton.setBounds   (DS::col2X, DS::monitorRowY, monitorBtnW, DS::monitorRowH);
            monitorAutoButton.setBounds (DS::col2X + monitorBtnW + DS::monitorGap, DS::monitorRowY, monitorBtnW, DS::monitorRowH);
            monitorOffButton.setBounds  (DS::col2X + (monitorBtnW + DS::monitorGap) * 2, DS::monitorRowY, monitorBtnW, DS::monitorRowH);
        }
        outputCategoryCombo.setBounds (DS::col2X, DS::outputCategoryY, DS::col2W, DS::col2RowH);
        outputSpecificCombo.setBounds (DS::col2X, DS::outputSpecificY, DS::col2W, DS::col2RowH);
    }

    // ---- Column 3 (Mini-Mixer): x=[180,300) -----------------------------------
    mutePlate.setBounds       (DS::mutePlateX, DS::mutePlateY, DS::mutePlateW, DS::mutePlateH); // track number
    volumeSlider.setBounds    (DS::volumeSliderX, DS::volumeSliderY, DS::volumeSliderW, DS::volumeSliderH); // numeric drag-box
    soloButton.setBounds      (DS::soloButtonX, DS::soloButtonY, DS::soloButtonWH, DS::soloButtonWH);
    recordArmButton.setBounds (DS::recordButtonX, DS::recordButtonY, DS::recordButtonWH, DS::recordButtonWH); // "R" (Record) — or "Pre/Post" for a return track, see constructor
    panKnob.setBounds         (DS::panKnobX, DS::panKnobY, DS::panKnobWH, DS::panKnobWH); // centred under S/R

    // Restore the Meters directive: this used to be left empty (meterBounds
    // = {}) to protect a "reserved for future Sends UI" strip at x=[282,300)
    // — that reservation is superseded now; the meter belongs here, compact,
    // on the far right edge of Column 3.
    meterBounds = { DS::meterX, DS::meterY, DS::meterW, DS::meterH };
}

void TrackHeaderComponent::layoutCollapsed (juce::Rectangle<int> full)
{
    // Single sleek strip: fold arrow | colour strip | name | mute plate | S | R |
    // thin vertical meter. Everything on one line, vertically centred. paint()'s
    // separators are gated on isCollapsed directly — nothing to reset here.
    meterBounds = full.removeFromRight (meterStripW).reduced (0, DS::collapsedPadY);

    auto area = full.reduced (DS::collapsedPadX, DS::collapsedPadY);

    foldArrow.setBounds (area.removeFromLeft (foldArrowW));
    {
        // Collapsed micro-state has no "Column 1" to fully fill (it's one
        // single-line strip) — reuse column1Bounds for a thin accent strip
        // instead, same as the old pre-mockup expanded look.
        auto stripSlot = area.removeFromLeft (colourStripW);
        column1Bounds = stripSlot;
        area.removeFromLeft (DS::nameToStripGap);
    }

    // Compact mixer controls hug the right edge; name takes whatever's left.
    const int plateW = DS::collapsedPlateW;
    const int srW    = DS::collapsedSRW;
    const int gap    = DS::collapsedGap;

    auto rightCluster = area.removeFromRight (plateW + gap + srW + gap + srW);
    mutePlate.setBounds (rightCluster.removeFromLeft (plateW));
    rightCluster.removeFromLeft (gap);
    soloButton.setBounds (rightCluster.removeFromLeft (srW));
    rightCluster.removeFromLeft (gap);
    recordArmButton.setBounds (rightCluster.removeFromLeft (srW));

    area.removeFromRight (gap);
    nameLabel.setBounds (area);
}
