#include "MixerComponent.h"
#include "MixerStrip.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;
    constexpr int stripWidth = 96;
    constexpr int topBarHeight = 28;
}

//==============================================================================
class MixerComponent::StripRowContent : public juce::Component
{
public:
    StripRowContent (te::Edit& e, CrateWorkflowManager& w) : edit (e), workflow (w) {}

    void rebuild (bool rackExpandedState)
    {
        strips.clear();

        for (auto* t : te::getAudioTracks (edit))
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
        layoutContent();
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

    rebuildStrips();
}

MixerComponent::~MixerComponent() = default;

void MixerComponent::rebuildStrips()
{
    content->rebuild (rackExpanded);
    layoutContent();
}

void MixerComponent::layoutContent()
{
    const int visibleW = viewport.getMaximumVisibleWidth();
    const int visibleH = viewport.getMaximumVisibleHeight();

    content->setMinWidth (visibleW);
    content->setSize (juce::jmax (content->preferredWidth(), visibleW),
                       juce::jmax (content->preferredHeight(), visibleH));
}

void MixerComponent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);
}

void MixerComponent::resized()
{
    auto area = getLocalBounds();

    auto topBar = area.removeFromTop (topBarHeight).reduced (6, 3);
    expandRackButton.setBounds (topBar.removeFromLeft (120));

    viewport.setBounds (area);
    layoutContent();
}
