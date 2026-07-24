#include "PluginBrowserComponent.h"

// Step 97 (Concurrent OOP Dispatcher) directive: the one file in this
// codebase allowed to reach into TE's implementation-only scan helper —
// see this include's own justification in PluginBrowserComponent.h, next
// to ConcurrentScanner's forward declaration. Safe to include directly:
// it's a plain header (has its own #pragma once) whose only dependencies
// (Engine, PluginDescription, ChildProcessCoordinator/Worker) are already
// fully defined by the umbrella <tracktion_engine/tracktion_engine.h>
// include that PluginBrowserComponent.h already pulls in above this line.
#include <tracktion_engine/plugins/tracktion_PluginScanHelpers.h>

namespace
{
    // Records "currently scanning X" before each plugin probe so a crash mid-scan
    // doesn't require rescanning everything — JUCE skips the culprit on next launch.
    juce::File getDeadMansPedalFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("The Crate Studio")
                    .getChildFile ("DeadMansPedal.txt");
    }
}

// Step 97 (Concurrent OOP Dispatcher) directive: each ThreadPool worker
// gets ITS OWN te::PluginScanHelpers::CustomScanner instance, constructed
// fresh inside scanOneFile() and never shared across threads — that's
// what makes this safe despite CustomScanner's own masterProcess/
// launched/crashed fields being plain, unsynchronised state (confirmed by
// direct source read, Step 95/96): the thread-safety hazard is ONLY ever
// about multiple threads calling into the SAME instance concurrently.
// N independent instances means N independent relaunched child processes
// scanning in true parallel, each still getting TE's own SEH-hardened,
// crash-isolated child-side handling and one-retry-with-fresh-process
// resilience, individually.
//
// Managed via std::shared_ptr/std::weak_ptr (the exact idiom JUCE's own
// PluginListComponent::Scanner uses for the identical hazard, confirmed
// by direct source read last round): a background job's own callAsync
// callbacks capture a weak_ptr and .lock() before touching anything, so
// closing the browser mid-scan can never leave a queued message thread
// callback pointing at a freed object.
class PluginBrowserComponent::ConcurrentScanner : public std::enable_shared_from_this<ConcurrentScanner>
{
public:
    ConcurrentScanner (te::Engine& engineToUse, juce::KnownPluginList& listToUse)
        : engine (engineToUse),
          knownPluginList (listToUse),
          pool (juce::jmax (1, juce::SystemStats::getNumCpus() - 1))
    {
    }

    ~ConcurrentScanner()
    {
        cancel();

        // Bounded — matches this codebase's own established "never wait
        // forever on a background resource" discipline. A job stuck
        // inside CustomScanner::findPluginTypesFor() with a genuinely
        // hung (not crashed) child could in principle outlast this; the
        // weak_ptr protection above is what keeps that survivable rather
        // than a guaranteed clean shutdown.
        pool.removeAllJobs (true, 10000);
    }

    bool isScanning() const noexcept { return activeJobCount.load (std::memory_order_acquire) > 0; }

    void cancel() { cancelRequested.store (true, std::memory_order_release); }

    std::function<void (int completed, int total)> onProgress;
    std::function<void()> onFinished;

    void startScan()
    {
        if (isScanning())
            return;

        cancelRequested.store (false, std::memory_order_release);
        pendingWork.clear();

        auto& formatManager = engine.getPluginManager().pluginFormatManager;

        for (int i = 0; i < formatManager.getNumFormats(); ++i)
        {
            auto* format = formatManager.getFormat (i);

            // Step 97 directive: mirrors PluginListComponent's own
            // per-format search-path persistence (the same static
            // helpers it already exposes), so a user's configured scan
            // folders are respected identically here.
            auto searchPath = juce::PluginListComponent::getLastSearchPath (
                engine.getPropertyStorage().getPropertiesFile(), *format);

            if (searchPath.getNumPaths() == 0)
                searchPath = format->getDefaultLocationsToSearch();

            if (searchPath.getNumPaths() == 0)
                continue; // formats with no filesystem concept (built-in types) — nothing to enumerate

            for (auto& file : format->searchPathsForPlugins (searchPath, true, false))
                pendingWork.add ({ format, file });
        }

        totalCount = pendingWork.size();
        completedCount.store (0, std::memory_order_relaxed);
        activeJobCount.store (totalCount, std::memory_order_relaxed);

        if (totalCount == 0)
        {
            if (onFinished)
                onFinished();
            return;
        }

        std::weak_ptr<ConcurrentScanner> weakSelf = shared_from_this();

        for (auto& work : pendingWork)
        {
            auto* format = work.format;
            auto file = work.fileOrIdentifier;

            pool.addJob ([weakSelf, format, file]
            {
                if (auto strongSelf = weakSelf.lock())
                    strongSelf->scanOneFile (weakSelf, *format, file);
            });
        }
    }

private:
    struct Work { juce::AudioPluginFormat* format; juce::String fileOrIdentifier; };
    juce::Array<Work> pendingWork;

    void scanOneFile (std::weak_ptr<ConcurrentScanner> weakSelf, juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier)
    {
        juce::Array<juce::PluginDescription> foundCopy;

        if (! cancelRequested.load (std::memory_order_acquire))
        {
            te::PluginScanHelpers::CustomScanner scanner (engine);
            juce::OwnedArray<juce::PluginDescription> found;

            scanner.findPluginTypesFor (format, found, fileOrIdentifier);

            for (auto* d : found)
                foundCopy.add (*d);
        }

        juce::MessageManager::callAsync ([weakSelf, foundCopy]
        {
            auto strongSelf = weakSelf.lock();

            if (strongSelf == nullptr)
                return; // browser closed mid-scan — nothing left to update

            for (auto& d : foundCopy)
                strongSelf->knownPluginList.addType (d);

            const auto completed = strongSelf->completedCount.fetch_add (1, std::memory_order_acq_rel) + 1;

            if (strongSelf->onProgress)
                strongSelf->onProgress (completed, strongSelf->totalCount);

            if (strongSelf->activeJobCount.fetch_sub (1, std::memory_order_acq_rel) == 1)
                if (strongSelf->onFinished)
                    strongSelf->onFinished();
        });
    }

    te::Engine& engine;
    juce::KnownPluginList& knownPluginList;
    juce::ThreadPool pool;

    std::atomic<bool> cancelRequested { false };
    std::atomic<int> activeJobCount { 0 };
    std::atomic<int> completedCount { 0 };
    int totalCount = 0;
};

PluginBrowserComponent::PluginBrowserComponent (CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse),
      pluginListComponent (workflow.getEngine().getPluginManager().pluginFormatManager,
                            workflow.getEngine().getPluginManager().knownPluginList,
                            getDeadMansPedalFile(),
                            &workflow.getEngine().getPropertyStorage().getPropertiesFile(),
                            false)
{
    addAndMakeVisible (pluginListComponent);
    pluginListComponent.setScanDialogText ("Scanning for Plugins", "Searching VST3 folders...");

    // Step 95 (The Scanner Was Already Out-Of-Process) directive — QA
    // finding that corrects a wrong assumption several of Steps 91-94 were
    // built on: this codebase's out-of-process scanner was NEVER dormant.
    // tracktion_PluginManager.cpp's own PluginManager::initialise() (which
    // runs automatically inside te::Engine's own constructor, before this
    // Component ever exists) unconditionally does
    // "knownPluginList.setCustomScanner (std::make_unique<PluginScanHelpers::
    // CustomScanner> (engine));" — every VST3 scan has ALWAYS gone through
    // Tracktion's own out-of-process CustomScanner (which relaunches this
    // app's own exe with a "PluginScan" command line, matching Main.cpp's
    // own startChildProcessPluginScan() hook — already correct, already
    // wired, nothing to add here; an attempt to call setCustomScanner()
    // again from here didn't even compile, since PluginScanHelpers is an
    // implementation-only TE header never exposed through the public
    // tracktion_engine.h umbrella — further confirming it isn't meant to
    // be touched from application code at all).
    //
    // So every actual plugin instantiation this whole investigation has
    // already been happening in a disposable, relaunched child process,
    // regardless of numThreads. What numThreads on THIS PluginListComponent
    // actually controls is only ever which thread on the HOST side calls
    // into PluginScanHelpers::CustomScanner::findPluginTypesFor() — and
    // that function's own wait for the child's reply
    // (PluginScanMasterProcess::waitForReply(), tracktion_PluginScanHelpers.h)
    // is a for(;;) { ...; Thread::sleep(10); continue; } loop with NO
    // timeout at all — only a crashed-child or user-cancelled check. With
    // numThreads=0, PluginListComponent's own Scanner (confirmed via
    // direct source read last round) calls this on the message thread
    // once per 20ms tick — meaning a relaunched child that hangs (a DRM
    // dialog, a slow load that never actually crashes) blocks the HOST's
    // OWN message thread inside that unbounded wait indefinitely: the
    // exact freeze just reproduced, and not something "yielding between
    // files" can help with, since the block happens INSIDE one file's own
    // wait, not between files.
    //
    // numThreads=1 moves that same bounded-only-by-a-crash wait onto a
    // background ThreadPool thread instead — the Host's message thread
    // stays responsive no matter how long one file's out-of-process round
    // trip takes, and since that thread never touches any plugin code
    // itself (the relaunched child does, on its own real message thread —
    // satisfying whatever message-thread affinity Melda/iZotope/UVI/BABY
    // Audio need, independently of anything on the Host side), there's no
    // repeat of the earlier vendor-static-collision Access Violation
    // either. Left at exactly 1, not getNumCpus()-1: TE's own
    // PluginScanHelpers::CustomScanner stores its single shared
    // masterProcess/launched/crashed state in plain (non-atomic, non-
    // locked) fields — concurrent calls from more than one Host-side
    // thread at once is a genuine, real race in TE's own implementation,
    // not something safe to parallelize from this side regardless of the
    // out-of-process isolation.
    pluginListComponent.setNumberOfThreadsForScanning (1);

    // Double-click-to-load: PluginListComponent's row model is private, but
    // sortOrderChanged() physically re-sorts the shared knownPluginList to match
    // what's displayed, so knownPluginList.getTypes()[row] is always correct
    // regardless of current sort — verified against JUCE's own source. Kept as a
    // bonus shortcut, but the button below is the reliable path — ListBox row
    // components get recycled, which can confuse JUCE's per-component double-click
    // timing detection.
    pluginListComponent.getTableListBox().addMouseListener (this, true);

    addAndMakeVisible (loadButton);
    loadButton.onClick = [this] { loadSelectedRow(); };

    // Step 97 (Concurrent OOP Dispatcher) directive: a second, additional
    // scan trigger — the Options-menu scan built into pluginListComponent
    // above is untouched and still works (sequential, via TE's single
    // shared CustomScanner instance), kept as the safe fallback. This
    // button is what actually uses the CPU: N independent out-of-process
    // scanners running at once instead of one.
    addAndMakeVisible (parallelScanButton);
    parallelScanButton.onClick = [this] { startParallelScan(); };

    addAndMakeVisible (scanStatusLabel);
    scanStatusLabel.setJustificationType (juce::Justification::centredLeft);
    scanStatusLabel.setText ({}, juce::dontSendNotification);

    setSize (700, 540);
}

PluginBrowserComponent::~PluginBrowserComponent()
{
    pluginListComponent.getTableListBox().removeMouseListener (this);

    // Step 97 directive: cancel() flips an atomic checked before the next
    // findPluginTypesFor() call on each worker — an ALREADY-in-flight one
    // (mid out-of-process round trip) still runs to completion, and the
    // shared_ptr keeps ConcurrentScanner itself alive via its own pending
    // callAsync captures regardless of what happens to THIS Component. So
    // onProgress/onFinished — plain std::functions that capture `this` —
    // MUST be cleared here, same discipline as every other owner-callback
    // in this codebase (e.g. CrateSandboxBridge::onResizeLimitsChanged):
    // a job finishing after this Component is gone would otherwise call
    // into a dangling PluginBrowserComponent even though ConcurrentScanner
    // itself is still perfectly alive.
    if (concurrentScanner != nullptr)
    {
        concurrentScanner->cancel();
        concurrentScanner->onProgress = nullptr;
        concurrentScanner->onFinished = nullptr;
    }
}

void PluginBrowserComponent::startParallelScan()
{
    if (concurrentScanner != nullptr && concurrentScanner->isScanning())
        return;

    concurrentScanner = std::make_shared<ConcurrentScanner> (
        workflow.getEngine(), workflow.getEngine().getPluginManager().knownPluginList);

    concurrentScanner->onProgress = [this] (int completed, int total)
    {
        scanStatusLabel.setText ("Scanning: " + juce::String (completed) + " / " + juce::String (total),
                                  juce::dontSendNotification);
    };

    concurrentScanner->onFinished = [this]
    {
        scanStatusLabel.setText ("Scan complete.", juce::dontSendNotification);
    };

    scanStatusLabel.setText ("Scanning...", juce::dontSendNotification);
    concurrentScanner->startScan();
}

void PluginBrowserComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& table = pluginListComponent.getTableListBox();
    const auto localPos = e.getEventRelativeTo (&table).getPosition();
    const auto row = table.getRowContainingPosition (localPos.x, localPos.y);

    if (row < 0)
        return;

    table.selectRow (row);
    loadSelectedRow();
}

void PluginBrowserComponent::loadSelectedRow()
{
    const auto row = pluginListComponent.getTableListBox().getSelectedRow();

    if (row < 0)
        return;

    auto& knownPluginList = workflow.getEngine().getPluginManager().knownPluginList;
    const auto types = knownPluginList.getTypes();

    if (row < types.size())
        workflow.loadPluginToSelectedTrack (types.getReference (row));
}

void PluginBrowserComponent::resized()
{
    auto area = getLocalBounds();
    loadButton.setBounds (area.removeFromBottom (36).reduced (8, 4));

    auto scanRow = area.removeFromBottom (36).reduced (8, 4);
    parallelScanButton.setBounds (scanRow.removeFromLeft (220));
    scanStatusLabel.setBounds (scanRow.withTrimmedLeft (8));

    pluginListComponent.setBounds (area);
}
