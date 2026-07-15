#include "MixerStrip.h"
#include "TheCrateLookAndFeel.h"
#include "PluginSlotComponent.h"

#include <map>

namespace
{
    using LAF = TheCrateLookAndFeel;

    // Must match TrackHeaderComponent's ToggleBlock role colours exactly (same
    // physical track, same two meanings, two views).
    const auto soloOnColour    = juce::Colour (0xff29d6f0);
    const auto muteOnColour    = juce::Colour (0xffff9500); // studio orange — Mute (ON = muted)
    const auto bypassOffColour = juce::Colour (0xff5a5a60);
    const auto sectionCaption  = juce::Colour (0xff6a6a72);

    constexpr float meterFloorDb = -60.0f;
    constexpr float meterRangeDb = 66.0f; // floor to +6 dB headroom

    constexpr int collapsedStripHeight = 210;

    // "Minimum-10 Scrolling Grid" (Pro Tools/Ableton hybrid Inserts rack): every
    // slot is a strict, hardcoded 24px row — NEVER divided from the parent's
    // height to squish more in — and the section always shows at least
    // insertMinVisibleRows rows' worth of viewport, real or "ghost" (empty).
    // A track with more real plugins than that just scrolls inside the fixed
    // viewport height instead of shrinking every row into illegibility.
    constexpr int insertRowHeight      = 24;
    constexpr int insertRowGap         = 2;
    constexpr int insertMinVisibleRows = 10;
    constexpr int insertsCaptionHeight = 14;
    constexpr int insertsSectionPadding = 8; // matches getLocalBounds().reduced (6, 4)'s top+bottom
    constexpr int insertsViewportHeight = insertMinVisibleRows * (insertRowHeight + insertRowGap);
    constexpr int insertsSectionHeight  = insertsCaptionHeight + insertsViewportHeight + insertsSectionPadding;

    constexpr int rackHeight = 50 + insertsSectionHeight + 60; // Routing(50) + Inserts + Sends(60), rigid sections

    // Pro Tools console aesthetic: a massive, clearly-visible meter next to the
    // fader, not a thin sliver — widened from the previous 18px outer column
    // (which left only a 14px actual bar) to a real 24px-wide presence. One
    // named constant, used by BOTH paint()'s meterColumn and resized()'s
    // reservation, so they can never drift apart (same discipline
    // UniversalDeviceChainComponent's DeviceBlock uses for its own shared
    // layout constants).
    constexpr int meterColumnWidth = 32;

    const juce::String pluginDragPrefix = "plugin_drag|";

    // Utility plugins every track carries (volume/pan, metering) aren't "inserts" in
    // the Pro Tools sense — InsertsBlock only lists plugins the user actually loaded.
    bool isUtilityPlugin (const te::Plugin& p)
    {
        return dynamic_cast<const te::VolumeAndPanPlugin*> (&p) != nullptr
            || dynamic_cast<const te::LevelMeterPlugin*> (&p) != nullptr
            || dynamic_cast<const te::AuxSendPlugin*> (&p) != nullptr;
    }
}

//==============================================================================
// ChannelStripRack — the collapsible Pro Tools-style block: Routing / Inserts / Sends.
// Nested (not top-level classes) since none of the three sections are meaningful
// outside a MixerStrip's context — they all operate on that strip's own track.
class MixerStrip::ChannelStripRack : public juce::Component
{
public:
    ChannelStripRack (te::AudioTrack::Ptr t, CrateWorkflowManager& w) : track (t), workflow (w)
    {
        addAndMakeVisible (routingSection);
        addAndMakeVisible (insertsSection);
        addAndMakeVisible (sendsSection);

        // Input routing isn't wired to the engine yet (MASTER_ARCHITECTURE.md
        // roadmap — full input-device routing is a later phase), so this combo is
        // a single-item placeholder, honestly disclosed via tooltip rather than
        // faking a working dropdown.
        routingSection.inputCombo.addItem ("Ext. In", 1);
        routingSection.inputCombo.setSelectedId (1, juce::dontSendNotification);
        routingSection.inputCombo.setTooltip ("Input routing isn't wired to the engine yet - display only.");

        populateOutputCombo();
        routingSection.outputCombo.onChange = [this] { applyOutputComboSelection(); };

        rebuild();
    }

    /** Re-reads the track's current plugin list — call after any plugin load/remove
        so Inserts/Sends don't go stale (same contract as ArrangementComponent's
        rebuildTracks() / MixerComponent's rebuildStrips()). */
    void rebuild()
    {
        insertSlots.clear();
        ghostInsertSlots.clear();
        sendSlots.clear();
        sendLevelSliders.clear();

        if (track == nullptr)
        {
            resized();
            return;
        }

        for (auto* p : track->pluginList)
        {
            if (p == nullptr || isUtilityPlugin (*p))
                continue;

            if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
            {
                auto slot = std::make_unique<PluginSlotComponent> ("Bus " + juce::String (send->getBusNumber()));
                slot->setBypassState (send->isEnabled(), juce::dontSendNotification);
                slot->onBypassToggle = [send] (bool isOn) { send->setEnabled (isOn); };
                // Deliberately no onClicked: AuxSendPlugin is excluded from the
                // Universal Device Chain (isChainablePlugin() there), so wiring a
                // focus request for it would just clear the chain's focus state —
                // matches the original SendRow's behaviour of not being selectable.

                auto levelSlider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);

                if (send->gain != nullptr)
                {
                    auto* gainParam = send->gain.get();
                    const auto range = gainParam->getValueRange();
                    levelSlider->setRange (range.getStart(), range.getEnd(), 0.001);
                    levelSlider->setValue (gainParam->getCurrentValue(), juce::dontSendNotification);

                    const auto busNumber = send->getBusNumber();
                    levelSlider->onDragStart = [gainParam, busNumber]
                    {
                        gainParam->getEdit().getUndoManager().beginNewTransaction ("Tweak Send " + juce::String (busNumber) + " Level");
                        gainParam->parameterChangeGestureBegin();
                    };
                    levelSlider->onDragEnd = [gainParam] { gainParam->parameterChangeGestureEnd(); };

                    auto* sliderPtr = levelSlider.get();
                    levelSlider->onValueChange = [gainParam, sliderPtr] { gainParam->setParameter ((float) sliderPtr->getValue(), juce::sendNotificationSync); };
                }

                slot->setTrailingComponent (levelSlider.get());
                sendsSection.addAndMakeVisible (*slot);
                sendLevelSliders.push_back (std::move (levelSlider));
                sendSlots.push_back (std::move (slot));
            }
            else
            {
                auto slot = std::make_unique<PluginSlotComponent> (p->getName());
                slot->setBypassState (p->isEnabled(), juce::dontSendNotification);
                slot->onBypassToggle = [p] (bool isOn) { p->setEnabled (isOn); };

                // 'Pop & Sync' workflow: a single click on a loaded insert both
                // focuses it in the Device Chain (as before) AND pops its own
                // native GUI over the DAW immediately — mix engineers no longer
                // need a second trip through the Device Chain just to open the
                // plugin's window. Reuses the exact showWindowExplicitly() path
                // CrateWorkflowManager already calls right after instantiating a
                // plugin, guarded the same way (hasEditor() — some plugins/
                // internal types have no UI at all). This stays here rather than
                // inside PluginSlotComponent itself deliberately: that class is
                // engine-agnostic by design (see its class doc comment) and has
                // no tracktion_engine include to call te::Plugin APIs with —
                // MixerStrip already owns the te::Plugin* binding, same as
                // onBypassToggle/onPluginDropped above.
                slot->onClicked = [this, p]
                {
                    if (onSlotSelected)
                        onSlotSelected (p);

                    if (auto* processor = p->getWrappedAudioProcessor())
                        if (processor->hasEditor())
                            p->showWindowExplicitly();
                };

                // Drag-and-drop from the Browser (MASTER_ARCHITECTURE.md section 1):
                // resolve the dropped plugin's identifier back to a real
                // juce::PluginDescription via the engine's own knownPluginList, then
                // insert it AT this slot's current index — pushing this plugin (and
                // everything after it) down one, not appending to the end. No manual
                // rack->rebuild() call needed afterwards: loadPluginOntoTrack()'s
                // insertPlugin() fires the track's own ValueTree PLUGIN-child-added
                // notification, which this class already listens for (see
                // MixerStrip::valueTreeChildAdded) and rebuilds from, deferred.
                slot->onPluginDropped = [this, p] (const juce::String& identifier)
                {
                    if (track == nullptr)
                        return;

                    if (auto desc = track->edit.engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
                    {
                        const int index = track->pluginList.indexOf (p);
                        workflow.loadPluginOntoTrack (*desc, *track, index);
                    }
                };

                insertsSection.content.addAndMakeVisible (*slot);
                insertSlots.push_back (std::move (slot));
            }
        }

        // "Minimum-10 Scrolling Grid": pad out to at least insertMinVisibleRows
        // rows with empty, explicitly-numbered ghost slots — exactly one
        // trailing ghost once the track already has more real plugins than
        // that minimum, so there's always exactly one obvious "drop here to
        // append" row no matter how many plugins are already loaded.
        const int filledCount = (int) insertSlots.size();
        const int totalRows = juce::jmax (insertMinVisibleRows, filledCount + 1);

        for (int index = filledCount; index < totalRows; ++index)
        {
            auto ghost = std::make_unique<PluginSlotComponent> (juce::String());
            ghost->setGhostState (true);

            // Dropping onto THIS numbered ghost row inserts at its exact
            // index, not a blanket append — slot #5 always means "become
            // plugin #5", even while slots #3/#4 are themselves still empty
            // ghosts (loadPluginOntoTrack/PluginList::insertPlugin clamps to
            // the end if index is ever past the real list's current size).
            ghost->onPluginDropped = [this, index] (const juce::String& identifier)
            {
                if (track == nullptr)
                    return;

                if (auto desc = track->edit.engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
                    workflow.loadPluginOntoTrack (*desc, *track, index);
            };

            insertsSection.content.addAndMakeVisible (*ghost);
            ghostInsertSlots.push_back (std::move (ghost));
        }

        // Explicit, cascade-independent relayout of the grid's new children —
        // see InsertsSection::refreshContentLayout()'s doc comment for why
        // this can't just be left to the resized() call below.
        insertsSection.refreshContentLayout();
        resized();
    }

    std::function<void (te::Plugin*)> onSlotSelected;

    // Rigid, Pro-Tools-style strict sequential subtraction — every section gets a
    // fixed, unconditional height. No FlexBox, no removeFromTop() chains that can
    // silently hand a row less space (down to zero) than it needs; each section is
    // a self-contained Component that lays out its own children within whatever
    // fixed rectangle it's handed here.
    void resized() override
    {
        auto b = getLocalBounds();
        routingSection.setBounds (b.removeFromTop (50));

        // Component::setBounds() is a silent no-op — it does NOT call
        // resized() — when the rect given is identical to the component's
        // CURRENT bounds. rebuild() runs from the async ValueTree listener
        // (a plugin dropped/loaded/removed) without ChannelStripRack's own
        // bounds ever changing, so insertsSection/sendsSection's rects here
        // are the same every time — meaning their resized() (and therefore
        // InsertsSection's viewport/content relayout) would silently never
        // re-run, leaving freshly rebuilt PluginSlotComponents sitting at
        // their default (0,0,0,0) bounds — invisible — until something else
        // (e.g. a tab switch forcing a real, size-changing top-down layout
        // pass) finally gave them real bounds. Forcing an explicit resized()
        // call after each setBounds() closes that gap: it always re-lays-out
        // the section's current children immediately, regardless of whether
        // the section's own rectangle moved a single pixel.
        insertsSection.setBounds (b.removeFromTop (insertsSectionHeight));
        insertsSection.resized();

        sendsSection.setBounds (b.removeFromTop (60));
        sendsSection.resized();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LAF::background);

        // Subtle dividers between the three fixed slots — reads as distinct Pro
        // Tools-style sections rather than one continuous block.
        g.setColour (juce::Colours::grey.withAlpha (0.3f));
        g.drawHorizontalLine (routingSection.getBottom(), 0.0f, (float) getWidth());
        g.drawHorizontalLine (insertsSection.getBottom(), 0.0f, (float) getWidth());
    }

private:
    void populateOutputCombo()
    {
        routingSection.outputCombo.clear (juce::dontSendNotification);
        outputComboActions.clear();

        if (track == nullptr)
            return;

        juce::StringArray deviceNames, aliases;
        juce::BigInteger hasAudio, hasMidi;
        te::TrackOutput::getPossibleOutputDeviceNames (te::getAudioTracks (track->edit), deviceNames, aliases, hasAudio, hasMidi);

        int itemId = 1;

        for (auto& deviceName : deviceNames)
        {
            routingSection.outputCombo.addItem (deviceName, itemId);
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
                routingSection.outputCombo.addSeparator();
                addedSeparator = true;
            }

            routingSection.outputCombo.addItem ("Track: " + other->getName(), itemId);
            outputComboActions[itemId] = [this, other] { track->getOutput().setOutputToTrack (other); };
            ++itemId;
        }

        // Best-effort selection match against the track's real current output —
        // TrackOutput doesn't expose a stable "which item id is this" query, so
        // this matches by the same descriptive text the old TextButton showed.
        const auto currentDescription = track->getOutput().getDescriptiveOutputName();

        for (int i = 0; i < routingSection.outputCombo.getNumItems(); ++i)
        {
            if (routingSection.outputCombo.getItemText (i) == currentDescription)
            {
                routingSection.outputCombo.setSelectedId (routingSection.outputCombo.getItemId (i), juce::dontSendNotification);
                break;
            }
        }
    }

    void applyOutputComboSelection()
    {
        const auto it = outputComboActions.find (routingSection.outputCombo.getSelectedId());

        if (it != outputComboActions.end())
            it->second();
    }

    //==========================================================================
    struct RoutingSection : public juce::Component
    {
        RoutingSection()
        {
            addAndMakeVisible (caption);
            caption.setText ("ROUTING", juce::dontSendNotification);
            caption.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            caption.setColour (juce::Label::textColourId, sectionCaption);

            // Sleek flat dropdowns for In/Out — CrateMixerLookAndFeel's
            // drawComboBox() (set as this whole rack's LookAndFeel, see
            // ChannelStripRack's constructor via getLookAndFeel() inheritance from
            // MixerStrip) replaces the bulky default OS/JUCE combo chrome.
            addAndMakeVisible (inputCombo);
            addAndMakeVisible (outputCombo);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (6, 4);
            caption.setBounds (b.removeFromTop (12));
            inputCombo.setBounds (b.removeFromTop (14));
            b.removeFromTop (1);
            outputCombo.setBounds (b.removeFromTop (14));
        }

        juce::Label caption;
        juce::ComboBox inputCombo, outputCombo;
    };

    //==========================================================================
    // Shared by InsertsSection and SendsSection: a fixed-height slot that stacks
    // whatever PluginSlotComponents have been added to it, top to bottom, at a
    // fixed row height — same rigid-subtraction philosophy as the outer rack,
    // just scoped to this one section instead of FlexBox-managed.
    struct RowStackSection : public juce::Component
    {
        explicit RowStackSection (juce::String captionText, juce::String emptyText)
            : placeholderText (std::move (emptyText))
        {
            addAndMakeVisible (caption);
            caption.setText (captionText, juce::dontSendNotification);
            caption.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            caption.setColour (juce::Label::textColourId, sectionCaption);
        }

        int getNumRows() const   { return getNumChildComponents() - 1; } // -1 excludes caption

        void resized() override
        {
            auto b = getLocalBounds().reduced (6, 4);
            caption.setBounds (b.removeFromTop (14));

            for (int i = 0; i < getNumChildComponents(); ++i)
            {
                auto* child = getChildComponent (i);

                if (child == &caption)
                    continue;

                // 20px slot height per the Lead UX Architect's spec — the compact,
                // dark, rounded-rect PluginSlotComponent look.
                child->setBounds (b.removeFromTop (20));
                b.removeFromTop (2);
            }
        }

        void paint (juce::Graphics& g) override
        {
            if (getNumRows() > 0)
                return;

            g.setColour (LAF::textDim);
            g.setFont (juce::FontOptions (9.5f));
            g.drawText (placeholderText, getLocalBounds().reduced (6, 4).withTop (18).withHeight (14),
                         juce::Justification::centredLeft);
        }

        juce::Label caption;
        juce::String placeholderText;
    };

    // "Minimum-10 Scrolling Grid" (Pro Tools/Ableton hybrid Inserts rack). NOT a
    // RowStackSection — that type divides nothing and grows unboundedly, which is
    // exactly wrong here. Instead: a juce::Viewport (scrollbars hidden, to keep
    // the flat analog-console look — mouse wheel still scrolls it regardless of
    // scrollbar visibility) wrapping a GridContent that stacks whatever
    // PluginSlotComponents (real, filled — or empty "ghost" placeholders) have
    // been added to it at a STRICT, hardcoded insertRowHeight per row, never
    // divided from the parent's height. rebuild() below always pads the child
    // list out to at least insertMinVisibleRows total rows (real + ghost), so
    // an empty track still shows the full 10-slot rack, and a 14-plugin track
    // scrolls inside the fixed viewport instead of squishing every row down to
    // illegibility. No section-level DragAndDropTarget is needed any more (the
    // old "empty track has zero droppable pixels" bug this used to guard
    // against): ghost rows now cover every row down to the minimum, so there's
    // always a real, exactly-numbered PluginSlotComponent under the cursor.
    struct InsertsSection : public juce::Component
    {
        // Lays out whatever children (real or ghost PluginSlotComponents) have
        // been added, top to bottom, at insertRowHeight each — the ONE place
        // that height is enforced. Height is driven by child COUNT, not the
        // viewport's visible height, which is what makes scrolling (rather than
        // squishing) happen once more than insertMinVisibleRows are present.
        struct GridContent : public juce::Component
        {
            void refreshHeight (int width)
            {
                // Explicit required-height calc from the ACTUAL current child
                // count (not the nominal insertsViewportHeight constant) —
                // this is what lets the grid keep growing past the 10-slot
                // minimum (14 plugins => 15 children => 15 rows tall, not
                // clamped back down to 10). One gap between each pair of rows,
                // none trailing after the last — GridContent::resized()'s loop
                // below still requests one gap per row including the last, but
                // Rectangle::removeFromTop() clamps at zero rather than going
                // negative, so that trailing over-request is a harmless no-op,
                // not a clipped last row.
                const int totalRows = juce::jmax (insertMinVisibleRows, getNumChildComponents());
                const int requiredHeight = totalRows * insertRowHeight + (totalRows - 1) * insertRowGap;
                setBounds (0, 0, width, requiredHeight);

                // setBounds() only re-invokes resized() when the SIZE actually
                // changes — but rebuild() always clears and re-adds children
                // (fresh PluginSlotComponent instances) even when the total row
                // count (and therefore height) comes out identical to before,
                // so an unconditional call here is required or the newly
                // added children would stay at their default (0,0,0,0) bounds.
                resized();
            }

            void resized() override
            {
                auto b = getLocalBounds();

                for (int i = 0; i < getNumChildComponents(); ++i)
                {
                    getChildComponent (i)->setBounds (b.removeFromTop (insertRowHeight));
                    b.removeFromTop (insertRowGap);
                }
            }
        };

        InsertsSection()
        {
            addAndMakeVisible (caption);
            caption.setText ("INSERTS", juce::dontSendNotification);
            caption.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            caption.setColour (juce::Label::textColourId, sectionCaption);

            // Viewport::setScrollBarsShown's first parameter is VERTICAL,
            // second is HORIZONTAL. Vertical shown when the rack actually
            // overflows past insertMinVisibleRows; horizontal strictly OFF —
            // GridContent's rows are always exactly the content's own width
            // (see refreshContentLayout()'s use of getMaximumVisibleWidth()
            // below), so there is nothing that should ever need horizontal
            // scroll. A long plugin name is a text-truncation problem
            // (PluginSlotComponent::paint()'s ellipsis), not a layout-width one.
            viewport.setScrollBarsShown (true, false);
            // ScrollOnDragMode::all (not the default nonHover) so a plain
            // desktop mouse-drag scrolls too, not just touch/non-hover input.
            viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
            viewport.setViewedComponent (&content, false); // not owned — see member declaration order below
            addAndMakeVisible (viewport);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (6, 4);
            caption.setBounds (b.removeFromTop (insertsCaptionHeight));
            viewport.setBounds (b);
            refreshContentLayout();
        }

        /** Re-lays-out the grid's current children NOW, independent of whether
            this section's own bounds have changed — see ChannelStripRack::
            rebuild()'s call to this and ::resized()'s doc comment for why that
            independence matters (a plain resized()-cascade from the top is not
            reliable here: setBounds() no-ops, and skips calling resized(),
            whenever a rect equals the component's current bounds — which is
            exactly the case every time rebuild() runs off the async plugin-
            list-changed listener without the strip's own size changing). */
        void refreshContentLayout()
        {
            // getMaximumVisibleWidth(), NOT getWidth() — returns the
            // viewport's content area width AFTER any vertical scrollbar's
            // reserved thickness has already been subtracted (JUCE source:
            // Viewport::getMaximumVisibleWidth() == contentHolder.getWidth(),
            // and contentHolder is shrunk by scrollbarWidth in
            // updateVisibleArea() whenever the vertical bar is visible). Using
            // plain getWidth() here would size every row UNDER the scrollbar,
            // so the bar visually overlapped/cut into the slot graphics
            // instead of sitting cleanly outside the content column.
            content.refreshHeight (viewport.getMaximumVisibleWidth());

            // Belt-and-suspenders: juce::Viewport already listens for its
            // viewed component's resize via ComponentListener::
            // componentMovedOrResized (wired in setViewedComponent()) and
            // recomputes scrollbars/visible area from that automatically, so
            // this call is redundant on a real content-size change — but it's
            // a cheap, idempotent way to guarantee the Viewport re-evaluates
            // its overflow state immediately after THIS method returns,
            // rather than depending on that listener callback having already
            // run by the time the caller's own next line executes.
            viewport.resized();
        }

        juce::Label caption;

        // Declaration order matters: members are destroyed in REVERSE order, so
        // content (declared first) is destroyed AFTER viewport (declared
        // second) — viewport's destructor runs first while content is still
        // alive, which is required since viewport holds a raw (non-owning)
        // pointer to it via setViewedComponent (..., false).
        GridContent content;
        juce::Viewport viewport;
    };

    struct SendsSection : public RowStackSection
    {
        SendsSection() : RowStackSection ("SENDS", "(none)") {}
    };

    te::AudioTrack::Ptr track;
    CrateWorkflowManager& workflow;

    RoutingSection routingSection;
    InsertsSection insertsSection;
    SendsSection sendsSection;

    std::vector<std::unique_ptr<PluginSlotComponent>> insertSlots;
    std::vector<std::unique_ptr<PluginSlotComponent>> ghostInsertSlots; // padding rows, see InsertsSection
    std::vector<std::unique_ptr<PluginSlotComponent>> sendSlots;
    std::vector<std::unique_ptr<juce::Slider>> sendLevelSliders; // trailing controls; not owned by PluginSlotComponent

    // Maps outputCombo item id -> the routing action to perform — avoids parallel
    // arrays keyed by index, which drift the moment items are added conditionally
    // (the "Track: X" separator/section makes indices non-contiguous with itemId).
    std::map<int, std::function<void()>> outputComboActions;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStripRack)
};

//==============================================================================
MixerStrip::MixerStrip (te::AudioTrack::Ptr trackToControl, CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse), track (trackToControl)
{
    if (track != nullptr)
    {
        // addDefaultTrackPlugins() (called when the track was created) already put a
        // VolumeAndPanPlugin + LevelMeterPlugin on the chain — reuse those instead of
        // inserting duplicates.
        volumePlugin = track->getVolumePlugin();
        meterPlugin = track->pluginList.findFirstPluginOfType<te::LevelMeterPlugin>();
        trackNameLabel.setText (track->getName(), juce::dontSendNotification);

        if (meterPlugin == nullptr)
        {
            auto plugin = track->edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, juce::PluginDescription());
            track->pluginList.insertPlugin (plugin, -1, nullptr);
            meterPlugin = dynamic_cast<te::LevelMeterPlugin*> (plugin.get());
        }

        if (meterPlugin != nullptr)
            meterPlugin->measurer.addClient (meterClient);
    }

    rack = std::make_unique<ChannelStripRack> (track, workflow);

    // Applies to the WHOLE rack subtree via JUCE's parent-chain LookAndFeel
    // lookup (RoutingSection's In/Out ComboBoxes, the Sends' level sliders) —
    // volumeFader/panKnob below are a separate sibling subtree and need their
    // own explicit setLookAndFeel() call, done further down.
    rack->setLookAndFeel (&mixerLookAndFeel);

    rack->onSlotSelected = [this] (te::Plugin* p)
    {
        if (onPluginSlotSelected)
            onPluginSlotSelected (track.get(), p);
    };
    addChildComponent (*rack); // addChildComponent, not addAndMakeVisible — starts hidden, setRackExpanded() controls visibility
    rack->setVisible (rackExpanded);

    addAndMakeVisible (trackNameLabel);
    trackNameLabel.setJustificationType (juce::Justification::centred);
    trackNameLabel.setColour (juce::Label::textColourId, LAF::text);
    trackNameLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));

    // True Mute polarity — MUST match TrackHeaderComponent's ToggleBlock exactly:
    // ON/lit (studio orange) = muted, OFF/dark = audible. Toggle state maps
    // DIRECTLY to track->isMuted(), no inversion.
    addAndMakeVisible (muteButton);
    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, muteOnColour);
    muteButton.setToggleState (track != nullptr && track->isMuted (false), juce::dontSendNotification);
    muteButton.onClick = [this]
    {
        if (track != nullptr)
            track->setMute (muteButton.getToggleState());
    };

    addAndMakeVisible (soloButton);
    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, soloOnColour);
    soloButton.setToggleState (track != nullptr && track->isSolo (false), juce::dontSendNotification);
    soloButton.onClick = [this]
    {
        if (track != nullptr)
            track->setSolo (soloButton.getToggleState());
    };

    // Pro Tools/Logic-grade console chrome, scoped to JUST these two controls —
    // not the app-wide default LookAndFeel (see CrateMixerLookAndFeel.h's doc
    // comment). Cleared explicitly in the destructor before mixerLookAndFeel is
    // destroyed.
    addAndMakeVisible (panKnob);
    panKnob.setLookAndFeel (&mixerLookAndFeel);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                  juce::MathConstants<float>::pi * 2.8f, true);

    addAndMakeVisible (volumeFader);
    volumeFader.setLookAndFeel (&mixerLookAndFeel);
    volumeFader.setRange (-60.0, 6.0, 0.1);
    volumeFader.setDoubleClickReturnValue (true, 0.0);

    addAndMakeVisible (volumeValueLabel);
    volumeValueLabel.setJustificationType (juce::Justification::centred);
    volumeValueLabel.setColour (juce::Label::textColourId, LAF::textDim);
    volumeValueLabel.setFont (juce::FontOptions (11.0f));

    if (volumePlugin != nullptr)
    {
        refreshFromEngine();

        // Bracket drags with gesture calls so TE treats each as a single automation
        // move (matters once automation recording lands in a later phase) rather than
        // a stream of discrete parameter writes. beginNewTransaction() is a SEPARATE
        // concern — gesture calls don't touch UndoManager at all — and is what
        // actually coalesces the whole drag into one named Undo step instead of one
        // step per mouse-move tick (the "Visual Undo History List" the Lead
        // Architect wants needs a real, single, human-readable name per action).
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
            volumeValueLabel.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);
        };

        panKnob.onDragStart = [this]
        {
            volumePlugin->panParam->getEdit().getUndoManager().beginNewTransaction (
                "Tweak Pan: " + (track != nullptr ? track->getName() : juce::String()));
            volumePlugin->panParam->parameterChangeGestureBegin();
        };
        panKnob.onDragEnd = [this] { volumePlugin->panParam->parameterChangeGestureEnd(); };
        panKnob.onValueChange = [this] { volumePlugin->setPan ((float) panKnob.getValue()); };

        // Native TE listeners — fire when volume/pan change from anywhere other than
        // this strip (automation curve, the Arrangement header's own volume slider,
        // a script), keeping both controls honest.
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
    rack->setLookAndFeel (nullptr);

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
        rack->rebuild(); // picks up any plugin loaded/removed while collapsed

    rack->setVisible (rackExpanded);

    // Deliberately NOT calling resized() here. This strip's own bounds are still
    // whatever they were before this call — the parent (MixerComponent::
    // StripRowContent) hasn't resized us to the new preferred height yet, since
    // that depends on getPreferredHeight(), which we just changed. Laying out now
    // would compute rack->setBounds() against the OLD (still-collapsed) height and
    // crush everything below it. The parent is responsible for calling setBounds()
    // — and therefore resized() — once it has actually given us correct bounds;
    // see StripRowContent::resized()'s explicit strip->resized() call.
}

int MixerStrip::getPreferredHeight() const
{
    return collapsedStripHeight + (rackExpanded ? rackHeight : 0);
}

void MixerStrip::refreshFromEngine()
{
    if (volumePlugin == nullptr)
        return;

    const auto currentDb = volumePlugin->getVolumeDb();
    volumeFader.setValue (currentDb, juce::dontSendNotification);
    volumeValueLabel.setText (juce::String (currentDb, 1) + " dB", juce::dontSendNotification);

    panKnob.setValue (volumePlugin->getPan(), juce::dontSendNotification);
}

void MixerStrip::currentValueChanged (te::AutomatableParameter&)
{
    // SafePointer, not raw this: if a track deletion (Phase 1's synchronous
    // teardown path) destroys this strip between the listener firing and this
    // deferred lambda running, the lambda no-ops instead of touching freed memory.
    juce::Component::SafePointer<MixerStrip> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        // setValue(..., dontSendNotification) inside refreshFromEngine() means this
        // can't re-trigger onValueChange, so no feedback loop against the
        // slider/knob->engine path.
        if (safeThis != nullptr)
            safeThis->refreshFromEngine();
    });
}

void MixerStrip::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& property)
{
    if (track == nullptr || v != track->state)
        return;

    if (property != te::IDs::mute && property != te::IDs::solo)
        return;

    juce::Component::SafePointer<MixerStrip> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->refreshMuteSoloFromEngine();
    });
}

void MixerStrip::refreshMuteSoloFromEngine()
{
    if (track == nullptr)
        return;

    muteButton.setToggleState (track->isMuted (false), juce::dontSendNotification);
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
    // rack always exists (constructed unconditionally in the constructor) — only
    // its visibility depends on rackExpanded — so this is safe to call regardless
    // of current expand state; a collapsed rack just rebuilds invisibly, ready for
    // whenever it's next expanded.
    rack->rebuild();
}

void MixerStrip::timerCallback()
{
    if (meterPlugin == nullptr)
        return;

    const auto levelL = meterClient.getAndClearAudioLevel (0);
    const auto levelR = meterClient.getAndClearAudioLevel (1);
    meterLevelDb = juce::jmax (levelL.dB, levelR.dB);

    // Peak Hold: entirely UI-side (TE's LevelMeasurer only reports near-instant
    // block peaks, not a held/decaying value) — snaps up instantly to a new
    // higher level, then decays at a fixed dB/sec once nothing higher has come
    // in. Only ever touched from this timer callback (message thread), reading
    // already-drained values out of meterClient (lock-free/atomic on the audio
    // side) — zero allocation, zero audio-thread access.
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

    repaint();
}

void MixerStrip::paint (juce::Graphics& g)
{
    g.fillAll (LAF::panel);
    g.setColour (LAF::background);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    if (rackExpanded)
        g.drawHorizontalLine (rackHeight, 0.0f, (float) getWidth());

    // meterBounds is computed once in resized(), sliced from the EXACT SAME
    // remaining-space rect volumeFader uses — see its declaration in the header
    // for why this can no longer drift out of sync with the fader's real height.
    g.setColour (LAF::background);
    g.fillRect (meterBounds);

    const auto normalised = juce::jlimit (0.0f, 1.0f, (meterLevelDb - meterFloorDb) / meterRangeDb);
    const auto fillHeight = meterBounds.getHeight() * normalised;
    const auto fillRect = meterBounds.withTop (meterBounds.getBottom() - fillHeight);

    if (fillHeight > 0.0f)
    {
        // Green -> Yellow -> Red across the meter's FULL height, so a short green
        // sliver and a tall near-red bar are both cut from the SAME ramp rather
        // than two independently-scaled gradients.
        juce::ColourGradient gradient (juce::Colours::limegreen, meterBounds.getBottomLeft(),
                                        juce::Colours::red, meterBounds.getTopLeft(), false);
        gradient.addColour (0.75, juce::Colours::yellow);
        g.setGradientFill (gradient);
        g.fillRect (fillRect);
    }

    // Peak Hold — a distinct bright 1px line at the held peak value, drawn ON TOP
    // of the live fill so it still reads once the live level has dropped below it.
    const auto peakNormalised = juce::jlimit (0.0f, 1.0f, (peakHoldDb - meterFloorDb) / meterRangeDb);
    const auto peakY = meterBounds.getBottom() - meterBounds.getHeight() * peakNormalised;
    g.setColour (juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (meterBounds.getX(), peakY - 0.5f, meterBounds.getWidth(), 1.0f));

    // Numerical peak-hold dB readout at the top of the column — skipped entirely
    // at/near the floor (effectively -INF) rather than printing a meaningless
    // "-60.0" when there's no real signal.
    if (peakHoldDb > meterFloorDb + 0.5f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (juce::String (peakHoldDb, 1), meterBounds.withHeight (12.0f),
                    juce::Justification::centred);
    }
}

void MixerStrip::resized()
{
    auto full = getLocalBounds();

    // Both branches explicitly consume from the top — rigid Pro Tools-style
    // allocation, not a conditional skip. rackHeight (200) matches
    // ChannelStripRack's own fixed 50+90+60 section stack exactly, so this can
    // never under- or over-allocate relative to what that block actually lays out.
    if (rackExpanded)
        rack->setBounds (full.removeFromTop (rackHeight));
    else
        rack->setBounds (full.removeFromTop (0));

    auto area = full.reduced (6);

    trackNameLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    auto buttonRow = area.removeFromTop (22);
    muteButton.setBounds (buttonRow.removeFromLeft (buttonRow.getWidth() / 2).reduced (2, 0));
    soloButton.setBounds (buttonRow.reduced (2, 0));
    area.removeFromTop (4);

    panKnob.setBounds (area.removeFromTop (40).reduced (10, 0));
    area.removeFromTop (4);

    // Fader/Meter container: EVERYTHING left (down to the dB value label at the
    // very bottom) is stretched to fill it completely — the value label is
    // carved off FIRST, then the meter column is sliced from the SAME remaining
    // rect volumeFader gets, so the two always share an identical, maximal
    // vertical span — "massive and tall", matching the fader exactly, not a
    // mismatched sliver next to it.
    volumeValueLabel.setBounds (area.removeFromBottom (16));
    meterBounds = area.removeFromRight (meterColumnWidth).reduced (4, 0).toFloat();
    volumeFader.setBounds (area);
}
