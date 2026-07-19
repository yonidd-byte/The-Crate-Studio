#include "CrateTrackInspectorComponent.h"
#include "CrateEQThumbnail.h"
#include "CrateSendSlot.h"
#include "CrateCompressorPopup.h"
#include "TheCrateLookAndFeel.h"
#include "CrateColors.h"
#include "AddIconButton.h"
#include "SendBusUtils.h"

#include <set>
#include <map>

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Restore-the-scrollbar-but-not-the-wheel directive: a Viewport variant
    // that swallows mouse wheel events entirely (never calls the base class's
    // own scroll handling) — the ONLY way to move this list is dragging the
    // visible thumb itself. Drag-on-empty-space is separately blocked via
    // setScrollOnDragMode(never) at the call site; this handles the wheel
    // half of "strictly no accidental scrolling."
    class NoWheelViewport : public juce::Viewport
    {
    public:
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}
    };

    constexpr int eqThumbnailHeight = 60; // hard contract with CrateEQThumbnail
    constexpr int captionHeight     = 20;
    constexpr int compButtonHeight  = 24; // Channel Comp toggle — ALWAYS reserved (see Height Alignment)
    constexpr int sendSlotHeight    = 26;
    constexpr int sendSlotGap       = 3;
    constexpr int sendsHeaderHeight = 18; // Task 2: bumped up for a legible header + bigger "+" button
    constexpr int addSendButtonSize = 20;
    constexpr int sendsVisibleSlots = 3;  // Task 3: fixed visual block height — infinite scroll INSIDE it
    constexpr int sendsViewportHeight = sendsVisibleSlots * (sendSlotHeight + sendSlotGap);
    constexpr int ioCaptionHeight   = 14; // Task 2: compact "I/O" readout directly above Pan/Fader
    constexpr int comboHeight       = 22;
    constexpr int panKnobHeight     = 44;
    constexpr int sectionGap        = 6;
    constexpr int meterColumnWidth  = 20;
    constexpr int grMeterColumnWidth = 8; // Gain-Reduction meter, ported from MixerStrip

    constexpr float meterFloorDb = -60.0f;
    constexpr float meterRangeDb = 66.0f;

    // Inspector/Channel Strip Parity directive — ported 1:1 from MixerStrip.
    constexpr int panValueHeight = 12;
    constexpr int dbReadoutHeight = 14; // fader position | peak level, between Pan and Fader
    constexpr int sendsWellMargin = 4; // Sends DarkBackground well inset — same relationship MixerStrip's rackMargin gives its own wells

    juce::String panValueText (float pan)
    {
        const int percent = juce::roundToInt (std::abs (pan) * 100.0f);
        if (percent == 0)
            return "C";
        return pan < 0.0f ? ("< " + juce::String (percent) + " L")
                           : (juce::String (percent) + " R >");
    }

    // QA Hardening precedent (see MixerStrip.cpp's own TrackMuteSoloAction):
    // te::AudioTrack::setMute() writes through a juce::CachedValue bound with
    // a nullptr UndoManager (verified against tracktion_AudioTrack.cpp), so
    // it is NOT undoable no matter how the caller wraps it. Fixed here with
    // an explicit UndoableAction, independent of CachedValue's own broken
    // undo wiring — holds a ref-counted AudioTrack::Ptr (not a raw pointer)
    // so the action stays valid in the undo stack even if this strip's own
    // (raw, non-owning) track pointer later moves on to a different track.
    class TrackMuteAction : public juce::UndoableAction
    {
    public:
        TrackMuteAction (te::AudioTrack::Ptr t, bool newState)
            : track (t), newValue (newState), oldValue (t != nullptr && t->isMuted (false)) {}

        bool perform() override { if (track != nullptr) track->setMute (newValue); return true; }
        bool undo() override    { if (track != nullptr) track->setMute (oldValue); return true; }

    private:
        te::AudioTrack::Ptr track;
        bool newValue, oldValue;
    };
}

//==============================================================================
// Sends content — the actual vertical stack of CrateSendSlots, viewed through
// a fixed-height juce::Viewport in InspectorStrip (Task 3: Infinite Sends).
// Grows to fit however many real te::AuxSendPlugin instances exist; the
// Viewport's OWN height never changes, so adding a 4th/5th/Nth send scrolls
// instead of pushing the Pan/Fader section down and breaking Height Alignment.
class CrateTrackInspectorComponent::SendsContent : public juce::Component
{
public:
    // Declared explicitly: JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR below
    // declares a deleted copy constructor, which suppresses the implicit
    // default constructor too — without this, make_unique<SendsContent>()
    // fails to compile (same fix BrowserComponent::PluginListContent needed).
    SendsContent() = default;

    void rebuild (te::Track* track, juce::LookAndFeel* laf)
    {
        slots.clear();

        if (track != nullptr)
        {
            for (auto* p : track->pluginList)
            {
                auto* send = dynamic_cast<te::AuxSendPlugin*> (p);
                if (send == nullptr)
                    continue;

                auto slot = std::make_unique<CrateSendSlot> ("Bus " + juce::String (send->getBusNumber()));
                slot->setLookAndFeelForKnob (laf);

                // Send Slot Bypass via Click — ported from MixerStrip's own
                // rebuildSends(): clicking the Bus name/label (CrateSendSlot's
                // own mouseUp() already gates this to its chip zone) toggles
                // the real te::AuxSendPlugin's enabled state. This was
                // missing here entirely — onBypassToggle was never set, so
                // CrateSendSlot's click handler had nothing to call.
                slot->setBypassState (send->isEnabled(), juce::dontSendNotification);
                slot->onBypassToggle = [send] (bool isOn) { send->setEnabled (isOn); };

                if (send->gain != nullptr)
                {
                    auto* gainParam = send->gain.get();
                    const auto range = gainParam->getValueRange();
                    auto& knob = slot->getAmountKnob();
                    knob.setRange (range.getStart(), range.getEnd(), 0.001);
                    knob.setValue (gainParam->getCurrentValue(), juce::dontSendNotification);

                    slot->onAmountChanged = [gainParam] (float newValue)
                    {
                        gainParam->setParameter (newValue, juce::sendNotificationSync);
                    };
                }

                addAndMakeVisible (*slot);
                slots.push_back (std::move (slot));
            }
        }

        relayout();
    }

    int getRequiredHeight() const
    {
        return juce::jmax ((int) slots.size() * (sendSlotHeight + sendSlotGap), 1);
    }

    void relayout()
    {
        // Task 3 fix: explicit setBounds (not setSize) — grows to fit EVERY
        // slot (not clamped to sendsVisibleSlots), and an explicit bounds call
        // is what reliably fires the Viewport's content-resize notification;
        // the caller (InspectorStrip) additionally re-syncs its Viewport
        // directly after this, since setSize()/resized() alone were leaving
        // the scroll range stuck at whatever fit in the FIRST layout pass.
        //
        // Rendering bug fix: setBounds() is a no-op — and never calls
        // resized() — if the new bounds happen to equal the CURRENT ones
        // (e.g. rebuild() just swapped N old slots for N new ones of the
        // same total height). resized() is the ONLY place that positions
        // each CrateSendSlot; skip it and every freshly (re)created slot sits
        // at its just-constructed default (0,0,0,0) bounds — invisible. Force
        // it unconditionally so a rebuild ALWAYS repositions every current
        // child, whether or not the overall size actually changed.
        setBounds (0, 0, getWidth(), getRequiredHeight());
        resized();
    }

    void resized() override
    {
        int y = 0;
        for (auto& slot : slots)
        {
            slot->setBounds (0, y, getWidth(), sendSlotHeight);
            y += sendSlotHeight + sendSlotGap;
        }
    }

private:
    std::vector<std::unique_ptr<CrateSendSlot>> slots;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SendsContent)
};

//==============================================================================
// One vertical channel strip in the dual layout. Built top-to-bottom in the
// strict Logic order the spec mandates: EQ thumbnail -> Channel Comp toggle ->
// Sends -> I/O caption -> Pan -> Fader + meter.
//
// Height Alignment (Task 2): the Channel Comp row's SPACE is ALWAYS reserved,
// even for the Master strip — only the button's visibility toggles. Branching
// the LAYOUT itself (removing the row's height entirely for Master, as an
// earlier pass did) would shift every element below it up by one row height,
// breaking Y-axis alignment between the Selected and Output strips whenever
// one of them is Master and the other isn't. Reserving fixed space unconditionally
// is what guarantees EQ thumbnails/Sends/Pan/Fader land on the same Y in both.
class CrateTrackInspectorComponent::InspectorStrip : public juce::Component,
                                                       private juce::Timer,
                                                       private juce::ValueTree::Listener,
                                                       private juce::ComponentListener, // watches the Channel Comp CallOutBox lifecycle — see showCompressorPopup()
                                                       private te::AutomatableParameter::Listener // Pan/Fader <-> VolumeAndPanPlugin binding — see setTrack()
{
public:
    // canMute: only the LEFT (selected-track) strip's name plate becomes the
    // Mute toggle — the RIGHT strip shows the OUTPUT/bus destination (often
    // Master, which MixerStrip's own MasterStrip doesn't wire to this same
    // name-plate-mute mechanism either), so it keeps its plain caption look.
    InspectorStrip (const juce::String& initialCaption, juce::LookAndFeel* sharedLaf, juce::LookAndFeel* hardwareLafToUse,
                     bool canMute, CrateWorkflowManager& workflowToUse)
        : laf (sharedLaf), hardwareLaf (hardwareLafToUse), supportsMuteToggle (canMute), workflow (workflowToUse)
    {
        captionLabel.setText (initialCaption, juce::dontSendNotification);
        captionLabel.setJustificationType (juce::Justification::centred);
        captionLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        addAndMakeVisible (captionLabel);

        if (supportsMuteToggle)
        {
            // Ableton Mute paradigm, ported from MixerStrip::mouseDown() —
            // the name plate itself IS the Mute toggle. Clicks pass through
            // to InspectorStrip's own mouseDown() (see below), same
            // interceptsMouseClicks(false, false) pattern MixerStrip uses.
            captionLabel.setInterceptsMouseClicks (false, false);
            captionLabel.setTooltip ("Click to mute.");
            // No applyTrackColourToPlate() call here — captionLabel stays
            // hidden until the first setTrack() call anyway (Hide on Empty),
            // which applies the real colour/mute state at that point.
        }
        else
        {
            captionLabel.setColour (juce::Label::textColourId, LAF::text);
        }

        eqThumbnail = std::make_unique<CrateEQThumbnail>();
        addAndMakeVisible (*eqThumbnail);

        // Pop-out Compressor: a single toggle instead of permanent rotaries —
        // clicking spawns a CrateCompressorPopup in a juce::CallOutBox anchored
        // to this button. Hidden (not removed — see class doc comment) for Master.
        // Hardware Unification Protocol: same tactile bevel as MixerStrip's
        // Comp/Setting chips (toggle-on state still reads as lit accent —
        // HardwareSlotLookAndFeel::drawButtonBackground checks getToggleState()).
        // NOT setClickingTogglesState — same fix as MixerStrip's own
        // channelCompButton: toggle state is driven MANUALLY, strictly
        // tracking the popup's real lifecycle (see showCompressorPopup() /
        // componentBeingDeleted()), instead of auto-flipping on every raw
        // click independent of whether the popup is actually still open —
        // THAT mismatch was the "snaps and glitches" bug.
        channelCompButton.setLookAndFeel (hardwareLafToUse);
        channelCompButton.onClick = [this] { showCompressorPopup(); };
        addAndMakeVisible (channelCompButton);

        // Sends header "+" — a proper drawn-Path icon (AddIconButton), not a
        // blurry TextButton("+") text glyph. Instantiates a real
        // te::AuxSendPlugin on the current track via a bus-picker menu (see
        // addNewSend()).
        addSendButton.setTooltip ("Add a new send");
        addSendButton.onClick = [this] { addNewSend(); };
        addAndMakeVisible (addSendButton);

        // Task 3: Sends live in a fixed-height Viewport — infinite sends scroll
        // inside it rather than growing the strip or overlapping Pan/Fader.
        sendsContent = std::make_unique<SendsContent>();
        sendsViewport.setViewedComponent (sendsContent.get(), false);

        // Bug fix: a Send knob's own click-drag was being hijacked by the
        // Viewport's drag-to-scroll (ScrollOnDragMode::nonHover is the JUCE
        // default), scrolling the whole Sends list instead of turning the
        // knob. setScrollOnDragMode(never) is the non-deprecated equivalent
        // of setScrollOnDragEnabled(false) — stays disabled.
        //
        // UX reversal: hiding the scrollbar entirely (previous round) meant
        // a 4th/5th send became permanently unreachable — restored, styled
        // exactly like MixerStrip's Sends viewport (same hardwareLaf ->
        // HardwareSlotLookAndFeel::drawScrollbar auto-hide thumb, same slim
        // 8px gutter, same hover-anywhere-inside repaint wiring). Mouse WHEEL
        // stays fully blocked — sendsViewport's own NoWheelViewport type (see
        // top of file) swallows wheel events outright, so dragging the
        // visible thumb is the ONLY way to scroll this list.
        sendsViewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
        sendsViewport.setScrollBarsShown (true, false); // vertical only
        sendsViewport.setScrollBarThickness (8);
        sendsViewport.setLookAndFeel (hardwareLafToUse);
        sendsViewport.addMouseListener (this, true); // hover anywhere inside reveals the auto-hide thumb — see mouseEnter/mouseExit below
        addAndMakeVisible (sendsViewport);

        // Task 2: compact "I/O" readout, directly above Pan/Fader (Ableton
        // style), replacing the old giant "Output: X" header up top.
        ioLabel.setJustificationType (juce::Justification::centred);
        ioLabel.setColour (juce::Label::textColourId, LAF::textDim);
        ioLabel.setFont (juce::FontOptions (9.5f));
        addAndMakeVisible (ioLabel);

        // Input / Output routing combos (display-only placeholders this pass) —
        // same tactile hardware-slot bevel as MixerStrip's OUT 1 dropdown.
        inputCombo.addItem ("Input", 1);
        inputCombo.setSelectedId (1, juce::dontSendNotification);
        inputCombo.setLookAndFeel (hardwareLafToUse);
        addAndMakeVisible (inputCombo);

        outputCombo.addItem ("Stereo Out", 1);
        outputCombo.setSelectedId (1, juce::dontSendNotification);
        outputCombo.setLookAndFeel (hardwareLafToUse);
        addAndMakeVisible (outputCombo);

        // Pan rotary.
        panKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        panKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        panKnob.setRange (-1.0, 1.0, 0.01);
        panKnob.setDoubleClickReturnValue (true, 0.0);
        panKnob.setLookAndFeel (sharedLaf);
        // Real binding (Inspector Sync fix): writes through to volumePlugin (set
        // by setTrack()) — a member reference read at CALL time, not captured per
        // track, so these lambdas stay correct across every setTrack() switch
        // without needing to be reassigned. Guarded null-checks make this a no-op
        // whenever no track (or a track with no volume plugin) is selected.
        // Undo transaction + parameterChangeGesture bracket the drag exactly like
        // MixerStrip's / TrackHeaderComponent's own pan knobs.
        panKnob.onDragStart = [this]
        {
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

            panValueLabel.setText (panValueText ((float) panKnob.getValue()), juce::dontSendNotification);
        };
        addAndMakeVisible (panKnob);

        // Pan value readout — ported from MixerStrip: "C" / "20 L" / "20 R"
        // directly below the knob, NeonBlue per the palette directive's own
        // "Pan readouts" line.
        panValueLabel.setJustificationType (juce::Justification::centred);
        panValueLabel.setColour (juce::Label::textColourId, CrateColors::NeonBlue);
        panValueLabel.setFont (juce::FontOptions (8.5f, juce::Font::bold));
        panValueLabel.setText ("C", juce::dontSendNotification);
        addAndMakeVisible (panValueLabel);

        // dB readouts (fader position | peak level) — ported from MixerStrip's
        // L5 row, same lcdText/lcdBackground "digital readout" look, same
        // position between Pan and Fader. Do NOT rely on paint() for these
        // values: both are driven by real listeners (fader.onValueChange for
        // the position readout, timerCallback()'s live meter for peak), the
        // same "listener updates a Label's text" mechanism MixerStrip uses.
        auto styleReadout = [] (juce::Label& l)
        {
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, LAF::lcdText);
            l.setColour (juce::Label::backgroundColourId, LAF::lcdBackground);
            l.setFont (juce::FontOptions (8.5f));
        };
        addAndMakeVisible (faderPositionLabel);
        styleReadout (faderPositionLabel);
        addAndMakeVisible (peakLevelLabel);
        styleReadout (peakLevelLabel);
        peakLevelLabel.setColour (juce::Label::textColourId, juce::Colours::white);

        // Long-throw vertical fader.
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (-60.0, 6.0, 0.1);
        fader.setValue (0.0, juce::dontSendNotification);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setLookAndFeel (sharedLaf);
        // Real binding (Inspector Sync fix) — same "member read at call time"
        // reasoning as panKnob above, same undo/gesture bracket MixerStrip's
        // own volumeFader uses (volParam is an AutomatableParameter exactly
        // like panParam — the drag needs the same gesture markers).
        fader.onDragStart = [this]
        {
            if (volumePlugin == nullptr) return;
            volumePlugin->volParam->getEdit().getUndoManager().beginNewTransaction (
                "Tweak Volume: " + (track != nullptr ? track->getName() : juce::String()));
            volumePlugin->volParam->parameterChangeGestureBegin();
        };
        fader.onDragEnd = [this]
        {
            if (volumePlugin != nullptr) volumePlugin->volParam->parameterChangeGestureEnd();
        };
        fader.onValueChange = [this]
        {
            if (volumePlugin != nullptr)
                volumePlugin->setVolumeDb ((float) fader.getValue());

            faderPositionLabel.setText (juce::String (fader.getValue(), 1), juce::dontSendNotification);
        };
        faderPositionLabel.setText (juce::String (fader.getValue(), 1), juce::dontSendNotification);
        addAndMakeVisible (fader);

        // Live meter — ported from MixerStrip::timerCallback(); guarded to a
        // no-op whenever meterPlugin is null (no track set, or the track has
        // no LevelMeterPlugin yet), so this is always safe to run.
        startTimerHz (24);

        // "No Track Selected" visibility bug fix: every child above was just
        // added via addAndMakeVisible(), so WITHOUT this call they'd all sit
        // visible — on top of paint()'s "No Track Selected" text — from
        // construction until whatever first EXTERNAL setTrack() call happens
        // to arrive (which may not be immediate; nothing calls setTrack() at
        // app startup). track is still nullptr here, so this hides everything
        // right away, matching a genuinely empty strip from frame one.
        refreshVisibility();
    }

    ~InspectorStrip() override
    {
        stopTimer();

        if (volumePlugin != nullptr)
        {
            volumePlugin->volParam->removeListener (this);
            volumePlugin->panParam->removeListener (this);
        }

        if (meterPlugin != nullptr)
            meterPlugin->measurer.removeClient (meterClient);

        if (listenedState.isValid())
            listenedState.removeListener (this);

        // Clear every shared-LAF pointer before the LAF (owned by the parent
        // inspector) is destroyed — same discipline MixerStrip uses.
        inputCombo.setLookAndFeel (nullptr);
        outputCombo.setLookAndFeel (nullptr);
        panKnob.setLookAndFeel (nullptr);
        fader.setLookAndFeel (nullptr);
        channelCompButton.setLookAndFeel (nullptr);
        sendsViewport.setLookAndFeel (nullptr);
    }

    void setCaption (const juce::String& text)
    {
        captionLabel.setText (text, juce::dontSendNotification);
    }

    /** Task 2: compact I/O readout shown directly above Pan/Fader — replaces
        the old top-of-panel "Output: X" header. */
    void setIOCaption (const juce::String& text)
    {
        ioLabel.setText (text, juce::dontSendNotification);
    }

    /** Points this strip at a real track (or nullptr — see "Hide on Empty"
        below) — Master hides the Channel Comp button (space stays reserved,
        see class doc comment) and rebuilds the Sends list from the track's
        actual te::AuxSendPlugin instances (Task 5: no more hardcoded slots). */
    void setTrack (te::Track* newTrack)
    {
        // Unregister from whatever we were PREVIOUSLY bound to before
        // switching — unlike MixerStrip (one track for its whole lifetime),
        // this strip's track changes every time the user's selection changes,
        // so listener/meter-client wiring must be torn down and rebuilt here,
        // not just once in the constructor.
        if (listenedState.isValid())
            listenedState.removeListener (this);
        listenedState = juce::ValueTree();

        if (meterPlugin != nullptr)
            meterPlugin->measurer.removeClient (meterClient);
        meterPlugin = nullptr;

        // Inspector Sync fix — identical listener registration synced on every
        // setTrack() call: unregister from the OLD volumePlugin (mirrors the
        // meterPlugin teardown right above) before track is reassigned below.
        if (volumePlugin != nullptr)
        {
            volumePlugin->volParam->removeListener (this);
            volumePlugin->panParam->removeListener (this);
        }
        volumePlugin = nullptr;

        track = newTrack; // Ptr assignment from a raw engine pointer — adds OUR reference
        isMaster = (dynamic_cast<te::MasterTrack*> (track.get()) != nullptr);

        sendsContent->rebuild (track, laf); // pan_knob.png image asset, same as the main Pan knob — see Revert Send Knobs directive
        refreshSendsViewport();

        if (track != nullptr)
        {
            listenedState = track->state;
            listenedState.addListener (this);

            meterPlugin = track->pluginList.findFirstPluginOfType<te::LevelMeterPlugin>();
            if (meterPlugin != nullptr)
                meterPlugin->measurer.addClient (meterClient);

            // Inspector Sync fix — bind fader/panKnob to the EXACT same
            // VolumeAndPanPlugin the Arrangement TrackHeader and MixerStrip
            // manipulate for this same track, so this strip stops being deaf to
            // external changes. Both an immediate refresh (so a freshly-selected
            // track shows its real values right away) and the live listener
            // (so it keeps updating afterward) are needed here.
            volumePlugin = resolveVolumePlugin();
            if (volumePlugin != nullptr)
            {
                volumePlugin->volParam->addListener (this);
                volumePlugin->panParam->addListener (this);
                refreshVolPanFromEngine();
            }

            if (supportsMuteToggle)
                applyTrackColourToPlate();
        }
        else
        {
            meterLevelDb = meterFloorDb;
            peakHoldDb = meterFloorDb;
        }

        refreshVisibility();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // Strip body.
        g.setColour (LAF::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        // Hide on Empty — no track bound: draw ONLY a centred, dimmed message
        // in the empty space. No meters/fader/knobs/sends chrome at all (they're
        // all setVisible(false) in setTrack(), so nothing else paints itself
        // over this regardless).
        if (track == nullptr)
        {
            g.setColour (CrateColors::BrandGray);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText ("No Track Selected", getLocalBounds(), juce::Justification::centred);
            return;
        }

        // Sends section — omitted ENTIRELY for the Master track (UX fix: the
        // final summing bus has no valid "send to a bus" routing at all, so
        // showing an empty SENDS well there was a dead, invalid affordance).
        // The reserved HEIGHT still stays (see resized()'s own Height
        // Alignment invariant, same precedent as channelCompButton) — only
        // the well/caption paint and the header/+/viewport components
        // (refreshVisibility()) are skipped.
        if (! isMaster)
        {
            // Sends DarkBackground recessed well — same treatment MixerStrip's
            // Sends well uses, wrapping the header + viewport with a consistent
            // inner padding (sendsWellMargin).
            if (! sendsWellBounds.isEmpty())
            {
                g.setColour (CrateColors::DarkBackground);
                g.fillRoundedRectangle (sendsWellBounds, 3.0f);
            }

            // SENDS header — bigger, legible font (was 8.5f dim caption); dim
            // BrandGray text per the palette directive (was bright/white).
            g.setColour (CrateColors::BrandGray);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("SENDS", sendsCaptionBounds, juce::Justification::centredLeft);
        }

        // Hi-res audio meter next to the fader — ported from MixerStrip:
        // live meterLevelDb (wired via meterClient in timerCallback()) plus a
        // peak-hold marker line, identical visual language to the Mixer.
        g.setColour (CrateColors::DarkBackground);
        g.fillRect (meterBounds);

        const auto normalised = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
        const auto fillHeight = meterBounds.getHeight() * normalised;

        if (fillHeight > 0.0f)
        {
            const auto fillRect = meterBounds.withTop (meterBounds.getBottom() - fillHeight);
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

        // GR (Gain Reduction) meter — ported from MixerStrip, identical
        // rendering. NOTE: gainReductionDb is a placeholder (always 0) in
        // BOTH MixerStrip and here — no te::CompressorPlugin exists anywhere
        // in this app yet (CrateCompressorPopup's own doc comment already
        // discloses "not yet bound to a te::CompressorPlugin"), so this was
        // never actually live in the Mixer either. Porting the VISUAL exactly
        // as directed; the "bind to real GR values" half needs that DSP pass
        // first, in both places, not something left out of this port.
        g.setColour (CrateColors::DarkBackground);
        g.fillRect (grMeterBounds);

        constexpr float grRangeDb = 20.0f;
        const auto grNormalised = juce::jlimit (0.0f, 1.0f, gainReductionDb / grRangeDb);
        const auto grFillHeight = grMeterBounds.getHeight() * grNormalised;

        if (grFillHeight > 0.0f)
        {
            g.setColour (juce::Colour (0xffffb000)); // amber, hanging from the top — semantic meter colour (same carve-out as MixerStrip's)
            g.fillRect (grMeterBounds.withHeight (grFillHeight));
        }

        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawVerticalLine ((int) grMeterBounds.getX(), grMeterBounds.getY(), grMeterBounds.getBottom());
    }

    // STRICT INVARIANT (Task 2 Y-Axis fix): every single removeFromTop() call
    // below consumes a FIXED constant height, unconditionally, regardless of
    // isMaster/track state — never a value that depends on which track is
    // bound or whether a section happens to be empty. Only WHICH CHILD is
    // visible inside a given reserved slot may differ (channelCompButton
    // hidden for Master, sendSlots count varies) — the SLOT'S HEIGHT itself
    // never does. This is what guarantees selectedStrip and outputStrip can
    // never drift apart on the Y-axis, no matter what's bound to either.
    void resized() override
    {
        auto area = getLocalBounds().reduced (5);

        captionLabel.setBounds (area.removeFromTop (captionHeight)); // fixed height title block — same both strips, no "Output: X" text here
        area.removeFromTop (sectionGap);

        // 1. EQ THUMBNAIL.
        eqThumbnail->setBounds (area.removeFromTop (eqThumbnailHeight));
        area.removeFromTop (sectionGap);

        // 2. CHANNEL COMP TOGGLE — space ALWAYS reserved (Height Alignment,
        //    see class doc comment); only visibility differs for Master.
        channelCompButton.setBounds (area.removeFromTop (compButtonHeight));
        area.removeFromTop (sectionGap);

        // 3. SENDS — header row ("SENDS" caption + bigger "+" button), then a
        //    FIXED-HEIGHT Viewport (Task 3) that scrolls internally once there
        //    are more real sends than fit in sendsVisibleSlots. Wrapped in a
        //    DarkBackground well (sendsWellBounds, painted in paint()), sized
        //    off the REAL control bounds with a small outward pad — same
        //    "well wraps the real controls" relationship MixerStrip's own
        //    Sends/Routing wells use, not a well the controls are shrunk to
        //    fit inside.
        {
            auto sendsArea = area.removeFromTop (sendsHeaderHeight + sendsViewportHeight);
            sendsWellBounds = sendsArea.toFloat().expanded ((float) sendsWellMargin);

            auto header = sendsArea.removeFromTop (sendsHeaderHeight);
            addSendButton.setBounds (header.removeFromRight (addSendButtonSize).reduced (1));
            sendsCaptionBounds = header.toFloat();

            sendsViewport.setBounds (sendsArea.removeFromTop (sendsViewportHeight));
        }
        refreshSendsViewport();
        area.removeFromTop (sectionGap);

        // 4. INPUT / OUTPUT COMBOS.
        inputCombo.setBounds (area.removeFromTop (comboHeight));
        area.removeFromTop (3);
        outputCombo.setBounds (area.removeFromTop (comboHeight));
        area.removeFromTop (sectionGap);

        // Task 2: compact I/O caption directly above Pan/Fader (Ableton-style
        // "I/O block"), not a giant header at the top of the whole panel.
        ioLabel.setBounds (area.removeFromTop (ioCaptionHeight));
        area.removeFromTop (2);

        // 5. PAN KNOB + its value readout directly below it.
        panKnob.setBounds (area.removeFromTop (panKnobHeight).reduced (area.getWidth() / 4, 0));
        panValueLabel.setBounds (area.removeFromTop (panValueHeight));
        area.removeFromTop (2);

        // dB readouts (fader position | peak level), side by side — same
        // position between Pan and Fader as MixerStrip's own L5 row.
        {
            auto row = area.removeFromTop (dbReadoutHeight);
            faderPositionLabel.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (1, 0));
            peakLevelLabel.setBounds (row.reduced (1, 0));
        }
        area.removeFromTop (sectionGap);

        // 6. LONG-THROW FADER + METER (+ GR meter). Sliced from the SAME
        //    remaining rect the fader fills, same order MixerStrip uses:
        //    GR meter (rightmost), then the main meter, then the fader.
        grMeterBounds = area.removeFromRight (grMeterColumnWidth).reduced (1, 0).toFloat();
        meterBounds   = area.removeFromRight (meterColumnWidth).reduced (2, 0).toFloat();
        area.removeFromRight (3);
        fader.setBounds (area);
    }

private:
    void showCompressorPopup()
    {
        // Manual toggle-state lifecycle, matching MixerStrip's own
        // openChannelCompPopup() exactly — see the constructor's comment on
        // channelCompButton for why this fixes the toggle/popup desync bug.
        channelCompButton.setToggleState (true, juce::dontSendNotification);

        auto popup = std::make_unique<CrateCompressorPopup> (laf);
        auto& box = juce::CallOutBox::launchAsynchronously (std::move (popup),
            channelCompButton.getScreenBounds(), nullptr);
        box.addComponentListener (this);
    }

    // juce::ComponentListener — fires when the CallOutBox we registered on in
    // showCompressorPopup() is dismissed (clicking the button again, or
    // anywhere outside the popup), un-toggling channelCompButton so it can
    // never stay stuck lit after the popup actually closes.
    void componentBeingDeleted (juce::Component&) override
    {
        channelCompButton.setToggleState (false, juce::dontSendNotification);
    }

    // Dynamic Sends Routing directive — replaces the old "always append the
    // next sequential bus number" behaviour (which forced creating Bus 1 and
    // 2 just to reach Bus 3) with a real routing choice: a menu of buses
    // OTHER tracks in the edit already send to (that THIS track doesn't yet),
    // plus "Create New Bus..." for a genuinely fresh one. SendsContent::rebuild()
    // already only ever draws a slot per REAL te::AuxSendPlugin found on this
    // track (see its own doc comment) — the empty-slot clutter this directive
    // flags was entirely in how new sends got CREATED, not how they're drawn.
    void addNewSend()
    {
        if (track == nullptr)
            return;

        // The Inspector "Mirror" fix — queries the absolute, always-current
        // GLOBAL te::Edit directly from CrateWorkflowManager, the same
        // authority MainComponent itself uses everywhere else, instead of
        // going through track->edit (a cached track pointer's own reference).
        auto& edit = workflow.getEdit();
        const auto scan = SendBusUtils::scanBuses (edit, track.get());

        // Menu contents now come from SendBusUtils::buildSendMenu() (Hybrid
        // Bus/Return Architecture directive) — one source of truth for the
        // label formatting/separator/macro-item layout, shared with
        // MixerStrip's identical menu.
        auto menuBuild = SendBusUtils::buildSendMenu (edit, scan);
        auto& menu = menuBuild.menu;
        auto& menuIdToBusNumber = menuBuild.menuIdToBusNumber;
        const int createFXChannelItemId = menuBuild.createFXChannelItemId;

        // Async — never blocks the message thread, and safe against this
        // strip (or its track) going away before the user picks something:
        // juce::Component::SafePointer + a re-checked `track` pointer cover
        // both "the whole Inspector strip was destroyed" and "the user
        // switched to a different/no track while the menu was open."
        juce::Component::SafePointer<InspectorStrip> safeThis (this);
        te::Track::Ptr trackAtMenuOpenTime = track; // ref-counted — see the UAF fix note below
        CrateWorkflowManager* workflowPtr = &workflow; // long-lived (owned by MainComponent)

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addSendButton),
            [safeThis, trackAtMenuOpenTime, menuIdToBusNumber, createFXChannelItemId, workflowPtr] (int result)
            {
                if (result == 0 || safeThis == nullptr || safeThis->track.get() != trackAtMenuOpenTime.get())
                    return; // dismissed, or the strip/selection moved on

                if (result == createFXChannelItemId)
                {
                    // Use-After-Free fix (same root cause as MixerStrip::addNewSend's
                    // identical menu): createAndRouteNewFXChannel() creates a new
                    // track, which fires CrateWorkflowManager::onTrackListChanged ->
                    // ArrangementComponent::rebuildTracks() -> MixerComponent::
                    // rebuildStrips(). This InspectorStrip isn't itself destroyed by
                    // that cascade today, but calling it synchronously here still
                    // ran the Mixer's whole teardown/rebuild mid-callback, on this
                    // Send-menu callback's own call stack — deferred via callAsync
                    // so that rebuild always runs on a clean message-loop iteration
                    // instead, matching MixerStrip's own fix and no longer
                    // depending on which docks happen to survive the cascade today.
                    // trackAtMenuOpenTime is a ref-counted Ptr, so the real engine
                    // track stays alive across the hop regardless.
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

    void createSendToBus (int busNumber)
    {
        if (track == nullptr)
            return;

        auto& edit = workflow.getEdit(); // same global-Edit authority as addNewSend() above
        auto plugin = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, juce::PluginDescription());

        if (plugin == nullptr)
            return;

        if (auto* send = dynamic_cast<te::AuxSendPlugin*> (plugin.get()))
            send->busNumber = busNumber;

        edit.getUndoManager().beginNewTransaction ("Add Send to Bus " + juce::String (busNumber) + ": " + track->getName());
        track->pluginList.insertPlugin (plugin, -1, nullptr);

        sendsContent->rebuild (track, laf);
        refreshSendsViewport();
    }

    // Task 3 fix: explicitly recomputes sendsContent's bounds AND re-syncs
    // sendsViewport to them — setSize()/resized() alone were leaving the
    // Viewport's internal scroll range stuck at whatever fit during the FIRST
    // layout pass, capping visible sends at sendsVisibleSlots regardless of
    // how many were actually added afterward. Calling setViewedComponent()
    // again forces JUCE's Viewport to recompute its scrollbar/scroll range
    // from the content's CURRENT bounds (verified against juce_Viewport.cpp:
    // setViewedComponent() unconditionally calls updateVisibleArea() at the
    // end), which is the same internal call a fresh setViewedComponent() at
    // construction time already relies on to get its FIRST layout right.
    void refreshSendsViewport()
    {
        sendsContent->setBounds (0, 0, sendsViewport.getMaximumVisibleWidth(), sendsContent->getRequiredHeight());
        sendsContent->resized(); // same no-op-setBounds fix as SendsContent::relayout() — force it unconditionally
        sendsViewport.setViewedComponent (sendsContent.get(), false);
    }

    // Hide on Empty — the ONE place that decides which children are visible,
    // called from both the constructor (track is nullptr there) and every
    // setTrack() call, so there is no window where a child's visibility is
    // whatever addAndMakeVisible() left it at. Definitive: resized() never
    // calls setVisible() itself, so nothing downstream can silently re-show
    // a hidden child on the next layout pass.
    void refreshVisibility()
    {
        const bool hasTrack = (track != nullptr);
        captionLabel.setVisible (hasTrack);
        eqThumbnail->setVisible (hasTrack);
        channelCompButton.setVisible (hasTrack && ! isMaster);
        addSendButton.setVisible (hasTrack && ! isMaster); // Master has no valid "send to a bus" routing — see paint()'s matching guard
        sendsViewport.setVisible (hasTrack && ! isMaster);
        ioLabel.setVisible (hasTrack);
        inputCombo.setVisible (hasTrack);
        outputCombo.setVisible (hasTrack);
        panKnob.setVisible (hasTrack);
        panValueLabel.setVisible (hasTrack);
        faderPositionLabel.setVisible (hasTrack);
        peakLevelLabel.setVisible (hasTrack);
        fader.setVisible (hasTrack);
    }

    // Ableton Mute paradigm, ported from MixerStrip::applyTrackColourToPlate()
    // — only ever called when supportsMuteToggle is true.
    void applyTrackColourToPlate()
    {
        trackAccentColour = (track != nullptr && ! track->getColour().isTransparent())
                                ? track->getColour()
                                : juce::Colour (0xff30506a); // default track identity colour — same fallback MixerStrip uses, not brand chrome

        const bool trackIsMuted = track != nullptr && track->isMuted (false);

        if (trackIsMuted)
        {
            captionLabel.setColour (juce::Label::backgroundColourId, CrateColors::DarkBackground);
            captionLabel.setColour (juce::Label::textColourId, CrateColors::BrandGray);
        }
        else
        {
            captionLabel.setColour (juce::Label::backgroundColourId, trackAccentColour);
            captionLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        }

        captionLabel.repaint();
    }

    // Inspector Sync fix — resolves volumePlugin for the CURRENT track (raw
    // te::Track*, so it must handle both AudioTrack and MasterTrack, unlike
    // MixerStrip which only ever binds one AudioTrack for its whole lifetime).
    // te::Track has no virtual getVolumePlugin() — AudioTrack and MasterTrack
    // reach their VolumeAndPanPlugin through entirely different paths (a plugin
    // on the track's own pluginList vs. edit.getMasterVolumePlugin()).
    te::VolumeAndPanPlugin::Ptr resolveVolumePlugin() const
    {
        if (track == nullptr)
            return nullptr;

        if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track.get()))
            return audioTrack->getVolumePlugin(); // raw -> Ptr, implicit — adds a reference

        if (dynamic_cast<te::MasterTrack*> (track.get()) != nullptr)
            return workflow.getEdit().getMasterVolumePlugin(); // already a Ptr

        return nullptr;
    }

    // Pushes volumePlugin's CURRENT engine values into fader/panKnob — called
    // once from setTrack() (so a freshly-selected track shows its real values
    // immediately) and from currentValueChanged() (so an external change, e.g.
    // the Arrangement TrackHeader's own fader/pan knob, mirrors here live).
    // dontSendNotification: this is the engine -> UI direction, so it must NOT
    // re-trigger onValueChange (which would just write the same value straight
    // back to the engine).
    void refreshVolPanFromEngine()
    {
        if (volumePlugin == nullptr)
            return;

        fader.setValue (volumePlugin->getVolumeDb(), juce::dontSendNotification);
        faderPositionLabel.setText (juce::String (fader.getValue(), 1), juce::dontSendNotification);

        panKnob.setValue (volumePlugin->getPan(), juce::dontSendNotification);
        panValueLabel.setText (panValueText (volumePlugin->getPan()), juce::dontSendNotification);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! supportsMuteToggle || track == nullptr)
            return;

        if (! captionLabel.getBounds().contains (e.getPosition()))
            return;

        // Guarded to a single click, same as MixerStrip's own name-plate
        // mute toggle (no rename/colour editor exists here to disambiguate
        // against, but keeping the identical guard costs nothing).
        if (e.getNumberOfClicks() != 1)
            return;

        auto* audioTrack = dynamic_cast<te::AudioTrack*> (track.get());
        if (audioTrack == nullptr)
            return;

        const bool newState = ! audioTrack->isMuted (false);
        audioTrack->edit.getUndoManager().beginNewTransaction ("Toggle Mute: " + audioTrack->getName());
        audioTrack->edit.getUndoManager().perform (new TrackMuteAction (audioTrack, newState));
    }

    // Bubbled from sendsViewport.addMouseListener(this, true) — hovering
    // ANYWHERE inside the Sends viewport (rows, knobs) repaints so the
    // auto-hide scrollbar thumb reveals itself, same wiring MixerStrip's own
    // SendsSection uses.
    void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override   { repaint(); }

    // juce::ValueTree::Listener — mirrors MixerStrip's own valueTreePropertyChanged
    // for mute/colour, so external changes (the Mixer's own name plate, the
    // Arrangement TrackHeader, Undo/Redo) stay in sync here too.
    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& property) override
    {
        if (! supportsMuteToggle || track == nullptr || v != track->state)
            return;

        if (property == te::IDs::mute || property == te::IDs::colour)
            applyTrackColourToPlate();
    }

    // te::AutomatableParameter::Listener — Inspector Sync fix. Fires when
    // Volume/Pan change from ANYWHERE ELSE (the Arrangement TrackHeader's
    // fader/pan knob, MixerStrip's, automation playback, a script) — not just
    // this strip's own fader/panKnob. Same message-thread-only + SafePointer
    // deferral TrackHeaderComponent's identical listener uses (TE itself
    // asserts this fires message-thread-only; the callAsync buys safety
    // against a track-deletion racing the deferred lambda, not an audio-thread
    // hazard). Refreshes both controls unconditionally — cheaper than tracking
    // which of volParam/panParam actually changed.
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override
    {
        juce::Component::SafePointer<InspectorStrip> safeThis (this);

        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->refreshVolPanFromEngine();
        });
    }

    // Live Sync Bug fix — mirrors MixerStrip's own valueTreeChildAdded/Removed
    // exactly (same te::IDs::PLUGIN filter, same async hop). Without this,
    // adding/removing a send from the OTHER view (e.g. the Mixer's "+" button)
    // never rebuilt this Inspector strip's Sends list — it only ever refreshed
    // when the Inspector's OWN "+" button was the one clicked, since that path
    // calls sendsContent->rebuild() directly. Both directions now go through
    // the SAME engine-level notification, so either view initiating a routing
    // change instantly rebuilds the other.
    void valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree) override
    {
        if (track == nullptr || parentTree != track->state || ! childTree.hasType (te::IDs::PLUGIN))
            return;

        juce::Component::SafePointer<InspectorStrip> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis == nullptr)
                return;

            safeThis->sendsContent->rebuild (safeThis->track, safeThis->laf);
            safeThis->refreshSendsViewport();
        });
    }

    void valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int) override
    {
        if (track == nullptr || parentTree != track->state || ! childTree.hasType (te::IDs::PLUGIN))
            return;

        juce::Component::SafePointer<InspectorStrip> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis == nullptr)
                return;

            safeThis->sendsContent->rebuild (safeThis->track, safeThis->laf);
            safeThis->refreshSendsViewport();
        });
    }

    // Live meter — ported from MixerStrip::timerCallback(). No-ops whenever
    // meterPlugin is null (no track set, or the track has no LevelMeterPlugin).
    void timerCallback() override
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

        // Do NOT rely on paint() for this value — set directly on the Label,
        // same as MixerStrip's own timerCallback().
        peakLevelLabel.setText (peakHoldDb > meterFloorDb + 0.5f ? juce::String (peakHoldDb, 1)
                                                                  : juce::String ("-inf"),
                                juce::dontSendNotification);

        repaint();
    }

    juce::Label captionLabel;
    std::unique_ptr<CrateEQThumbnail> eqThumbnail;

    juce::TextButton channelCompButton { "Channel Comp" };
    juce::LookAndFeel* laf = nullptr; // shared mixer look/feel, reused for the popup's rotaries
    juce::LookAndFeel* hardwareLaf = nullptr; // hardware-slot chrome for combos/comp/send-knob

    AddIconButton addSendButton;
    NoWheelViewport sendsViewport;
    std::unique_ptr<SendsContent> sendsContent;

    juce::Label ioLabel;
    juce::ComboBox inputCombo, outputCombo;
    juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label  panValueLabel; // "C" / "20 L" / "20 R" readout directly below panKnob
    juce::Label  faderPositionLabel, peakLevelLabel; // dB readouts, between Pan and Fader — ported from MixerStrip's L5
    juce::Slider fader   { juce::Slider::LinearVertical, juce::Slider::NoTextBox };

    // UAF fix: track/volumePlugin/meterPlugin were all raw, non-owning pointers
    // — since InspectorStrip is LONG-LIVED (constructed once, re-pointed via
    // setTrack() on every selection change, unlike MixerStrip/TrackHeaderComponent
    // which are destroyed+recreated whenever a track goes away), a track
    // deletion that happened to be the currently-inspected one left this strip
    // holding dangling pointers with an ACTIVE te::AutomatableParameter::Listener
    // registration still attached — a real, confirmed use-after-free (see
    // MainComponent.cpp's onDeleteTrackRequested for the other half of this fix).
    // All three are now REFERENCE-COUNTED: assigning a raw engine pointer into
    // a Ptr member (below, in setTrack()) adds OUR OWN reference, so the
    // underlying object stays alive until WE release it — regardless of
    // whether the track's own pluginList/Edit drops its reference first. This
    // makes the class of bug structurally impossible, independent of get
    // ting the teardown-ordering exactly right at every call site.
    te::Track::Ptr track; // was: te::Track* track = nullptr;
    // te::VolumeAndPanPlugin::Ptr — same alias TrackHeaderComponent's own
    // (already-safe) volumePlugin member uses.
    te::VolumeAndPanPlugin::Ptr volumePlugin;
    bool isMaster = false;
    const bool supportsMuteToggle; // true for the LEFT (selected-track) strip only
    CrateWorkflowManager& workflow; // the Inspector "Mirror" fix — addNewSend() queries workflow.getEdit() directly, not track->edit

    // Real, live meter state — wired to the track's te::LevelMeterPlugin via
    // meterClient in setTrack()/timerCallback(), same pattern MixerStrip uses.
    // Starts at the -INF floor and stays there whenever no track/meter is set.
    // te::LevelMeterPlugin has no dedicated ::Ptr alias (only the base
    // te::Plugin::Ptr exists) — juce::ReferenceCountedObjectPtr instantiated
    // directly against the DERIVED type gives both ref-counting AND typed
    // ->measurer access in one member, no separate raw pointer needed.
    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> meterPlugin;
    te::LevelMeasurer::Client meterClient;
    float meterLevelDb = meterFloorDb;
    float peakHoldDb = meterFloorDb;
    juce::int64 peakHoldLastUpdateMs = 0;

    // The ValueTree currently listened to (this strip's track->state) — torn
    // down/rebuilt every setTrack() call, since (unlike MixerStrip) this
    // strip's track changes repeatedly as the user's selection changes.
    juce::ValueTree listenedState;
    juce::Colour trackAccentColour { 0xff30506a };

    // Computed in resized(), read by paint().
    juce::Rectangle<float> sendsCaptionBounds, meterBounds, sendsWellBounds, grMeterBounds;

    // GR meter placeholder — see paint()'s doc comment: unbound in MixerStrip
    // too, no te::CompressorPlugin exists anywhere in this app yet.
    float gainReductionDb = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InspectorStrip)
};

//==============================================================================
CrateTrackInspectorComponent::CrateTrackInspectorComponent (CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse)
{
    // Legacy Header Removal directive: the old collapsible "[v] Track Name"
    // band above the two strips is gone entirely — it reserved a whole row's
    // height but its collapse toggle only ever hid the text (not the row
    // itself), and each InspectorStrip already shows its own track name on
    // its own Mute-toggle name plate now, making this a redundant duplicate.
    selectedStrip = std::make_unique<InspectorStrip> ("Selected", &mixerLookAndFeel, &hardwareSlotLookAndFeel, true, workflow);
    outputStrip   = std::make_unique<InspectorStrip> ("Master",   &mixerLookAndFeel, &hardwareSlotLookAndFeel, false, workflow);
    addAndMakeVisible (*selectedStrip);
    addAndMakeVisible (*outputStrip);
}

CrateTrackInspectorComponent::~CrateTrackInspectorComponent() = default;

void CrateTrackInspectorComponent::setTrack (te::Track* trackToShow)
{
    track = trackToShow;

    if (track != nullptr)
    {
        selectedStrip->setCaption (track->getName());
        selectedStrip->setTrack (track);

        // Resolve the REAL routing destination rather than hardcoding "Master":
        // an AudioTrack that explicitly routes to another track shows that
        // track; a null destination genuinely means "goes to the default
        // output" in TE's own TrackOutput model, which is Master unless this
        // track IS Master (nothing routes further from there).
        te::Track* destination = nullptr;

        if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track))
        {
            if (auto* destTrack = audioTrack->getOutput().getDestinationTrack())
                destination = destTrack;
            else
                destination = audioTrack->edit.getMasterTrack();
        }

        outputStrip->setTrack (destination);

        if (destination != nullptr)
        {
            outputStrip->setCaption (destination->getName());
            selectedStrip->setIOCaption ("OUT: " + destination->getName());
            outputStrip->setIOCaption ("Master Bus");
        }
        else
        {
            // track IS the Master track (or a type with no further destination) —
            // nothing to route to.
            outputStrip->setCaption ("-");
            selectedStrip->setIOCaption ("OUT: -");
            outputStrip->setIOCaption ("-");
        }
    }
    else
    {
        selectedStrip->setCaption ("Selected");
        selectedStrip->setTrack (nullptr);
        selectedStrip->setIOCaption ({});
        outputStrip->setCaption ("Master");
        outputStrip->setTrack (nullptr);
        outputStrip->setIOCaption ({});
    }

    repaint();
}

void CrateTrackInspectorComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);
}

void CrateTrackInspectorComponent::resized()
{
    // Legacy Header Removal directive: both strips now start right at the
    // top margin and span the FULL available height — no reserved header row.
    auto area = getLocalBounds();

    // Two parallel strips side by side, equal width — Height Alignment (Task
    // 2): both get the exact SAME rectangle height here, and (per
    // InspectorStrip's own doc comment) never branch their internal layout by
    // track type, so every row lands on the same Y in both columns.
    auto strips = area.reduced (4);
    const int halfWidth = (strips.getWidth() - 4) / 2;
    selectedStrip->setBounds (strips.removeFromLeft (halfWidth));
    strips.removeFromLeft (4);
    outputStrip->setBounds (strips);
}
