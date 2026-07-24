#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include "../CrateWorkflowManager.h"

namespace te = tracktion::engine;

/**
    Phase 4: VST3 scanning + browsing. Thin wrapper around JUCE's own
    juce::PluginListComponent, bound directly to the engine's PluginManager —
    scanning, sortable columns (name/format/category/manufacturer), and crash-safe
    "dead man's pedal" handling all come from JUCE, not reimplemented here.

    Persistence is already handled by TE: PluginManager listens for changes on
    knownPluginList and writes it to PropertyStorage automatically, so there's no
    separate save/load step in this class.

    Routes instantiation through CrateWorkflowManager::loadPluginToSelectedTrack()
    rather than holding its own track pointer — the load target is always whatever
    is currently selected at click time, and the transaction/undo wrapping lives in
    one place instead of being duplicated here.
*/
class PluginBrowserComponent : public juce::Component
{
public:
    explicit PluginBrowserComponent (CrateWorkflowManager& workflowToUse);
    ~PluginBrowserComponent() override;

    void resized() override;

private:
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void loadSelectedRow();
    void startParallelScan();

    CrateWorkflowManager& workflow;
    juce::PluginListComponent pluginListComponent;
    juce::TextButton loadButton { "Load Selected Plugin" };

    // Step 97 (Concurrent OOP Dispatcher) directive: PIMPL — the real
    // implementation constructs multiple genuinely independent
    // te::PluginScanHelpers::CustomScanner instances (one per ThreadPool
    // worker, never shared), which requires including
    // tracktion_PluginScanHelpers.h directly — an implementation-only TE
    // header never exposed through the public tracktion_engine.h umbrella
    // (confirmed: only tracktion_PluginManager.cpp includes it). Kept out
    // of this header entirely via an incomplete type so nothing else in
    // the app needs that include.
    class ConcurrentScanner;
    std::shared_ptr<ConcurrentScanner> concurrentScanner;

    juce::TextButton parallelScanButton { "Scan All Formats (Parallel)" };
    juce::Label scanStatusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowserComponent)
};
