#include "TrackHeaderComponent.h"
#include "TheCrateLookAndFeel.h"
#include "TrackColourEditor.h"
#include "CrateColors.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Global Color Centralization & Purge: this was an independent near-black
    // literal (0xff252525) — now the exact same CrateColors::DarkBackground
    // MasterHeaderRow's identical container styling uses (see
    // ArrangementComponent.cpp's "literal copy" comment). Column 1's own
    // fallback fill when a track has no colour set yet (getIdentityFillColour()).
    const auto headerDefault  = CrateColors::DarkBackground;

    // Role colours (soloOnColour/recordOnColour) now live as TrackHeaderComponent's
    // own inline static members in the header, reading straight from CrateColors —
    // both this file and the default member initializers that construct
    // recordArmButton/soloButton read the exact same constants, so the glyph a
    // button is built with can never drift from the colour its click logic reasons
    // about here.

    // Design System Centralization: thin aliases into
    // CrateDesignSystem::Metrics::TrackHeader (see that namespace's own
    // layout for the Content-Driven Dynamic Height architecture this class
    // now computes its geometry from).
    namespace DS = CrateDesignSystem::Metrics::TrackHeader;

    constexpr float meterFloorDb = DS::meterFloorDb;
    constexpr float meterRangeDb = DS::meterRangeDb; // floor to +6 dB headroom

    // Persisted as a plain property on the track's own ValueTree — round-trips
    // through the same .crate save/load path as every other track property
    // (MASTER_ARCHITECTURE.md invariant 3: "Everything persists"), no separate
    // serialization step needed.
    const juce::Identifier armedPropertyID ("crateRecordArmed");

    // Bus/Return Default Collapsed State directive: same key
    // CrateWorkflowManager::createAndRouteNewFXChannel() stamps onto a
    // brand-new FX Return track — duplicated rather than sharing a header
    // (engine-level code never includes a UI header; see that function's own
    // doc comment on anchorMetadataProperty for the identical reasoning).
    // Round-trips through the same .crate save/load path as every other
    // track property (MASTER_ARCHITECTURE.md invariant 3).
    const juce::Identifier foldedPropertyID ("crateFolded");

    // Content-Driven Dynamic Height directive: input/output category must now
    // be persisted too — getPreferredHeight() depends on them, and
    // ArrangementComponent::TrackRow computes the SAME height independently
    // (it has no live header instance to ask), so both sides need a shared,
    // round-tripping source of truth rather than combo-box-only UI state
    // that silently reset to "No Input"/"Master" on every rebuild.
    const juce::Identifier inputCategoryPropertyID ("crateInputCategory");
    const juce::Identifier outputCategoryPropertyID ("crateOutputCategory");

    const juce::String pluginDragPrefix = "plugin_drag|";
}

void TrackHeaderComponent::ToggleBlock::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Sunken Boxes directive: unlit reads as a dark LCD cutout sunk into the
    // now-lighter LightBackground panel (DarkBackground, not a brightened
    // LightBackground — that used to camouflage against the OLD dark panel,
    // then would have camouflaged the OPPOSITE way against the new lighter
    // one). A quiet hairline rim (low-alpha black) defines the box's edge
    // against the panel, same convention the column dividers use.
    auto bg = getToggleState() ? onColour : CrateColors::DarkBackground;

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (DS::pressedBrighten);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (DS::hoverBrighten);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    // Inner Element Strokes directive: crisp 1px solid black, not a soft grey.
    g.setColour (juce::Colours::black);
    g.drawRect (getLocalBounds(), 1);

    g.setColour (getToggleState() ? juce::Colours::black : LAF::textDim);
    g.setFont (juce::FontOptions (fontSize, juce::Font::bold));
    g.drawText (glyph, getLocalBounds(), juce::Justification::centred);
}

void TrackHeaderComponent::MutePlate::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Restore Column 1 directive: Box A reverts to its original flat-filled
    // look — lit CrateColors::NeonBlue when AUDIBLE (the common resting state
    // reads as "signal alive"), dim DarkBackground when muted, so a muted
    // track visibly recedes. The track number stays legible in both states
    // (dark text on the neon plate, grey text on the dim one).
    const bool muted = getToggleState();
    auto bg = muted ? CrateColors::DarkBackground : CrateColors::NeonBlue;

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (DS::pressedBrighten);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (DS::muteHoverBrighten);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    // Inner Element Strokes directive: Box A gets the same crisp 1px solid
    // black border every other Column 3 box now has.
    g.setColour (juce::Colours::black);
    g.drawRect (getLocalBounds(), 1);

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

    // Desaturated Accents directive: NeonBlue is reserved for active values/
    // automation/the fader thumb — a hover highlight on the fold disclosure
    // glyph isn't any of those, so it just brightens the same neutral grey.
    g.setColour (shouldDrawButtonAsHighlighted ? CrateColors::BrandGray.brighter (0.4f) : CrateColors::BrandGray);
    g.fillPath (tri);
}

void TrackHeaderComponent::MonitorButton::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Monitor Triad Borders directive: unlit reads as a dark sunken box
    // against the LightBackground panel — same DarkBackground fix
    // ToggleBlock's identical off-state got (LightBackground.brighter() here
    // was nearly indistinguishable from the panel itself now, which is
    // exactly why the triad read as "one merged grey blob"). Desaturated
    // Accents directive: AUTO/IN's lit state stays a muted console amber,
    // not the bright brand SoloYellow — NeonBlue/saturated brights are
    // reserved for active values, automation, and the fader thumb.
    const bool active = (isActive != nullptr) && isActive();
    auto bg = active ? juce::Colour (DS::mutedAmber) : CrateColors::DarkBackground;

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (DS::pressedBrighten);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (DS::hoverBrighten);

    g.setColour (bg);
    g.fillRect (getLocalBounds());

    // Monitor Triad Borders directive: each of IN/AUTO/OFF gets its own
    // crisp 1px solid black border — this IS what makes them read as three
    // separate boxes rather than one merged strip (the outer frame around
    // the whole triad is drawn once by the owner in TrackHeaderComponent::
    // paint(), using these buttons' own live bounds).
    g.setColour (juce::Colours::black);
    g.drawRect (getLocalBounds(), 1);

    g.setColour (active ? juce::Colours::black : CrateColors::BrandGray);
    // Title Case directive: "Auto" (vs "AUTO") is narrower on its own, so the
    // squeeze only needs to be a light 0.95 condense now for that authentic
    // DAW look — not the aggressive 0.85 the all-caps label required.
    // useEllipses stays forced off as a belt-and-suspenders guard regardless.
    auto triadFont = juce::Font (juce::FontOptions (CrateDesignSystem::Typography::monitorLabelFontSize, juce::Font::bold).withHorizontalScale (0.95f));
    g.setFont (triadFont);
    g.drawText (label, getLocalBounds(), juce::Justification::centred, false);
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

    // Inner Element Strokes directive: crisp 1px solid black rim, same
    // convention every other Column 3 box now uses.
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (b.reduced (0.5f), corner, 1.0f);

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

void TrackHeaderComponent::PanBar::setValue (double newValue, juce::NotificationType nt)
{
    const auto clamped = juce::jlimit (-1.0, 1.0, newValue);

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (nt == juce::sendNotification && onValueChange)
        onValueChange();
}

void TrackHeaderComponent::PanBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    constexpr float corner = DS::volumeBarCornerRadius;

    // Flat dark well — same language as VolumeBar.
    g.setColour (CrateColors::DarkBackground);
    g.fillRoundedRectangle (b, corner);

    // NeonBlue fill growing from CENTRE outward toward whichever side the
    // value leans — the correct bipolar analogue of VolumeBar's left->right
    // unipolar fill.
    const auto centreX = b.getCentreX();
    const auto halfWidth = b.getWidth() * 0.5f;
    const auto offset = (float) value * halfWidth;

    if (std::abs (offset) > 0.5f)
    {
        juce::Graphics::ScopedSaveState clip (g);
        juce::Path well;
        well.addRoundedRectangle (b, corner);
        g.reduceClipRegion (well);

        const auto fill = (offset > 0.0f)
                              ? juce::Rectangle<float> (centreX, b.getY(), offset, b.getHeight())
                              : juce::Rectangle<float> (centreX + offset, b.getY(), -offset, b.getHeight());
        g.setColour (CrateColors::NeonBlue.withAlpha (DS::volumeBarFillAlpha));
        g.fillRect (fill);
    }

    // Inner Element Strokes directive: crisp 1px solid black rim, same
    // convention every other Column 3 box now uses.
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (b.reduced (0.5f), corner, 1.0f);

    // Centred readout — "C" dead centre, otherwise the percentage toward
    // whichever side ("50L" / "50R" — PNG Pivot directive's own format).
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (CrateDesignSystem::Typography::volumeBarFontSize, juce::Font::bold));

    const int percent = juce::roundToInt (std::abs (value) * 100.0);
    const auto text = percent == 0 ? juce::String ("C") : (juce::String (percent) + (value < 0.0 ? "L" : "R"));
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
}

void TrackHeaderComponent::PanBar::mouseDown (const juce::MouseEvent&)
{
    valueOnDragStart = value;

    if (onDragStart)
        onDragStart();
}

void TrackHeaderComponent::PanBar::mouseDrag (const juce::MouseEvent& e)
{
    // Same getDistanceFromDragStartX() relative-drag mechanic as VolumeBar —
    // full drag width sweeps the full bipolar range (2.0, i.e. -1..1).
    constexpr float pixelsForFullRange = DS::volumeBarDragPixelsForFullRange;
    const double delta = (double) e.getDistanceFromDragStartX() / (double) pixelsForFullRange * 2.0;
    setValue (valueOnDragStart + delta, juce::sendNotification);
}

void TrackHeaderComponent::PanBar::mouseUp (const juce::MouseEvent&)
{
    if (onDragEnd)
        onDragEnd();
}

void TrackHeaderComponent::PanBar::mouseDoubleClick (const juce::MouseEvent&)
{
    setValue (0.0, juce::sendNotification); // reset to centre ("C")
}

TrackHeaderComponent::TrackHeaderComponent (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse)
    : track (trackToControl), workflow (workflowToUse)
{
    if (track != nullptr)
    {
        isReturnTrackFlag = TrackUtils::isReturnTrack (*track);

        // Bus/Return Default Collapsed State directive: seeds the fold
        // micro-state from the track's own persisted property — a brand-new
        // FX Return track is stamped folded=true at creation time (see
        // CrateWorkflowManager::createAndRouteNewFXChannel()), so it opens as
        // a minimal collapsed strip; every other track defaults to false
        // (expanded) exactly as before. toggleFold() writes this same
        // property back, so a manual fold/unfold round-trips too.
        isCollapsed = (bool) track->state.getProperty (foldedPropertyID, false);

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

    // Column 3's Box A: the track-number plate IS the Mute toggle. Number
    // read once from the engine's own 1-based ordering (getAudioTrackNumber());
    // every add/delete rebuilds every header from scratch, so renumbering
    // falls out for free. Toggle state == isMuted (true polarity, no inversion).
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
    refreshIdentityContrast(); // sets Column 1's effective fill + name/number text colours together
    // Column 1 Click Area directive: the label must NOT swallow clicks — the
    // ENTIRE Column 1 background (not just the small unclaimed padding
    // around it) has to trigger track selection via this component's own
    // mouseDown(). Rename still works, just via the right-click "rename +
    // colour" menu (showNameColourEditor()) rather than an inline
    // double-click-to-edit-in-place, which would be unreachable now that
    // clicks pass straight through anyway.
    nameLabel.setInterceptsMouseClicks (false, false);

    // Column 2: Two-Tier Ableton-style I/O (Hybrid Bus/Return Architecture
    // directive). Flat + dark — strip the V4 combo's outline (transparent)
    // and bevel; fill with DarkBackground, text BrandGray, arrow NeonBlue.
    // Display-only: no input-device enumeration or output-bus routing exists
    // in this engine (grepped — no AuxReturnPlugin, no input-device-category
    // model anywhere), so none of these carry an onChange that mutates real
    // routing. The ONE exception is outputSpecificCombo's "Master" item,
    // which still shows the track's real destination name, same as the
    // single output combo this replaces used to.
    const auto styleFlatCombo = [this] (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, CrateColors::DarkBackground);
        c.setColour (juce::ComboBox::outlineColourId,    juce::Colours::transparentBlack);
        c.setColour (juce::ComboBox::textColourId,       CrateColors::BrandGray);
        c.setColour (juce::ComboBox::arrowColourId,      CrateColors::NeonBlue);
        c.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colours::transparentBlack);
        c.setJustificationType (juce::Justification::centredLeft);

        // Strict I/O Grid directive — flat data-table chrome, crisp 1px
        // border, minimalist 'V' arrow (see FlatGridComboLookAndFeel).
        c.setLookAndFeel (&flatGridLookAndFeel);
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
    // Content-Driven Dynamic Height directive: seeded from the track's own
    // persisted state, not always defaulting to "No Input" — getPreferredHeight()
    // depends on this, so it must survive a rebuild exactly like Fold state does.
    inputCategoryCombo.setSelectedId (
        track != nullptr ? (int) track->state.getProperty (inputCategoryPropertyID, DS::noInputCategoryId) : DS::noInputCategoryId,
        juce::dontSendNotification);
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

        constexpr int resamplingCategoryId = 3;
        const auto categoryId = inputCategoryCombo.getSelectedId();

        if (categoryId == DS::noInputCategoryId)
        {
            // "No Input" Logic: nothing to pick — updateInputDependentVisibility()
            // hides the combo (and Monitor triad, and Record Arm) entirely
            // rather than showing an empty dropdown.
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

        updateInputDependentVisibility();
    };
    refreshInputSpecificItems();
    inputCategoryCombo.onChange = [this, refreshInputSpecificItems]
    {
        // Content-Driven Dynamic Height directive: this may add/remove the
        // Input Specific row AND the Monitor triad AND Record Arm — persist
        // the new category, then tell the row/lane owner this header's
        // preferred height may have just changed.
        if (track != nullptr)
            track->state.setProperty (inputCategoryPropertyID, inputCategoryCombo.getSelectedId(), &track->edit.getUndoManager());

        refreshInputSpecificItems();
        resized();

        if (onFoldToggle)
            onFoldToggle();
    };

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
    // Content-Driven Dynamic Height directive: seeded from persisted state,
    // same reasoning as inputCategoryCombo above.
    outputCategoryCombo.setSelectedId (
        track != nullptr ? (int) track->state.getProperty (outputCategoryPropertyID, DS::masterOutputCategoryId) : DS::masterOutputCategoryId,
        juce::dontSendNotification);
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
    outputCategoryCombo.onChange = [this, refreshOutputSpecificItems]
    {
        // Content-Driven Dynamic Height directive: may add/remove the Output
        // Specific row — persist, then notify the row/lane owner.
        if (track != nullptr)
            track->state.setProperty (outputCategoryPropertyID, outputCategoryCombo.getSelectedId(), &track->edit.getUndoManager());

        refreshOutputSpecificItems();
        resized();

        if (onFoldToggle)
            onFoldToggle();
    };

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

    // Column 3: Pan — Ableton Geometry / PNG Pivot directive: a flat
    // horizontal drag-box (PanBar), not the pan_knob.png rotary MixerStrip/
    // MasterStrip still use elsewhere — a knob breaks Column 3's strict
    // row-height grid. Bound strictly via te::AutomatableParameter (panParam)
    // — no AudioProcessorParameterAttachment (wrong framework for Tracktion
    // Engine).
    addAndMakeVisible (panBar);

    if (volumePlugin != nullptr)
    {
        volumeSlider.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
        volumeSlider.onValueChange = [this]
        {
            if (volumePlugin != nullptr)
                volumePlugin->setVolumeDb ((float) volumeSlider.getValue());
        };

        panBar.setValue (volumePlugin->getPan(), juce::dontSendNotification);
        // Pan lifecycle mirrors MixerStrip's: an explicit undo transaction +
        // parameterChangeGesture pair around the drag, so a pan tweak is one
        // undoable step and TE's automation sees a proper gesture, not a stream
        // of unrelated value writes.
        panBar.onDragStart = [this]
        {
            if (onSelect) onSelect();
            if (volumePlugin == nullptr) return;
            volumePlugin->panParam->getEdit().getUndoManager().beginNewTransaction (
                "Tweak Pan: " + (track != nullptr ? track->getName() : juce::String()));
            volumePlugin->panParam->parameterChangeGestureBegin();
        };
        panBar.onDragEnd = [this]
        {
            if (volumePlugin != nullptr) volumePlugin->panParam->parameterChangeGestureEnd();
        };
        panBar.onValueChange = [this]
        {
            if (volumePlugin != nullptr)
                volumePlugin->setPan ((float) panBar.getValue());
        };

        // Bidirectional sync with MixerStrip: fires when Volume/Pan change from
        // that strip's fader/pan knob (or automation, or a script), not just from
        // this header's own controls. currentValueChanged() refreshes BOTH the
        // volume slider and the pan bar now that the header owns a real pan control.
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

    inputCategoryCombo.setLookAndFeel (nullptr);
    inputSpecificCombo.setLookAndFeel (nullptr);
    outputCategoryCombo.setLookAndFeel (nullptr);
    outputSpecificCombo.setLookAndFeel (nullptr);

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
            safeThis->refreshIdentityContrast(); // Column 1's fill colour may have just changed
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

    panBar.setValue (volumePlugin->getPan(), juce::dontSendNotification);
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

juce::Colour TrackHeaderComponent::getIdentityFillColour() const
{
    // Restore Column 1 directive: the base fill — strictly trackColor (no
    // mute-dimming any more, that's Box A's own concern in Column 3 now).
    // Return & Master Track Colors directive: a return track never shows its
    // own (possibly randomly-assigned) te::Track colour — forced to a fixed
    // neutral grey.
    const auto baseColour = isReturnTrackFlag
                                ? CrateColors::BrandGray
                                : (track != nullptr && ! track->getColour().isTransparent())
                                    ? track->getColour()
                                    : headerDefault;

    // Anti-Fatigue / Alpha-Blend Selection directive: selection is a 40%
    // white overlay ON TOP of the base colour, not a flat whitesmoke block
    // that erases the track's identity entirely — still reads as brightly
    // highlighted, but the underlying trackColor (or Return/Master grey)
    // stays visible through it.
    if (selected)
        return baseColour.interpolatedWith (juce::Colours::white, 0.4f);

    return baseColour;
}

void TrackHeaderComponent::refreshIdentityContrast()
{
    // Restore Column 1 directive: only the name label's text colour reads
    // off trackColor.contrasting() now — pure white or pure black, whichever
    // Column 1's fill actually needs to stay legible.
    nameLabel.setColour (juce::Label::textColourId, getIdentityFillColour().contrasting());
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

        // Column 1: The Selection Behaviour directive — getIdentityFillColour()
        // just changed (whitesmoke vs trackColor), so the name label's text
        // colour must be recomputed to stay legible against whichever it is now.
        refreshIdentityContrast();
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
    // Stroke Occlusion Fix directive: this is now FILLS ONLY — every border/
    // separator stroke moved to paintOverChildren() (see its own doc comment
    // and the .h declaration) since JUCE paints child components (Column 2's
    // combos/buttons) strictly between this method and that one; a stroke
    // drawn here that overlapped a child's bounds got silently painted over
    // the instant that child rendered itself.

    // Hardware Lift directive: Columns 2 + 3's panel is LightBackground, not
    // DarkBackground — visibly lifts this track panel off the main
    // arrangement void behind it.
    g.fillAll (CrateColors::LightBackground);

    // Fused Identity Block directive: the ENTIRE column is filled SOLID with
    // getIdentityFillColour() — Column 1: The Selection Behaviour directive:
    // a bright fixed highlight when selected (completely overriding
    // trackColor), otherwise the track's real colour (dimmed if muted), or
    // BrandGray for a return track — exactly like Ableton, not a translucent
    // tint over the whole header. Strict Bordered Grid directive: no
    // left-edge accent stripe drawn over this any more — Column 1 is a BOLD,
    // full, uninterrupted block.
    if (! column1Bounds.isEmpty())
    {
        g.setColour (getIdentityFillColour());
        g.fillRect (column1Bounds);
    }

    // Column 2 Folded Placeholder directive: a blank, disabled dummy box
    // holds Column 2's grid line open when folded — same DarkBackground
    // "sunken LCD cutout" fill every real ComboBox in this column uses (see
    // FlatGridComboLookAndFeel), just with no text/arrow drawn into it.
    if (isCollapsed && ! collapsedRoutingPlaceholder.isEmpty())
    {
        g.setColour (CrateColors::DarkBackground);
        g.fillRect (collapsedRoutingPlaceholder);
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

    // Premium drop-target glow for a Browser plugin drag — drawn last (on top of
    // every fill above), but STILL before paintOverChildren()'s strokes, so
    // the "grill" always reads on top of even this.
    if (isDragHovering)
    {
        g.setColour (LAF::accent.withAlpha (0.12f));
        g.fillRect (getLocalBounds());
        g.setColour (LAF::accent);
        g.drawRect (getLocalBounds(), 2);
    }
}

void TrackHeaderComponent::paintOverChildren (juce::Graphics& g)
{
    // Stroke Occlusion Fix directive: every 2px/1px border/separator lives
    // here now, drawn AFTER every child Component (Column 2's combos/
    // buttons, the Monitor triad, etc.) has painted itself — the strokes
    // act as a hardware "grill" laid on top, and nothing can ever hide them.

    // Global Column Separators directive: absolute, opaque 2px black cuts
    // between the main UI zones. Immutable Column Widths directive: these
    // draw at the SAME x coordinates whether folded or not — the 3-column
    // grid boundaries never move, so the separators never gate on
    // isCollapsed any more.
    g.setColour (juce::Colours::black);
    g.fillRect (DS::column1Right, 0, DS::separatorW, getHeight());
    g.fillRect (DS::column2Right, 0, DS::separatorW, getHeight());

    // Column 2 Folded Placeholder directive: crisp 1px opaque border around
    // the dummy box, same visual language as FlatGridComboLookAndFeel's real
    // ComboBox border — reads as "a disabled control", not a rendering gap.
    if (isCollapsed && ! collapsedRoutingPlaceholder.isEmpty())
    {
        g.setColour (juce::Colours::black);
        g.drawRect (collapsedRoutingPlaceholder, 1);
    }

    // Monitor Triad Borders directive: each IN/AUTO/OFF button already draws
    // its own 1px border (see MonitorButton::paintButton) — this is the
    // OUTER frame around the whole triad as one block, read straight from
    // the three buttons' own live bounds (resized() math untouched) so it
    // can never drift out of sync with wherever they actually are.
    if (monitorInButton.isVisible())
    {
        const auto triadBounds = monitorInButton.getBounds()
                                     .getUnion (monitorAutoButton.getBounds())
                                     .getUnion (monitorOffButton.getBounds());
        g.setColour (juce::Colours::black);
        g.drawRect (triadBounds, 1);
    }

    // Global Column Separators directive: opaque 2px black cuts at the
    // ABSOLUTE top (y=0) and bottom (y=height-2) of the entire header,
    // spanning the full width — strict separation from the timeline grid
    // and adjacent tracks.
    g.setColour (juce::Colours::black);
    g.fillRect (0, 0, getWidth(), DS::rowBottomBorderH);
    g.fillRect (0, getHeight() - DS::rowBottomBorderH, getWidth(), DS::rowBottomBorderH);

    // Right-edge separator — the strict boundary against the timeline/
    // arrangement grid. Same opaque 2px cut.
    g.fillRect (getWidth() - DS::separatorW, 0, DS::separatorW, getHeight());
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

    // Bus/Return Default Collapsed State directive: persists the fold
    // micro-state on the track's own ValueTree — same round-trips-through-
    // save/load treatment armedPropertyID already gets, so a track a user
    // explicitly folded/unfolded stays that way across a project reload,
    // not just this session (and CrateWorkflowManager stamps this same
    // property true for a brand-new FX Return track, so it opens folded).
    if (track != nullptr)
        track->state.setProperty (foldedPropertyID, isCollapsed, &track->edit.getUndoManager());

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
    // Column 2 (Routing) + the pan knob + volume drag-box are the controls
    // that only exist in the expanded state.
    const bool showInputControls = ! isCollapsed && ! isReturnTrackFlag;

    // Hybrid Bus/Return Architecture (Ableton Accuracy): return tracks don't
    // record from external inputs — Input Category/Specific and the
    // IN/AUTO/OFF monitor triad never show for one, expanded or not.
    inputCategoryCombo.setVisible (showInputControls);
    updateInputDependentVisibility(); // "No Input" Logic: also hides Input Specific + Monitor triad + Record Arm
    outputCategoryCombo.setVisible (! isCollapsed);
    updateOutputSpecificVisibility(); // Output Combo Box Logic: also hides when category == Master
    volumeSlider.setVisible (! isCollapsed);
    panBar.setVisible (! isCollapsed);
}

void TrackHeaderComponent::updateInputDependentVisibility()
{
    // "No Input" & Monitor Logic directive: selecting "No Input" doesn't just
    // grey out the Input Specific combo — it REMOVES the Input Specific row,
    // the entire IN/AUTO/OFF Monitor triad, AND (expanded state only) Record
    // Arm, exactly like real Ableton (arming record with no input source is
    // meaningless). Collapsed micro-state keeps showing Record/Post-Pre
    // regardless — that compact strip's content is unrelated to this rule.
    const bool showExpandedInputRows = ! isCollapsed && ! isReturnTrackFlag;
    const bool hasInput = inputCategoryCombo.getSelectedId() != DS::noInputCategoryId;

    inputSpecificCombo.setVisible (showExpandedInputRows && hasInput);
    monitorInButton.setVisible   (showExpandedInputRows && hasInput);
    monitorAutoButton.setVisible (showExpandedInputRows && hasInput);
    monitorOffButton.setVisible  (showExpandedInputRows && hasInput);

    if (! isReturnTrackFlag)
        recordArmButton.setVisible (isCollapsed || hasInput);
}

void TrackHeaderComponent::updateOutputSpecificVisibility()
{
    outputSpecificCombo.setVisible (! isCollapsed && outputCategoryCombo.getSelectedId() != DS::masterOutputCategoryId);
}

void TrackHeaderComponent::resized()
{
    applyCollapsedVisibility();

    if (isCollapsed)
        layoutCollapsed (getLocalBounds());
    else
        layoutExpanded (getLocalBounds());
}

int TrackHeaderComponent::getPreferredHeight() const
{
    return computePreferredHeight (isReturnTrackFlag, isCollapsed,
                                    inputCategoryCombo.getSelectedId(),
                                    outputCategoryCombo.getSelectedId());
}

int TrackHeaderComponent::computePreferredHeight (bool isReturnTrack, bool isCollapsedState,
                                                   int inputCategoryId, int outputCategoryId)
{
    if (isReturnTrack)
        return isCollapsedState ? DS::returnCollapsedHeight : DS::returnExpandedHeight;

    if (isCollapsedState)
        return DS::collapsedHeight;

    // Column 2's dynamic row count: input block (1 or 2 rows) + monitor
    // triad (0 or 1) + output block (1 or 2 rows).
    const bool hasInput = inputCategoryId != DS::noInputCategoryId;
    const bool hasOutputSpecific = outputCategoryId != DS::masterOutputCategoryId;

    const int inputRows  = hasInput ? 2 : 1;
    const int monitorRows = hasInput ? 1 : 0;
    const int outputRows = hasOutputSpecific ? 2 : 1;
    const int totalRows  = inputRows + monitorRows + outputRows;

    // 2px Breathing Room directive: (numVisibleRows * 13) +
    // ((numVisibleRows - 1) * 4) + topInset(4) + bottomInset(4) = +8px total
    // — MUST match layoutExpanded()'s col2 construction (reduced(col2Inset)
    // + withTrimmedTop/Bottom(rowBottomBorderH), DS::col2RowGap between
    // rows) exactly, term for term, or the bottom rows compress.
    const int column2Height = totalRows * DS::rowH + (totalRows - 1) * DS::col2RowGap
                                 + 2 * (DS::col2Inset + DS::rowBottomBorderH);

    // Column 3's content is fixed regardless of state — the floor under
    // Column 2's dynamic range (see CrateDesignSystem.h's own doc comment).
    return juce::jmax (column2Height, DS::column3FixedHeight);
}

void TrackHeaderComponent::layoutExpanded (juce::Rectangle<int> full)
{
    // Content-Driven Dynamic Height directive: column WIDTHS stay fixed
    // (0/90/180 boundaries), only HEIGHT flexes — `full`'s height already
    // equals getPreferredHeight() (the row/lane owner sized us to that
    // before calling resized(), same contract getPreferredHeight()'s own
    // doc comment describes). Every row below is stacked top-down and
    // conditionally, not a literal hardcoded rect any more.

    // ---- Column 1 (Identity Only): x=[0,90), full height ---------------------
    layoutIdentityColumn (full);

    // ---- Column 2 (Routing): x=[90,180), dynamic top-down stack --------------
    // Column 2 Breathing Room directive: a strict 2px inset on all four
    // sides — rows no longer stretch flush against the Column 1/2, Column
    // 2/3, top, or bottom boundaries. computePreferredHeight() accounts for
    // the added 4px (2 top + 2 bottom) so nothing gets cut off.
    //
    // Column 2 Left Collision directive: the Column 1/2 divider line itself
    // is drawn 2px WIDE (DS::separatorW), occupying x=[90,92) — a plain
    // .reduced(col2Inset) alone only pushes content to x=92, i.e. flush
    // against the line's OWN far edge with zero visible gap after it. The
    // Column 2/3 side doesn't have this problem (that divider starts AT
    // column2Right, so the right-side .reduced() already clears it with a
    // real gap) — only the left side needs the line's width trimmed off on
    // top of the normal inset, which is what actually gives it "breathing
    // room to match the right side."
    //
    // 2px Breathing Room directive: startY MUST be 4 — 2px for the
    // paintOverChildren() global stroke itself (DS::rowBottomBorderH) PLUS a
    // further 2px of truly EMPTY space (DS::col2Inset) before content
    // begins. reduced(col2Inset) alone only reaches y=2, flush against the
    // stroke's own far edge with zero visible gap — trimming the stroke's
    // own width off on top of the normal inset is what actually clears it.
    // Same treatment on the bottom (4px clearance from the header's
    // absolute bottom edge). computePreferredHeight() adds this exact 8px
    // (4 top + 4 bottom) so nothing gets compressed.
    auto col2 = full.withX (DS::column1Right).withWidth (DS::column2Right - DS::column1Right)
                    .reduced (DS::col2Inset)
                    .withTrimmedLeft (DS::separatorW)
                    .withTrimmedTop (DS::rowBottomBorderH)
                    .withTrimmedBottom (DS::rowBottomBorderH);

    if (isReturnTrackFlag)
    {
        // Return/FX Track Dynamics directive: only the routing dropdown(s) —
        // Input Category/Specific + the Monitor triad never exist for a
        // return at all (applyCollapsedVisibility() keeps them hidden).
        outputCategoryCombo.setBounds (col2.removeFromTop (DS::rowH));

        if (outputSpecificCombo.isVisible())
        {
            col2.removeFromTop (DS::col2RowGap);
            outputSpecificCombo.setBounds (col2.removeFromTop (DS::rowH));
        }
    }
    else
    {
        // "No Input" & Monitor Logic directive: each row below only claims
        // space if updateInputDependentVisibility()/updateOutputSpecificVisibility()
        // actually left it visible — a hidden row contributes ZERO height,
        // so everything after it shifts up instead of leaving dead space.
        // Column 2 Row Gaps directive: a gap is only inserted BEFORE a row
        // that's actually about to be laid out — never a dangling gap with
        // nothing after it.
        inputCategoryCombo.setBounds (col2.removeFromTop (DS::rowH));

        if (inputSpecificCombo.isVisible())
        {
            col2.removeFromTop (DS::col2RowGap);
            inputSpecificCombo.setBounds (col2.removeFromTop (DS::rowH));
        }

        if (monitorInButton.isVisible())
        {
            col2.removeFromTop (DS::col2RowGap);

            // IN | AUTO | OFF — 3 equal buttons, 2px gaps. Blurry Text Fix
            // directive: explicit integer snap (juce::roundToInt over a
            // float divide) on the width split, rather than leaving it to
            // plain int truncation — belt-and-suspenders against any
            // sub-pixel drift feeding into the buttons' own text rendering.
            auto row = col2.removeFromTop (DS::rowH);
            constexpr int gap = 2;
            const int btnW = juce::roundToInt ((float) (row.getWidth() - 2 * gap) / 3.0f);
            monitorInButton.setBounds   (row.removeFromLeft (btnW));
            row.removeFromLeft (gap);
            monitorAutoButton.setBounds (row.removeFromLeft (btnW));
            row.removeFromLeft (gap);
            monitorOffButton.setBounds  (row); // remainder absorbs integer-division rounding
        }

        col2.removeFromTop (DS::col2RowGap);
        outputCategoryCombo.setBounds (col2.removeFromTop (DS::rowH));

        if (outputSpecificCombo.isVisible())
        {
            col2.removeFromTop (DS::col2RowGap);
            outputSpecificCombo.setBounds (col2.removeFromTop (DS::rowH));
        }
    }

    // ---- Column 3 (Mini-Mixer): x=[181,300) --------------------------------
    layoutColumn3 (full);
}

void TrackHeaderComponent::layoutIdentityColumn (juce::Rectangle<int> full)
{
    // Restore Column 1 directive: strictly the fold arrow + track name on the
    // full-height trackColor fill — no track number, no Mute interaction
    // here any more (that's Column 3's Box A now). Immutable Column Widths
    // directive: this same code runs for BOTH layoutExpanded() and
    // layoutCollapsed() — Column 1's x=[0,90) box and its two children's
    // positions (parametrized entirely off full.getHeight()) never change
    // shape between the two states, only the height they're centred/spread
    // across does.
    column1Bounds = { 0, 0, DS::column1Right, full.getHeight() };

    const int foldY = (full.getHeight() - DS::foldArrowSize) / 2;
    foldArrow.setBounds (DS::identityPad, foldY, DS::foldArrowSize, DS::foldArrowSize);

    const int nameX = DS::identityPad + DS::foldArrowSize + DS::identityPad;
    const int nameW = DS::column1Right - nameX - DS::identityPad;

    if (isCollapsed)
    {
        // Text Baseline Symmetry directive: folded state doesn't stretch the
        // name label across the full row height any more — it gets the
        // SAME y/height (13px, absolute-centred on the current folded
        // height) as Column 2's dummy box and Column 3's Box A/B/C, so the
        // name's own vertical centring inside that 13px band lines its
        // baseline up with theirs exactly, instead of two independently
        // "centred" boxes of different heights only looking approximately aligned.
        const int foldedY = (full.getHeight() - DS::rowH) / 2;
        nameLabel.setBounds (nameX, foldedY, nameW, DS::rowH);
    }
    else
    {
        nameLabel.setBounds (nameX, 0, nameW, full.getHeight()); // vertically centred by the label's own Justification::centredLeft
    }
}

void TrackHeaderComponent::layoutColumn3 (juce::Rectangle<int> full)
{
    // Ableton Geometry directive: ABSOLUTE, hardcoded pixel math — no
    // FlexBox/relative-percentage builders. Column 3's own bounds are a
    // fixed (col3Width x col3Height) block starting 1px past the Column 2/3
    // divider hairline; every child position below is literal integer
    // arithmetic within THAT block's local coordinate space, exactly per
    // spec (comments show the same math the directive itself specified).
    //
    // Immutable Column Widths directive: this method is now the ONE place
    // Column 3's geometry is computed, called from BOTH layoutExpanded() and
    // layoutCollapsed() with identical results — folding a track can never
    // shift Box A/B/C, the meter, or shrink Column 3's width, since there is
    // no second hand-maintained copy of these numbers left to drift.
    constexpr int col3Width  = DS::col3Width;  // 119 — height is full.getHeight() now, see the Meter comment below
    const int col3X = DS::column2Right + 1;    // 181 — 1px past the divider line

    // The Meter (right-aligned anchor) — width 7, 10px from Column 3's right
    // edge, with a guaranteed 3px of empty space above AND below it (Meter
    // Padding directive — 2px read as touching the row dividers in practice,
    // so this is the more generous fallback). Height tracks the header's
    // ACTUAL current height (full.getHeight()), not the fixed col3Height
    // literal — Column 2 Row Gaps can make a track taller than the 86px
    // floor, and the meter must still never touch that taller box's own
    // top/bottom edges.
    constexpr int meterVerticalPad = 3;
    meterBounds = { col3X + col3Width - 10 - 7, meterVerticalPad, 7, full.getHeight() - meterVerticalPad * 2 };

    // Row 1: Box A (Mute Plate, track number) | Box B (Solo) | Box C (Record
    // — Ableton parity: only rendered when input isn't "No Input"). 4px
    // gaps: Box A right edge (2+46=48) -> Box B at 52; Box B right edge
    // (52+21=73) -> Box C at 77; Box C right edge (77+21=98) -> Meter at 102
    // (col3X + 119 - 17 = col3X + 102) — the exact 4px gap the directive
    // calls out.
    //
    // Expanded: y=4 (2px Breathing Room directive — identical startY to
    // Column 2's first row [2px stroke + 2px empty gap], so Box A's top
    // edge forms a perfect horizontal line with Column 2's top row).
    //
    // Absolute Y-Axis Centering directive: folded state does NOT reuse that
    // startY=4 — it's improving on Ableton's cramped collapsed rows, not
    // matching them, so Box A/B/C centre on the actual (breathable) folded
    // track height instead, landing on the same 13px band Column 1's name
    // label and Column 2's dummy box both centre on too.
    const int col3Row1Y = isCollapsed ? (full.getHeight() - 13) / 2 : 4;
    const int col3Row2Y = col3Row1Y + 13 + 4; // Row 1 bottom + 4 — expanded-only (Row 2 is hidden when folded)

    mutePlate.setBounds  (col3X + 2,  col3Row1Y, 46, 13); // Box A
    soloButton.setBounds (col3X + 52, col3Row1Y, 21, 13); // Box B

    if (recordArmButton.isVisible())
        recordArmButton.setBounds (col3X + 77, col3Row1Y, 21, 13); // Box C
    // else: "No Input" & Monitor Logic directive — Box C is hidden entirely,
    // its slot stays blank (NOT absorbed by Box B, which stays fixed-width
    // per the mathematically-locked grid — Protect Column 3 Spacing directive).

    // Row 2: Volume directly under Box A at the SAME width; Pan — PNG Pivot
    // directive's flat drag-box, not a rotary knob — spans exactly the
    // Box B + gap + Box C width (21 + 4 + 21 = 46) directly under them.
    // Harmless to set bounds even when collapsed (volumeSlider/panBar are
    // hidden via applyCollapsedVisibility() in that state) — one geometry
    // source, whether or not the children are currently visible.
    volumeSlider.setBounds (col3X + 2,  col3Row2Y, 46, 13);
    panBar.setBounds       (col3X + 52, col3Row2Y, 46, 13);

    // Sends/Returns (Row 3+) directive: any FUTURE row below Row 2 follows
    // the identical y = previousRowBottom + 4 grid rule, aligned within the
    // same col3X + {2, 52} horizontal constraints above — no such row exists
    // yet (this component has no Sends UI of its own), so there is nothing
    // to lay out here today; this comment is the contract for whoever adds one.
}

void TrackHeaderComponent::layoutCollapsed (juce::Rectangle<int> full)
{
    // Immutable Column Widths directive: folding NEVER changes the 3-column
    // grid's x/width boundaries (0/90/180/300) — the old single-strip
    // micro-state (Column 1 stretching to swallow Column 2's space) is
    // gone. Column 1 and Column 3 reuse the EXACT same layout code the
    // expanded state runs (see layoutIdentityColumn()/layoutColumn3()), so
    // there is nothing left to drift out of sync when toggling the fold.
    layoutIdentityColumn (full);

    // ---- Column 2 (Routing): folded placeholder ---------------------------
    // Column 2 Folded Placeholder directive: Ableton doesn't leave this
    // column empty when folded — it keeps a single blank, disabled
    // ComboBox-styled box in place to hold the grid line. Width/x stay
    // identical to the expanded state's row (same left-divider-clearance
    // math), but the Absolute Y-Axis Centering directive means the Y
    // position does NOT reuse the expanded startY=4 any more — it centres
    // on the actual folded track height instead, landing on the same 13px
    // band Column 1's name label and Column 3's Box A/B/C both centre on.
    // paint()/paintOverChildren() draw its fill/border, gated on isCollapsed
    // — there's no live ComboBox component here to draw itself.
    auto col2 = full.withX (DS::column1Right).withWidth (DS::column2Right - DS::column1Right)
                    .reduced (DS::col2Inset)
                    .withTrimmedLeft (DS::separatorW);
    const int foldedY = (full.getHeight() - DS::rowH) / 2;
    collapsedRoutingPlaceholder = { col2.getX(), foldedY, col2.getWidth(), DS::rowH };

    // ---- Column 3 (Mini-Mixer): x=[181,300) --------------------------------
    layoutColumn3 (full);
}
