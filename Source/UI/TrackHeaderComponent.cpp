#include "TrackHeaderComponent.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    const auto headerDefault  = juce::Colour (0xff252525);
    const auto headerSelected = juce::Colour (0xff3a3a3a);
    const auto toggleOff      = juce::Colour (0xff3a3a3a);

    // Role colours (armOnColour/soloOnColour/activeOnColour) now live as
    // TrackHeaderComponent's own inline static members in the header — both this
    // file and the default member initializers that construct recordArmButton/
    // soloButton/muteButton read the exact same three constants, so the glyph a
    // button is built with can never drift from the colour its click logic reasons
    // about here.

    constexpr float meterFloorDb = -60.0f;
    constexpr float meterRangeDb = 66.0f; // floor to +6 dB headroom

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
    auto bg = getToggleState() ? onColour : juce::Colour (0xff3a3a3e);

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (0.15f);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (0.08f);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    g.setColour (getToggleState() ? juce::Colours::black : LAF::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (glyph, getLocalBounds(), juce::Justification::centred);
}

TrackHeaderComponent::TrackHeaderComponent (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse)
    : track (trackToControl), workflow (workflowToUse)
{
    if (track != nullptr)
    {
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

    // Track Number badge (Ableton/Logic/Pro Tools style) — persistent, non-editable,
    // read once from the engine's own track ordering (getAudioTrackNumber() is
    // 1-based and already accounts for this track's real position in the Edit).
    // Every add/delete rebuilds every TrackRow/TrackHeaderComponent from scratch
    // (ArrangementComponent::rebuildTracks()), so renumbering after a track is
    // inserted/removed falls out for free — nothing here needs to react live.
    addAndMakeVisible (trackNumberLabel);
    trackNumberLabel.setText (track != nullptr ? juce::String (track->getAudioTrackNumber()) : juce::String ("-"),
                               juce::dontSendNotification);
    trackNumberLabel.setJustificationType (juce::Justification::centred);
    trackNumberLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff1a1a1a)); // slightly darker than header body
    trackNumberLabel.setColour (juce::Label::textColourId, LAF::text);
    trackNumberLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    trackNumberLabel.setInterceptsMouseClicks (false, false); // clicks fall through to the header's own mouseDown (select track)

    addAndMakeVisible (nameLabel);
    nameLabel.setText (track != nullptr ? track->getName() : juce::String ("—"), juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId, LAF::text);
    nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    nameLabel.setEditable (false, true, false); // double-click to rename
    nameLabel.onTextChange = [this]
    {
        if (track != nullptr)
            track->setName (nameLabel.getText());
    };

    addAndMakeVisible (ioLabel);
    ioLabel.setText ("In: Ext. In   Out: " + (track != nullptr ? track->getOutput().getDescriptiveOutputName() : juce::String ("Master")),
                      juce::dontSendNotification);
    ioLabel.setFont (juce::FontOptions (9.5f));
    ioLabel.setColour (juce::Label::textColourId, LAF::textDim);
    ioLabel.setTooltip ("Input routing isn't wired to the engine yet - display only. Output reflects the track's real destination.");

    recordArmButton.setClickingTogglesState (true);
    soloButton.setClickingTogglesState (true);
    muteButton.setClickingTogglesState (true);
    automationButton.setClickingTogglesState (true);

    addAndMakeVisible (recordArmButton);
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

    addAndMakeVisible (soloButton);
    soloButton.setToggleState (track != nullptr && track->isSolo (false), juce::dontSendNotification);
    soloButton.onClick = [this]
    {
        if (onSelect) onSelect();
        if (track != nullptr) track->setSolo (soloButton.getToggleState());
    };

    // True Mute polarity (per the Lead UX Architect's explicit correction): ON
    // (lit, studio orange) means the track IS MUTED; OFF (dark) means audible.
    // Toggle state maps DIRECTLY to track->isMuted() — no inversion anywhere.
    addAndMakeVisible (muteButton);
    muteButton.setToggleState (track != nullptr && track->isMuted (false), juce::dontSendNotification);
    muteButton.onClick = [this]
    {
        if (onSelect) onSelect();
        if (track != nullptr) track->setMute (muteButton.getToggleState());
    };

    addAndMakeVisible (automationButton);
    automationButton.onClick = [this]
    {
        if (onSelect) onSelect();
        if (onAutomationToggle) onAutomationToggle();
    };

    addAndMakeVisible (deleteButton);
    deleteButton.setColour (juce::TextButton::buttonColourId, toggleOff);
    deleteButton.setColour (juce::TextButton::textColourOffId, LAF::textDim);
    deleteButton.onClick = [this]
    {
        if (onSelect) onSelect();

        // Deferred: onDeleteRequested ends in a full track-list rebuild that
        // destroys this row (and this very button). Running that synchronously
        // from inside the button's own click handler would free it mid-callback —
        // defer to the next message-loop iteration so this call stack unwinds first.
        if (onDeleteRequested)
        {
            auto callback = onDeleteRequested;
            juce::MessageManager::callAsync (callback);
        }
    };

    addAndMakeVisible (volumeSlider);
    volumeSlider.setRange (-60.0, 6.0, 0.1);
    volumeSlider.setDoubleClickReturnValue (true, 0.0);
    volumeSlider.onDragStart = [this] { if (onSelect) onSelect(); };

    if (volumePlugin != nullptr)
    {
        volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
        volumeSlider.onValueChange = [this]
        {
            if (volumePlugin != nullptr)
                volumePlugin->setVolumeDb ((float) volumeSlider.getValue());
        };

        // Bidirectional sync with MixerStrip: fires when Volume/Pan change from
        // that strip's fader/pan knob (or automation, or a script), not just from
        // this slider. No visible Pan control exists in THIS header today — the
        // panParam listener is still attached so nothing needs revisiting when one
        // is added — but currentValueChanged() below only has volume to refresh.
        volumePlugin->volParam->addListener (this);
        volumePlugin->panParam->addListener (this);
    }

    // Mute/Solo/Armed aren't AutomatableParameters — plain CachedValue<bool> /
    // ValueTree properties on the track's own state — so they need a
    // juce::ValueTree::Listener instead of te::AutomatableParameter::Listener to
    // catch changes made from MixerStrip or from Undo/Redo.
    if (track != nullptr)
        track->state.addListener (this);

    startTimerHz (24); // live meter poll only — see timerCallback()
}

TrackHeaderComponent::~TrackHeaderComponent()
{
    stopTimer();

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
    juce::Component::SafePointer<TrackHeaderComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->refreshVolumeFromEngine();
    });
}

void TrackHeaderComponent::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& property)
{
    if (track == nullptr || v != track->state)
        return;

    if (property != te::IDs::mute && property != te::IDs::solo && property != armedPropertyID)
        return;

    juce::Component::SafePointer<TrackHeaderComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->refreshToggleStatesFromEngine();
    });
}

void TrackHeaderComponent::refreshVolumeFromEngine()
{
    if (volumePlugin == nullptr)
        return;

    volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
}

void TrackHeaderComponent::refreshToggleStatesFromEngine()
{
    if (track == nullptr)
        return;

    muteButton.setToggleState (track->isMuted (false), juce::dontSendNotification);
    soloButton.setToggleState (track->isSolo (false), juce::dontSendNotification);
    recordArmButton.setToggleState ((bool) track->state.getProperty (armedPropertyID, false), juce::dontSendNotification);
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

void TrackHeaderComponent::mouseDown (const juce::MouseEvent&)
{
    if (onSelect)
        onSelect();
}

void TrackHeaderComponent::paint (juce::Graphics& g)
{
    g.fillAll (selected ? headerSelected : headerDefault);

    // Left accent stripe when selected — quick visual anchor for the active track.
    if (selected)
    {
        g.setColour (LAF::accent);
        g.fillRect (juce::Rectangle<int> (0, 0, 3, getHeight()));
    }

    // Live horizontal volume meter — flat fill, no gradient/bevel, matching the
    // rest of the flat-design language.
    if (! meterBounds.isEmpty())
    {
        g.setColour (LAF::lcdBackground);
        g.fillRect (meterBounds);

        const auto normalised = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
        auto fill = meterBounds.toFloat();
        fill.setWidth (fill.getWidth() * normalised);

        g.setColour (meterLevelDb > -3.0f ? LAF::meterHot : LAF::accent);
        g.fillRect (fill);

        // Numerical dB readout, overlaid on the far right of the meter — skipped
        // entirely at/near the floor (effectively -INF) rather than printing a
        // meaningless "-60.0" every frame there's no real signal.
        if (meterLevelDb > meterFloorDb + 0.5f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText (juce::String (meterLevelDb, 1), meterBounds.reduced (3, 0),
                        juce::Justification::centredRight);
        }
    }

    // Flat border separating this header from the timeline — no gradients, no 3D
    // bevels, just a solid 1px line on each shared edge.
    g.setColour (LAF::background);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
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

void TrackHeaderComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto topRow = area.removeFromTop (16);
    deleteButton.setBounds (topRow.removeFromRight (16));
    trackNumberLabel.setBounds (topRow.removeFromLeft (20));
    topRow.removeFromLeft (4);
    nameLabel.setBounds (topRow);
    area.removeFromTop (2);

    ioLabel.setBounds (area.removeFromTop (12));
    area.removeFromTop (3);

    auto buttonRow = area.removeFromTop (18);
    const int gap = 2; // Zone 3 spec: "cleanly in a row with 2px spacing"
    const int buttonWidth = juce::jmax (1, (buttonRow.getWidth() - 3 * gap) / 4);
    recordArmButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    soloButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    muteButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    automationButton.setBounds (buttonRow);
    area.removeFromTop (4);

    volumeSlider.setBounds (area.removeFromTop (14));
    area.removeFromTop (3);

    meterBounds = area.removeFromTop (11); // tall enough to overlay the dB readout legibly
}
