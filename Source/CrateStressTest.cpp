#include "CrateStressTest.h"
#include "Engine/CrateAnticipativeWrapper.h"

#if JUCE_DEBUG

namespace
{
    namespace te = tracktion::engine;
    namespace tcore = tracktion::core;

    constexpr int numTracks       = 100;
    constexpr int clipsPerTrack   = 4;
    constexpr double clipLengthS  = 4.0;
    constexpr int automationPoints = 8;
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
    const bool qaPassed  = tracksOk && pluginsOk && innerOk && clipsOk;

    const auto cpuPercent = edit.engine.getDeviceManager().getCpuUsage() * 100.0f;

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

#endif // JUCE_DEBUG
