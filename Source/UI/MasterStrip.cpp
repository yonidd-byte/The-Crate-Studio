#include "MasterStrip.h"
#include "TheCrateLookAndFeel.h"
#include "CrateEQThumbnail.h"
#include "CrateCompressorPopup.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    constexpr float meterFloorDb = -60.0f;
    constexpr float meterRangeDb = 66.0f;
    constexpr int meterColumnWidth = 22;

    // Same bottom-up level heights as MixerStrip (kept in step deliberately, so
    // Master and a real track strip line up level-for-level side by side).
    constexpr int levelGap   = 4;
    constexpr int nameH      = 20; // L1
    constexpr int toggleH    = 22; // L2
    constexpr int dbReadoutH = 16; // L5
    constexpr int panH       = 42; // L6
    constexpr int iconH      = 26; // L7
    constexpr int routingH   = 22; // L9 (single read-only output slot)
    constexpr int compH      = 24; // L12 — neat single-slot-height rect (opens a popup only)
    constexpr int eqH        = 60; // L13
    constexpr int settingsH  = 22; // L14
    constexpr int outerMargin = 6;
}

//==============================================================================
MasterStrip::MasterStrip (te::Edit& editToUse, CrateWorkflowManager& workflowToUse)
    : edit (editToUse), workflow (workflowToUse)
{
    // The REAL master bus volume/pan — the same object the transport's own
    // master fader would bind to, not a fabricated stand-in.
    volumePlugin = edit.getMasterVolumePlugin();

    meterPlugin = edit.getMasterPluginList().findFirstPluginOfType<te::LevelMeterPlugin>();

    if (meterPlugin == nullptr)
    {
        auto plugin = edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, juce::PluginDescription());
        edit.getMasterPluginList().insertPlugin (plugin, -1, nullptr);
        meterPlugin = dynamic_cast<te::LevelMeterPlugin*> (plugin.get());
    }

    if (meterPlugin != nullptr)
        meterPlugin->measurer.addClient (meterClient);

    // ---- L1: name plate -------------------------------------------------------
    addAndMakeVisible (nameLabel);
    nameLabel.setText ("MASTER", juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, LAF::colorTextPrimary);
    nameLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff6a3a6a)); // distinct master plate (violet)
    nameLabel.setColour (juce::Label::outlineColourId, LAF::colorFaderGroove);
    nameLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));

    // ---- L2: Mute (only) — real, via VolumeAndPanPlugin::muteOrUnmute() -------
    addAndMakeVisible (muteButton);
    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, LAF::colorMuteRed);
    muteButton.onClick = [this]
    {
        if (volumePlugin == nullptr)
            return;

        volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction ("Mute Master");
        volumePlugin->muteOrUnmute();
        refreshMuteState();
    };

    // ---- L4: Sexy fader -------------------------------------------------------
    addAndMakeVisible (volumeFader);
    volumeFader.setLookAndFeel (&mixerLookAndFeel);
    volumeFader.setRange (-60.0, 6.0, 0.1);
    volumeFader.setDoubleClickReturnValue (true, 0.0);

    // ---- L5: dB readout boxes -------------------------------------------------
    auto styleReadout = [] (juce::Label& l)
    {
        l.setJustificationType (juce::Justification::centred);
        l.setColour (juce::Label::textColourId, LAF::lcdText);
        l.setColour (juce::Label::backgroundColourId, LAF::lcdBackground);
        l.setFont (juce::FontOptions (9.5f));
    };
    addAndMakeVisible (faderPositionLabel);
    styleReadout (faderPositionLabel);
    addAndMakeVisible (peakLevelLabel);
    styleReadout (peakLevelLabel);
    peakLevelLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    // ---- L6: Pan --------------------------------------------------------------
    addAndMakeVisible (panKnob);
    panKnob.setLookAndFeel (&mixerLookAndFeel);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                  juce::MathConstants<float>::pi * 2.8f, true);

    // ---- L9: Output slot (fixed Stereo Out, read-only) ------------------------
    // A plain Label — draws its own matching hardware-slot bevel by hand in
    // paint() below (see MixerStrip::RoutingBlock::paint()'s identical
    // reasoning: Labels don't route through a Button/ComboBox LookAndFeel).
    addChildComponent (outputSlot);
    outputSlot.setText ("Stereo Out", juce::dontSendNotification);
    outputSlot.setJustificationType (juce::Justification::centred);
    outputSlot.setColour (juce::Label::textColourId, HardwareSlotLookAndFeel::dimTextColour);
    outputSlot.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    outputSlot.setFont (juce::FontOptions (11.0f, juce::Font::bold));

    // ---- L11: Inserts (shared InsertsRackComponent) — no L10 sends on Master --
    addChildComponent (insertsRack);
    insertsRack.onSlotSelected = [this] (te::Plugin* p) { if (onInsertSlotSelected) onInsertSlotSelected (p); };
    rebuildInserts();

    // ---- L12: Channel Comp block ----------------------------------------------
    addChildComponent (channelCompButton);
    channelCompButton.setColour (juce::TextButton::buttonColourId, LAF::colorGhostedOff);
    channelCompButton.setColour (juce::TextButton::buttonOnColourId, LAF::colorNeonCyan);
    channelCompButton.onClick = [this] { openChannelCompPopup(); };

    // ---- L13: EQ display ------------------------------------------------------
    eqThumbnail = std::make_unique<CrateEQThumbnail>();
    addChildComponent (*eqThumbnail);

    // ---- L14: Settings --------------------------------------------------------
    addChildComponent (settingsButton);
    settingsButton.setColour (juce::TextButton::buttonColourId, LAF::colorGhostedOff);
    settingsButton.setTooltip ("Master-strip settings - placeholder.");

    const bool showDeep = rackExpanded;
    outputSlot.setVisible (showDeep);
    insertsRack.setVisible (showDeep);
    channelCompButton.setVisible (showDeep);
    eqThumbnail->setVisible (showDeep);
    settingsButton.setVisible (showDeep);

    if (volumePlugin != nullptr)
    {
        const auto db = volumePlugin->getVolumeDb();
        volumeFader.setValue (db, juce::dontSendNotification);
        faderPositionLabel.setText (juce::String (db, 1), juce::dontSendNotification);
        panKnob.setValue (volumePlugin->getPan(), juce::dontSendNotification);
        refreshMuteState();

        volumeFader.onDragStart = [this]
        {
            volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction ("Tweak Master Volume");
            volumePlugin->volParam->parameterChangeGestureBegin();
        };
        volumeFader.onDragEnd = [this] { volumePlugin->volParam->parameterChangeGestureEnd(); };
        volumeFader.onValueChange = [this]
        {
            const auto newDb = (float) volumeFader.getValue();
            volumePlugin->setVolumeDb (newDb);
            faderPositionLabel.setText (juce::String (newDb, 1), juce::dontSendNotification);
        };

        panKnob.onDragStart = [this]
        {
            volumePlugin->panParam->getEdit().getUndoManager().beginNewTransaction ("Tweak Master Pan");
            volumePlugin->panParam->parameterChangeGestureBegin();
        };
        panKnob.onDragEnd = [this] { volumePlugin->panParam->parameterChangeGestureEnd(); };
        panKnob.onValueChange = [this] { volumePlugin->setPan ((float) panKnob.getValue()); };

        volumePlugin->volParam->addListener (this);
        volumePlugin->panParam->addListener (this);
    }

    startTimerHz (24);
}

MasterStrip::~MasterStrip()
{
    stopTimer();

    volumeFader.setLookAndFeel (nullptr);
    panKnob.setLookAndFeel (nullptr);

    if (volumePlugin != nullptr)
    {
        volumePlugin->volParam->removeListener (this);
        volumePlugin->panParam->removeListener (this);
    }

    if (meterPlugin != nullptr)
        meterPlugin->measurer.removeClient (meterClient);
}

void MasterStrip::rebuildInserts()
{
    if (auto* masterTrack = edit.getMasterTrack())
        insertsRack.rebuild (*masterTrack, workflow);
}

void MasterStrip::refreshMuteState()
{
    if (volumePlugin != nullptr)
        muteButton.setToggleState (volumePlugin->getVolumeDb() <= -90.0f, juce::dontSendNotification);
}

void MasterStrip::openChannelCompPopup()
{
    channelCompButton.setToggleState (true, juce::dontSendNotification);

    auto popup = std::make_unique<CrateCompressorPopup> (&mixerLookAndFeel);
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (popup),
                                                        channelCompButton.getScreenBounds(), nullptr);
    box.addComponentListener (this);
}

void MasterStrip::componentBeingDeleted (juce::Component&)
{
    channelCompButton.setToggleState (false, juce::dontSendNotification);
}

void MasterStrip::setSelected (bool shouldBeSelected)
{
    if (selected == shouldBeSelected)
        return;

    selected = shouldBeSelected;
    repaint();
}

void MasterStrip::setRackExpanded (bool shouldBeExpanded)
{
    if (rackExpanded == shouldBeExpanded)
        return;

    rackExpanded = shouldBeExpanded;
    rebuildInserts();

    outputSlot.setVisible (rackExpanded);
    insertsRack.setVisible (rackExpanded);
    channelCompButton.setVisible (rackExpanded);
    eqThumbnail->setVisible (rackExpanded);
    settingsButton.setVisible (rackExpanded);

    resized();
}

void MasterStrip::currentValueChanged (te::AutomatableParameter&)
{
    if (volumePlugin == nullptr)
        return;

    const auto db = volumePlugin->getVolumeDb();
    volumeFader.setValue (db, juce::dontSendNotification);
    faderPositionLabel.setText (juce::String (db, 1), juce::dontSendNotification);
    panKnob.setValue (volumePlugin->getPan(), juce::dontSendNotification);
    refreshMuteState();
}

void MasterStrip::timerCallback()
{
    if (meterPlugin == nullptr)
        return;

    const auto levelL = meterClient.getAndClearAudioLevel (0);
    const auto levelR = meterClient.getAndClearAudioLevel (1);
    meterLevelDb = juce::jmax (levelL.dB, levelR.dB);

    const auto nowMs = juce::Time::getMillisecondCounter();

    if (meterLevelDb >= peakHoldDb)
    {
        peakHoldDb = meterLevelDb;
        peakHoldLastUpdateMs = nowMs;
    }
    else
    {
        constexpr float decayDbPerSecond = 20.0f;
        const float elapsedSeconds = (float) (nowMs - peakHoldLastUpdateMs) * 0.001f;
        peakHoldDb = juce::jmax (meterLevelDb, peakHoldDb - decayDbPerSecond * elapsedSeconds);
    }

    peakLevelLabel.setText (peakHoldDb > meterFloorDb + 0.5f ? juce::String (peakHoldDb, 1)
                                                             : juce::String ("-inf"),
                            juce::dontSendNotification);

    repaint();
}

void MasterStrip::paint (juce::Graphics& g)
{
    g.fillAll (LAF::colorHardware);

    if (selected)
    {
        g.setColour (LAF::colorNeonCyan);
        g.fillRect (0, 0, 3, getHeight());
    }

    g.setColour (LAF::colorTheVoid);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    // L9 output slot ("Stereo Out") — hand-drawn hardware-slot bevel, since a
    // plain Label never routes through a Button/ComboBox LookAndFeel (see the
    // constructor's comment on outputSlot).
    if (rackExpanded && outputSlot.isVisible())
    {
        HardwareSlotLookAndFeel::drawRaisedChip (g, outputSlot.getBounds().toFloat(), HardwareSlotLookAndFeel::fillColour);
    }

    // L7 track-icon placeholder.
    if (! trackIconBounds.isEmpty())
    {
        auto ib = trackIconBounds.toFloat();
        g.setColour (LAF::colorGhostedOff);
        g.fillRoundedRectangle (ib, 3.0f);
        g.setColour (LAF::colorFaderGroove);
        g.drawRoundedRectangle (ib.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (LAF::colorTextSecondary);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("M", trackIconBounds, juce::Justification::centred);
    }

    // L4 main meter.
    g.setColour (LAF::colorTheVoid);
    g.fillRect (meterBounds);

    const auto normalised = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
    const auto fillHeight = meterBounds.getHeight() * normalised;
    const auto fillRect = meterBounds.withTop (meterBounds.getBottom() - fillHeight);

    if (fillHeight > 0.0f)
    {
        juce::ColourGradient gradient (juce::Colours::limegreen, meterBounds.getBottomLeft(),
                                       juce::Colours::red, meterBounds.getTopLeft(), false);
        gradient.addColour (0.75, juce::Colours::yellow);
        g.setGradientFill (gradient);
        g.fillRect (fillRect);
    }

    const auto peakNormalised = juce::jlimit (0.0f, 1.0f, (peakHoldDb - meterFloorDb) / meterRangeDb);
    const auto peakY = meterBounds.getBottom() - meterBounds.getHeight() * peakNormalised;
    g.setColour (juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (meterBounds.getX(), peakY - 0.5f, meterBounds.getWidth(), 1.0f));
}

void MasterStrip::resized()
{
    // Same strict bottom-up idiom as MixerStrip::resized() — Master simply omits
    // the levels it doesn't have (no L3 Record/Input, no L10 Sends).
    auto bounds = getLocalBounds().reduced (outerMargin);

    // ----- Bottom group (L1 → L2) ---------------------------------------------
    nameLabel.setBounds (bounds.removeFromBottom (nameH)); // L1
    bounds.removeFromBottom (levelGap);

    muteButton.setBounds (bounds.removeFromBottom (toggleH).withSizeKeepingCentre (
                              juce::jmin (bounds.getWidth(), 60), toggleH)); // L2 (Mute only, centred)
    bounds.removeFromBottom (levelGap);

    // ----- Top group (L14 → L9 when expanded, then L8 → L5) -------------------
    if (rackExpanded)
    {
        settingsButton.setBounds (bounds.removeFromTop (settingsH));                 // L14
        bounds.removeFromTop (levelGap);

        eqThumbnail->setBounds (bounds.removeFromTop (eqH));                         // L13
        bounds.removeFromTop (levelGap);

        channelCompButton.setBounds (bounds.removeFromTop (compH));                 // L12
        bounds.removeFromTop (levelGap);

        insertsRack.setBounds (bounds.removeFromTop (InsertsRackComponent::getFixedHeight())); // L11
        insertsRack.resized();
        bounds.removeFromTop (levelGap);

        // (no L10 Sends on Master)

        outputSlot.setBounds (bounds.removeFromTop (routingH));                      // L9
        bounds.removeFromTop (levelGap);
    }

    trackIconBounds = bounds.removeFromTop (iconH).withSizeKeepingCentre (iconH, iconH); // L7
    bounds.removeFromTop (levelGap);

    panKnob.setBounds (bounds.removeFromTop (panH).withSizeKeepingCentre (panH, panH)); // L6
    bounds.removeFromTop (levelGap);

    {
        auto row = bounds.removeFromTop (dbReadoutH);                                // L5
        faderPositionLabel.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (1, 0));
        peakLevelLabel.setBounds (row.reduced (1, 0));
    }
    bounds.removeFromTop (levelGap);

    // ----- L4: fader + meter (fills remainder) --------------------------------
    meterBounds = bounds.removeFromRight (meterColumnWidth).reduced (2, 0).toFloat();
    volumeFader.setBounds (bounds);
}
