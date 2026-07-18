#include "InsertsRackComponent.h"
#include "PluginSlotComponent.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;
    const auto sectionCaption = CrateColors::BrandGray;

    // "Minimum-10 Scrolling Grid" (Pro Tools/Ableton hybrid Inserts rack): every
    // slot is a strict, hardcoded row height — NEVER divided from the parent's
    // height to squish more in — and the rack always shows at least
    // insertMinVisibleRows rows' worth of viewport, real or "ghost" (empty).
    constexpr int insertRowHeight       = 24;
    constexpr int insertRowGap          = 2;
    constexpr int insertMinVisibleRows  = 10;
    constexpr int insertsCaptionHeight  = 14;
    constexpr int insertsSectionPadding = 8; // matches getLocalBounds().reduced (6, 4)'s top+bottom
    constexpr int insertsViewportHeight = insertMinVisibleRows * (insertRowHeight + insertRowGap);

    // Utility plugins every track carries (volume/pan, metering, sends) aren't
    // "inserts" in the Pro Tools sense.
    bool isUtilityPlugin (const te::Plugin& p)
    {
        return dynamic_cast<const te::VolumeAndPanPlugin*> (&p) != nullptr
            || dynamic_cast<const te::LevelMeterPlugin*> (&p) != nullptr
            || dynamic_cast<const te::AuxSendPlugin*> (&p) != nullptr;
    }
}

int InsertsRackComponent::getFixedHeight()
{
    return insertsCaptionHeight + insertsViewportHeight + insertsSectionPadding;
}

void InsertsRackComponent::paint (juce::Graphics& g)
{
    // Distinct hardware-grey container — SAME box for every owner (Track 1's
    // MixerStrip, Master's MasterStrip), so the two can never diverge
    // depending on whichever colour the parent happened to fill behind this
    // component. Fill is colorGhostedOff (#2A2A30) — a lighter, visibly
    // DISTINCT grey from the dark mixer background it sits on, not
    // colorFaderGroove (#101012, that first attempt was too close to the
    // surrounding dark panel and read as "blends in, no container at all").
    // 1px colorFaderGroove border + 3.0f corners is the physical "hardware
    // slot" language the Manifesto's SSL-console section specifies.
    constexpr float cornerRadius = 3.0f;
    g.setColour (LAF::colorGhostedOff);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
    g.setColour (LAF::colorFaderGroove);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius, 1.0f);
}

InsertsRackComponent::~InsertsRackComponent()
{
    cancelPendingUpdate();

    if (listenedState.isValid())
        listenedState.removeListener (this);
}

InsertsRackComponent::InsertsRackComponent()
{
    addAndMakeVisible (caption);
    caption.setText ("INSERTS", juce::dontSendNotification);
    caption.setFont (juce::FontOptions (9.0f, juce::Font::bold));
    caption.setColour (juce::Label::textColourId, sectionCaption);

    // Viewport::setScrollBarsShown's first parameter is VERTICAL, second is
    // HORIZONTAL. Vertical shown when the rack actually overflows past
    // insertMinVisibleRows; horizontal strictly OFF — GridContent's rows are
    // always exactly the content's own width, so nothing should ever need
    // horizontal scroll (a long plugin name is a text-truncation problem,
    // PluginSlotComponent::paint()'s ellipsis, not a layout-width one).
    viewport.setScrollBarsShown (true, false);
    // ScrollOnDragMode::all (not the default nonHover) so a plain desktop
    // mouse-drag scrolls too, not just touch/non-hover input.
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    viewport.setViewedComponent (&content, false); // not owned — see header's declaration-order note
    addAndMakeVisible (viewport);
}

void InsertsRackComponent::GridContent::refreshHeight (int width)
{
    // Explicit required-height calc from the ACTUAL current child count (not
    // the nominal insertsViewportHeight constant) — this is what lets the
    // grid keep growing past the minimum (14 plugins => 15 children => 15
    // rows tall, not clamped back down to 10).
    const int totalRows = juce::jmax (insertMinVisibleRows, getNumChildComponents());
    const int requiredHeight = totalRows * insertRowHeight + (totalRows - 1) * insertRowGap;
    setBounds (0, 0, width, requiredHeight);

    // setBounds() only re-invokes resized() when the SIZE actually changes —
    // but rebuild() always clears and re-adds children (fresh
    // PluginSlotComponent instances) even when the total row count (and
    // therefore height) comes out identical to before, so an unconditional
    // call here is required or the newly added children would stay at their
    // default (0,0,0,0) bounds.
    resized();
}

void InsertsRackComponent::GridContent::resized()
{
    auto b = getLocalBounds();

    for (int i = 0; i < getNumChildComponents(); ++i)
    {
        getChildComponent (i)->setBounds (b.removeFromTop (insertRowHeight));
        b.removeFromTop (insertRowGap);
    }
}

void InsertsRackComponent::refreshContentLayout()
{
    // getMaximumVisibleWidth(), NOT getWidth() — returns the viewport's
    // content area width AFTER any vertical scrollbar's reserved thickness
    // has already been subtracted, so rows never lay out underneath the bar.
    content.refreshHeight (viewport.getMaximumVisibleWidth());

    // Belt-and-suspenders: juce::Viewport already listens for its viewed
    // component's resize and recomputes scrollbars/visible area from that
    // automatically, so this call is redundant on a real content-size
    // change — but it's a cheap, idempotent way to guarantee the Viewport
    // re-evaluates its overflow state immediately, rather than depending on
    // that listener callback having already run by the time the caller's
    // own next line executes.
    viewport.resized();
}

void InsertsRackComponent::resized()
{
    auto b = getLocalBounds().reduced (6, 4);
    caption.setBounds (b.removeFromTop (insertsCaptionHeight));
    viewport.setBounds (b);
    refreshContentLayout();
}

namespace
{
    // For a real te::AudioTrack, track.state IS track.pluginList.state
    // (PluginList::initialise() does state = v, not a child tree). te::MasterTrack
    // is different: MasterTrack::initialise() does pluginList.initialise
    // (edit.getMasterPluginList().state) — a SEPARATE tree from the MasterTrack's
    // own track.state. This picks whichever one actually carries targetTrack's
    // PLUGIN children, so the listener below can never silently attach to the
    // wrong tree for Master.
    juce::ValueTree pluginListStateFor (te::Track& track)
    {
        return track.isMasterTrack() ? track.edit.getMasterPluginList().state : track.state;
    }
}

void InsertsRackComponent::rebuild (te::Track& targetTrack, CrateWorkflowManager& workflowToUse)
{
    if (listenedState.isValid())
        listenedState.removeListener (this);

    currentTrack = &targetTrack;
    currentWorkflow = &workflowToUse;
    listenedState = pluginListStateFor (targetTrack);
    listenedState.addListener (this);

    rebuildSlotsNow();
}

void InsertsRackComponent::rebuildSlotsNow()
{
    if (currentTrack == nullptr || currentWorkflow == nullptr)
        return;

    auto& targetTrack = *currentTrack;

    slots.clear();

    for (auto* p : targetTrack.pluginList)
    {
        if (p == nullptr || isUtilityPlugin (*p) || p->isSynth())
            continue;

        auto slot = std::make_unique<PluginSlotComponent> (p->getName());
        slot->setBypassState (p->isEnabled(), juce::dontSendNotification);
        slot->onBypassToggle = [p] (bool isOn) { p->setEnabled (isOn); };

        // 'Pop & Sync': a single click on a loaded insert both focuses it in
        // the Device Chain AND pops its own native GUI over the DAW
        // immediately.
        slot->onClicked = [this, p]
        {
            if (onSlotSelected)
                onSlotSelected (p);

            if (auto* processor = p->getWrappedAudioProcessor())
                if (processor->hasEditor())
                    p->showWindowExplicitly();
        };

        // Drag-and-drop from the Browser: resolve the dropped plugin's
        // identifier back to a real juce::PluginDescription via the engine's
        // own knownPluginList, then insert it AT this slot's current index —
        // pushing this plugin (and everything after it) down one, not
        // appending to the end. No manual rebuild() call needed afterwards:
        // loadPluginOntoTrack()'s insertPlugin() fires this rack's own
        // ValueTree listener (attached in rebuild(), above), which triggers
        // an async rebuild automatically.
        slot->onPluginDropped = [this, p] (const juce::String& identifier)
        {
            if (currentTrack == nullptr || currentWorkflow == nullptr)
                return;

            if (auto desc = currentTrack->edit.engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
            {
                const int index = currentTrack->pluginList.indexOf (p);
                currentWorkflow->loadPluginOntoTrack (*desc, *currentTrack, index);
            }
        };

        content.addAndMakeVisible (*slot);
        slots.push_back (std::move (slot));
    }

    // "Minimum-10 Scrolling Grid": pad out to at least insertMinVisibleRows
    // rows with empty, explicitly-numbered ghost slots — exactly one
    // trailing ghost once the track already has more real plugins than that
    // minimum, so there's always exactly one obvious "drop here to append"
    // row no matter how many plugins are already loaded.
    const int filledCount = (int) slots.size();
    const int totalRows = juce::jmax (insertMinVisibleRows, filledCount + 1);

    for (int index = filledCount; index < totalRows; ++index)
    {
        auto ghost = std::make_unique<PluginSlotComponent> (juce::String());
        ghost->setGhostState (true);

        // Dropping onto THIS numbered ghost row inserts at its exact index,
        // not a blanket append.
        ghost->onPluginDropped = [this, index] (const juce::String& identifier)
        {
            if (currentTrack == nullptr || currentWorkflow == nullptr)
                return;

            if (auto desc = currentTrack->edit.engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
                currentWorkflow->loadPluginOntoTrack (*desc, *currentTrack, index);
        };

        content.addAndMakeVisible (*ghost);
        slots.push_back (std::move (ghost));
    }

    refreshContentLayout();
}

void InsertsRackComponent::valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree& childTree)
{
    if (parentTree != listenedState || ! childTree.hasType (te::IDs::PLUGIN))
        return;

    triggerAsyncUpdate();
}

void InsertsRackComponent::valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree& childTree, int)
{
    if (parentTree != listenedState || ! childTree.hasType (te::IDs::PLUGIN))
        return;

    triggerAsyncUpdate();
}

void InsertsRackComponent::valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged, const juce::Identifier&)
{
    // A plugin child's own property changed (e.g. bypass toggled by
    // automation rather than this rack's own slot) — refresh so the slot's
    // bypass indicator doesn't go stale. Not the listened tree itself
    // changing a property (that's not a plugin-list-shape change at all).
    if (treeWhosePropertyHasChanged.getParent() != listenedState)
        return;

    triggerAsyncUpdate();
}

void InsertsRackComponent::handleAsyncUpdate()
{
    rebuildSlotsNow();
    resized();
    repaint();
}
