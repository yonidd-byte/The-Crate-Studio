#include "MixerStrip.h"
#include "TheCrateLookAndFeel.h"
#include "CrateColors.h"
#include "CrateDesignSystem.h"
#include "PluginSlotComponent.h"
#include "CrateSendSlot.h"
#include "CrateEQThumbnail.h"
#include "CrateCompressorPopup.h"
#include "InsertsRackComponent.h"
#include "TrackColourEditor.h"
#include "AddIconButton.h"
#include "SendBusUtils.h"
#include "TrackUtils.h"

#include <map>
#include <set>

namespace
{
    using LAF = TheCrateLookAndFeel;

    // QA Hardening fix — te::AudioTrack::setMute()/setSolo() write straight
    // through a juce::CachedValue bound with a NULLPTR UndoManager (verified
    // against tracktion_AudioTrack.cpp: muted/soloed .referTo(state, IDs::...,
    // nullptr) in its constructor), so those calls are NOT undoable no matter
    // how the CALLER wraps them — a beginNewTransaction() around a plain
    // track->setMute() call does nothing, because the nullptr is baked into
    // the CachedValue itself at construction, not chosen per call. The value
    // still PERSISTS (it's a real ValueTree property write, survives Edit
    // save/reload), just without Ctrl+Z. Fixed here with an explicit
    // UndoableAction that re-applies old/new state itself, independent of
    // CachedValue's own broken undo wiring — no engine files touched.
    class TrackMuteSoloAction : public juce::UndoableAction
    {
    public:
        TrackMuteSoloAction (te::AudioTrack::Ptr t, bool isSoloAction, bool newState)
            : track (t), isSolo (isSoloAction), newValue (newState),
              oldValue (t != nullptr ? (isSoloAction ? t->isSolo (false) : t->isMuted (false)) : false) {}

        bool perform() override { apply (newValue); return true; }
        bool undo() override    { apply (oldValue); return true; }

    private:
        void apply (bool value) const
        {
            if (track == nullptr)
                return;

            if (isSolo) track->setSolo (value);
            else        track->setMute (value);
        }

        te::AudioTrack::Ptr track;
        bool isSolo, newValue, oldValue;
    };

    // Role colours — Lead Architect's "Ghost Buttons" spec: matte, desaturated
    // console colours for the R/S/I triad (Mute has no button anymore — the
    // name plate is the Mute toggle, see applyTrackColourToPlate()). Global
    // Color Centralization & Purge directive: these now read from
    // CrateColors' own centralized status colours instead of independent
    // local literals, so Solo/Record render identically here and in
    // TrackHeaderComponent (previously this file used its own dark-goldenrod
    // Solo shade, visibly different from the Arrangement header's bright
    // yellow — that split is gone now).
    const auto soloOnColour   = CrateColors::SoloYellow;
    const auto recordOnColour = CrateColors::RecordCrimson;
    const auto sectionCaption = CrateColors::BrandGray;

    // Cubase-style pan readout — "C" dead centre, otherwise the percentage
    // toward whichever side, with a small arrow flanking the OUTER edge
    // (pointing away from centre) as a directional hint.
    juce::String panValueText (float pan)
    {
        const int percent = juce::roundToInt (std::abs (pan) * 100.0f);
        if (percent == 0)
            return "C";
        return pan < 0.0f ? ("< " + juce::String (percent) + " L")
                           : (juce::String (percent) + " R >");
    }

    // Design System Centralization: thin aliases into
    // CrateDesignSystem::Metrics::ChannelStrip — the SAME values MasterStrip
    // reads (see that file's own identical alias block and
    // CrateDesignSystem.h's doc comment on why the two are deliberately
    // unified, not independently duplicated — this is what keeps the two
    // strips' fader rails from drifting out of alignment again).
    namespace DS = CrateDesignSystem::Metrics::ChannelStrip;
    namespace DSMixer = CrateDesignSystem::Metrics::Mixer;

    constexpr float meterFloorDb = DS::meterFloorDb;
    constexpr float meterRangeDb = DS::meterRangeDb; // floor to +6 dB headroom

    constexpr int meterColumnWidth   = DS::meterColumnWidth;
    constexpr int grMeterColumnWidth = DSMixer::grMeterColumnWidth;

    // ---- Strict bottom-up level heights (see MixerStrip.h's anatomy diagram) --
    constexpr int levelGap   = DS::levelGap;
    constexpr int nameH      = DS::nameH; // L1  track-name plate — IS the Mute toggle now (Ableton Mute paradigm)
    constexpr int tripletH   = DS::tripletH; // R/S/I triad row, directly above the name plate
    constexpr int dbReadoutH = DS::dbReadoutH; // L5  dB readout boxes
    constexpr int panH       = DS::panH; // L6  pan knob — extracted, sits below the Routing well now
    constexpr int faderMinH  = DSMixer::faderMinH; // L4 fader block minimum (it stretches past this)

    // Scribble Strip (L1) — embedded icon (no plate/border, drawn straight
    // onto the dark void background) at the absolute bottom, track name
    // (Mute toggle) directly above it. The old dedicated M button and 4px
    // colour strip are GONE — the name plate's own fill IS the colour cue now
    // (see applyTrackColourToPlate()).
    constexpr int scribbleIconH  = DS::scribbleIconH;
    constexpr int scribbleGap    = DS::scribbleGap;

    // Pan value readout, directly below the (now-extracted) Pan knob.
    constexpr int panValueH   = DS::panValueH;
    constexpr int panValueGap = DS::panValueGap;

    // ---- Deep stack (L9–L14), only laid out when the rack is expanded ---------
    // L9 Routing is now DYNAMIC height (RoutingBlock::getPreferredHeight()) —
    // routingRowH alone when the Group chip is hidden (no group assigned;
    // always true today, see RoutingBlock::setGroupName()'s doc comment),
    // routingRowH*2 + routingRowGap when a group IS assigned and its chip
    // shows. coreStripHeight()/deepStackHeight() below use the single-row
    // value since that's the only reachable state right now — MixerComponent
    // shares ONE row height across every strip (see its own doc comment), so
    // a per-track-dynamic well height here would need that architecture to
    // change too, once real track-group data exists.
    constexpr int routingRowH   = DS::routingRowH;
    constexpr int routingRowGap = DSMixer::routingRowGap;
    constexpr int sendsH    = DSMixer::sendsH; // L10 sends
    // L11 inserts height == InsertsRackComponent::getFixedHeight()
    constexpr int compH     = DS::compH; // L12 channel comp block — a neat single-slot-height rect (it only opens a popup)
    constexpr int eqH       = DS::eqH; // L13 EQ display
    constexpr int settingsH = DS::settingsH; // L14 settings button

    constexpr int outerMargin = DS::outerMargin; // strict 4px margin — name plate / R-S-I triad / fader / pan / icon family only

    // "Universal Rack Width" (Absolute Symmetry directive) — ONE constant
    // positions EVERY dark rack container: Comp, Inserts well, Sends well,
    // Routing well. No exceptions, no per-block margin scheme — this is the
    // single source of X/width for all four, so their edges are guaranteed to
    // line up top-to-bottom. universalWidth = getWidth() - rackMargin*2.
    // (InsertsRackComponent draws its own fill/border across its full bounds,
    // so giving it these exact bounds in resized() makes it BE the well;
    // Sends/Routing get a separately hand-painted well in paint() sized off
    // this same constant; Comp is a raised button, not a well, but shares the
    // identical bounding box so its edges still align with the others.)
    // Bus 1 rows sit rackButtonPadding further in again inside Sends
    // specifically (scrollbar clearance) — an internal detail, not a
    // different CONTAINER margin, so it doesn't violate the rule above.
    constexpr int rackMargin        = DSMixer::rackMargin;
    constexpr int rackButtonPadding = DSMixer::rackButtonPadding;

    // Strict breathing room between the SENDS block and the Routing block —
    // bigger than the ordinary levelGap so the two dark wells visibly never
    // touch (both wells wrap their component snugly, zero extra vertical
    // padding, so this IS the exact gap that ends up on screen between them).
    constexpr int sendsToRoutingGap = DSMixer::sendsToRoutingGap;

    const juce::String pluginDragPrefix = "plugin_drag|";

    int coreStripHeight()
    {
        return outerMargin * 2
             + scribbleIconH + scribbleGap + nameH + levelGap // L1 Scribble Strip (icon below the Mute-toggle name plate)
             + tripletH + levelGap         // R/S/I triad
             + faderMinH + levelGap        // L4
             + dbReadoutH + levelGap       // L5
             + panValueH + panValueGap     // pan value readout, tight gap below the knob
             + panH;                       // L6 — topmost core term now that Read (L8) is gone
    }

    int deepStackHeight()
    {
        return routingRowH + levelGap       // L9 Routing (single-row baseline) -> Pan knob gap
             + sendsToRoutingGap            // strict Sends<->Routing well gap, not the ordinary levelGap
             + sendsH + levelGap
             + InsertsRackComponent::getFixedHeight() + levelGap
             + compH + levelGap
             + eqH + levelGap
             + settingsH + levelGap;
    }
}

//==============================================================================
// L9 — Routing: OUT 1+2 (a REAL functional ComboBox — the output routing logic
// that used to live in the dissolved ChannelStripRack) alone, or stacked with
// the Group chip BELOW it when the track is actually assigned to a group.
// DYNAMIC height: getPreferredHeight() reflects whichever state groupSlot is
// currently in, and MixerStrip::resized()/paint() both read that back so the
// well can never wrap a hidden chip's dead space.
struct MixerStrip::RoutingBlock : public juce::Component
{
    RoutingBlock()
    {
        addAndMakeVisible (outputCombo);

        // groupSlot is a plain Label — it never routes through
        // HardwareSlotLookAndFeel (that only intercepts Button/ComboBox
        // painting), so its own backgroundColourId stays fully transparent
        // and paint() below draws the IDENTICAL bevel by hand, using the
        // exact same constants outputCombo's LookAndFeel draws with, so the
        // two chips can never visually drift apart. Hidden by default —
        // see setGroupName()'s doc comment on why this is the only reachable
        // state today.
        addAndMakeVisible (groupSlot);
        groupSlot.setJustificationType (juce::Justification::centred);
        groupSlot.setColour (juce::Label::textColourId, HardwareSlotLookAndFeel::dimTextColour);
        groupSlot.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        groupSlot.setFont (juce::FontOptions (CrateDesignSystem::Typography::groupSlotFontSize, juce::Font::bold));
        groupSlot.setVisible (false);
    }

    /** Empty groupName hides the chip entirely (getPreferredHeight() shrinks
        to just OUT 1+2); a non-empty name shows it with that text. Tracktion
        Engine has no track-group concept to query yet (verified — no
        TrackGroup/getGroupID API exists on te::Track, only unrelated per-clip
        grouping), so every call site today passes an empty name and this
        chip is honestly always hidden until that data model exists. */
    void setGroupName (const juce::String& groupName)
    {
        const bool isGrouped = groupName.isNotEmpty();
        if (isGrouped)
            groupSlot.setText (groupName, juce::dontSendNotification);
        groupSlot.setVisible (isGrouped);
        resized();
    }

    int getPreferredHeight() const
    {
        return groupSlot.isVisible() ? (routingRowH * 2 + routingRowGap) : routingRowH;
    }

    void resized() override
    {
        auto b = getLocalBounds();
        if (groupSlot.isVisible())
        {
            outputCombo.setBounds (b.removeFromTop (routingRowH));
            b.removeFromTop (routingRowGap);
            groupSlot.setBounds (b);
        }
        else
        {
            outputCombo.setBounds (b);
        }
    }

    void paint (juce::Graphics& g) override
    {
        // Hand-drawn hardware-slot bevel for groupSlot ("No Group") — see the
        // constructor comment above for why this can't just go through the
        // LookAndFeel like outputCombo does. Only meaningful while visible.
        if (groupSlot.isVisible())
            HardwareSlotLookAndFeel::drawRaisedChip (g, groupSlot.getBounds().toFloat(), HardwareSlotLookAndFeel::fillColour);
    }

    juce::ComboBox outputCombo;
    juce::Label    groupSlot;
};

//==============================================================================
// L10 — Sends: a caption plus a vertical stack of CrateSendSlots (destination
// chip + mini level knob, one per te::AuxSendPlugin on the track) — the SAME
// shared component the Dual-Strip Inspector uses. Master has no sends, so
// MasterStrip omits this section entirely.
struct MixerStrip::SendsSection : public juce::Component
{
    // The scrollable INNER content — holds the actual CrateSendSlot stack.
    // sendsH (the OUTER SendsSection height) stays a fixed constant so every
    // strip's shared row height never changes; once more sends exist than fit,
    // this Content just grows taller than its Viewport and the Viewport's own
    // drag-scroll (scrollbars hidden) reveals the rest — same pattern as
    // CrateTrackInspectorComponent's own sendsViewport.
    struct Content : public juce::Component
    {
        void resized() override
        {
            auto b = getLocalBounds();
            for (int i = 0; i < getNumChildComponents(); ++i)
            {
                getChildComponent (i)->setBounds (b.removeFromTop (DSMixer::sendRowH)); // matches the hardware-chip family's row height elsewhere
                b.removeFromTop (DSMixer::sendRowGap);
            }
        }
    };

    SendsSection()
    {
        addAndMakeVisible (caption);
        caption.setText ("SENDS", juce::dontSendNotification);
        caption.setFont (juce::FontOptions (CrateDesignSystem::Typography::sendsCaptionFontSize, juce::Font::bold));
        caption.setColour (juce::Label::textColourId, sectionCaption);

        // "+" add-send affordance — MixerStrip's Sends had NO way to CREATE a
        // new send at all until this directive; onClick is wired externally
        // by MixerStrip (see its own addNewSend()), matching how
        // routingBlock->outputCombo.onChange etc. are wired from outside too.
        addAndMakeVisible (addSendButton);

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false); // vertical ONLY — the horizontal scrollbar path is fully disabled
        viewport.setScrollBarThickness (DSMixer::sendsScrollbarThickness); // slim gutter; drawScrollbar only paints a 4px thumb centred inside it
        // Bug fix: ScrollOnDragMode::all meant dragging a Send knob (rather
        // than empty space) also kicked off the Viewport's own drag-to-scroll,
        // fighting the knob for the same gesture. The custom auto-hide
        // scrollbar thumb above is still fully drag-able on its own — this
        // only removes the "drag anywhere to scroll" shortcut, which is
        // exactly what was colliding with knob drags.
        viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
        // Bubbles mouseEnter/mouseExit from the viewport AND everything nested
        // inside it (rows, knobs) up to US, so hovering anywhere in the Sends
        // area — not just directly over the scrollbar — repaints and reveals
        // the auto-hide scrollbar (see HardwareSlotLookAndFeel::drawScrollbar's
        // parentHovering check).
        viewport.addMouseListener (this, true);
        addAndMakeVisible (viewport);
    }

    void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override   { repaint(); }

    int numSendRows() const   { return content.getNumChildComponents(); }

    void addSendSlot (juce::Component& slot)   { content.addAndMakeVisible (slot); }

    void resized() override
    {
        // This component's OWN bounds are the full "Universal Rack Width"
        // well (see MixerStrip::resized() — same rackMargin as Inserts/the
        // hand-painted wells), so the scrollbar can dock flush with the
        // well's true right edge. Buttons/rows still need to START at the
        // shared rackMargin+rackButtonPadding left edge, so that inset is
        // applied HERE, on the left only — the right edge stays untouched.
        auto b = getLocalBounds();
        auto captionRow = b.removeFromTop (DSMixer::sendsCaptionH).withTrimmedLeft (rackButtonPadding);
        addSendButton.setBounds (captionRow.removeFromRight (DSMixer::sendsCaptionH).reduced (1));
        caption.setBounds (captionRow);
        b.removeFromTop (DSMixer::sendsCaptionBottomGap);
        b = b.withTrimmedLeft (rackButtonPadding);

        viewport.setBounds (b);

        constexpr int rowH = DSMixer::sendRowH + DSMixer::sendRowGap;
        const int contentH = juce::jmax (b.getHeight(), numSendRows() * rowH);

        // Axis lock (the "jelly" bug): content width is set to the viewport's
        // available width MINUS the scrollbar gutter whenever a scrollbar is
        // actually needed — so the viewed component's width can never exceed
        // the visible area even by a single pixel, which is what silently
        // enables free horizontal drag-panning in JUCE's Viewport. When no
        // scrollbar is needed, content just fills the full width.
        const bool needsScrollbar = contentH > b.getHeight();
        const int contentW = b.getWidth() - (needsScrollbar ? viewport.getScrollBarThickness() : 0);

        content.setBounds (0, 0, juce::jmax (0, contentW), contentH);
        content.resized();
    }

    void paint (juce::Graphics& g) override
    {
        if (numSendRows() > 0)
            return;

        g.setColour (LAF::colorTextSecondary);
        g.setFont (juce::FontOptions (CrateDesignSystem::Typography::sendsEmptyCaptionFontSize));
        g.drawText ("(none)", getLocalBounds().reduced (4).withTop (14).withHeight (14),
                     juce::Justification::centredLeft);
    }

    juce::Label caption;
    AddIconButton addSendButton;
    juce::Viewport viewport;
    Content content;
};

//==============================================================================
MixerStrip::MixerStrip (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse), track (trackToControl)
{
    isReturnTrackFlag = track != nullptr && TrackUtils::isReturnTrack (*track);

    if (track != nullptr)
    {
        // addDefaultTrackPlugins() (called when the track was created) already put a
        // VolumeAndPanPlugin + LevelMeterPlugin on the chain — reuse those instead of
        // inserting duplicates.
        volumePlugin = track->getVolumePlugin();
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

    // ---- L1: Track name plate — bordered, distinct coloured rectangle ---------
    addAndMakeVisible (trackNameLabel);
    trackNameLabel.setText (track != nullptr ? track->getName() : juce::String ("Track"), juce::dontSendNotification);
    trackNameLabel.setJustificationType (juce::Justification::centred);
    trackNameLabel.setColour (juce::Label::textColourId, LAF::colorTextPrimary);
    trackNameLabel.setColour (juce::Label::outlineColourId, LAF::colorFaderGroove);
    trackNameLabel.setFont (juce::FontOptions (CrateDesignSystem::Typography::stripNameFontSize, juce::Font::bold));
    // Clicks pass THROUGH to MixerStrip so mouseDown/mouseDoubleClick can open
    // the rename+colour editor over the plate (a plain Label has no double/right
    // click hooks of its own).
    trackNameLabel.setInterceptsMouseClicks (false, false);
    trackNameLabel.setTooltip ("Click to mute. Right-click or double-click to rename/recolour.");
    applyTrackColourToPlate();

    // ---- R/S/I triad -----------------------------------------------------------
    // GhostButtonLookAndFeel — OFF is flush with the track background (not
    // TheCrateLookAndFeel's app-wide ghosted-grey chip), ON pops a small matte
    // cap with a drop shadow. Scoped to just these 3 buttons (Mute is gone —
    // the name plate is the Mute toggle now).
    addAndMakeVisible (soloButton);
    soloButton.setLookAndFeel (&ghostButtonLookAndFeel);
    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, soloOnColour);
    soloButton.setToggleState (track != nullptr && track->isSolo (false), juce::dontSendNotification);
    soloButton.onClick = [this]
    {
        if (track == nullptr)
            return;

        const bool newState = soloButton.getToggleState();
        track->edit.getUndoManager().beginNewTransaction ("Toggle Solo: " + track->getName());
        track->edit.getUndoManager().perform (new TrackMuteSoloAction (track, true, newState));
    };

    // ---- L3: Record enable / Input monitor (visual placeholders) --------------
    // No engine wiring yet (record-arm lives in TrackHeaderComponent; input
    // monitoring routing is a later phase) — honest, disclosed placeholders.
    addAndMakeVisible (recordButton);
    recordButton.setLookAndFeel (&ghostButtonLookAndFeel);
    recordButton.setClickingTogglesState (true);
    recordButton.setColour (juce::TextButton::buttonOnColourId, recordOnColour);

    if (isReturnTrackFlag)
    {
        // Return Track Button directive (Mixer parity with the Arrangement):
        // return tracks don't record — this slot becomes the same cosmetic
        // Pre/Post toggle TrackHeaderComponent's recordArmButton uses. POST
        // is the default, matching real-console convention.
        recordButton.setButtonText ("POST");
        recordButton.setToggleState (true, juce::dontSendNotification);
        recordButton.setTooltip ("Pre/Post fader tap point - not wired to the engine yet (display only).");
        recordButton.onClick = [this]
        {
            const bool isPost = recordButton.getToggleState();
            recordButton.setButtonText (isPost ? "POST" : "PRE");
        };
    }
    else
    {
        recordButton.setTooltip ("Record enable - not wired to the engine yet (display only).");
    }

    // Return tracks have no external input at all — the 'I' slot is hidden
    // completely, not just relabelled (see resized()'s R/S/I row for how the
    // remaining two buttons then split the freed width).
    addAndMakeVisible (inputMonitorButton);
    inputMonitorButton.setLookAndFeel (&ghostButtonLookAndFeel);
    inputMonitorButton.setClickingTogglesState (true);
    inputMonitorButton.setColour (juce::TextButton::buttonOnColourId, LAF::colorNeonCyan);
    inputMonitorButton.setTooltip ("Input monitor - not wired to the engine yet (display only).");
    inputMonitorButton.setVisible (! isReturnTrackFlag);

    // ---- L4: Sexy fader (CrateMixerLookAndFeel) -------------------------------
    addAndMakeVisible (volumeFader);
    volumeFader.setLookAndFeel (&mixerLookAndFeel);
    volumeFader.setRange (-60.0, 6.0, 0.1);
    volumeFader.setDoubleClickReturnValue (true, 0.0);

    // ---- L5: dB readout boxes (fader position | peak level) -------------------
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

    // ---- L6: Pan knob ---------------------------------------------------------
    addAndMakeVisible (panKnob);
    panKnob.setLookAndFeel (&mixerLookAndFeel);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                  juce::MathConstants<float>::pi * 2.8f, true);

    // Pan value readout — Cubase-style "C" / "20 L" / "20 R" directly below
    // the knob, updated from refreshFromEngine() (initial + engine-driven) and
    // panKnob.onValueChange (immediate drag feedback) below.
    addAndMakeVisible (panValueLabel);
    panValueLabel.setJustificationType (juce::Justification::centred);
    panValueLabel.setColour (juce::Label::textColourId, CrateColors::NeonBlue); // "Pan readouts" — explicitly NeonBlue per the palette directive
    panValueLabel.setFont (juce::FontOptions (CrateDesignSystem::Typography::panValueFontSize, juce::Font::bold));
    panValueLabel.setText ("C", juce::dontSendNotification);

    // ---- L9: Routing (Output slot + Group slot) -------------------------------
    routingBlock = std::make_unique<RoutingBlock>();
    addChildComponent (*routingBlock);
    // outputCombo is the ONLY thing inside RoutingBlock routed through a
    // LookAndFeel at all — groupSlot draws its own matching bevel by hand
    // (see RoutingBlock::paint()), so this is scoped to the combo itself
    // rather than the whole block.
    routingBlock->outputCombo.setLookAndFeel (&hardwareSlotLookAndFeel);
    routingBlock->outputCombo.setJustificationType (juce::Justification::centred); // centre "OUT 1+2" like every other hardware chip
    populateOutputCombo();
    routingBlock->outputCombo.onChange = [this] { applyOutputComboSelection(); };

    // ---- L10: Sends -----------------------------------------------------------
    sendsSection = std::make_unique<SendsSection>();
    sendsSection->setLookAndFeel (&mixerLookAndFeel);
    sendsSection->viewport.setLookAndFeel (&hardwareSlotLookAndFeel); // overrides the inherited mixerLookAndFeel just for the scrollbar
    sendsSection->addSendButton.onClick = [this] { addNewSend(); };
    addChildComponent (*sendsSection);

    // ---- L11: Inserts (shared InsertsRackComponent) ---------------------------
    insertsSection = std::make_unique<InsertsRackComponent>();
    insertsSection->onSlotSelected = [this] (te::Plugin* p)
    {
        if (onPluginSlotSelected)
            onPluginSlotSelected (track.get(), p);
    };
    addChildComponent (*insertsSection);
    if (track != nullptr)
        insertsSection->rebuild (*track, workflow);

    // ---- L12: Channel Comp block (opens CrateCompressorPopup) -----------------
    // NOT setClickingTogglesState: the toggle state is driven manually so it
    // strictly tracks the popup's real lifecycle (see openChannelCompPopup() /
    // componentBeingDeleted()) rather than flipping on every raw click and
    // getting stuck lit after the popup dismisses.
    addChildComponent (channelCompButton);
    channelCompButton.setLookAndFeel (&hardwareSlotLookAndFeel);
    channelCompButton.onClick = [this] { openChannelCompPopup(); };

    // ---- L13: EQ display ------------------------------------------------------
    eqThumbnail = std::make_unique<CrateEQThumbnail>();
    addChildComponent (*eqThumbnail);

    // ---- L14: Settings --------------------------------------------------------
    addChildComponent (settingsButton);
    settingsButton.setLookAndFeel (&hardwareSlotLookAndFeel);
    settingsButton.setTooltip ("Channel-strip settings / presets - placeholder.");

    // Deep-stack visibility follows the current expand state.
    const bool showDeep = rackExpanded;
    routingBlock->setVisible (showDeep);
    sendsSection->setVisible (showDeep);
    insertsSection->setVisible (showDeep);
    channelCompButton.setVisible (showDeep);
    eqThumbnail->setVisible (showDeep);
    settingsButton.setVisible (showDeep);

    rebuildSends();

    if (volumePlugin != nullptr)
    {
        refreshFromEngine();

        volumeFader.onDragStart = [this]
        {
            volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction (
                "Tweak Volume: " + (track != nullptr ? track->getName() : juce::String()));
            volumePlugin->volParam->parameterChangeGestureBegin();
        };
        volumeFader.onDragEnd = [this] { volumePlugin->volParam->parameterChangeGestureEnd(); };
        volumeFader.onValueChange = [this]
        {
            const auto db = (float) volumeFader.getValue();
            volumePlugin->setVolumeDb (db);
            faderPositionLabel.setText (juce::String (db, 1), juce::dontSendNotification);
        };

        panKnob.onDragStart = [this]
        {
            volumePlugin->panParam->getEdit().getUndoManager().beginNewTransaction (
                "Tweak Pan: " + (track != nullptr ? track->getName() : juce::String()));
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

    if (track != nullptr)
        track->state.addListener (this);

    startTimerHz (24);
}

MixerStrip::~MixerStrip()
{
    stopTimer();

    volumeFader.setLookAndFeel (nullptr);
    panKnob.setLookAndFeel (nullptr);
    channelCompButton.setLookAndFeel (nullptr);
    settingsButton.setLookAndFeel (nullptr);
    soloButton.setLookAndFeel (nullptr);
    recordButton.setLookAndFeel (nullptr);
    inputMonitorButton.setLookAndFeel (nullptr);

    if (routingBlock != nullptr) routingBlock->outputCombo.setLookAndFeel (nullptr);
    if (sendsSection != nullptr)
    {
        sendsSection->viewport.setLookAndFeel (nullptr);
        sendsSection->setLookAndFeel (nullptr);
    }

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

void MixerStrip::setRackExpanded (bool shouldBeExpanded)
{
    if (rackExpanded == shouldBeExpanded)
        return;

    rackExpanded = shouldBeExpanded;

    if (rackExpanded)
        rebuildSends(); // pick up any send added while collapsed (inserts self-refresh)

    routingBlock->setVisible (rackExpanded);
    sendsSection->setVisible (rackExpanded);
    insertsSection->setVisible (rackExpanded);
    channelCompButton.setVisible (rackExpanded);
    eqThumbnail->setVisible (rackExpanded);
    settingsButton.setVisible (rackExpanded);

    // Deliberately NOT calling resized() here — the parent
    // (MixerComponent::StripRowContent) hasn't yet resized us to the new
    // preferred height (which just changed), so laying out now would compute
    // against the OLD height. The parent calls setBounds()/resized() once it
    // has given us correct bounds; see StripRowContent::resized().
}

int MixerStrip::getPreferredHeight() const
{
    return coreStripHeight() + (rackExpanded ? deepStackHeight() : 0);
}

//==============================================================================
void MixerStrip::openChannelCompPopup()
{
    channelCompButton.setToggleState (true, juce::dontSendNotification);

    auto popup = std::make_unique<CrateCompressorPopup> (&mixerLookAndFeel);
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (popup),
                                                        channelCompButton.getScreenBounds(), nullptr);
    // Un-toggle the moment the CallOutBox is dismissed/destroyed — clicking the
    // button again, or anywhere outside the popup, dismisses it (JUCE consumes
    // that click, so onClick does NOT re-fire), and this listener resets the
    // button so it can never stay stuck cyan.
    box.addComponentListener (this);
}

void MixerStrip::componentBeingDeleted (juce::Component&)
{
    channelCompButton.setToggleState (false, juce::dontSendNotification);
}

//==============================================================================
void MixerStrip::mouseDown (const juce::MouseEvent& e)
{
    if (! trackNameLabel.getBounds().contains (e.getPosition()))
        return;

    if (e.mods.isPopupMenu())
    {
        showNameColourEditor();
        return;
    }

    // The Track Name Plate IS the Mute toggle now (Ableton Mute paradigm) —
    // plain left-click toggles. Guarded to numberOfClicks() == 1 so the
    // second click of a double-click (which separately opens the rename/
    // colour editor via mouseDoubleClick below) doesn't re-toggle it back off.
    if (e.getNumberOfClicks() == 1 && track != nullptr)
    {
        const bool newState = ! track->isMuted (false);
        track->edit.getUndoManager().beginNewTransaction ("Toggle Mute: " + track->getName());
        track->edit.getUndoManager().perform (new TrackMuteSoloAction (track, false, newState));
    }
}

void MixerStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (trackNameLabel.getBounds().contains (e.getPosition()))
        showNameColourEditor();
}

void MixerStrip::applyTrackColourToPlate()
{
    // Ableton Mute paradigm — the name plate itself IS the Mute toggle (see
    // mouseDown()): unmuted shows the track's own colour with bright/white
    // text (the "Active" identity plate); muted flattens to dark grey with
    // dimmed text, same visual language GhostButtonLookAndFeel's OFF state
    // uses elsewhere. Falls back to a neutral blue when the track has no
    // colour set yet (te::Track's default is a transparent Colour()).
    trackAccentColour = (track != nullptr && ! track->getColour().isTransparent())
                            ? track->getColour()
                            : juce::Colour (CrateDesignSystem::Colors::mixerStripDefaultAccent);

    const bool isMuted = track != nullptr && track->isMuted (false);

    if (isMuted)
    {
        trackNameLabel.setColour (juce::Label::backgroundColourId, CrateColors::DarkBackground);
        trackNameLabel.setColour (juce::Label::textColourId, CrateColors::BrandGray);
    }
    else
    {
        trackNameLabel.setColour (juce::Label::backgroundColourId, trackAccentColour);
        trackNameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    }

    trackNameLabel.repaint();
}

void MixerStrip::showNameColourEditor()
{
    if (track == nullptr)
        return;

    juce::Component::SafePointer<MixerStrip> safeThis (this);

    CrateTrackEditor::showNameColourMenu (
        this,
        trackNameLabel.getScreenBounds(),
        track->getName(),
        track->getColour(),
        [safeThis] (juce::String newName) // onRename
        {
            if (safeThis != nullptr && safeThis->track != nullptr)
            {
                safeThis->track->edit.getUndoManager().beginNewTransaction ("Rename Track");
                safeThis->track->setName (newName);
            }
        },
        [safeThis]                        // onColourGestureBegin
        {
            if (safeThis != nullptr && safeThis->track != nullptr)
                safeThis->track->edit.getUndoManager().beginNewTransaction ("Set Track Colour");
        },
        [safeThis] (juce::Colour c)       // onColour (live)
        {
            if (safeThis != nullptr && safeThis->track != nullptr)
                safeThis->track->setColour (c);
        });
}

//==============================================================================
void MixerStrip::populateOutputCombo()
{
    auto& combo = routingBlock->outputCombo;
    combo.clear (juce::dontSendNotification);
    outputComboActions.clear();

    if (track == nullptr)
        return;

    juce::StringArray deviceNames, aliases;
    juce::BigInteger hasAudio, hasMidi;
    te::TrackOutput::getPossibleOutputDeviceNames (te::getAudioTracks (track->edit), deviceNames, aliases, hasAudio, hasMidi);

    int itemId = 1;

    for (auto& deviceName : deviceNames)
    {
        combo.addItem (deviceName, itemId);
        outputComboActions[itemId] = [this, deviceName] { track->getOutput().setOutputToDeviceID (deviceName); };
        ++itemId;
    }

    bool addedSeparator = false;

    for (auto* other : te::getAudioTracks (track->edit))
    {
        if (other == track.get())
            continue;

        if (! addedSeparator)
        {
            combo.addSeparator();
            addedSeparator = true;
        }

        combo.addItem ("Track: " + other->getName(), itemId);
        outputComboActions[itemId] = [this, other] { track->getOutput().setOutputToTrack (other); };
        ++itemId;
    }

    const auto currentDescription = track->getOutput().getDescriptiveOutputName();

    for (int i = 0; i < combo.getNumItems(); ++i)
    {
        if (combo.getItemText (i) == currentDescription)
        {
            combo.setSelectedId (combo.getItemId (i), juce::dontSendNotification);
            break;
        }
    }
}

void MixerStrip::applyOutputComboSelection()
{
    const auto it = outputComboActions.find (routingBlock->outputCombo.getSelectedId());

    if (it != outputComboActions.end())
        it->second();
}

void MixerStrip::rebuildSends()
{
    sendSlots.clear();

    if (track == nullptr)
    {
        sendsSection->resized();
        return;
    }

    for (auto* p : track->pluginList)
    {
        auto* send = dynamic_cast<te::AuxSendPlugin*> (p);
        if (send == nullptr)
            continue;

        // CrateSendSlot — the SAME shared destination-chip + mini-knob
        // component the Dual-Strip Inspector uses (CrateTrackInspectorComponent),
        // now with the hardware-slot bevel baked in. Replaces the old
        // PluginSlotComponent + raw LinearHorizontal juce::Slider pair.
        auto slot = std::make_unique<CrateSendSlot> ("Bus " + juce::String (send->getBusNumber()));
        slot->setLookAndFeelForKnob (&mixerLookAndFeel); // pan_knob.png image asset, same as the main Pan knob — see Revert Send Knobs directive
        slot->setBypassState (send->isEnabled(), juce::dontSendNotification);
        slot->onBypassToggle = [send] (bool isOn) { send->setEnabled (isOn); };

        if (send->gain != nullptr)
        {
            auto* gainParam = send->gain.get();
            const auto range = gainParam->getValueRange();
            auto& knob = slot->getAmountKnob();
            knob.setRange (range.getStart(), range.getEnd(), 0.001);
            knob.setValue (gainParam->getCurrentValue(), juce::dontSendNotification);

            const auto busNumber = send->getBusNumber();
            knob.onDragStart = [gainParam, busNumber]
            {
                gainParam->getEdit().getUndoManager().beginNewTransaction ("Tweak Send " + juce::String (busNumber) + " Level");
                gainParam->parameterChangeGestureBegin();
            };
            knob.onDragEnd = [gainParam] { gainParam->parameterChangeGestureEnd(); };

            slot->onAmountChanged = [gainParam] (float newValue) { gainParam->setParameter (newValue, juce::sendNotificationSync); };
        }

        sendsSection->addSendSlot (*slot);
        sendSlots.push_back (std::move (slot));
    }

    sendsSection->resized();
}

// Dynamic Sends Routing directive, ported from CrateTrackInspectorComponent's
// own addNewSend()/createSendToBus() — MixerStrip's Sends had no way to
// CREATE a new send at all before this; see AddIconButton's own wiring in
// SendsSection's constructor. Menu contents now come from SendBusUtils::
// buildSendMenu() (Hybrid Bus/Return Architecture directive) — one source of
// truth for the label formatting/separator/macro-item layout, shared with
// InspectorStrip's identical menu.
void MixerStrip::addNewSend()
{
    if (track == nullptr)
        return;

    auto& edit = track->edit;
    const auto scan = SendBusUtils::scanBuses (edit, track.get());
    auto menuBuild = SendBusUtils::buildSendMenu (edit, scan);
    auto& menu = menuBuild.menu;
    auto& menuIdToBusNumber = menuBuild.menuIdToBusNumber;
    const int createFXChannelItemId = menuBuild.createFXChannelItemId;

    juce::Component::SafePointer<MixerStrip> safeThis (this);
    te::AudioTrack::Ptr trackAtMenuOpenTime = track;
    CrateWorkflowManager* workflowPtr = &workflow; // long-lived (owned by MainComponent) — safe to outlive this strip

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sendsSection->addSendButton),
        [safeThis, trackAtMenuOpenTime, menuIdToBusNumber, createFXChannelItemId, workflowPtr] (int result)
        {
            if (result == 0 || safeThis == nullptr || safeThis->track != trackAtMenuOpenTime)
                return; // dismissed, or the strip's track moved on before the choice was made

            if (result == createFXChannelItemId)
            {
                // Use-After-Free fix: createAndRouteNewFXChannel() creates a new
                // track, which fires CrateWorkflowManager::onTrackListChanged ->
                // MixerComponent::rebuildStrips() -> destroys every live
                // MixerStrip, INCLUDING this one — the strip whose own Send-menu
                // callback is still executing right now, on this very call
                // stack (PopupMenu::showMenuAsync invokes its result callback
                // from JUCE's internal async-menu machinery, which is still
                // "above" us here). Calling it directly would delete `this`
                // (and this lambda's captured `safeThis`) mid-callback, then
                // crash the instant control returned here and touched
                // `safeThis` again (the old code's trailing rebuildSends() call
                // did exactly that). Deferred via callAsync so the destructive
                // rebuild runs on a fresh message-loop iteration, fully outside
                // this callback's (and the dying strip's) own call stack.
                // trackAtMenuOpenTime is a ref-counted te::AudioTrack::Ptr, so
                // the real engine track stays alive across the hop even though
                // the UI component asking for it will not — no raw `this`/
                // `safeThis` capture crosses the async boundary at all.
                juce::MessageManager::callAsync ([workflowPtr, trackAtMenuOpenTime]
                {
                    if (trackAtMenuOpenTime != nullptr)
                        workflowPtr->createAndRouteNewFXChannel (*trackAtMenuOpenTime);
                });
                return;
            }

            if (auto it = menuIdToBusNumber.find (result); it != menuIdToBusNumber.end())
                safeThis->createSendToBus (it->second);
        });
}

void MixerStrip::createSendToBus (int busNumber)
{
    if (track == nullptr)
        return;

    auto& edit = track->edit;
    auto plugin = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, juce::PluginDescription());

    if (plugin == nullptr)
        return;

    if (auto* send = dynamic_cast<te::AuxSendPlugin*> (plugin.get()))
        send->busNumber = busNumber;

    edit.getUndoManager().beginNewTransaction ("Add Send to Bus " + juce::String (busNumber) + ": " + track->getName());
    track->pluginList.insertPlugin (plugin, -1, nullptr);

    rebuildSends();
}

//==============================================================================
void MixerStrip::refreshFromEngine()
{
    if (volumePlugin == nullptr)
        return;

    const auto currentDb = volumePlugin->getVolumeDb();
    volumeFader.setValue (currentDb, juce::dontSendNotification);
    faderPositionLabel.setText (juce::String (currentDb, 1), juce::dontSendNotification);
    const auto pan = volumePlugin->getPan();
    panKnob.setValue (pan, juce::dontSendNotification);
    panValueLabel.setText (panValueText (pan), juce::dontSendNotification);
}

void MixerStrip::currentValueChanged (te::AutomatableParameter&)
{
    juce::Component::SafePointer<MixerStrip> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->refreshFromEngine();
    });
}

void MixerStrip::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& property)
{
    if (track == nullptr || v != track->state)
        return;

    juce::Component::SafePointer<MixerStrip> safeThis (this);

    if (property == te::IDs::mute || property == te::IDs::solo)
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->refreshMuteSoloFromEngine();
        });
        return;
    }

    // Name / colour changed anywhere (this Mixer plate, the Arrangement
    // TrackHeader, Undo/Redo) — reflect it on the plate. Bidirectional sync
    // comes for free: both views mutate the SAME track ValueTree and both
    // listen to it.
    if (property == te::IDs::name || property == te::IDs::colour)
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis == nullptr || safeThis->track == nullptr)
                return;

            safeThis->trackNameLabel.setText (safeThis->track->getName(), juce::dontSendNotification);
            safeThis->applyTrackColourToPlate();
        });
    }
}

void MixerStrip::refreshMuteSoloFromEngine()
{
    if (track == nullptr)
        return;

    // Mute has no dedicated button anymore — the name plate IS the Mute
    // toggle (see applyTrackColourToPlate()), so re-syncing its look covers
    // mute changes made elsewhere (Arrangement TrackHeader, Undo/Redo).
    applyTrackColourToPlate();
    soloButton.setToggleState (track->isSolo (false), juce::dontSendNotification);
}

void MixerStrip::valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree)
{
    if (track == nullptr || parentTree != track->state || ! childTree.hasType (te::IDs::PLUGIN))
        return;

    juce::Component::SafePointer<MixerStrip> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->refreshRackFromPluginListChange();
    });
}

void MixerStrip::valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int)
{
    if (track == nullptr || parentTree != track->state || ! childTree.hasType (te::IDs::PLUGIN))
        return;

    juce::Component::SafePointer<MixerStrip> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->refreshRackFromPluginListChange();
    });
}

void MixerStrip::refreshRackFromPluginListChange()
{
    // Inserts self-refresh via their own listener; Sends are rebuilt here.
    rebuildSends();
}

//==============================================================================
void MixerStrip::timerCallback()
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

void MixerStrip::paint (juce::Graphics& g)
{
    g.fillAll (LAF::colorHardware);
    g.setColour (LAF::colorTheVoid);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    // ---- Recessed Hardware Plate (Routing well) -------------------------------
    // Replaces the earlier engraved separator lines — those read as too weak
    // for the Manifesto's "SSL Tactile Experience". This carves an actual
    // sunken well housing ONLY the currently-visible Routing chips (OUT 1+2
    // alone, or stacked with the Group chip) — the Pan knob is deliberately
    // EXTRACTED below it, on the plain track background; Read is gone
    // entirely (Eradicate directive). Bounds come from the REAL live control
    // bounds (routingBlock's own getBounds(), which is already sized to
    // RoutingBlock::getPreferredHeight()), not recomputed layout math, so the
    // well can never drift out of sync with wherever resized() actually put
    // those controls or wrap a hidden Group chip's dead space. Only
    // meaningful while the deep stack (routingBlock) is expanded/visible.
    // Shared "sunken hardware well" drawing — same recessed fill + 3D cutout
    // bevel used for BOTH the Routing well and the Sends well below, so the
    // two pools can never visually drift apart.
    auto drawVoidWell = [&g] (juce::Rectangle<float> well)
    {
        constexpr float cornerRadius = DSMixer::wellCornerRadius;

        if (well.getHeight() <= 0.0f || well.getWidth() <= 0.0f) // QA: guard against a negative/zero rect on a pathologically narrow strip
            return;

        // Deep recessed fill — the exact DarkBackground brand colour, reading
        // as sunken relative to the lighter LightBackground panel around it.
        g.setColour (CrateColors::DarkBackground);
        g.fillRoundedRectangle (well, cornerRadius);

        // 3D cutout: shadow on the top/left INSIDE edges (depth falling away
        // from the viewer), highlight on the bottom/right OUTSIDE edges (the
        // rim catching light) — the same "dark-then-light" carved language
        // the fader ridges/groove use elsewhere. Shadow line is DarkBackground
        // darkened (a derived runtime shade, not a new stored hex) so it stays
        // visible against the well's own DarkBackground fill.
        g.setColour (CrateColors::DarkBackground.darker (0.5f));
        g.drawLine (well.getX() + 1.0f, well.getY() + 1.0f, well.getRight() - 1.0f, well.getY() + 1.0f, 1.0f);
        g.drawLine (well.getX() + 1.0f, well.getY() + 1.0f, well.getX() + 1.0f, well.getBottom() - 1.0f, 1.0f);

        g.setColour (CrateColors::LightBackground);
        g.drawLine (well.getX(), well.getBottom(), well.getRight(), well.getBottom(), 1.0f);
        g.drawLine (well.getRight(), well.getY(), well.getRight(), well.getBottom(), 1.0f);
    };

    // Universal Rack Width (Absolute Symmetry) — BOTH wells share the exact
    // same X/width as Inserts and Comp (see resized()), all computed from
    // THIS component's own getWidth() — never from a child's already-inset
    // bounds, which is what made wells' widths/X-positions disagree in past
    // rounds. Each well snugly wraps its own component (zero extra vertical
    // padding).
    const float universalWellX     = (float) rackMargin;
    const float universalWellWidth = (float) getWidth() - (float) rackMargin * 2.0f;

    // Routing well — wraps ONLY the currently-visible Routing chips
    // (routingBlock's own bounds, already sized to getPreferredHeight()). The
    // Pan knob is EXTRACTED: it sits strictly BELOW this well, directly on
    // the plain track background, not housed inside it.
    if (rackExpanded && routingBlock != nullptr)
    {
        const auto b = routingBlock->getBounds();
        drawVoidWell ({ universalWellX, (float) b.getY(), universalWellWidth, (float) b.getHeight() });
    }

    // Sends "Void Well" — the entire Sends area gets the identical sunken
    // pool treatment, so the slots read as sitting IN a deep recess cut into
    // the hardware rather than floating on the flat panel.
    if (rackExpanded && sendsSection != nullptr)
    {
        const auto b = sendsSection->getBounds();
        drawVoidWell ({ universalWellX, (float) b.getY(), universalWellWidth, (float) b.getHeight() });
    }

    // Scribble Strip tape-label plate — a 1px inner shadow along the top edge
    // (the "tape pressed into a slot" look) — reads fine whether the plate is
    // currently filled with the track's colour (unmuted) or the dark
    // muted-grey (see applyTrackColourToPlate() — the plate IS the Mute
    // toggle now, no separate M button or colour strip).
    {
        const auto nb = trackNameLabel.getBounds().toFloat();
        if (! nb.isEmpty())
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawLine (nb.getX() + 1.0f, nb.getY() + 0.5f, nb.getRight() - 1.0f, nb.getY() + 0.5f, 1.0f);
        }
    }

    // Embedded track icon — NO plate/border of its own; drawn straight onto
    // the dark void background (DarkBackground) so it reads as seamlessly
    // milled into the mixer surface, not a bolted-on placeholder square.
    if (! trackIconBounds.isEmpty())
    {
        auto ib = trackIconBounds.toFloat();
        g.setColour (CrateColors::DarkBackground);
        g.fillRoundedRectangle (ib, (float) DSMixer::trackIconCornerRadius);
        g.setColour (LAF::colorTextSecondary);
        g.setFont (juce::FontOptions (CrateDesignSystem::Typography::trackIconGlyphFontSize));
        g.drawText (juce::String::charToString ((juce::juce_wchar) 0x266A), trackIconBounds, juce::Justification::centred); // ♪
    }

    // L4 main audio meter (upward-filling).
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

    // Peak-hold marker line.
    const auto peakNormalised = juce::jlimit (0.0f, 1.0f, (peakHoldDb - meterFloorDb) / meterRangeDb);
    const auto peakY = meterBounds.getBottom() - meterBounds.getHeight() * peakNormalised;
    g.setColour (juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (meterBounds.getX(), peakY - 0.5f, meterBounds.getWidth(), 1.0f));

    // L4 GR meter (downward-filling) — thin column beside the main meter.
    g.setColour (LAF::colorTheVoid);
    g.fillRect (grMeterBounds);

    constexpr float grRangeDb = DSMixer::grRangeDb;
    const auto grNormalised = juce::jlimit (0.0f, 1.0f, gainReductionDb / grRangeDb);
    const auto grFillHeight = grMeterBounds.getHeight() * grNormalised;

    if (grFillHeight > 0.0f)
    {
        g.setColour (juce::Colour (CrateDesignSystem::Colors::grMeterAmber)); // amber, hanging from the top
        g.fillRect (grMeterBounds.withHeight (grFillHeight));
    }

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawVerticalLine ((int) grMeterBounds.getX(), grMeterBounds.getY(), grMeterBounds.getBottom());
}

//==============================================================================
void MixerStrip::resized()
{
    // STRICT BOTTOM-UP LAYOUT. Bottom-anchored levels are carved off the BOTTOM
    // (removeFromBottom), top-anchored levels off the TOP (removeFromTop), and
    // the L4 fader/meter block fills whatever remains in the middle — so it
    // always stretches to zero dead space regardless of strip height.
    auto bounds = getLocalBounds().reduced (outerMargin);

    // QA Hardening: clamped so a pathologically narrow strip (dragged mixer
    // column, extreme zoom) can never produce a negative width fed into
    // setBounds() below — every rackMargin-based container uses THIS, not a
    // raw getWidth() - rackMargin*2 recomputed at each call site.
    const int universalWidth = juce::jmax (0, getWidth() - rackMargin * 2);

    // ----- Bottom group (Scribble Strip + R/S/I triad) -------------------------
    // Embedded icon at the ABSOLUTE bottom (no plate/border — see paint()),
    // track name (the Mute toggle) directly above it, R/S/I triad directly
    // above that. No more separate M button or colour strip.
    //
    // Mixer Bottom-Alignment (Ableton Accuracy): a return track (TrackUtils::
    // isReturnTrack()) has no track-type icon at all — same as MasterStrip,
    // which never draws one either. Fader Alignment directive: MasterStrip's
    // outerMargin now matches this class's EXACTLY (4, not 6), so the full
    // scribbleIconH+scribbleGap slot must always be reserved regardless of
    // track type — only whether trackIconBounds is non-empty (and therefore
    // painted) changes. Reserving a smaller slot for return tracks (the old
    // ad-hoc removeFromBottom(2) compensation) is what used to make the
    // baselines line up against MasterStrip's old, now-corrected geometry —
    // with both sides fixed to the identical budget, that compensation would
    // instead throw the alignment off.
    auto iconSlot = bounds.removeFromBottom (scribbleIconH).withSizeKeepingCentre (scribbleIconH, scribbleIconH);
    bounds.removeFromBottom (scribbleGap);
    trackIconBounds = (track != nullptr && TrackUtils::isReturnTrack (*track)) ? juce::Rectangle<int>() : iconSlot;
    trackNameLabel.setBounds (bounds.removeFromBottom (nameH));
    bounds.removeFromBottom (levelGap);

    {
        // (universalWidth - 4) / 3, 2px gaps — the exact Lead Architect
        // formula, so all three buttons are equally sized and symmetrically
        // spaced within the universal rack width.
        auto row = bounds.removeFromBottom (tripletH)
                          .withX (rackMargin).withWidth (universalWidth);
        constexpr int gap = DSMixer::triadGap;

        if (isReturnTrackFlag)
        {
            // R/S/I Return-Track Logic: no 'I' slot at all — Post/Pre and
            // Solo split the full row width between just the two of them
            // instead of leaving the freed third dead/empty.
            const int each = juce::jmax (0, (row.getWidth() - gap) / 2);
            recordButton.setBounds (row.removeFromLeft (each));
            row.removeFromLeft (gap);
            soloButton.setBounds (row); // remainder absorbs integer-division rounding
            inputMonitorButton.setBounds ({});
        }
        else
        {
            const int each = juce::jmax (0, (row.getWidth() - gap * 2) / 3); // QA: never a negative width if the strip is ever narrower than the gaps
            recordButton.setBounds (row.removeFromLeft (each));
            row.removeFromLeft (gap);
            soloButton.setBounds (row.removeFromLeft (each));
            row.removeFromLeft (gap);
            inputMonitorButton.setBounds (row); // remainder absorbs integer-division rounding
        }
    }
    bounds.removeFromBottom (levelGap);

    // ----- Top group (L14 → L9 when expanded, then L8 → L5) -------------------
    if (rackExpanded)
    {
        settingsButton.setBounds (bounds.removeFromTop (settingsH));           // L14
        bounds.removeFromTop (levelGap);

        eqThumbnail->setBounds (bounds.removeFromTop (eqH));                    // L13
        bounds.removeFromTop (levelGap);

        channelCompButton.setBounds (bounds.removeFromTop (compH)
                                           .withX (rackMargin).withWidth (universalWidth)); // L12 — Universal Rack Width
        bounds.removeFromTop (levelGap);

        insertsSection->setBounds (bounds.removeFromTop (InsertsRackComponent::getFixedHeight())
                                         .withX (rackMargin).withWidth (universalWidth)); // L11 — IS the dark block, draws its own fill
        insertsSection->resized();
        bounds.removeFromTop (levelGap);

        // Full well width (matches Inserts/the well painters exactly) — the
        // scrollbar needs to dock at the well's true right edge, so the
        // rackButtonPadding left-inset for its rows is applied INSIDE
        // SendsSection::resized() instead of out here.
        sendsSection->setBounds (bounds.removeFromTop (sendsH)
                                       .withX (rackMargin).withWidth (universalWidth)); // L10
        sendsSection->resized();
        bounds.removeFromTop (sendsToRoutingGap); // strict 6-8px — the two dark wells must never touch

        // Routing well is now DYNAMIC height: just OUT 1+2 when no group is
        // assigned (routingBlock->getPreferredHeight() == routingRowH), or
        // OUT 1+2 + the Group chip stacked when one is (Read is gone entirely
        // — see the Eradicate directive).
        routingBlock->setBounds (bounds.removeFromTop (routingBlock->getPreferredHeight())
                                       .withX (rackMargin).withWidth (universalWidth)); // L9 — Universal Rack Width
        routingBlock->resized();
        bounds.removeFromTop (levelGap); // Routing well -> Pan knob gap
    }

    // L6 Pan knob — EXTRACTED: sits strictly BELOW the Routing well now,
    // directly on the plain track background, not inside any well/rack chip.
    panKnob.setBounds (bounds.removeFromTop (panH).withSizeKeepingCentre (panH, panH));
    bounds.removeFromTop (panValueGap);

    panValueLabel.setBounds (bounds.removeFromTop (panValueH));
    bounds.removeFromTop (levelGap);

    {
        auto row = bounds.removeFromTop (dbReadoutH);                          // L5
        faderPositionLabel.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (1, 0));
        peakLevelLabel.setBounds (row.reduced (1, 0));
    }
    bounds.removeFromTop (levelGap);

    // ----- L4: the tall unified fader + meters block (fills the remainder) ----
    grMeterBounds = bounds.removeFromRight (grMeterColumnWidth).reduced (1, 0).toFloat();
    meterBounds   = bounds.removeFromRight (meterColumnWidth).reduced (2, 0).toFloat();
    volumeFader.setBounds (bounds);
}
