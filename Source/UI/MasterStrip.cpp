#include "MasterStrip.h"
#include "TheCrateLookAndFeel.h"
#include "CrateColors.h"
#include "CrateDesignSystem.h"
#include "CrateEQThumbnail.h"
#include "CrateCompressorPopup.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Cubase-style pan readout — identical text logic to MixerStrip's own
    // panValueText(), ported verbatim (Restore Pan Readout Label directive)
    // so Master's readout reads exactly like every real track's.
    juce::String panValueText (float pan)
    {
        const int percent = juce::roundToInt (std::abs (pan) * 100.0f);
        if (percent == 0)
            return "C";
        return pan < 0.0f ? ("< " + juce::String (percent) + " L")
                           : (juce::String (percent) + " R >");
    }

    // Design System Centralization: these are now thin aliases into
    // CrateDesignSystem::Metrics::ChannelStrip — the SAME values MixerStrip
    // reads (see that file's own identical alias block and
    // CrateDesignSystem.h's doc comment on why the two are deliberately
    // unified, not independently duplicated).
    namespace DS = CrateDesignSystem::Metrics::ChannelStrip;
    constexpr float meterFloorDb = DS::meterFloorDb;
    constexpr float meterRangeDb = DS::meterRangeDb;
    constexpr int meterColumnWidth = DS::meterColumnWidth;

    // EXACT same bottom-up level heights/gaps as MixerStrip (including
    // outerMargin, now 4 not 6) — Fader Alignment directive: Master carries
    // the SAME blank-space budget a real track reserves for the icon and the
    // R/S/I triad (it just never draws anything in them), so its fader rail
    // starts and ends on the identical pixel line as every MixerStrip.
    constexpr int levelGap   = DS::levelGap;
    constexpr int scribbleIconH = DS::scribbleIconH; // blank — Master has no scribble-strip icon
    constexpr int scribbleGap   = DS::scribbleGap;
    constexpr int nameH      = DS::nameH; // L1
    constexpr int tripletH   = DS::tripletH; // blank — Master has no R/S/I triad
    constexpr int dbReadoutH = DS::dbReadoutH; // L5
    constexpr int panH       = DS::panH; // L6
    constexpr int panValueH   = DS::panValueH; // Pan readout label — matches MixerStrip
    constexpr int panValueGap = DS::panValueGap;
    constexpr int routingH   = DS::routingRowH; // L9 (single read-only output slot)
    constexpr int compH      = DS::compH; // L12 — neat single-slot-height rect (opens a popup only)
    constexpr int eqH        = DS::eqH; // L13
    constexpr int settingsH  = DS::settingsH; // L14
    constexpr int outerMargin = DS::outerMargin;
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
    nameLabel.setColour (juce::Label::backgroundColourId, juce::Colour (CrateDesignSystem::Colors::masterNameplateViolet)); // distinct master plate (violet)
    nameLabel.setColour (juce::Label::outlineColourId, LAF::colorFaderGroove);
    nameLabel.setFont (juce::FontOptions (CrateDesignSystem::Typography::stripNameFontSize, juce::Font::bold));
    // Nuke the 'M' Button directive: the nameplate IS the Mute toggle now
    // (Ableton nameplate-mute paradigm — matches MixerStrip::trackNameLabel
    // and ArrangementComponent::MasterHeaderRow). Clicks pass through to
    // MasterStrip::mouseDown(), same reasoning as MixerStrip's trackNameLabel.
    nameLabel.setInterceptsMouseClicks (false, false);
    nameLabel.setTooltip ("Click to mute Master.");

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
        l.setFont (juce::FontOptions (CrateDesignSystem::Typography::dbReadoutFontSize));
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

    // Pan value readout — Restore Pan Readout Label directive: EXACT same
    // font size/colour/justification as MixerStrip::panValueLabel, so it
    // reads flush with the readouts on every real track strip beside it.
    addAndMakeVisible (panValueLabel);
    panValueLabel.setJustificationType (juce::Justification::centred);
    panValueLabel.setColour (juce::Label::textColourId, CrateColors::NeonBlue);
    panValueLabel.setFont (juce::FontOptions (CrateDesignSystem::Typography::panValueFontSize, juce::Font::bold));
    panValueLabel.setText ("C", juce::dontSendNotification);

    // ---- L9: Output slot (fixed Stereo Out, read-only) ------------------------
    // A plain Label — draws its own matching hardware-slot bevel by hand in
    // paint() below (see MixerStrip::RoutingBlock::paint()'s identical
    // reasoning: Labels don't route through a Button/ComboBox LookAndFeel).
    addChildComponent (outputSlot);
    outputSlot.setText ("Stereo Out", juce::dontSendNotification);
    outputSlot.setJustificationType (juce::Justification::centred);
    outputSlot.setColour (juce::Label::textColourId, HardwareSlotLookAndFeel::dimTextColour);
    outputSlot.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    outputSlot.setFont (juce::FontOptions (CrateDesignSystem::Typography::outputSlotFontSize, juce::Font::bold));

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
        panValueLabel.setText (panValueText (volumePlugin->getPan()), juce::dontSendNotification);
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
        panKnob.onValueChange = [this]
        {
            const auto pan = (float) panKnob.getValue();
            volumePlugin->setPan (pan);
            panValueLabel.setText (panValueText (pan), juce::dontSendNotification);
        };

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
    if (volumePlugin == nullptr)
        return;

    const bool isMuted = volumePlugin->getVolumeDb() <= CrateDesignSystem::Metrics::Master::muteThresholdDb;

    if (isMuted)
    {
        nameLabel.setColour (juce::Label::backgroundColourId, CrateColors::DarkBackground);
        nameLabel.setColour (juce::Label::textColourId, CrateColors::BrandGray);
    }
    else
    {
        nameLabel.setColour (juce::Label::backgroundColourId, juce::Colour (CrateDesignSystem::Colors::masterNameplateViolet));
        nameLabel.setColour (juce::Label::textColourId, LAF::colorTextPrimary);
    }

    nameLabel.repaint();
}

void MasterStrip::mouseDown (const juce::MouseEvent& e)
{
    setSelected (true);
    if (onSelected)
        onSelected();

    // Nuke the 'M' Button directive: nameplate click toggles Master mute,
    // same gesture as MixerStrip::trackNameLabel and MasterHeaderRow.
    if (e.getNumberOfClicks() == 1 && nameLabel.getBounds().contains (e.getPosition()) && volumePlugin != nullptr)
    {
        volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction ("Mute Master");
        volumePlugin->muteOrUnmute();
        refreshMuteState();
    }
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
    panValueLabel.setText (panValueText (volumePlugin->getPan()), juce::dontSendNotification);
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
        g.fillRect (0, 0, CrateDesignSystem::Metrics::Master::selectedAccentStripeW, getHeight());
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
    // Fader Alignment directive: identical bottom-up idiom AND identical
    // blank-space budget to MixerStrip::resized(), level-for-level, so the
    // Master fader rail starts/ends on the exact same horizontal pixel lines
    // as every real MixerStrip — Master just has nothing to draw in the
    // blank slots (no scribble icon, no R/S/I triad, no pan-value readout).
    auto bounds = getLocalBounds().reduced (outerMargin);

    // ----- Bottom group (blank icon slot, L1, blank triplet slot) -------------
    bounds.removeFromBottom (scribbleIconH); // blank — no scribble-strip icon on Master
    bounds.removeFromBottom (scribbleGap);

    nameLabel.setBounds (bounds.removeFromBottom (nameH)); // L1
    bounds.removeFromBottom (levelGap);

    bounds.removeFromBottom (tripletH); // blank — no R/S/I triad on Master
    bounds.removeFromBottom (levelGap);

    // ----- Top group (L14 → L9 when expanded, then L6 → L5) -------------------
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

    panKnob.setBounds (bounds.removeFromTop (panH).withSizeKeepingCentre (panH, panH)); // L6
    bounds.removeFromTop (panValueGap);

    panValueLabel.setBounds (bounds.removeFromTop (panValueH));
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
