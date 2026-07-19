#include "MixerComponent.h"
#include "MixerStrip.h"
#include "MasterStrip.h"
#include "TheCrateLookAndFeel.h"
#include "TrackUtils.h"

namespace
{
    using LAF = TheCrateLookAndFeel;
    constexpr int stripWidth = 96;
    constexpr int topBarHeight = 28;
    constexpr int returnDockMargin = 6; // "slight margin... to emulate an SSL console's return section"
}

//==============================================================================
class MixerComponent::StripRowContent : public juce::Component
{
public:
    StripRowContent (te::Edit& e, CrateWorkflowManager& w) : edit (e), workflow (w) {}

    // Hybrid Bus/Return Architecture: this class is now reused for BOTH the
    // scrolling regular-track row (inside `viewport`) AND the pinned return-
    // track dock (no viewport, fixed bounds, right of the regular row and
    // left of masterStrip) — see MixerComponent::returnStripDock. Same
    // rendering, same rack-expand behaviour, just a different container.
    // `tracksToShow` is whichever subset (TrackUtils::splitTracks()'s
    // regularTracks or returnTracks) the caller owns.
    void rebuild (bool rackExpandedState, const std::vector<te::AudioTrack*>& tracksToShow)
    {
        strips.clear();

        for (auto* t : tracksToShow)
        {
            auto strip = std::make_unique<MixerStrip> (t, workflow);
            strip->setRackExpanded (rackExpandedState);
            strip->onPluginSlotSelected = [this] (te::AudioTrack* track, te::Plugin* plugin)
            {
                if (onPluginSlotSelectedExternally)
                    onPluginSlotSelectedExternally (track, plugin);
            };
            addAndMakeVisible (*strip);
            strips.push_back (std::move (strip));
        }

        relayout();
    }

    void setAllStripsExpanded (bool shouldBeExpanded)
    {
        for (auto& strip : strips)
            strip->setRackExpanded (shouldBeExpanded);

        relayout();
    }

    std::function<void (te::AudioTrack*, te::Plugin*)> onPluginSlotSelectedExternally;

    void setMinWidth (int w)   { minWidth = w; }
    int preferredWidth() const { return (int) strips.size() * stripWidth; }

    int preferredHeight() const
    {
        // All strips share the same expand state, so their preferred height is
        // identical — query the first one (or fall back to a sane default with no
        // tracks at all, so an empty mixer isn't zero-height).
        return strips.empty() ? 210 : strips.front()->getPreferredHeight();
    }

    void resized() override
    {
        int x = 0;
        for (auto& strip : strips)
        {
            strip->setBounds (x, 0, stripWidth, getHeight());

            // Forced explicitly: JUCE's setBounds() skips calling resized() on the
            // child when the new size is identical to its current one — which
            // happens whenever this row's total height was already being driven by
            // the viewport (getHeight() unchanged) rather than by preferredHeight().
            // setRackExpanded() deliberately does NOT lay itself out (see
            // MixerStrip.cpp), so without this, toggling the rack in that case would
            // never re-run MixerStrip::resized() at all — worse than the original bug.
            strip->resized();

            x += stripWidth;
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LAF::background);

        if (strips.empty())
        {
            g.setColour (LAF::textDim);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText ("No tracks yet", getLocalBounds(), juce::Justification::centred);
        }
    }

private:
    void relayout()
    {
        setSize (juce::jmax (preferredWidth(), minWidth), preferredHeight());
        resized();
    }

    te::Edit& edit;
    CrateWorkflowManager& workflow;
    std::vector<std::unique_ptr<MixerStrip>> strips;
    int minWidth = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StripRowContent)
};

//==============================================================================
MixerComponent::MixerComponent (te::Edit& editToShow, CrateWorkflowManager& workflowToUse)
    : edit (editToShow), workflow (workflowToUse)
{
    addAndMakeVisible (expandRackButton);
    expandRackButton.onClick = [this]
    {
        rackExpanded = ! rackExpanded;
        expandRackButton.setButtonText (rackExpanded ? "Collapse Rack" : "Expand Rack");
        content->setAllStripsExpanded (rackExpanded);
        returnStripDock->setAllStripsExpanded (rackExpanded);

        // The pinned MasterStrip must join this toggle too — "Expand Rack"
        // affecting every real track strip but silently doing nothing to
        // Master would leave its rack stuck/broken-looking.
        if (masterStrip != nullptr)
            masterStrip->setRackExpanded (rackExpanded);

        layoutContent();
        resized(); // returnStripDock's preferred width may have just changed height
    };

    content = std::make_unique<StripRowContent> (edit, workflow);
    content->onPluginSlotSelectedExternally = [this] (te::AudioTrack* t, te::Plugin* p)
    {
        if (onPluginSlotSelected)
            onPluginSlotSelected (t, p);
    };

    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, true); // rack-expanded height can exceed the zone
    addAndMakeVisible (viewport);

    // Hybrid Bus/Return Architecture — return-track strips, pinned to the
    // right of the scrolling regular row and directly left of masterStrip
    // (an SSL console's own return section placement), never scrolling.
    // Reuses StripRowContent as-is (identical MixerStrip rendering/rack-expand
    // behaviour) — just placed directly via setBounds() instead of inside a
    // Viewport, same "docked, not scrolled" pattern masterStrip already uses.
    returnStripDock = std::make_unique<StripRowContent> (edit, workflow);
    returnStripDock->onPluginSlotSelectedExternally = [this] (te::AudioTrack* t, te::Plugin* p)
    {
        if (onPluginSlotSelected)
            onPluginSlotSelected (t, p);
    };
    addAndMakeVisible (*returnStripDock);

    // Master — always rendered, pinned to the far right, OUTSIDE the scrolling
    // viewport (see the header's doc comment on this member for why).
    masterStrip = std::make_unique<MasterStrip> (edit, workflow);
    masterStrip->onSelected = [this] { if (onMasterSelected) onMasterSelected(); };
    masterStrip->onInsertSlotSelected = [this] (te::Plugin* p) { if (onMasterInsertSelected) onMasterInsertSelected (p); };
    addAndMakeVisible (*masterStrip);

    rebuildStrips();
}

MixerComponent::~MixerComponent() = default;

void MixerComponent::rebuildStrips()
{
    // Hybrid Bus/Return Architecture — split ONCE, hand both vectors to their
    // respective docks, same "compute once, never disagree" discipline
    // ArrangementComponent::rebuildTracks() uses.
    const auto split = TrackUtils::splitTracks (edit);

    content->rebuild (rackExpanded, split.regularTracks);
    returnStripDock->rebuild (rackExpanded, split.returnTracks);

    layoutContent();
    resized(); // returnStripDock's preferred width may have just changed (track added/removed)
}

void MixerComponent::layoutContent()
{
    const int visibleW = viewport.getMaximumVisibleWidth();
    const int visibleH = viewport.getMaximumVisibleHeight();

    content->setMinWidth (visibleW);
    content->setSize (juce::jmax (content->preferredWidth(), visibleW),
                       juce::jmax (content->preferredHeight(), visibleH));

    // returnStripDock isn't inside a Viewport, so it has no "minimum width to
    // fill" concept — just size it to exactly what its strips need.
    returnStripDock->setSize (returnStripDock->preferredWidth(),
                               juce::jmax (returnStripDock->preferredHeight(), visibleH));
}

void MixerComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);
}

void MixerComponent::paintOverChildren (juce::Graphics& g)
{
    // "Visually separate them with a slight margin or divider line to
    // emulate an SSL console's return section" — a 1px line in each of the
    // return dock's own margin gaps (see resized()), full topBar-to-bottom
    // height. Drawn in paintOverChildren() (not paint()) for the same reason
    // ArrangementComponent's own dividers are: viewport/returnStripDock/
    // masterStrip are opaque child components that would otherwise paint
    // straight over a line drawn before them.
    if (! returnDividerBounds.isEmpty())
    {
        g.setColour (juce::Colours::darkgrey);
        g.drawVerticalLine (returnDividerBounds.getX(), (float) returnDividerBounds.getY(), (float) returnDividerBounds.getBottom());
        g.drawVerticalLine (returnDividerBounds.getRight(), (float) returnDividerBounds.getY(), (float) returnDividerBounds.getBottom());
    }
}

void MixerComponent::resized()
{
    auto area = getLocalBounds();

    auto topBar = area.removeFromTop (topBarHeight).reduced (6, 3);
    expandRackButton.setBounds (topBar.removeFromLeft (120));

    // Master claimed FIRST, from the right — fixed width, full remaining
    // height, entirely outside the viewport, so it can never scroll away
    // alongside the real track strips.
    masterStrip->setBounds (area.removeFromRight (stripWidth));

    // Hybrid Bus/Return Architecture — return strips claimed NEXT (still from
    // the right, so they land directly left of Master), with a visible margin
    // on each side. Zero width (invisible, no space consumed) when there are
    // no return tracks — returnDividerBounds stays empty then too, so
    // paintOverChildren() skips drawing the divider lines.
    const int returnWidth = returnStripDock->preferredWidth();

    if (returnWidth > 0)
    {
        area.removeFromRight (returnDockMargin);
        returnStripDock->setBounds (area.removeFromRight (returnWidth));
        area.removeFromRight (returnDockMargin);
        returnDividerBounds = returnStripDock->getBounds().expanded (returnDockMargin, 0);
    }
    else
    {
        returnStripDock->setBounds (area.removeFromRight (0));
        returnDividerBounds = {};
    }

    viewport.setBounds (area);
    layoutContent();

    // Mixer Fader Alignment: masterStrip lives OUTSIDE the viewport (it never
    // scrolls), so the raw area height it was just given above does NOT
    // account for viewport's horizontal scrollbar — which shows whenever the
    // track row is wider than the visible area and eats into
    // getMaximumVisibleHeight(), the exact value layoutContent() just used to
    // size content/returnStripDock. Without this correction masterStrip ends
    // up several px taller than every real MixerStrip, so its fader (which
    // fills whatever height remains after its fixed-size chrome) stretches
    // further down than theirs. Re-sized to content's actual final height —
    // the same value returnStripDock already converges to — so all three
    // fader rails end on the identical pixel line.
    masterStrip->setSize (masterStrip->getWidth(), content->getHeight());
}
