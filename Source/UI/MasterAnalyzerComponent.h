#pragma once

#include <JuceHeader.h>

#include "CrateColors.h"

/**
    Hybrid Device & Mixer Paradigm (MASTER_ARCHITECTURE.md) — the Mixer view's
    bottom-panel content, replacing the Arrangement's UniversalDeviceChainComponent
    while Mixer is active. Placeholder for a future Tonal Balance / Insight
    module (spectrum analyzer, loudness metering, correlation) — genuinely a
    NEW, separate module from the Device Chain, not a reskin of it, since the
    Mixer's whole point per the new paradigm is "Insert slots handle Audio FX
    here; this bottom panel is for session-wide ANALYSIS, not device editing."
*/
class MasterAnalyzerComponent : public juce::Component
{
public:
    MasterAnalyzerComponent() = default;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (CrateColors::DarkBackground);

        g.setColour (CrateColors::BrandGray);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("[ GLOBAL MASTER ANALYZER: Tonal Balance / Insight Module Ready ]",
                     getLocalBounds(), juce::Justification::centred);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterAnalyzerComponent)
};
