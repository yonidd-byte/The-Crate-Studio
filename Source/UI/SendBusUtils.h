#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "TrackUtils.h"
#include <set>
#include <map>

namespace te = tracktion::engine;

/**
    Dynamic Sends Routing — the bus-enumeration logic behind the "+" menu's
    bus list / "+ Create New FX Channel" choices. Shared by MixerStrip
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

        // Send-to-Send feedback prevention: the bus number `thisTrack` ITSELF
        // returns FROM, if it is a return track (TrackUtils::isReturnTrack())
        // — buildSendMenu() must never let a return track send to its own
        // bus (Return A -> Bus 1 -> Return A is an instant feedback loop).
        // -1 means thisTrack isn't a return track (or is nullptr).
        int ownReturnBusNumber = -1;
    };

    /** Scans every te::AudioTrack in `edit` for te::AuxSendPlugin instances
        (and, for `thisTrack` specifically, its own te::AuxReturnPlugin, if
        any — see BusScan::ownReturnBusNumber above). `thisTrack` (may be
        nullptr) is compared by address against each scanned track to
        populate busesThisTrackAlreadyUses. */
    inline BusScan scanBuses (te::Edit& edit, const te::Track* thisTrack)
    {
        BusScan result;

        for (auto* t : te::getAudioTracks (edit))
            for (auto* p : t->pluginList)
            {
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                {
                    result.busesInUseAnywhere.insert (send->getBusNumber());
                    if (t == thisTrack)
                        result.busesThisTrackAlreadyUses.insert (send->getBusNumber());
                }
                else if (t == thisTrack)
                {
                    if (auto* ret = dynamic_cast<te::AuxReturnPlugin*> (p))
                        result.ownReturnBusNumber = ret->busNumber;
                }
            }

        return result;
    }

    /** Everything needed to interpret a click on the menu buildSendMenu()
        returns — the caller still owns showMenuAsync() + the actual routing
        action itself (createSendToBus() / CrateWorkflowManager::
        createAndRouteNewFXChannel()) since those differ per call site
        (MixerStrip's AudioTrack::Ptr vs. InspectorStrip's Track::Ptr,
        different post-action refresh calls). */
    struct SendMenuBuild
    {
        juce::PopupMenu menu;
        std::map<int, int> menuIdToBusNumber; // item ID -> bus number, existing-bus items only
        int createFXChannelItemId = -1;        // item ID for "+ Create New FX Channel"
    };

    /** Builds the "+" menu's contents from an already-computed BusScan (see
        scanBuses() above): every existing bus, formatted "Bus N (Return
        Track Name)" when a real return track claims that bus (via
        TrackUtils::findReturnTrackForBus() — createAndRouteNewFXChannel() is
        what creates those now), or plain "Bus N" for an older bus with no
        return track backing it (e.g. one created before this pass existed).
        Appends " (Already Sending)", disabled, if `thisTrack` already routes
        to it (Strict Sends Menu Logic / Grey-out-not-hide: every bus that
        exists ANYWHERE in the project is listed, never hidden, so the user
        gets visual confirmation the routing graph is intact) — then a
        separator, then "+ Create New FX Channel".

        Send-to-Send feedback prevention: `scan.ownReturnBusNumber` (the bus
        `thisTrack` itself returns from, if any) is skipped entirely — not
        just disabled like "Already Sending" — since sending a return track
        to its own bus isn't a valid-but-redundant choice the way re-sending
        to an already-used bus is, it's a literal feedback loop. */
    inline SendMenuBuild buildSendMenu (te::Edit& edit, const BusScan& scan)
    {
        SendMenuBuild result;
        int nextItemId = 1;

        for (int bus : scan.busesInUseAnywhere)
        {
            if (bus == scan.ownReturnBusNumber)
                continue;

            const bool alreadyRouted = scan.busesThisTrackAlreadyUses.count (bus) > 0;

            juce::String label = "Bus " + juce::String (bus);
            if (auto* returnTrack = TrackUtils::findReturnTrackForBus (edit, bus))
                label += " (" + returnTrack->getName() + ")";
            if (alreadyRouted)
                label += " (Already Sending)";

            result.menu.addItem (nextItemId, label, ! alreadyRouted);
            result.menuIdToBusNumber[nextItemId] = bus;
            ++nextItemId;
        }

        if (! result.menuIdToBusNumber.empty())
            result.menu.addSeparator();

        result.createFXChannelItemId = nextItemId;
        result.menu.addItem (result.createFXChannelItemId, "+ Create New FX Channel");

        return result;
    }
}
