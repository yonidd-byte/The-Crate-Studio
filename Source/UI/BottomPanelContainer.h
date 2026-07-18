#pragma once

#include <JuceHeader.h>

#include "MasterAnalyzerComponent.h"
#include "CrateColors.h"

/**
    Hybrid Device & Mixer Paradigm (MASTER_ARCHITECTURE.md) — a context-aware
    swap container for the bottom zone. Arrangement/Mixer/Piano Roll each want
    DIFFERENT bottom-panel content (the real Device Chain, a Master Analyzer,
    or a MIDI FX chain), but they're all just synced views onto the same
    underlying DSP graph — so nothing gets destroyed/recreated when the active
    view changes, only setVisible() toggles (Strict UI Rendering Rule).

    Ownership split: MasterAnalyzerComponent and the MIDI FX placeholder are
    NOT Edit-bound, so THIS container owns them directly and they survive a
    project Load untouched, exactly like BrowserComponent does. The real
    UniversalDeviceChainComponent IS Edit-bound (MainComponent tears it down
    and reconstructs it every rebuildUIForEdit()), so this container only ever
    holds a non-owning pointer to whichever instance is currently alive —
    setDeviceChainComponent() re-points it after every rebuild.
*/
class BottomPanelContainer : public juce::Component
{
public:
    enum class ActiveView { arrangement, mixer, pianoRoll };

    BottomPanelContainer()
    {
        addChildComponent (masterAnalyzer);
        addChildComponent (midiFxPlaceholder);

        midiFxPlaceholder.setJustificationType (juce::Justification::centred);
        midiFxPlaceholder.setText ("[ MIDI FX / Generators chain \xe2\x80\x94 coming soon ]", juce::dontSendNotification);
        midiFxPlaceholder.setColour (juce::Label::textColourId, CrateColors::BrandGray);
        midiFxPlaceholder.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    }

    /** MainComponent calls this with its CURRENT UniversalDeviceChainComponent
        every rebuildUIForEdit() (construction, and every project Load) — safe
        to call with a different instance or nullptr at any time; the OLD
        pointer is just removed as a child here, never destroyed (MainComponent
        owns its lifetime). */
    void setDeviceChainComponent (juce::Component* newDeviceChain)
    {
        if (deviceChainComp == newDeviceChain)
            return;

        if (deviceChainComp != nullptr)
            removeChildComponent (deviceChainComp);

        deviceChainComp = newDeviceChain;

        if (deviceChainComp != nullptr)
        {
            addChildComponent (*deviceChainComp);
            applyVisibility();
            resized();
        }
    }

    /** Which view is currently active — swaps visibility only (Strict UI
        Rendering Rule: no destroy/recreate, ever). */
    void setActiveView (ActiveView newView)
    {
        if (activeView == newView)
            return;

        activeView = newView;
        applyVisibility();
    }

    ActiveView getActiveView() const noexcept   { return activeView; }

    // Fixed height regardless of which view is active — matches the existing,
    // deliberate "Device Chain is a FIXED height whenever visible, not
    // dynamically shrunk to fit content" UX decision (see MainComponent's own
    // prior doc comment); this just extends that same fixed-height contract
    // to the Analyzer/MIDI-FX panels too, so toggling the bottom zone never
    // makes the window jitter differently depending on which view you're in.
    static constexpr int preferredHeight = 235;

    void resized() override
    {
        auto bounds = getLocalBounds();

        if (deviceChainComp != nullptr)
            deviceChainComp->setBounds (bounds);

        masterAnalyzer.setBounds (bounds);
        midiFxPlaceholder.setBounds (bounds);
    }

private:
    void applyVisibility()
    {
        if (deviceChainComp != nullptr)
            deviceChainComp->setVisible (activeView == ActiveView::arrangement);

        masterAnalyzer.setVisible (activeView == ActiveView::mixer);
        midiFxPlaceholder.setVisible (activeView == ActiveView::pianoRoll);
    }

    juce::Component* deviceChainComp = nullptr; // non-owning — see class doc comment
    MasterAnalyzerComponent masterAnalyzer;
    juce::Label midiFxPlaceholder;
    ActiveView activeView = ActiveView::arrangement;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomPanelContainer)
};
