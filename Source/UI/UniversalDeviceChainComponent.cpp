#include "UniversalDeviceChainComponent.h"
#include "TheCrateLookAndFeel.h"
#include "../Engine/CrateSandboxBridge.h"

#include <algorithm>

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Ableton-style collapsed device: a narrow vertical sliver — bypass circle,
    // wrench icon, rotated name, and a right-edge level meter — not a
    // horizontal header-only strip. Click anywhere on it to unfold.
    // STRICT per Lead UX Architect spec: exactly 44px, not "about" 44px — every
    // dimension below (reduce/padding/meter-strip width) is sized to fit
    // comfortably INSIDE this exact width, not the other way around.
    constexpr int foldedStripWidth      = 44;
    constexpr int foldedMeterStripWidth = 4;   // right-edge vertical meter, folded state only

    static_assert (foldedStripWidth == 44, "Folded DeviceBlock width is a hard UX spec, not a tunable");

    constexpr float meterFloorDb = -60.0f;
    constexpr float meterRangeDb = 66.0f; // floor to +6 dB headroom — same convention as MixerStrip's meter

    constexpr int addDeviceBlockWidth   = 90; // trailing "+ Add Device" block, end of chain — see ChainRowContent
    constexpr int interBlockGap         = 0; // "Ableton Tight" (Task 4): zero dead space — devices sit pixel-flush
    constexpr int headerRowHeight       = 20;
    constexpr int headerButtonSize      = 20; // Ableton Tight: every header icon button is this ONE fixed square, no ad-hoc widths
    constexpr int headerButtonGap       = 4;  // uniform gap between every header button, both sides

    constexpr int xyPadSize             = 120;
    constexpr int xyPadComboRowHeight   = 20;
    constexpr int xyPadComboGap         = 2;  // stacked X/Y combo gap — see XYPadComponent::resized()
    constexpr int xyPadColumnWidth      = 130; // pad + margin

    constexpr int miniSliderWidth       = 90;
    constexpr int miniSliderHeight      = 38; // tighter — Ableton's reference cells are denser than 48px
    constexpr int gridMaxHeight         = 150; // Ableton-style column cap — wraps into new columns, not taller ones
    constexpr int gridColumnGap         = 4;

    // Every one of these is used BY NAME in both DeviceBlock::getPreferredWidth()
    // and DeviceBlock::resized() — sharing the constants (not recomputing the
    // same margins as separate literals in two places) is what guarantees the
    // preferred-width formula and the actual laid-out geometry can't drift
    // apart, which is what let the last-turn "+10" guess go wrong.
    // Small perimeter inset on all 4 sides — NOT the header-to-grid gap (that
    // stays permanently zero, per the explicit "Absolute Flush Geometry"
    // directive). Without this, header buttons sit at literal (0,0) and their
    // square bounds visually clip past the block's own rounded top corners —
    // this is the minimum needed to keep every control fully inside the
    // rounded container.
    constexpr int blockOuterPadding      = 2;

    // XY Pad / Header Z-order fix: paint()'s blue header band used to be a
    // hardcoded "4 + headerRowHeight" (= 24px) — a leftover from before
    // blockOuterPadding existed. resized() actually starts the header row at
    // y = blockOuterPadding and ends it at blockOuterPadding + headerRowHeight
    // (= 22px with the current 2px padding), so the band was being painted 2px
    // TALLER than the real header content. Since the XY pad (a child
    // component, painted AFTER this band) starts at that same real 22px
    // boundary, its opaque content covered the band's extra bottom 2px —
    // reading as the XY pad "clipping into" the header. Deriving this
    // directly from blockOuterPadding makes the two impossible to drift
    // apart again.
    constexpr int headerBandHeight       = blockOuterPadding + headerRowHeight;
    constexpr int xyToGridGap            = 8; // gap between the XY pad and the slider grid
    constexpr int gridRightBreathingRoom = 8; // reserved AFTER the grid's last column, outside the FlexBox entirely

    constexpr int maxAutoPopulateParams = 64;

    const auto blockBodyColour     = juce::Colours::darkgrey.darker(); // ALWAYS this — selection/focus never repaints the body
    const auto blockHeaderColour   = juce::Colour (0xff232328);
    const auto blockSelectedColour = juce::Colour (0xff3a6ea5); // "prominent active" light blue — header band only
    const auto bypassOffColour     = juce::Colour (0xff5a5a60);

    // AutomatableParameter::getValueRange() returns a plain juce::Range<float>,
    // not a NormalisableRange — no convertTo0to1()/convertFrom0to1() members.
    float normalise01 (juce::Range<float> range, float value)
    {
        return range.getLength() > 0.0f ? (value - range.getStart()) / range.getLength() : 0.5f;
    }

    float denormalise01 (juce::Range<float> range, float t)
    {
        return range.getStart() + t * range.getLength();
    }

    // Same utility-plugin exclusion MixerStrip's InsertsBlock uses — the device
    // chain shows things a user drags/reorders/generates with, not the always-
    // present volume/pan/metering plumbing or aux sends/returns (those live in
    // the Pro Tools-style mixer strip's Sends section instead — Hybrid Bus/
    // Return Architecture: AuxReturnPlugin is the OTHER half of a bus, same
    // "internal routing plumbing, not a user-facing effect" category
    // AuxSendPlugin already was).
    bool isChainablePlugin (const te::Plugin& p)
    {
        return dynamic_cast<const te::VolumeAndPanPlugin*> (&p) == nullptr
            && dynamic_cast<const te::LevelMeterPlugin*> (&p) == nullptr
            && dynamic_cast<const te::AuxSendPlugin*> (&p) == nullptr
            && dynamic_cast<const te::AuxReturnPlugin*> (&p) == nullptr;
    }

    //==========================================================================
    // Per-plugin "which parameters are exposed in the Device Chain grid" list.
    // Same convention as AutomationLaneComponent's "crateAnchors" property: a
    // custom, TE-opaque string property on the plugin's OWN state ValueTree
    // (confirmed public: te::Plugin::state), round-tripping through normal
    // Edit save/load and Undo/Redo (setProperty is passed a real UndoManager)
    // for free, with zero extra persistence code.
    const juce::Identifier configuredParamsProperty ("crateConfiguredParams");

    juce::StringArray readConfiguredParamIDs (te::Plugin& plugin)
    {
        return juce::StringArray::fromTokens (plugin.state[configuredParamsProperty].toString(), "|", "");
    }

    void writeConfiguredParamIDs (te::Plugin& plugin, const juce::StringArray& ids)
    {
        plugin.state.setProperty (configuredParamsProperty, ids.joinIntoString ("|"), &plugin.edit.getUndoManager());
    }

    // The EFFECTIVE configured list, accounting for the "never explicitly
    // configured yet" bootstrap case. Deliberately NOT persisted here — only an
    // explicit user action (Configure-mode discovery, or deleting a slider)
    // writes the property. Persisting this derived state on every read would
    // turn "nothing configured yet" into "explicitly configured to exactly
    // these N params" the instant this function is called, which would then
    // make a >64-param plugin (which should stay empty until the user opts in
    // via Configure) impossible to distinguish from a <=64 one that already
    // went through auto-populate.
    std::vector<te::AutomatableParameter*> getConfiguredParams (te::Plugin& plugin)
    {
        const auto ids = readConfiguredParamIDs (plugin);
        const auto allParams = plugin.getAutomatableParameters();

        std::vector<te::AutomatableParameter*> result;

        if (ids.isEmpty())
        {
            if (allParams.size() <= maxAutoPopulateParams)
                for (auto* p : allParams)
                    if (p != nullptr)
                        result.push_back (p);

            return result; // > 64 and never configured: stays empty, forces Configure
        }

        for (auto& id : ids)
            for (auto* p : allParams)
                if (p != nullptr && p->paramID == id)
                {
                    result.push_back (p);
                    break;
                }

        return result;
    }
}

//==============================================================================
// Small circular bypass toggle — Component, not Button: JUCE's Button can't
// easily be made circular without a full LookAndFeel override, and this is
// simple enough to hand-draw directly. "On" == plugin.isEnabled() (not
// bypassed) — matches Plugin::setEnabled()'s own polarity.
class CircularToggleButton : public juce::Component
{
public:
    // Declared explicitly: JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR below
    // declares a deleted copy constructor, which suppresses the implicit
    // default constructor — needed since DeviceBlock holds this as a plain
    // (not unique_ptr) member, default-constructed implicitly.
    CircularToggleButton() = default;

    std::function<void (bool)> onToggle;

    void setToggleState (bool shouldBeOn, juce::NotificationType nt)
    {
        if (on == shouldBeOn)
            return;

        on = shouldBeOn;
        repaint();

        if (nt != juce::dontSendNotification && onToggle)
            onToggle (on);
    }

    bool getToggleState() const noexcept   { return on; }

    void mouseUp (const juce::MouseEvent&) override
    {
        setToggleState (! on, juce::sendNotificationSync);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.5f);
        g.setColour (on ? LAF::accent : bypassOffColour);
        g.fillEllipse (bounds);
        g.setColour (LAF::background);
        g.drawEllipse (bounds, 1.0f);
    }

private:
    bool on = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircularToggleButton)
};

//==============================================================================
// Ableton-style XY pad: dark square, small round thumb, two dropdowns beneath
// mapping the pad's X/Y axes onto two of the block's configured parameters.
class XYPadComponent : public juce::Component,
                        private te::AutomatableParameter::Listener
{
public:
    explicit XYPadComponent (std::vector<te::AutomatableParameter*> availableParamsIn)
        : availableParams (std::move (availableParamsIn))
    {
        addAndMakeVisible (xCombo);
        addAndMakeVisible (yCombo);

        xCombo.addItem ("None", 1);
        yCombo.addItem ("None", 1);

        int itemId = 2;
        for (auto* p : availableParams)
        {
            if (p == nullptr)
                continue;

            xCombo.addItem (p->getParameterName(), itemId);
            yCombo.addItem (p->getParameterName(), itemId);
            ++itemId;
        }

        // Default mapping: 1st configured parameter -> X, 2nd -> Y.
        xCombo.setSelectedId (availableParams.size() >= 1 ? 2 : 1, juce::dontSendNotification);
        yCombo.setSelectedId (availableParams.size() >= 2 ? 3 : 1, juce::dontSendNotification);

        xCombo.onChange = [this] { applyComboSelection(); };
        yCombo.onChange = [this] { applyComboSelection(); };

        applyComboSelection();
    }

    ~XYPadComponent() override
    {
        detachListeners();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto side = juce::jmin (xyPadSize, area.getWidth());
        padBounds = area.removeFromTop (xyPadSize).withWidth (side);
        area.removeFromTop (4);

        // Stacked, not side-by-side: splitting the column in half truncated
        // every parameter name ("Dr...", "We..."). Each combo now gets the
        // FULL column width, one above the other — matches Ableton's own
        // XY-pad axis selectors (full-width, stacked).
        xCombo.setBounds (area.removeFromTop (xyPadComboRowHeight).withWidth (side));
        area.removeFromTop (xyPadComboGap);
        yCombo.setBounds (area.removeFromTop (xyPadComboRowHeight).withWidth (side));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (LAF::background);
        g.fillRect (padBounds);
        g.setColour (LAF::panelLight);
        g.drawRect (padBounds, 1);

        const auto thumb = thumbPosition();
        g.setColour (LAF::accent);
        g.fillEllipse (thumb.x - 5.0f, thumb.y - 5.0f, 10.0f, 10.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! padBounds.contains (e.getPosition()))
            return;

        if (xParam != nullptr)
        {
            xParam->getEdit().getUndoManager().beginNewTransaction ("Tweak XY Pad: " + xParam->getParameterName());
            xParam->parameterChangeGestureBegin();
        }

        if (yParam != nullptr)
            yParam->parameterChangeGestureBegin();

        updateFromMouse (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override   { updateFromMouse (e); }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (xParam != nullptr) xParam->parameterChangeGestureEnd();
        if (yParam != nullptr) yParam->parameterChangeGestureEnd();
    }

private:
    juce::Point<float> thumbPosition() const
    {
        const float nx = xParam != nullptr ? normalise01 (xParam->getValueRange(), xParam->getCurrentValue()) : 0.5f;
        const float ny = yParam != nullptr ? normalise01 (yParam->getValueRange(), yParam->getCurrentValue()) : 0.5f;

        // Y inverted: top of the pad = maximum value, matching Ableton/standard XY pad convention.
        return { (float) padBounds.getX() + nx * (float) padBounds.getWidth(),
                 (float) padBounds.getBottom() - ny * (float) padBounds.getHeight() };
    }

    void updateFromMouse (const juce::MouseEvent& e)
    {
        if (padBounds.getWidth() <= 0 || padBounds.getHeight() <= 0)
            return;

        const auto nx = juce::jlimit (0.0f, 1.0f, ((float) e.position.x - (float) padBounds.getX()) / (float) padBounds.getWidth());
        const auto ny = juce::jlimit (0.0f, 1.0f, 1.0f - ((float) e.position.y - (float) padBounds.getY()) / (float) padBounds.getHeight());

        if (xParam != nullptr)
            xParam->setParameter (denormalise01 (xParam->getValueRange(), nx), juce::sendNotificationSync);

        if (yParam != nullptr)
            yParam->setParameter (denormalise01 (yParam->getValueRange(), ny), juce::sendNotificationSync);

        repaint();
    }

    void applyComboSelection()
    {
        detachListeners();

        const auto xId = xCombo.getSelectedId();
        const auto yId = yCombo.getSelectedId();

        xParam = (xId >= 2 && (size_t) (xId - 2) < availableParams.size()) ? availableParams[(size_t) (xId - 2)] : nullptr;
        yParam = (yId >= 2 && (size_t) (yId - 2) < availableParams.size()) ? availableParams[(size_t) (yId - 2)] : nullptr;

        if (xParam != nullptr) xParam->addListener (this);
        if (yParam != nullptr) yParam->addListener (this);

        repaint();
    }

    void detachListeners()
    {
        if (xParam != nullptr) xParam->removeListener (this);
        if (yParam != nullptr) yParam->removeListener (this);
    }

    // te::AutomatableParameter::Listener — external changes (automation, the
    // grid's own MiniParamSliders, another view) must move the thumb too.
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override
    {
        juce::Component::SafePointer<XYPadComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis] { if (safeThis != nullptr) safeThis->repaint(); });
    }

    std::vector<te::AutomatableParameter*> availableParams;
    te::AutomatableParameter* xParam = nullptr;
    te::AutomatableParameter* yParam = nullptr;
    juce::Rectangle<int> padBounds;

    juce::ComboBox xCombo, yCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XYPadComponent)
};

//==============================================================================
// Minimal parameter slider: name / live value text / thin line + triangle
// pointer. Custom-painted rather than juce::Slider::LinearBar — full control
// over the exact three-zone layout the Lead UX Architect specified, and this
// needs click-to-select + keyPressed (Delete/Backspace) anyway, which is
// simpler on a plain Component than fighting Slider's own mouse handling.
class MiniParamSlider : public juce::Component,
                        private te::AutomatableParameter::Listener
{
public:
    explicit MiniParamSlider (te::AutomatableParameter& p) : param (p)
    {
        setWantsKeyboardFocus (true);
        param.addListener (this);
    }

    ~MiniParamSlider() override
    {
        param.removeListener (this);
    }

    // Fires on Delete/Backspace while this slider has keyboard focus — Configure
    // mechanic's removal path. Owner (DeviceBlock) handles the actual
    // crateConfiguredParams edit + grid/XY-pad rebuild.
    std::function<void (te::AutomatableParameter&)> onDeleteRequested;

    void mouseDown (const juce::MouseEvent&) override
    {
        grabKeyboardFocus(); // click-to-select, per the Configure mechanic spec
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
        {
            dragging = true;
            param.getEdit().getUndoManager().beginNewTransaction ("Tweak " + param.getParameterName());
            param.parameterChangeGestureBegin();
        }

        const auto range = param.getValueRange();
        const auto proportion = juce::jlimit (0.0f, 1.0f, (float) e.position.x / (float) juce::jmax (1, getWidth()));
        param.setParameter (range.getStart() + proportion * range.getLength(), juce::sendNotificationSync);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging)
        {
            param.parameterChangeGestureEnd();
            dragging = false;
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            if (onDeleteRequested)
                onDeleteRequested (param);

            return true;
        }

        return false;
    }

    void focusGained (juce::Component::FocusChangeType) override   { repaint(); }
    void focusLost (juce::Component::FocusChangeType) override     { repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        // Selection border — Configure mechanic: "clicking a slider visually
        // selects it (draws a border)".
        if (hasKeyboardFocus (false))
        {
            g.setColour (LAF::accent);
            g.drawRect (area, 1.5f);
            area = area.reduced (2.0f);
        }

        g.setColour (LAF::textDim);
        g.setFont (juce::FontOptions (9.0f));
        g.drawText (param.getParameterShortName (12), area.removeFromTop (14.0f), juce::Justification::centred);

        g.setColour (LAF::text);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (param.getCurrentValueAsString(), area.removeFromTop (16.0f), juce::Justification::centred);

        // Track: thin horizontal line across the bottom 4px of the remaining bounds.
        const auto trackY = area.getBottom() - 2.0f;
        g.setColour (LAF::panelLight);
        g.drawLine (area.getX(), trackY, area.getRight(), trackY, 1.5f);

        // Thumb: filled triangle, X = normalized current value * width — moves
        // live as the parameter changes (currentValueChanged() below repaints).
        const auto range = param.getValueRange();
        const auto proportion = range.getLength() > 0.0f
                                     ? (param.getCurrentValue() - range.getStart()) / range.getLength()
                                     : 0.5f;
        const auto pointerX = area.getX() + proportion * area.getWidth();

        juce::Path triangle;
        triangle.addTriangle (pointerX - 4.0f, trackY - 7.0f, pointerX + 4.0f, trackY - 7.0f, pointerX, trackY - 1.0f);
        g.setColour (LAF::accent);
        g.fillPath (triangle);
    }

private:
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override
    {
        juce::Component::SafePointer<MiniParamSlider> safeThis (this);
        juce::MessageManager::callAsync ([safeThis] { if (safeThis != nullptr) safeThis->repaint(); });
    }

    te::AutomatableParameter& param;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiniParamSlider)
};

//==============================================================================
class UniversalDeviceChainComponent::DeviceBlock : public juce::Component,
                                                    private te::AutomatableParameter::Listener,
                                                    private juce::Timer
{
public:
    DeviceBlock (te::Plugin& p, bool startsFocused) : plugin (p), folded (! startsFocused)
    {
        setWantsKeyboardFocus (true);

        addAndMakeVisible (foldButton);
        foldButton.setColour (juce::TextButton::buttonColourId, bypassOffColour);
        foldButton.setButtonText (folded ? "+" : "-");
        foldButton.setTooltip ("Fold / unfold");
        foldButton.onClick = [this] { setFolded (! folded); };

        addAndMakeVisible (nameLabel);
        nameLabel.setText (plugin.getName(), juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, LAF::text);
        nameLabel.setInterceptsMouseClicks (false, false);

        addAndMakeVisible (bypassButton);
        bypassButton.setToggleState (plugin.isEnabled(), juce::dontSendNotification);
        bypassButton.onToggle = [this] (bool isOn) { plugin.setEnabled (isOn); };

        // "Show Native UI" (Ableton's wrench) — Ableton Live paradigm: a 3rd-party
        // VST's real GUI can be enormous (900x700+, freely resizable) — nowhere
        // near compatible with this block's compact, tightly-constrained width
        // formula (getPreferredWidth()). Rather than force-fit that into the
        // bottom dock, this opens the plugin's native editor in its own standard
        // floating desktop window — the SAME TE-native hosting path
        // CrateWorkflowManager::loadPluginToSelectedTrack() already uses on
        // first load (showWindowExplicitly(), routed through
        // CrateUIBehaviour::createPluginWindow() -> PluginWindow). This block
        // itself stays the minimal "name + wrench" representation; the auto-
        // populated param grid alongside it is this app's OWN compact tweak
        // surface, not an attempt to reproduce the plugin's real UI inline.
        addAndMakeVisible (showNativeUiButton);
        showNativeUiButton.setColour (juce::TextButton::buttonColourId, bypassOffColour);
        showNativeUiButton.setTooltip ("Show native plugin UI");
        showNativeUiButton.onClick = [this] { plugin.showWindowExplicitly(); };
        updateNativeUiButtonEnablement();

        addAndMakeVisible (configureButton);
        configureButton.setClickingTogglesState (true);
        configureButton.setColour (juce::TextButton::buttonOnColourId, LAF::accent);
        configureButton.setColour (juce::TextButton::buttonColourId, bypassOffColour);
        configureButton.setTooltip ("Configure: tweak a parameter in the native plugin UI to add it here");
        configureButton.onClick = [this]
        {
            configureMode = configureButton.getToggleState();

            if (configureMode)
                setupConfigureDiscoveryListeners();
            else
                teardownConfigureDiscoveryListeners();
        };

        addAndMakeVisible (deleteButton);
        deleteButton.setColour (juce::TextButton::buttonColourId, bypassOffColour);
        deleteButton.setTooltip ("Delete plugin");
        deleteButton.onClick = [&pluginRef = plugin]
        {
            // Deferred: deleteFromParent() synchronously fires the pluginList
            // ValueTree listener (UniversalDeviceChainComponent::
            // valueTreeChildRemoved) that rebuilds every DeviceBlock in this
            // chain — including this one, whose own button click handler would
            // still be on the call stack. Same self-destruction-from-own-click-
            // handler hazard as TrackHeaderComponent's delete button; same fix.
            // Captures the te::Plugin& itself (owned by the track, not by this
            // DeviceBlock), not `this` — valid independent of whether this
            // DeviceBlock survives to the next message-loop iteration.
            juce::MessageManager::callAsync ([&pluginRef]
            {
                pluginRef.edit.getUndoManager().beginNewTransaction ("Delete Plugin: " + pluginRef.getName());
                pluginRef.deleteFromParent();
            });
        };

        updateHeaderControlVisibility();
        rebuildGrid();

        // Folded-state level meter. No per-device output-level API exists on
        // te::Plugin (verified — grepped the header, nothing like
        // getActiveLevel()), so this reuses the OWNING TRACK's real
        // LevelMeterPlugin/measurer — the same one MixerStrip's fader meter
        // reads. Track-wide, not per-device, but genuinely audio-reactive
        // rather than a fabricated call.
        if (auto* track = plugin.getOwnerTrack())
            trackMeterPlugin = track->pluginList.findFirstPluginOfType<te::LevelMeterPlugin>();

        if (trackMeterPlugin != nullptr)
            trackMeterPlugin->measurer.addClient (meterClient);

        startTimerHz (30);
    }

    ~DeviceBlock() override
    {
        stopTimer();

        if (trackMeterPlugin != nullptr)
            trackMeterPlugin->measurer.removeClient (meterClient);

        teardownConfigureDiscoveryListeners();
    }

    std::function<void (te::Plugin*)> onSelected;
    std::function<void()> onSizeChanged;

    te::Plugin& getPlugin() const noexcept   { return plugin; }

    void setSelected (bool shouldBeSelected)
    {
        if (selected == shouldBeSelected)
            return;

        selected = shouldBeSelected;
        repaint();
    }

    int getPreferredWidth() const
    {
        if (folded)
            return foldedStripWidth;

        const int numColumns = juce::jmax (1, gridColumnCount());

        // EXACT required width — every term here is a named constant also used
        // verbatim in resized(), so this can't drift from the real geometry the
        // way the previous "+10" guess did:
        //   blockOuterPadding on BOTH left and right (getLocalBounds().reduced(blockOuterPadding, 0)
        //     — "Ableton Tight", Task 4: this is now 0, content stretches flush)
        //   xyPadColumnWidth + xyToGridGap before the grid starts
        //   numColumns * (miniSliderWidth + gridColumnGap) — the grid's own content width
        //     (gridColumnGap is each FlexItem's own right margin, applied per column
        //     including the last, so this is the exact width FlexBox needs, no more)
        //   gridRightBreathingRoom reserved AFTER the grid, matching resized()'s
        //     gridArea.removeFromRight(gridRightBreathingRoom) exactly — this is
        //     the hard margin that keeps the rightmost slider's triangle clear of
        //     the block's rounded edge.
        const int gridContentWidth = numColumns * (miniSliderWidth + gridColumnGap);

        return (blockOuterPadding * 2) + xyPadColumnWidth + xyToGridGap + gridContentWidth + gridRightBreathingRoom;
    }

    int getPreferredHeight() const
    {
        // BUG FIX: this used to return a small fixed foldedStripMinHeight while
        // folded, collapsing a lone folded device down to a near-square sliver.
        // Ableton keeps every device in a chain at the SAME tall strip height
        // regardless of fold state — only WIDTH shrinks when folded, never
        // height. The formula below doesn't reference paramSliders/xyPad (which
        // don't exist while folded — rebuildGrid() early-returns in that state),
        // only fixed layout constants, so it's exactly as valid folded as not.
        const auto xyHeight = xyPadSize + 4 + (xyPadComboRowHeight * 2) + xyPadComboGap; // stacked X + Y rows now
        const auto gridHeight = juce::jmin (gridMaxHeight, rowsPerColumn() * miniSliderHeight);

        // Absolute Flush Geometry: the old "+ 6 ... + 8" here were the SAME
        // top-gap and bottom-margin resized() just had removed — this formula
        // must report EXACTLY what resized() actually lays out (that's the
        // documented "can't drift apart" contract these two functions share),
        // so both terms are gone now too. Zero slack top or bottom.
        return (blockOuterPadding * 2) + headerRowHeight + juce::jmax (xyHeight, gridHeight);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onSelected)
            onSelected (&plugin);

        // Ableton behaviour: clicking anywhere on a collapsed sliver unfolds
        // it — there's no visible fold button while folded to click instead.
        if (folded)
            setFolded (false);
    }

    void resized() override
    {
        if (folded)
        {
            // Vertical sliver: bypass circle, then the wrench (native UI) icon
            // spaced clearly below it, then the rotated name filling the rest of
            // what is now the SAME full block height as an unfolded device (see
            // getPreferredHeight() — folded no longer reports a squashed height),
            // drawn in paint() (juce::Label doesn't support rotated text), which
            // reads showNativeUiButton.getBottom() directly rather than a magic
            // number, so the two can never drift apart. Cfg/Delete/fold-button
            // only appear once unfolded. Right edge reserved for the level meter
            // strip, drawn in paint(), so nothing lays out into it.
            auto area = getLocalBounds().reduced (3, 6);
            area.removeFromRight (foldedMeterStripWidth);

            bypassButton.setBounds (area.removeFromTop (16).withSizeKeepingCentre (16, 16));
            area.removeFromTop (10);
            showNativeUiButton.setBounds (area.removeFromTop (18));
            return;
        }

        // Small 4-side inset (blockOuterPadding) so buttons/grid don't visually
        // clip the rounded corners — the header-to-grid gap below stays zero.
        auto area = getLocalBounds().reduced (blockOuterPadding);

        // Ableton Tight: every header button is the SAME fixed square
        // (headerButtonSize) with the SAME gap (headerButtonGap) on both
        // sides — the old ad-hoc widths (16/16/28/18/34) read as uneven,
        // scattered spacing next to Ableton's compact, uniform icon row.
        auto headerRow = area.removeFromTop (headerRowHeight);
        foldButton.setBounds (headerRow.removeFromLeft (headerButtonSize).reduced (0, 2));
        headerRow.removeFromLeft (headerButtonGap);
        bypassButton.setBounds (headerRow.removeFromLeft (headerButtonSize).reduced (3));
        headerRow.removeFromLeft (headerButtonGap);
        showNativeUiButton.setBounds (headerRow.removeFromLeft (headerButtonSize).reduced (0, 2));
        headerRow.removeFromLeft (headerButtonGap);
        deleteButton.setBounds (headerRow.removeFromRight (headerButtonSize).reduced (0, 2));
        headerRow.removeFromRight (headerButtonGap);
        configureButton.setBounds (headerRow.removeFromRight (headerButtonSize).reduced (0, 2));
        headerRow.removeFromRight (headerButtonGap);
        nameLabel.setBounds (headerRow);

        // Absolute Flush Geometry (override of the earlier judgment call):
        // ZERO gap between the header row and the parameter grid — the dark
        // grey param area touches the header band directly, Ableton
        // zero-padding standard. No area.removeFromTop() here at all.

        if (xyPad != nullptr)
            xyPad->setBounds (area.removeFromLeft (xyPadColumnWidth));

        area.removeFromLeft (xyToGridGap);

        // Dynamic Grid: column-direction FlexBox, wrapping into new columns once
        // gridMaxHeight is reached — Ableton-style, not an ever-taller single column.
        juce::FlexBox grid;
        grid.flexDirection = juce::FlexBox::Direction::column;
        grid.flexWrap = juce::FlexBox::Wrap::wrap;
        grid.alignItems = juce::FlexBox::AlignItems::flexStart;

        for (auto& slider : paramSliders)
            grid.items.add (juce::FlexItem (*slider)
                                 .withWidth ((float) miniSliderWidth)
                                 .withHeight ((float) miniSliderHeight)
                                 .withMargin ({ 0, gridColumnGap, gridColumnGap, 0 }));

        // gridRightBreathingRoom — same named constant getPreferredWidth() adds
        // to the block's total width, so this reservation and that calculation
        // can never drift apart. This is the hard margin keeping the rightmost
        // slider's triangle pointer clear of the block's rounded edge.
        auto gridArea = area;
        gridArea.removeFromRight (gridRightBreathingRoom);
        grid.performLayout (gridArea.withHeight (gridMaxHeight));
    }

    void paint (juce::Graphics& g) override
    {
        constexpr float cornerSize = 6.0f;
        auto bounds = getLocalBounds().toFloat();

        // Body is ALWAYS dark grey — selection/focus must never repaint it.
        g.setColour (blockBodyColour);
        g.fillRoundedRectangle (bounds, cornerSize);

        if (folded)
        {
            // Vertical sliver: the WHOLE strip is the "header" here (there's no
            // separate button row to isolate a tint to), so selection/focus
            // tints the full strip rather than a top band.
            g.setColour (selected ? blockSelectedColour : blockHeaderColour);
            g.fillRoundedRectangle (bounds, cornerSize);

            // Rotated name — scoped in its own block so the ScopedSaveState's
            // destructor (which pops the rotation transform) runs BEFORE the
            // meter strip is drawn below, not after.
            {
                auto textArea = getLocalBounds().toFloat().reduced (2.0f);
                textArea.removeFromRight ((float) foldedMeterStripWidth);

                // Reads the wrench button's REAL bottom edge (set in resized()) rather
                // than a magic number — the two can no longer drift apart if either
                // one's spacing changes.
                textArea.removeFromTop ((float) showNativeUiButton.getBottom() + 6.0f);

                g.setColour (LAF::text);
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));

                // Rotate -90 degrees about the remaining area's own centre, then
                // draw into a rectangle with width/height swapped — after the
                // rotation, what WAS the area's height reads as text-flow width.
                juce::Graphics::ScopedSaveState save (g);
                g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi,
                                                                  textArea.getCentreX(), textArea.getCentreY()));
                const juce::Rectangle<float> rotatedTextArea (textArea.getCentreX() - textArea.getHeight() * 0.5f,
                                                               textArea.getCentreY() - textArea.getWidth() * 0.5f,
                                                               textArea.getHeight(), textArea.getWidth());
                g.drawText (plugin.getName(), rotatedTextArea, juce::Justification::centred);
            }

            // Right-edge level meter — folded state only, per the Ableton
            // reference. Reuses the TRACK's own real LevelMeterPlugin/measurer
            // (same one MixerStrip's fader meter reads): te::Plugin has no
            // per-device output-level API at all (verified — no getActiveLevel()
            // or equivalent exists), so this is the track's overall output, not
            // this specific device's — genuinely audio-reactive, not fabricated,
            // but not per-device either. Noted plainly rather than faking it.
            auto meterStrip = getLocalBounds().toFloat().removeFromRight ((float) foldedMeterStripWidth);
            g.setColour (LAF::background);
            g.fillRect (meterStrip);

            const auto normalisedLevel = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
            auto fillRect = meterStrip.removeFromBottom (meterStrip.getHeight() * normalisedLevel);

            g.setGradientFill (juce::ColourGradient (juce::Colours::green, fillRect.getBottomLeft(),
                                                      juce::Colours::yellow, fillRect.getTopLeft(), false));
            g.fillRect (fillRect);
        }
        else
        {
            // Only the header band (buttons + name, top headerBandHeight px)
            // changes colour when unfolded — matching Ableton's actual
            // device-header-only highlight.
            auto headerBand = bounds.removeFromTop ((float) headerBandHeight);
            g.setColour (selected ? blockSelectedColour : blockHeaderColour);
            g.fillRoundedRectangle (headerBand, cornerSize);
            // Re-square the header's own bottom edge so it reads as "rounded
            // top, flat bottom" flowing straight into the body below, not a
            // rounded pill floating with two extra corner gaps against the
            // body's square top edge underneath it.
            g.fillRect (headerBand.removeFromBottom (cornerSize));
        }

        // Subtle 1px border, slightly lighter than the body, so the rounded
        // corners actually read against the background instead of relying on
        // the fill colour alone.
        g.setColour (LAF::panelLight);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerSize, 1.0f);
    }

private:
    int rowsPerColumn() const   { return juce::jmax (1, gridMaxHeight / miniSliderHeight); }

    int gridColumnCount() const
    {
        if (paramSliders.empty())
            return 1;

        const auto rows = rowsPerColumn();
        return ((int) paramSliders.size() + rows - 1) / rows;
    }

    void rebuildGrid()
    {
        paramSliders.clear();
        xyPad.reset();

        if (folded)
            return;

        const auto configured = getConfiguredParams (plugin);

        xyPad = std::make_unique<XYPadComponent> (configured);
        addAndMakeVisible (*xyPad);

        for (auto* p : configured)
        {
            if (p == nullptr)
                continue;

            auto slider = std::make_unique<MiniParamSlider> (*p);
            slider->onDeleteRequested = [this] (te::AutomatableParameter& removed)
            {
                // Deferred: removeConfiguredParam() -> rebuildGrid() destroys
                // every MiniParamSlider via paramSliders.clear() — including
                // THIS one, whose own keyPressed() call is still on the stack
                // (unlike Configure-mode discovery's currentValueChanged(), where
                // the caller is the changed AutomatableParameter's listener list,
                // not the slider itself). Same self-destruction-from-own-handler
                // hazard as the plugin Delete button; same fix. paramID is
                // copied (not a reference into `removed`, which may also be
                // gone by the time this runs); SafePointer guards the rest since
                // rebuildGrid()/resized()/repaint() DO need to run on this
                // specific DeviceBlock, if it's still alive.
                const auto paramID = removed.paramID;
                juce::Component::SafePointer<DeviceBlock> safeThis (this);

                juce::MessageManager::callAsync ([safeThis, paramID]
                {
                    if (safeThis != nullptr)
                        safeThis->removeConfiguredParamByID (paramID);
                });
            };
            addAndMakeVisible (*slider);
            paramSliders.push_back (std::move (slider));
        }

        if (configureMode)
            setupConfigureDiscoveryListeners();
    }

    void removeConfiguredParamByID (const juce::String& paramID)
    {
        auto ids = readConfiguredParamIDs (plugin);

        if (ids.isEmpty())
        {
            // Implicit auto-populated state (see getConfiguredParams()'s doc
            // comment) — materialize it into an explicit list first. Writing
            // back an empty list after removing one entry would otherwise mean
            // "auto-populate everything again", silently undoing the deletion.
            for (auto* configuredParam : getConfiguredParams (plugin))
                if (configuredParam != nullptr)
                    ids.add (configuredParam->paramID);
        }

        ids.removeString (paramID);
        writeConfiguredParamIDs (plugin, ids);

        rebuildGrid();
        resized();
        repaint();

        if (onSizeChanged)
            onSizeChanged();
    }

    void addConfiguredParam (te::AutomatableParameter& p)
    {
        auto ids = readConfiguredParamIDs (plugin);

        if (! ids.contains (p.paramID))
        {
            ids.add (p.paramID);
            writeConfiguredParamIDs (plugin, ids);
        }

        rebuildGrid();
        resized();
        repaint();

        if (onSizeChanged)
            onSizeChanged();
    }

    void setupConfigureDiscoveryListeners()
    {
        teardownConfigureDiscoveryListeners();

        const auto configured = getConfiguredParams (plugin);

        for (auto* p : plugin.getAutomatableParameters())
        {
            if (p == nullptr)
                continue;

            const bool alreadyConfigured = std::find (configured.begin(), configured.end(), p) != configured.end();

            if (! alreadyConfigured)
            {
                p->addListener (this);
                discoveryListenedParams.push_back (p);
            }
        }
    }

    void teardownConfigureDiscoveryListeners()
    {
        for (auto* p : discoveryListenedParams)
            if (p != nullptr)
                p->removeListener (this);

        discoveryListenedParams.clear();
    }

    // te::AutomatableParameter::Listener — ONLY attached (via
    // setupConfigureDiscoveryListeners()) to parameters NOT currently
    // configured, while Configure mode is on. Any value change on one of these
    // means the user tweaked it in the plugin's own native editor.
    // TRACKTION_ASSERT_MESSAGE_THREAD-guaranteed message-thread callback (same
    // fact verified for currentValueChanged elsewhere this session), and
    // addConfiguredParam()/rebuildGrid() only touch THIS block's own children —
    // never destroy `this` — so no defer-via-callAsync is needed here, unlike
    // the delete button above.
    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter& changedParam) override
    {
        addConfiguredParam (changedParam);
        setupConfigureDiscoveryListeners(); // re-derive: changedParam is configured now, shouldn't stay discoverable
    }

    // 30Hz meter poll — only matters (and only repaints) while folded, since
    // that's the only state that draws the meter strip at all.
    void timerCallback() override
    {
        if (! folded || trackMeterPlugin == nullptr)
            return;

        const auto levelL = meterClient.getAndClearAudioLevel (0);
        const auto levelR = meterClient.getAndClearAudioLevel (1);
        meterLevelDb = juce::jmax (levelL.dB, levelR.dB);
        repaint();
    }

    void updateNativeUiButtonEnablement()
    {
        auto* processor = plugin.getWrappedAudioProcessor();
        const bool hasWrappedEditor = processor != nullptr && processor->hasEditor();

        // Step 32 (Exorcise the Ghost & Fix the HWND) directive — the
        // actual root cause of the "UI Blackout" investigation: this was
        // the REAL reason nothing ever rendered. CrateSandboxBridge never
        // wraps a local juce::AudioProcessor at all (the real one lives
        // entirely inside the sandboxed CHILD process — see this class's
        // own architecture doc comment) — getWrappedAudioProcessor()
        // returns nullptr for it unconditionally (te::Plugin's own
        // default), which permanently disabled this button. Clicking a
        // disabled JUCE button never fires onClick — plugin.
        // showWindowExplicitly() was never actually being called, so
        // CrateEditorComponent (Step 29's createEditor() override) was
        // never even constructed, let alone reaching a black/blank state.
        // A sandboxed plugin DOES provide a real editor — it just doesn't
        // route through getWrappedAudioProcessor() to prove that.
        const bool isSandboxed = dynamic_cast<CrateSandboxBridge*> (&plugin) != nullptr;

        showNativeUiButton.setEnabled (hasWrappedEditor || isSandboxed);
    }

public:
    // "Focus" (from a MixerStrip insert click) and "fold" are related but not
    // identical: focus auto-unfolds this block and auto-folds every sibling,
    // but the fold button can ALSO be clicked directly at any time, independent
    // of which block is "focused" — matching real Ableton behaviour, where any
    // device can be folded/unfolded by hand regardless of what's selected
    // elsewhere. "Selected" (blue header) is a THIRD, separate piece of state —
    // set purely by clicking the block, unrelated to fold/focus.
    void setFocused (bool shouldBeFocused)   { setFolded (! shouldBeFocused); }

private:
    // Single place that changes fold state — used by the fold button, by
    // clicking a collapsed sliver to unfold it (mouseUp()), and by setFocused()
    // (auto-unfold-this/auto-fold-siblings from a MixerStrip insert click).
    // Toggles which header controls exist at all (a collapsed sliver shows
    // none of them, per the Ableton reference), not just their bounds.
    void setFolded (bool shouldBeFolded)
    {
        if (folded == shouldBeFolded)
            return;

        folded = shouldBeFolded;
        foldButton.setButtonText (folded ? "+" : "-");
        updateHeaderControlVisibility();

        rebuildGrid();
        resized();
        repaint();

        if (onSizeChanged)
            onSizeChanged();
    }

    // Shared by setFolded() (toggling later) and the constructor (setting the
    // CORRECT initial state) — without calling this at construction too, a
    // block that starts folded would have its header buttons technically
    // still "visible" (just laid out at zero size by resized()'s early return),
    // relying on JUCE's zero-size-doesn't-paint behaviour rather than being
    // explicitly correct.
    void updateHeaderControlVisibility()
    {
        // showNativeUiButton (the wrench) is visible in BOTH states now — the
        // folded sliver shows bypass + wrench + rotated name, per spec. Only
        // the unfolded-only controls (fold button itself, Cfg, Delete, the
        // horizontal name label) toggle off when folded.
        const bool showsFullHeader = ! folded;
        foldButton.setVisible (showsFullHeader);
        configureButton.setVisible (showsFullHeader);
        deleteButton.setVisible (showsFullHeader);
        nameLabel.setVisible (showsFullHeader);
    }

private:
    te::Plugin& plugin;
    bool folded = true;
    bool selected = false;
    bool configureMode = false;

    juce::Label nameLabel;
    juce::TextButton foldButton;
    CircularToggleButton bypassButton;
    juce::TextButton showNativeUiButton { "UI" }; // plain ASCII label, not a wrench glyph — a raw
                                                   // non-ASCII source literal is exactly what caused
                                                   // the mangled-text bug fixed earlier this project
    juce::TextButton configureButton { "C" };
    juce::TextButton deleteButton { "X" };

    std::unique_ptr<XYPadComponent> xyPad;
    std::vector<std::unique_ptr<MiniParamSlider>> paramSliders;
    std::vector<te::AutomatableParameter*> discoveryListenedParams; // raw, non-owning — see setup/teardown above

    // Folded-state meter — track-wide (see the constructor's doc comment for
    // why there's no per-device equivalent). Raw pointer: lifetime owned by
    // the track's pluginList, same pattern MixerStrip uses for its own meter.
    te::LevelMeterPlugin* trackMeterPlugin = nullptr;
    te::LevelMeasurer::Client meterClient;
    float meterLevelDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceBlock)
};

//==============================================================================
class UniversalDeviceChainComponent::ChainRowContent : public juce::Component
{
public:
    ChainRowContent()
    {
        // Spatial optimization (Task 4): "+ Add Device" now lives HERE — a
        // trailing block at the END of the horizontal chain, taking the FULL
        // row height like every DeviceBlock, not a fixed top toolbar stealing
        // height from every plugin's UI. Style matches a folded DeviceBlock's
        // vertical-sliver language (dim button, minimal chrome) so it reads as
        // "one more slot in the rail" rather than a separate global control.
        addDeviceButton.setColour (juce::TextButton::buttonColourId, bypassOffColour);
        addDeviceButton.onClick = [this] { if (onAddDeviceClicked) onAddDeviceClicked(); };
        addAndMakeVisible (addDeviceButton);
    }

    void rebuild (te::Track* track, te::Plugin* pluginToFocus)
    {
        blocks.clear();

        if (track != nullptr)
        {
            for (auto* p : track->pluginList)
            {
                if (p == nullptr || ! isChainablePlugin (*p))
                    continue;

                auto block = std::make_unique<DeviceBlock> (*p, p == pluginToFocus);
                block->onSelected = [this] (te::Plugin* selected)
                {
                    for (auto& b : blocks)
                        b->setSelected (&b->getPlugin() == selected);

                    if (onBlockSelected)
                        onBlockSelected (selected);
                };
                block->onSizeChanged = [this] { relayout(); };
                addAndMakeVisible (*block);
                blocks.push_back (std::move (block));
            }
        }

        relayout();
    }

    /** Re-focuses without a full rebuild — called when the same track's chain is
        already showing and the user just clicked a different insert slot. */
    void setFocusedPlugin (te::Plugin* pluginToFocus, const std::vector<te::Plugin*>& pluginOrder)
    {
        for (size_t i = 0; i < blocks.size() && i < pluginOrder.size(); ++i)
            blocks[i]->setFocused (pluginOrder[i] == pluginToFocus);

        relayout();
    }

    std::function<void (te::Plugin*)> onBlockSelected;

    /** Fires when the trailing "+ Add Device" block is clicked — wired by
        UniversalDeviceChainComponent to its own showInstrumentMenu() (this
        class has no edit/workflow access of its own to build that menu itself). */
    std::function<void()> onAddDeviceClicked;

    // Fires whenever relayout() runs — bubbles up to UniversalDeviceChainComponent
    // so a fold/unfold click (which only otherwise triggers THIS component's own
    // internal relayout) can also re-check whether the OUTER zone's height
    // (MainComponent's Grid row) should change.
    std::function<void()> onContentSizeChanged;

    void setMinWidth (int w)   { minWidth = w; }
    void setMinHeight (int h)  { minHeight = h; }

    int preferredWidth() const
    {
        int total = 0;
        for (auto& b : blocks)
            total += b->getPreferredWidth() + interBlockGap;
        return total + addDeviceBlockWidth + interBlockGap; // trailing Add Device block
    }

    // All blocks share one row height (folded ones just have empty space below
    // their header) — the tallest block sets it for everyone, same pattern as
    // MixerComponent's shared strip row height.
    int preferredHeight() const
    {
        int maxHeight = 40;
        for (auto& b : blocks)
            maxHeight = juce::jmax (maxHeight, b->getPreferredHeight());
        return maxHeight;
    }

    void resized() override
    {
        // Zero Dead Space Law: every block shares THIS row's height
        // (getHeight()), not its own individual getPreferredHeight() —
        // required for the vertical-sliver fold to actually span the row
        // instead of being a short rectangle floating next to a taller
        // unfolded sibling, and it's what makes a device rail read as one
        // clean strip (Ableton: all devices in a chain line up top AND
        // bottom, regardless of how much each one needs). getHeight() itself
        // is stretched to at least the viewport's visible height in
        // relayout(), above, so this row — and therefore every block in it —
        // fills the entire Device Chain strip with no exposed background
        // beneath a shorter card.
        int x = 0;
        for (auto& b : blocks)
        {
            const auto w = b->getPreferredWidth();
            b->setBounds (x, 0, w, getHeight());
            x += w + interBlockGap; // rounded corners need daylight between blocks to actually read
        }

        // Trailing "+ Add Device" block — same FULL row height as every
        // DeviceBlock (Task 4: no top-margin-crushing global button any more).
        addDeviceButton.setBounds (x, 0, addDeviceBlockWidth, getHeight());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LAF::background);

        if (blocks.empty())
        {
            // Leaves the trailing Add Device block's own column alone —
            // it's always present now, even with zero real devices.
            auto textArea = getLocalBounds().withTrimmedRight (addDeviceBlockWidth + interBlockGap);
            g.setColour (LAF::textDim);
            g.setFont (juce::FontOptions (14.0f));
            g.drawText ("No devices - select a track with a loaded plugin",
                         textArea, juce::Justification::centred);
        }
    }

private:
    void relayout()
    {
        // Zero Dead Space Law: height stretches to AT LEAST minHeight (the
        // viewport's visible height), same as width does to minWidth — so
        // every DeviceBlock shares a row height that fills the whole visible
        // Device Chain strip, Ableton-style (every device card is the same
        // tall slot, top AND bottom, regardless of what its own content
        // needs), rather than leaving the strip's background exposed below a
        // shorter card. ChainRowContent::resized() below already hands every
        // block THIS component's own getHeight(), so stretching it here is
        // all that's needed — no per-block change required.
        setSize (juce::jmax (preferredWidth(), minWidth), juce::jmax (preferredHeight(), minHeight));
        resized();

        if (onContentSizeChanged)
            onContentSizeChanged();
    }

    std::vector<std::unique_ptr<DeviceBlock>> blocks;
    juce::TextButton addDeviceButton { "+ Add Device" };
    int minWidth = 0;
    int minHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainRowContent)
};

//==============================================================================
UniversalDeviceChainComponent::UniversalDeviceChainComponent (te::Edit& editToShow, CrateWorkflowManager& workflowToUse)
    : edit (editToShow), workflow (workflowToUse)
{
    content = std::make_unique<ChainRowContent>();
    content->onBlockSelected = [this] (te::Plugin* p) { focusPlugin (currentTrack.get(), p); };
    content->onContentSizeChanged = [this] { notifyIfPreferredHeightChanged(); };

    // Task 4: "+ Add Device" moved here from a top toolbar — it's now the
    // trailing block at the end of the chain (see ChainRowContent), so this
    // class just supplies the actual menu logic when it's clicked.
    content->onAddDeviceClicked = [this] { showInstrumentMenu(); };

    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (false, true); // horizontal only

    // Real cause of the "floating above the bottom" bug: a shown scrollbar
    // reserves a horizontal gutter of this thickness along the bottom of the
    // viewport's OWN content area, shrinking what DeviceBlocks actually get to
    // occupy vertically without shrinking the zone MainComponent allocated for
    // this component — the visible symptom being dead dark space at the
    // bottom. Zero thickness removes the reserved gutter entirely; mouse wheel/
    // trackpad scrolling still works on a JUCE Viewport with no visible thumb.
    viewport.setScrollBarThickness (0);

    addAndMakeVisible (viewport);

    rebuildBlocks();
}

UniversalDeviceChainComponent::~UniversalDeviceChainComponent()
{
    if (listenedPluginListState.isValid())
        listenedPluginListState.removeListener (this);
}

juce::ValueTree UniversalDeviceChainComponent::pluginListStateFor (te::Edit& e, te::Track* track)
{
    if (track == nullptr)
        return {};

    if (track == e.getMasterTrack())
        return e.getMasterPluginList().state;

    return track->state;
}

void UniversalDeviceChainComponent::setCurrentTrack (te::Track* newTrack)
{
    if (currentTrack.get() == newTrack)
        return;

    if (listenedPluginListState.isValid())
        listenedPluginListState.removeListener (this);

    currentTrack = newTrack;
    listenedPluginListState = pluginListStateFor (edit, newTrack);

    if (listenedPluginListState.isValid())
        listenedPluginListState.addListener (this);
}

void UniversalDeviceChainComponent::valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree)
{
    if (currentTrack == nullptr || parentTree != listenedPluginListState || ! childTree.hasType (te::IDs::PLUGIN))
        return;

    // Deferred, not called inline: this fires synchronously from wherever the
    // plugin got added (a Load, or Undo/Redo of a Delete) — potentially still
    // nested inside that call's own stack. Deferring one message-loop tick
    // means rebuildBlocks() always runs from a clean top-level call, guaranteed
    // safe regardless of what triggered the add. SafePointer, not raw this:
    // this component can itself be torn down (Load-project teardown, Phase 1)
    // before the deferred lambda fires.
    juce::Component::SafePointer<UniversalDeviceChainComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->rebuildBlocks();
    });
}

void UniversalDeviceChainComponent::valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int)
{
    if (currentTrack == nullptr || parentTree != listenedPluginListState || ! childTree.hasType (te::IDs::PLUGIN))
        return;

    // If the removed plugin was the focused one (e.g. deleted via the Device
    // Chain's own Delete button, or Undo of a Load), drop the stale pointer
    // before rebuildBlocks() re-reads the track — rebuildBlocks() itself doesn't
    // touch focusedPlugin, so this must happen here, not there.
    focusedPlugin = nullptr;

    // Same deferral reasoning as valueTreeChildAdded() above.
    juce::Component::SafePointer<UniversalDeviceChainComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->rebuildBlocks();
    });
}

void UniversalDeviceChainComponent::showTrack (te::Track* trackToShow)
{
    if (currentTrack.get() == trackToShow)
        return;

    setCurrentTrack (trackToShow);
    focusedPlugin = nullptr;
    rebuildBlocks();
}

void UniversalDeviceChainComponent::clearTrack()
{
    showTrack (nullptr);
}

void UniversalDeviceChainComponent::refreshCurrentTrack()
{
    if (currentTrack == nullptr)
        return;

    // rebuildBlocks() reads currentTrack/focusedPlugin directly — unlike
    // showTrack(), there's no "did the track argument change" early-out to worry
    // about here, since we're intentionally re-reading the SAME track's
    // (now-different) plugin list.
    rebuildBlocks();
}

void UniversalDeviceChainComponent::focusPlugin (te::Track* pluginOwner, te::Plugin* pluginToFocus)
{
    if (pluginToFocus == nullptr)
        return;

    if (currentTrack.get() != pluginOwner)
    {
        setCurrentTrack (pluginOwner);
        focusedPlugin = pluginToFocus;
        rebuildBlocks();
        return;
    }

    focusedPlugin = pluginToFocus;

    std::vector<te::Plugin*> order;
    if (currentTrack != nullptr)
        for (auto* p : currentTrack->pluginList)
            if (p != nullptr && isChainablePlugin (*p))
                order.push_back (p);

    content->setFocusedPlugin (focusedPlugin, order);
    layoutContent();
}

void UniversalDeviceChainComponent::rebuildBlocks()
{
    content->rebuild (currentTrack.get(), focusedPlugin);
    layoutContent();
}

void UniversalDeviceChainComponent::layoutContent()
{
    const int visibleW = viewport.getMaximumVisibleWidth();
    const int visibleH = viewport.getMaximumVisibleHeight();

    // Zero Dead Space Law (Lead Architect directive): every DeviceBlock and
    // the trailing "+ Add Device" block stretch to fill the FULL visible
    // height of the Device Chain strip — see ChainRowContent::relayout()'s
    // comment.
    content->setMinWidth (visibleW);
    content->setMinHeight (visibleH);
    content->setSize (juce::jmax (content->preferredWidth(), visibleW),
                       juce::jmax (content->preferredHeight(), visibleH));

    notifyIfPreferredHeightChanged();
}

int UniversalDeviceChainComponent::getPreferredContentHeight() const
{
    return content->preferredHeight() + 1; // +1 for the top divider (paintOverChildren)
}

void UniversalDeviceChainComponent::notifyIfPreferredHeightChanged()
{
    // Self-terminating, not a loop risk: MainComponent::resized() reacting to
    // this by resizing US triggers our own resized() -> layoutContent() again,
    // but that only changes visibleW/visibleH, not any block's own preferred
    // height — so the second call always computes the SAME newHeight and this
    // early-out stops it there.
    const auto newHeight = getPreferredContentHeight();

    if (newHeight == lastNotifiedHeight)
        return;

    lastNotifiedHeight = newHeight;

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void UniversalDeviceChainComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);
}

void UniversalDeviceChainComponent::paintOverChildren (juce::Graphics& g)
{
    // Drawn AFTER children (the viewport) rather than in paint() (drawn
    // before) — this is what lets the viewport occupy this component's EXACT
    // full bounds with zero reduction (see resized()) while the divider still
    // reads on top of it, instead of the two fighting over the top pixel.
    g.setColour (juce::Colours::grey.withAlpha (0.5f));
    g.fillRect (0, 0, getWidth(), 1);

    // Premium drop-target glow for a Browser plugin drag — a bright cyan border
    // around the WHOLE chain area, drawn over every child same as the divider
    // above, so it reads regardless of what's currently in the chain.
    if (isDragHovering)
    {
        g.setColour (LAF::accent.withAlpha (0.10f));
        g.fillRect (getLocalBounds());
        g.setColour (LAF::accent);
        g.drawRect (getLocalBounds(), 2);
    }
}

bool UniversalDeviceChainComponent::isInterestedInDragSource (const SourceDetails& details)
{
    return currentTrack != nullptr && details.description.toString().startsWith ("plugin_drag|");
}

void UniversalDeviceChainComponent::itemDragEnter (const SourceDetails&)
{
    isDragHovering = true;
    repaint();
}

void UniversalDeviceChainComponent::itemDragExit (const SourceDetails&)
{
    isDragHovering = false;
    repaint();
}

void UniversalDeviceChainComponent::itemDropped (const SourceDetails& details)
{
    isDragHovering = false;
    repaint();

    if (currentTrack == nullptr)
        return;

    const auto identifier = details.description.toString().fromFirstOccurrenceOf ("plugin_drag|", false, false);

    if (identifier.isEmpty())
        return;

    if (auto desc = edit.engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
    {
        // -1 appends to the end of the chain, per spec ("If dropped on the
        // Device Chain, append it"). No manual rebuildBlocks() call needed
        // afterwards: loadPluginOntoTrack()'s insertPlugin() fires the track's
        // own ValueTree PLUGIN-child-added notification, which this class
        // already listens for (valueTreeChildAdded, deferred via callAsync)
        // and rebuilds from — the same path a Browser double-click load
        // already relies on.
        workflow.loadPluginOntoTrack (*desc, *currentTrack, -1);
    }
}

void UniversalDeviceChainComponent::showInstrumentMenu()
{
    if (currentTrack == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "No Track Selected",
            "Select a track first, then load an instrument — there's currently "
            "no target track for the Device Chain.");
        return;
    }

    // Ableton Live model: no OS file browser. The engine already knows every
    // scanned plugin (edit.engine.getPluginManager().knownPluginList — the
    // exact same list the Browser/PluginBrowserComponent already read), so
    // this button is just a filtered, instant PopupMenu over it — no disk
    // navigation, no file paths, no scanning-on-demand.
    const auto& knownPluginList = edit.engine.getPluginManager().knownPluginList;
    const auto allTypes = knownPluginList.getTypes();

    // menuIdToDescription maps PopupMenu item IDs (1-based, PopupMenu reserves
    // 0 for "nothing selected") back to the actual juce::PluginDescription —
    // the menu itself can only carry a String + int, not a full description.
    // Master is a mastering/effects bus — it can NEVER host an instrument,
    // so its "+ Add Device" lists effects instead of the usual instrument-only
    // filter (see loadPluginOntoTrack()'s matching guard, which would reject
    // every single pick if this stayed instrument-only for Master).
    const bool isMasterTrack = (currentTrack.get() == edit.getMasterTrack());

    std::vector<juce::PluginDescription> menuIdToDescription;
    juce::PopupMenu menu;

    for (const auto& desc : allTypes)
    {
        // ONLY synths/instruments on this button — this is the "load an
        // instrument" entry point specifically (Ableton's own "+" on a MIDI
        // track's device chain does exactly this, never lists FX here) —
        // except on Master, where it's the exact opposite (effects only).
        if (desc.isInstrument == isMasterTrack)
            continue;

        menuIdToDescription.push_back (desc);
        menu.addItem ((int) menuIdToDescription.size(), desc.name);
    }

    if (menuIdToDescription.empty())
    {
        menu.addItem (-1, isMasterTrack ? "No effects found - scan plugins first"
                                        : "No instruments found - scan plugins first", false);
    }

    menu.showMenuAsync (juce::PopupMenu::Options(),
        [this, menuIdToDescription] (int result)
    {
        if (result <= 0 || (size_t) result > menuIdToDescription.size())
            return; // cancelled, or the disabled "none found" placeholder

        if (currentTrack == nullptr)
            return; // track selection could have changed while the menu was open

        const auto& chosenDesc = menuIdToDescription[(size_t) result - 1];

        // Same instantiate+insert pipeline every other load path in this app
        // already funnels through (undo transaction, 256-plugin-limit fix,
        // track's own ValueTree PLUGIN-child-added notification that rebuilds
        // this chain automatically) — nothing new to instantiate by hand here.
        workflow.loadPluginOntoTrack (chosenDesc, *currentTrack, -1);
    });
}

void UniversalDeviceChainComponent::resized()
{
    // Task 4 (Vertical Real Estate): no more top toolbar stealing height from
    // every plugin's UI — "+ Add Device" is a trailing block INSIDE the chain
    // now (see ChainRowContent), so the viewport gets the FULL bounds here.
    viewport.setBounds (getLocalBounds());
    layoutContent();
}
