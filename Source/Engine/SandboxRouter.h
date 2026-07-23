#pragma once

#include <juce_core/juce_core.h>
#include "PluginHealthRegistry.h"

/**
    Plugin Sandboxing Step 22 (The Profiling Database & Smart Confinement
    Routing / "The Warden") — the routing RULES, extracted out of
    SandboxManager so the policy itself is one single, independently
    readable place, separate from the IPC/dispatch machinery that acts on
    its verdict. SandboxManager::createSandboxPlugin() is still the actual
    interception point (called before edit.getPluginCache().createNewPlugin()/
    track->pluginList.insertPlugin() ever build the real bridge/IPC channel)
    — this class only decides, it never dispatches anything itself.

    STEP 36 (Optimistic Warden Routing / "Trust but Verify") — REPLACES the
    original Step 22 vendor-whitelist policy below. Disclosed policy
    reversal, not a silent drift:

      OLD (Step 22): crashCount == 0 AND vendor on a hardcoded trusted
      list -> Shared; anything else (including every plugin whose vendor
      simply hadn't been scanned yet) -> Solitary. This punished every
      unrecognised indie/boutique plugin with a full isolated process on
      EVERY load, forever, purely for being unfamiliar — real RAM cost on
      a heavy project full of legitimately safe but obscure plugins.

      NEW (Step 36): ANY plugin with crashCount == 0 goes to the Shared
      Sandbox, regardless of vendor. A plugin is ONLY forced into Solitary
      Confinement if it has ACTUALLY crashed on this machine before
      (crashCount > 0) or has been explicitly flagged unsafe in the
      registry (PluginHealthRegistry::isUnsafeForLookahead() — the same
      persisted flag Step 21's Watchdog already sets when a plugin hangs
      the lookahead pipeline; reused here as a general "known-bad" marker,
      not just a lookahead-specific one). "Innocent until proven guilty,"
      restored — a brand-new, never-before-seen plugin (empty cachedUID,
      or a cached UID with no crash history yet) is innocent BY
      DEFINITION and goes straight to the Fast Lane.

      Vendor resolution (recordResolvedVendor()/getVendorName()) is left
      completely untouched and still runs on every load — it's no longer
      consulted by route() at all, but the telemetry/registry logging
      value the user asked to retain doesn't depend on route() reading it;
      CrateSandboxBridge::timerCallback() still calls
      recordResolvedVendor() exactly as before, so the health registry's
      JSON still fills in vendorName for anyone inspecting it later
      (support diagnostics, a future "known vendors" UI, etc).
*/
class SandboxRouter
{
public:
    enum class Verdict
    {
        SharedSandbox,
        SolitaryConfinement
    };

    struct Decision
    {
        Verdict verdict;
        juce::String reason; // human-readable — goes straight into the CRATE BRAIN log as-is
    };

    // cachedUID may be empty (a plugin whose path has never resolved a
    // real UID at all yet, or never been loaded before) — under the
    // Optimistic policy that's just "no crash history," same as any other
    // crashCount == 0 case, and routes Shared exactly the same way.
    static Decision route (const juce::String& cachedUID)
    {
        auto& registry = PluginHealthRegistry::getInstance();

        const auto crashCount = cachedUID.isNotEmpty() ? registry.getCrashCount (cachedUID) : 0;

        // Checked first and unconditionally — a plugin that has actually
        // crashed on THIS machine before is never safe to share a process
        // with anything else again, no matter its vendor or how long ago.
        if (crashCount > 0)
            return { Verdict::SolitaryConfinement,
                      "Hostile Plugin Detected (crashCount=" + juce::String (crashCount)
                          + "). Routing to Solitary Confinement." };

        if (cachedUID.isNotEmpty() && registry.isUnsafeForLookahead (cachedUID))
            return { Verdict::SolitaryConfinement,
                      "Plugin explicitly flagged unsafe in the health registry. "
                      "Routing to Solitary Confinement." };

        const auto vendorName = cachedUID.isNotEmpty() ? registry.getVendorName (cachedUID) : juce::String();

        // Optimistic default: zero crashes, not flagged unsafe — trusted
        // regardless of vendor. Vendor name still surfaced in the reason
        // string purely for telemetry/log readability, never as a gate.
        return { Verdict::SharedSandbox,
                  "Plugin trusted by default (crashCount=0"
                      + (vendorName.isNotEmpty() ? (", vendor '" + vendorName + "'") : juce::String (", vendor not yet resolved"))
                      + "). Optimistic routing to Shared Sandbox." };
    }

private:
    SandboxRouter() = delete;
};
