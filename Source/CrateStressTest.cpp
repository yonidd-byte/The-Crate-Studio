#include "CrateStressTest.h"
#include "Engine/CrateAnticipativeWrapper.h"
#include "Engine/CrateSandboxBridge.h"
#include "Engine/SandboxManager.h"

#include <functional>
#include <memory>
#include <vector>
#include <thread>

#if JUCE_DEBUG

namespace
{
    namespace te = tracktion::engine;
    namespace tcore = tracktion::core;

    constexpr int numTracks       = 100;
    constexpr int clipsPerTrack   = 4;
    constexpr double clipLengthS  = 4.0;
    constexpr int automationPoints = 8;

    // Step 9 (The Multi-Process Scalability Stress Test) directive.
    constexpr int scaleTestPluginCount           = 50;
    constexpr int scaleTestConnectPollIntervalMs = 250;
    constexpr int scaleTestConnectMaxAttempts    = 60;    // 15s worth of polling — 50 CONCURRENT VST3 loads is heavier than one
    constexpr int scaleTestSustainedWindowMs     = 10000; // per directive: "sustained 10-second window"

    // Step 19 (Tracktion Engine Lookahead Integration) directive: a real,
    // non-silent audio clip for the Time-Slip Engine to actually render
    // via te::Renderer::renderToFile(). Without this, ensureRawTrackAudioAvailable()
    // would legitimately render silence — a CORRECT result (there's
    // genuinely nothing on an empty track), but a useless one for
    // verifying real clip content actually reaches the pipeline. 6
    // seconds — comfortably longer than the 4-second Time-Slip target,
    // with margin for the test's own read/verify window.
    juce::File createTimeSlipTestClipFile()
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("CrateTimeSlipTestClip.wav");
        file.deleteFile();

        constexpr double durationSeconds = 6.0;
        constexpr double sampleRate = 44100.0;
        const int numSamples = (int) (durationSeconds * sampleRate);

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (new juce::FileOutputStream (file),
                                                                                     sampleRate, 2, 16, {}, 0));

        if (writer != nullptr)
        {
            juce::AudioBuffer<float> buffer (2, numSamples);

            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);

                for (int i = 0; i < numSamples; ++i)
                    data[i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) i / sampleRate);
            }

            writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
        }

        return file;
    }
}

void CrateStressTest::runExtremeLoadTest (te::Edit& edit)
{
    const auto startMs = juce::Time::getMillisecondCounter();

    // Backend Logic QA / Undo Test directive: the ENTIRE 100-track
    // generation is one undo transaction — if the undo system chokes on a
    // massive single-transaction state mutation, this is exactly the kind
    // of load that should surface it, not a real user hitting it mid-session.
    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("QA Stress Test: Generate Extreme Load Session");

    int tracksCreated       = 0;
    int pluginsLoaded       = 0;
    int innerPluginsWired   = 0;
    int clipsCreated        = 0;

    for (int i = 0; i < numTracks; ++i)
    {
        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

        if (track == nullptr)
        {
            jassertfalse; // QA FAIL: track creation itself failed mid-loop
            continue;
        }

        ++tracksCreated;

        // Anticipative FX Step 2 Verification directive: the plugin actually
        // inserted onto the track is the CrateAnticipativeWrapper PROXY, not
        // a bare EQ/Compressor — the real DSP lives behind it as
        // innerPlugin, so this stress test exercises the synchronous
        // forwarding path (initialise/applyToBuffer/state) under 100-track
        // load, not just a hand-picked single-plugin unit test. Alternating
        // EQ/Compressor as the inner type still exercises both built-in DSP
        // paths across the multi-core distribution (CrateEngineBehaviour::
        // getNumberOfCPUsToUseForAudio()).
        if (auto wrapperPlugin = edit.getPluginCache().createNewPlugin (CrateAnticipativeWrapper::xmlTypeName, juce::PluginDescription()))
        {
            track->pluginList.insertPlugin (wrapperPlugin, -1, nullptr);
            ++pluginsLoaded;

            if (auto* wrapper = dynamic_cast<CrateAnticipativeWrapper*> (wrapperPlugin.get()))
            {
                const auto innerType = (i % 2 == 0) ? te::EqualiserPlugin::xmlTypeName
                                                     : te::CompressorPlugin::xmlTypeName;

                if (auto innerPlugin = edit.getPluginCache().createNewPlugin (innerType, juce::PluginDescription()))
                {
                    wrapper->setInnerPlugin (innerPlugin);

                    // Verify the wiring actually took (getInnerPlugin() reads
                    // back the SAME pointer just assigned) rather than
                    // trusting setInnerPlugin() silently succeeded.
                    if (wrapper->getInnerPlugin() == innerPlugin)
                        ++innerPluginsWired;
                }
            }
        }

        // Dense dummy clips — MIDI (no source audio file dependency, so this
        // never touches disk I/O) purely to force the Arrangement into
        // rendering a heavily populated timeline: OpenGL compositing load
        // and setBufferedToImage cache-invalidation load, both at once.
        for (int c = 0; c < clipsPerTrack; ++c)
        {
            const auto start = (double) c * clipLengthS;
            const tcore::TimeRange range (tcore::TimePosition::fromSeconds (start),
                                           tcore::TimePosition::fromSeconds (start + clipLengthS));

            if (track->insertNewClip (te::TrackItem::Type::midi, range, nullptr) != nullptr)
                ++clipsCreated;
        }

        // Aggressive volume automation — GUI<->audio-thread sync stress: a
        // real multi-point curve for AutomationLaneComponent to draw AND the
        // audio thread to read every block, not a single static value.
        if (auto volumePlugin = track->getVolumePlugin())
        {
            auto& curve = volumePlugin->volParam->getCurve();
            curve.clear (&um);

            for (int p = 0; p < automationPoints; ++p)
            {
                const auto t = tcore::TimePosition::fromSeconds ((double) p * 2.0);
                const float v = (p % 2 == 0) ? -6.0f : 0.0f;
                curve.addPoint (t, v, 0.0f, &um);
            }
        }
    }

    const auto elapsedMs = juce::Time::getMillisecondCounter() - startMs;

    // Backend Logic QA directive: verify the mutation actually landed —
    // assert on COUNTS read back from what was created, not just that the
    // loop above ran to completion without an exception.
    const bool tracksOk  = (tracksCreated     == numTracks);
    const bool pluginsOk = (pluginsLoaded     == numTracks);
    const bool innerOk   = (innerPluginsWired == numTracks);
    const bool clipsOk   = (clipsCreated      == numTracks * clipsPerTrack);

    const auto cpuPercent = edit.engine.getDeviceManager().getCpuUsage() * 100.0f;

    // Plugin Sandboxing Step 5.5 Verification directive: prove the process-
    // lifecycle/heartbeat bridge out — insert ONE CrateSandboxBridge onto its
    // OWN dedicated track (not part of the 100-track loop: Step 5.5 built one
    // shared control block, not a per-plugin one, so there's nothing to gain
    // by wiring 100 of these the way the Anticipative FX wrapper is wired).
    // Inserting it (rather than just constructing it) is what actually fires
    // te::Plugin::initialise() through TE's normal graph-build lifecycle,
    // which is what launches CrateSandbox.exe and starts the liveness timer
    // — a bare createNewPlugin() with no insertPlugin() would never call
    // initialise() at all, and this check would be silently vacuous.
    // isConnected() read immediately after insertion is NOT a meaningful
    // pass/fail signal (the child process needs time to launch, create the
    // shared file, and send its first heartbeat) — the live pass/fail signal
    // for THIS step is CrateSandboxBridge.log in the OS temp directory,
    // watched externally while the app keeps running.
    bool sandboxBridgeInserted = false;
    if (auto sandboxTrack = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr))
    {
        // Step 19 directive: give the Time-Slip Engine real upstream
        // content to render, BEFORE the bridge plugin is even inserted —
        // see createTimeSlipTestClipFile()'s own doc comment.
        sandboxTrack->insertWaveClip ("Crate Time-Slip Test Clip", createTimeSlipTestClipFile(),
                                      te::ClipPosition { tcore::TimeRange (tcore::TimePosition::fromSeconds (0.0),
                                                                            tcore::TimePosition::fromSeconds (6.0)) },
                                      false);

        if (auto bridgePlugin = SandboxManager::getInstance().createSandboxPlugin (edit, CrateSandboxBridge::getTestPluginPath()))
        {
            sandboxTrack->pluginList.insertPlugin (bridgePlugin, -1, nullptr);
            sandboxBridgeInserted = (dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()) != nullptr);

            // Echo/Phase Test Verification directive (Step 6): isConnected()
            // is never meaningful immediately after insertion — the child
            // needs real wall-clock time to launch and send its first
            // heartbeat (see the comment above), and that time isn't fixed
            // (a busier machine, a slower launch, can all push it past any
            // single guessed delay). Rather than one fragile deferred call,
            // poll every 250ms (up to 20 tries / 5 seconds) until
            // isConnected() actually reports true, THEN run the echo test —
            // the te::Plugin::Ptr is captured BY VALUE in the shared
            // std::function (ref-counted) so the instance stays alive across
            // every retry regardless of what happens to the track meanwhile.
            // The std::function stored INSIDE pollForEchoTest captures a
            // WEAK_PTR to ITSELF, not a shared_ptr — capturing the owning
            // shared_ptr by value there would be a permanent self-reference
            // cycle (the object would hold a strong reference to itself, so
            // its refcount could never reach zero), a real leak. Each
            // SCHEDULED retry instead promotes that weak_ptr to a fresh
            // strong one right when callAfterDelay() is called — that's an
            // ordinary EXTERNAL reference (owned by that one scheduled
            // callback, not by the object's own stored function), so it
            // keeps the chain alive exactly until the next callback fires,
            // with no cycle.
            auto pollForEchoTest = std::make_shared<std::function<void (int)>> ();
            std::weak_ptr<std::function<void (int)>> weakPoll = pollForEchoTest;
            *pollForEchoTest = [bridgePlugin, weakPoll] (int attemptsLeft)
            {
                auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get());

                if (bridge == nullptr)
                    return;

                if (! bridge->isConnected())
                {
                    if (attemptsLeft > 0)
                        if (auto strongPoll = weakPoll.lock())
                            juce::Timer::callAfterDelay (250, [strongPoll, attemptsLeft] { (*strongPoll) (attemptsLeft - 1); });

                    return;
                }

                // Step 7 directive: isConnected() only proves the CHILD's
                // heartbeat is pulsing — as of Step 7 that starts BEFORE the
                // (potentially slow, real-DLL-load) VST3 finishes loading,
                // by design (see CrateSandbox's own comment on the restart
                // livelock this fixed). Running the round-trip test the
                // INSTANT heartbeat confirms risks catching the plugin
                // mid-load, reporting a false "unchanged/no plugin" verdict
                // that isn't actually true a moment later. A flat extra
                // delay here — separate from the connection-polling above —
                // gives the load time to finish first.
                juce::Timer::callAfterDelay (1500, [bridgePlugin]
                {
                    if (auto* bridge2 = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
                    {
                        double roundTripMs = 0.0;
                        bridge2->runEchoPhaseTest (roundTripMs); // logs its own PASS/FAIL + timing to CrateSandboxBridge.log

                        // Step 18 (The Time-Slip Engine) directive: SUPERSEDES
                        // the Step 17 runLookaheadPipelineTest() call that used
                        // to run here — that test asserted phase-inversion,
                        // which was correct for Step 17's own deliberate dummy
                        // transform, but LookaheadWorkerThread now runs the
                        // REAL VST3 (a filter, not a phase inverter), so that
                        // specific assertion is obsolete, not broken. This
                        // test verifies the real-DSP successor instead: real
                        // VST3 audio actually reaching the TimeSlipBuffer,
                        // the Parent's audio-thread read path consuming it,
                        // and Flush-on-Change correctly resetting the
                        // pipeline. Logs its own PASS/FAIL + timing to
                        // CrateSandboxBridge.log.
                        //
                        // MUST run off the message thread. This whole
                        // callback fires ON the message thread (that's how
                        // juce::Timer::callAfterDelay works), and
                        // runTimeSlipEngineTest()'s body busy-polls with a
                        // raw sleep loop waiting for the buffer to fill.
                        // Real track-audio rendering (te::Renderer::
                        // renderToFile(), invoked from
                        // LookaheadProducerThread) internally depends on the
                        // message thread being free to pump its own
                        // callBlocking()/triggerAsyncUpdate() handoff — so
                        // polling HERE would starve the very thing it's
                        // waiting on, deadlocking the render for the whole
                        // test timeout. Launching a background thread for
                        // the test body keeps the message thread free to
                        // service that handoff.
                        juce::Thread::launch ([bridgePlugin]
                        {
                            if (auto* bridge3 = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
                            {
                                double bufferedSeconds = 0.0;
                                bridge3->runTimeSlipEngineTest (bufferedSeconds);
                            }
                        });
                    }
                });
            };

            juce::Timer::callAfterDelay (250, [pollForEchoTest] { (*pollForEchoTest) (19); });
        }
    }

    const bool qaPassed = tracksOk && pluginsOk && innerOk && clipsOk && sandboxBridgeInserted;

    juce::String report;
    report << "\n========================================\n"
           << "   CRATE STRESS TEST - PERFORMANCE REPORT\n"
           << "========================================\n"
           << "Setup time:       " << (int) elapsedMs << " ms\n"
           << "Tracks created:   " << tracksCreated << " / " << numTracks << "\n"
           << "Plugins loaded:   " << pluginsLoaded << " / " << numTracks << "\n"
           << "Inner FX wired:   " << innerPluginsWired << " / " << numTracks << "\n"
           << "Clips created:    " << clipsCreated  << " / " << (numTracks * clipsPerTrack) << "\n"
           << "Engine CPU usage: " << juce::String (cpuPercent, 1) << " %\n"
           << "Sandbox bridge:   " << (sandboxBridgeInserted ? "inserted live (see %TEMP%/CrateSandboxBridge.log for heartbeat status)"
                                                              : "FAILED to insert") << "\n"
           << "Backend QA:       " << (qaPassed ? "PASS" : "FAIL") << "\n"
           << "========================================\n";

    DBG (report);
    juce::Logger::writeToLog (report);

    // Inescapable Report directive: DBG()/Logger both write via
    // OutputDebugString, which is invisible with no debugger attached (the
    // exact gap that prompted this) — a native OS message box needs no
    // debugger, no log file, no IDE integration, just the exe running.
    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Stress Test Report", report);

    jassert (qaPassed); // QA FAIL: trip the debugger immediately if backend integrity didn't hold
}

void CrateStressTest::runSandboxScaleTest (te::Edit& edit)
{
    const auto startMs = juce::Time::getMillisecondCounter();

    // Step 9.1 (Scatter-Gather & Yield Refactor) directive — a DELIBERATE
    // change from Step 9's own approach: these 50 bridges are constructed
    // via the Edit's own plugin cache (still required for te::Plugin's
    // internal ValueTree machinery) but NEVER inserted into any track's
    // pluginList. Step 9 DID insert onto 50 real tracks, which meant the
    // REAL Edit graph's own audio thread was ALSO calling applyToBuffer()
    // on all 50 continuously in the background — a genuine, unavoidable
    // conflict with this test's OWN scatter-then-gather driving of the
    // exact same instances: parentReady/childProcessed is a SINGLE-
    // PRODUCER protocol by design (see CrateIPC::ControlBlock's own doc
    // comment), and two independent callers (the real engine's audio
    // thread AND this test's own driver thread) racing to dispatch/gather
    // the SAME instance concurrently would violate that invariant, not just
    // risk a data race on bookkeeping fields. te::PluginInitialisationInfo
    // is a trivial three-field aggregate (startTime/sampleRate/
    // blockSizeSamples) with no live-graph dependency, so initialise() can
    // be called DIRECTLY, exactly like runEchoPhaseTest() already calls
    // applyToBuffer() directly — this test is now the SOLE, EXCLUSIVE
    // driver of these 50 instances' dispatch/gather cycles, start to finish.
    auto bridges = std::make_shared<std::vector<te::Plugin::Ptr>> ();
    bridges->reserve (scaleTestPluginCount);

    const te::PluginInitialisationInfo initInfo { tcore::TimePosition::fromSeconds (0.0), 44100.0, 512 };

    for (int i = 0; i < scaleTestPluginCount; ++i)
    {
        if (auto bridgePlugin = SandboxManager::getInstance().createSandboxPlugin (edit, CrateSandboxBridge::getTestPluginPath()))
        {
            if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get()))
            {
                bridge->initialise (initInfo); // launches THIS instance's own CrateSandbox.exe — see comment above
                bridges->push_back (bridgePlugin);
            }
        }
    }

    const int bridgesInserted = (int) bridges->size();
    const auto setupElapsedMs = juce::Time::getMillisecondCounter() - startMs;

    // Same weak_ptr self-reference-avoiding poller pattern the single-
    // instance echo test already established (see runExtremeLoadTest's own
    // comment for why), generalized here to ALL 50 at once: poll until
    // EVERY bridge reports isConnected(), or give up after
    // scaleTestConnectMaxAttempts and report the truth about how many
    // actually made it rather than hanging forever. isConnected() itself
    // still works correctly with no track membership at all — the health-
    // check juce::Timer inside CrateSandboxBridge fires on JUCE's own
    // global timer queue regardless, driven by the object's own lifetime,
    // not by graph participation.
    auto pollForAllConnected = std::make_shared<std::function<void (int)>> ();
    std::weak_ptr<std::function<void (int)>> weakPoll = pollForAllConnected;

    *pollForAllConnected = [bridges, weakPoll, bridgesInserted, setupElapsedMs] (int attemptsLeft)
    {
        int connectedCount = 0;
        for (auto& p : *bridges)
            if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (p.get()))
                if (bridge->isConnected())
                    ++connectedCount;

        const bool allConnected = (connectedCount == (int) bridges->size());

        if (! allConnected && attemptsLeft > 0)
        {
            if (auto strongPoll = weakPoll.lock())
                juce::Timer::callAfterDelay (scaleTestConnectPollIntervalMs,
                                             [strongPoll, attemptsLeft] { (*strongPoll) (attemptsLeft - 1); });
            return;
        }

        // Either every bridge is alive, or attempts ran out — proceed to
        // the sustained measurement window regardless, and report the
        // truth either way. Reset EVERY bridge's max-round-trip tracker
        // right here so the sustained window below measures ONLY what
        // happens during that window, not anything from the connection
        // ramp-up (50 concurrent VST3 loads is real, heavy contention that
        // shouldn't count against the steady-state number).
        for (auto& p : *bridges)
            if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (p.get()))
                bridge->resetMaxRoundTripMsObserved();

        // Scatter-Gather Dispatch directive (Step 9.1): the sustained
        // measurement itself runs on a DEDICATED background std::thread,
        // not the message thread — a real 10-second window would otherwise
        // freeze the entire UI event loop, and precise back-to-back
        // scatter/gather cycling needs tight timing control a juce::Timer
        // callback chain can't cleanly provide. juce::MessageManager::
        // callAsync() marshals the FINAL report back onto the message
        // thread once the window ends, since NativeMessageBox and file I/O
        // for the report should happen from the same thread this whole
        // test started on, matching this codebase's convention elsewhere.
        std::thread ([bridges, bridgesInserted, setupElapsedMs]
        {
            const int n = (int) bridges->size();
            std::vector<juce::AudioBuffer<float>> testBuffers ((size_t) n);
            std::vector<te::MidiMessageArray> midiBuffers ((size_t) n);
            std::vector<bool> dispatched ((size_t) n, false);

            for (auto& buf : testBuffers)
                buf.setSize (2, 512);

            const tcore::TimeRange editTime (tcore::TimePosition::fromSeconds (0.0), tcore::TimePosition::fromSeconds (0.01));

            const auto windowStartMs = juce::Time::getMillisecondCounterHiRes();
            const auto windowDeadlineMs = windowStartMs + (double) scaleTestSustainedWindowMs;
            int64_t cyclesCompleted = 0;

            while (juce::Time::getMillisecondCounterHiRes() < windowDeadlineMs)
            {
                // Phase 1 — SCATTER: fire every instance's dispatch first,
                // waiting on NOTHING, so all 50 children get to start
                // working concurrently across whatever cores are free.
                for (int i = 0; i < n; ++i)
                {
                    if (auto* bridge = dynamic_cast<CrateSandboxBridge*> ((*bridges)[i].get()))
                    {
                        te::PluginRenderContext ctx (&testBuffers[(size_t) i], juce::AudioChannelSet::stereo(), 0, 512,
                                                     &midiBuffers[(size_t) i], 0.0, editTime, false, false, false, true);
                        dispatched[(size_t) i] = bridge->dispatchToSandbox (ctx);
                    }
                }

                // Phase 2 — GATHER: only NOW does any instance actually
                // wait for its own child — by the time we reach instance
                // 49 here, its child has had the ENTIRE scatter pass plus
                // 48 other gathers' worth of head start to finish.
                for (int i = 0; i < n; ++i)
                {
                    if (auto* bridge = dynamic_cast<CrateSandboxBridge*> ((*bridges)[i].get()))
                    {
                        te::PluginRenderContext ctx (&testBuffers[(size_t) i], juce::AudioChannelSet::stereo(), 0, 512,
                                                     &midiBuffers[(size_t) i], 0.0, editTime, false, false, false, true);
                        bridge->gatherFromSandbox (ctx, dispatched[(size_t) i]);
                    }
                }

                ++cyclesCompleted;
            }

            juce::MessageManager::callAsync ([bridges, bridgesInserted, setupElapsedMs, cyclesCompleted]
            {
                int stillConnected = 0;
                double worstRoundTripMs = 0.0;
                juce::String perInstanceLog;

                for (int i = 0; i < (int) bridges->size(); ++i)
                {
                    auto* bridge = dynamic_cast<CrateSandboxBridge*> ((*bridges)[i].get());

                    if (bridge == nullptr)
                        continue;

                    const bool connected = bridge->isConnected();
                    const double maxRt = bridge->getMaxRoundTripMsObserved();

                    if (connected)
                        ++stillConnected;

                    worstRoundTripMs = juce::jmax (worstRoundTripMs, maxRt);

                    perInstanceLog << "Instance " << i << ": " << (connected ? "CONNECTED" : "not connected")
                                   << ", maxRoundTrip=" << juce::String (maxRt, 3) << "ms\n";
                }

                // Per-instance detail goes to a file, not the message box —
                // a 50-line report is unwieldy for a dialog, same reasoning
                // CrateSandboxBridge.log already established for verbose
                // diagnostics elsewhere in this codebase.
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("CrateSandboxScaleTest.log")
                    .replaceWithText (perInstanceLog);

                const bool qaPassed = (bridgesInserted == scaleTestPluginCount) && (stillConnected == scaleTestPluginCount);

                juce::String report;
                report << "\n========================================\n"
                       << "  SANDBOX SCALE TEST - 50 PLUGIN REPORT\n"
                       << "  (Step 9.1: Scatter-Gather + Yield Refactor)\n"
                       << "========================================\n"
                       << "Setup time:          " << (int) setupElapsedMs << " ms\n"
                       << "Bridges initialised: " << bridgesInserted << " / " << scaleTestPluginCount << "\n"
                       << "Connected (final):   " << stillConnected << " / " << scaleTestPluginCount << "\n"
                       << "Scatter/gather cycles completed: " << (int) cyclesCompleted << " in "
                                                               << scaleTestSustainedWindowMs << " ms\n"
                       << "Worst round trip:    " << juce::String (worstRoundTripMs, 3) << " ms (cap "
                                                   << CrateIPC::spinWaitTimeoutMs << " ms)\n"
                       << "Per-instance detail: %TEMP%/CrateSandboxScaleTest.log\n"
                       << "Backend QA:          " << (qaPassed ? "PASS" : "FAIL") << "\n"
                       << "========================================\n";

                DBG (report);
                juce::Logger::writeToLog (report);

                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Sandbox Scale Test Report", report);

                // bridges (the shared_ptr<vector<Plugin::Ptr>>) drops out of
                // scope at the end of this lambda — every te::Plugin::Ptr's
                // refcount then reaches zero, running each CrateSandboxBridge's
                // destructor, which kills its own CrateSandbox.exe. All 50
                // children are torn down cleanly here, not left orphaned.
            });
        }).detach();
    };

    juce::Timer::callAfterDelay (scaleTestConnectPollIntervalMs,
                                 [pollForAllConnected] { (*pollForAllConnected) (scaleTestConnectMaxAttempts); });
}

namespace
{
    // Step 74 (The Flight Recorder) directive, Task 2: hardcoded to the
    // SAME real, heavy third-party plugin every live reproduction this
    // whole session used — a synthetic tone-generator test double has no
    // heavy internal recalculation to stress in the first place, and
    // reproducing the Airlock/IPC deadlock requires a genuinely heavy
    // third-party UI reacting to real parameter automation. Machine-local
    // by nature (a dev-only harness, never shipped — see this file's own
    // JUCE_DEBUG guard); skips cleanly if this exact path doesn't exist on
    // whatever machine runs it.
    const juce::String airlockStressPluginPath =
        "C:\\Program Files\\Common Files\\VST3\\MeldaProduction\\EQ\\MAutoDynamicEq.vst3";

    constexpr int airlockStressWindowMs        = 10000; // per directive: "10-second window"
    constexpr int airlockStressTickMs          = 15;    // faster than any real human drag — deliberately worse than the real-world reproduction
}

// Step 74 (The Flight Recorder) directive, Task 2: the actual 10-second
// flood — a real parameter burst (genuine AutomatableParameter::
// setParameter() calls, which route through CrateSyncedParameter's own
// parameterChanged() override into the real bridge.setParameterEvent()
// IPC push, exactly the production path) alongside rapid display-scale
// toggling (the one geometry-disturbance channel still available to the
// HOST side under the current Pure Follower architecture — Step 65 made
// all REAL resizing exclusively CHILD-driven, so this is the closest
// re-creatable proxy for "the Host repeatedly asks the reparented HWND to
// react to a geometry-adjacent change while the plugin's own UI is under
// heavy load"). Self-perpetuating via juce::Timer::callAfterDelay, same
// pattern as this file's own polling helpers above — never a blocking
// loop on the message thread, which would defeat the entire point of a
// test whose PASS condition is "the message thread never seizes up."
void CrateStressTest::runAirlockDeadlockStressTickInternal (te::Plugin::Ptr bridgePlugin, double startMs,
                                                             std::shared_ptr<std::function<void()>> selfHolder)
{
    auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get());

    if (bridge == nullptr)
        return; // torn down mid-test — stop rescheduling

    const auto elapsedMs = juce::Time::getMillisecondCounter() - startMs;

    if (elapsedMs > (double) airlockStressWindowMs)
    {
        const juce::String report = "\nCrateStressTest: Airlock Deadlock Stress Test — "
                                     + juce::String (airlockStressWindowMs)
                                     + "ms flood window complete. If this message is visible, the "
                                       "message thread survived the whole run without seizing up.\n";
        DBG (report);
        juce::Logger::writeToLog (report);
        return; // stop rescheduling — test ends; bridgePlugin's refcount drops when this lambda chain unwinds
    }

    // (a) Real parameter flood.
    auto params = bridgePlugin->getAutomatableParameters();

    if (! params.isEmpty())
    {
        juce::Random random;

        // Several params per tick, not just one — matches "flood," not
        // "trickle." dontSendNotification: this test cares about the real
        // IPC/DSP/UI path (setParameter() -> parameterChanged() override
        // -> bridge.setParameterEvent()), not redundant plain-JUCE UI
        // repaint notifications on top of it.
        for (int i = 0; i < 8; ++i)
        {
            auto* param = params[random.nextInt (params.size())];

            if (param != nullptr)
                param->setParameter (random.nextFloat(), juce::dontSendNotification);
        }
    }

    // (b) Rapid display-scale toggling — see this function's own doc
    // comment for why this stands in for a direct forced resize under the
    // current Pure Follower architecture.
    bridge->publishDisplayScaleFactor (((int) (elapsedMs / (double) airlockStressTickMs) % 2 == 0) ? 1.0f : 1.25f);

    juce::Timer::callAfterDelay (airlockStressTickMs, [selfHolder] { (*selfHolder)(); });
}

void CrateStressTest::runAirlockDeadlockStressTest (te::Edit& edit)
{
    if (! juce::File (airlockStressPluginPath).existsAsFile())
    {
        DBG ("CrateStressTest::runAirlockDeadlockStressTest — plugin not found at "
                 + airlockStressPluginPath + " on this machine, skipping.");
        return;
    }

    auto stressTrack = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

    if (stressTrack == nullptr)
    {
        DBG ("CrateStressTest::runAirlockDeadlockStressTest — could not create a track, aborting.");
        return;
    }

    auto bridgePlugin = SandboxManager::getInstance().createSandboxPlugin (edit, airlockStressPluginPath);

    if (bridgePlugin == nullptr)
    {
        DBG ("CrateStressTest::runAirlockDeadlockStressTest — createSandboxPlugin() returned null, aborting.");
        return;
    }

    stressTrack->pluginList.insertPlugin (bridgePlugin, -1, nullptr);

    // Same connection-polling idiom as runSandboxScaleTest's own echo test
    // above — the child needs real wall-clock time to launch and load a
    // genuinely heavy third-party DLL.
    auto pollForReady = std::make_shared<std::function<void (int)>> ();
    std::weak_ptr<std::function<void (int)>> weakPoll = pollForReady;

    *pollForReady = [bridgePlugin, weakPoll] (int attemptsLeft)
    {
        auto* bridge = dynamic_cast<CrateSandboxBridge*> (bridgePlugin.get());

        if (bridge == nullptr)
            return;

        if (! bridge->isConnected())
        {
            if (attemptsLeft > 0)
                if (auto strongPoll = weakPoll.lock())
                    juce::Timer::callAfterDelay (250, [strongPoll, attemptsLeft] { (*strongPoll) (attemptsLeft - 1); });

            return;
        }

        DBG ("CrateStressTest: Airlock Deadlock Stress Test — plugin connected; opening its real editor "
             "(exercises SandboxAirlock's own createSlot()/reparenting path) and starting the flood.");

        // Real editor window — the point is exercising the Airlock's
        // actual reparenting path, not merely constructing the bridge.
        bridgePlugin->showWindowExplicitly();

        const auto startMs = juce::Time::getMillisecondCounter();
        auto selfHolder = std::make_shared<std::function<void()>>();
        te::Plugin::Ptr pluginForFlood = bridgePlugin;

        *selfHolder = [pluginForFlood, startMs, selfHolder]
        {
            CrateStressTest::runAirlockDeadlockStressTickInternal (pluginForFlood, startMs, selfHolder);
        };

        (*selfHolder)();
    };

    (*pollForReady) (60); // 60 * 250ms = 15s connection budget — a real heavy DLL load can be slow
}

#endif // JUCE_DEBUG
