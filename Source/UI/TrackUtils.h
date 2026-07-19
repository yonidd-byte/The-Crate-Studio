#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <vector>

namespace te = tracktion::engine;

/**
    Hybrid Bus/Return Architecture — the single, canonical definition of what
    counts as an "FX Return Track". Tracktion Engine has no special subclass
    for one: it's just a plain te::AudioTrack that happens to host a
    te::AuxReturnPlugin (see CrateWorkflowManager::createAndRouteNewFXChannel(),
    the only place one is ever created). Every place that needs to tell
    regular tracks and return tracks apart — MixerComponent's return-strip
    dock, ArrangementComponent's return-track dock, SendBusUtils' menu-name
    lookup — reads it from HERE, so none of them can ever quietly disagree on
    the definition.
*/
namespace TrackUtils
{
    inline bool isReturnTrack (te::AudioTrack& t)
    {
        return t.pluginList.findFirstPluginOfType<te::AuxReturnPlugin>() != nullptr;
    }

    struct TrackSplit
    {
        std::vector<te::AudioTrack*> regularTracks;
        std::vector<te::AudioTrack*> returnTracks; // in te::getAudioTracks() order — NOT sorted by bus number
    };

    /** Partitions every te::AudioTrack in `edit` into regular vs. return,
        preserving te::getAudioTracks()'s own relative order within each
        group. Callers needing regularTracks/returnTracks 1:1 rebuilt into
        parallel UI lists (MixerComponent's two strip groups,
        ArrangementComponent's scrollable rows vs. docked return rows) should
        call this ONCE and hand both vectors to their respective rebuild()s,
        so a track can never appear in both places or neither. */
    inline TrackSplit splitTracks (te::Edit& edit)
    {
        TrackSplit result;

        for (auto* t : te::getAudioTracks (edit))
            (isReturnTrack (*t) ? result.returnTracks : result.regularTracks).push_back (t);

        return result;
    }

    /** The return track currently routed to `busNumber` (if any) — used to
        show a real descriptive name in the Sends "+" menu ("Bus 1 (Vocal
        Reverb)") now that createAndRouteNewFXChannel() actually creates named
        return tracks. Returns nullptr if no return track claims that bus
        (e.g. a bus created before this pass existed, or one whose return
        track was since deleted). */
    inline te::AudioTrack* findReturnTrackForBus (te::Edit& edit, int busNumber)
    {
        for (auto* t : te::getAudioTracks (edit))
            if (auto* ret = t->pluginList.findFirstPluginOfType<te::AuxReturnPlugin>())
                if (ret->busNumber == busNumber)
                    return t;

        return nullptr;
    }
}
