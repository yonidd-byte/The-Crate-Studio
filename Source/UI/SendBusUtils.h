#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <set>

namespace te = tracktion::engine;

/**
    Dynamic Sends Routing — the bus-enumeration logic behind the "+" menu's
    "Add Send to Bus N" / "Create New Bus..." choices. Shared by MixerStrip
    and CrateTrackInspectorComponent (both used to carry their OWN separately-
    written copy of this scan) — a bug where Inspector's menu failed to list
    existing buses turned out to have no actual logic divergence between the
    two copies under inspection, but living in two places at all meant there
    was no GUARANTEE they'd stay identical the next time either one changed.
    One function, one source of truth, used by both call sites now.
*/
namespace SendBusUtils
{
    struct BusScan
    {
        std::set<int> busesInUseAnywhere;      // every bus number ANY track in the edit currently sends to
        std::set<int> busesThisTrackAlreadyUses; // the subset `thisTrack` itself already sends to
    };

    /** Scans every te::AudioTrack in `edit` for te::AuxSendPlugin instances.
        `thisTrack` (may be nullptr) is compared by address against each
        scanned track to populate busesThisTrackAlreadyUses. */
    inline BusScan scanBuses (te::Edit& edit, const te::Track* thisTrack)
    {
        BusScan result;

        for (auto* t : te::getAudioTracks (edit))
            for (auto* p : t->pluginList)
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                {
                    result.busesInUseAnywhere.insert (send->getBusNumber());
                    if (t == thisTrack)
                        result.busesThisTrackAlreadyUses.insert (send->getBusNumber());
                }

        return result;
    }
}
