#pragma once

#include <juce_core/juce_core.h>

/**
    Plugin Sandboxing Step 13 (The Profiling Database) — a persistent,
    cross-session health tracker for hosted VST3 plugins.

    Phase 3 needs to know which plugins are trustworthy BEFORE deciding how
    to sandbox them (shared vs. isolated, Step 14+). That decision can't be
    made from in-memory state alone — it has to survive restarts of the DAW
    itself, since a plugin that crashed three times yesterday is still a
    plugin that's crashed three times today. Hence disk persistence, not
    just an in-process std::map.

    STORAGE: a single JSON file at
    getSpecialLocation(userApplicationDataDirectory)/The Crate Studio/CratePluginHealth.json
    — same directory convention as any other well-behaved Windows app
    (%APPDATA%\The Crate Studio\...). The file holds one JSON object with a
    "plugins" array; each entry is {"pluginUID", "successfulLoads",
    "crashCount", "unsafeForLookahead" (Step 21)} rather than using
    pluginUID as an object KEY, specifically
    because pluginUID is currently a full filesystem path (colons,
    backslashes, spaces, dots) and juce::Identifier/JSON object keys are
    happiest with simple names — an array of records sidesteps that
    entirely rather than fighting it.

    THREADING: every method here is called from the message thread only
    (CrateSandboxBridge::timerCallback() and its restart paths) — never
    the audio thread. The CriticalSection below is defensive, not load-
    bearing, in case that assumption is ever violated by future callers.

    IDENTIFIER CAVEAT (disclosed honestly, not glossed over): this
    codebase does not yet resolve a real VST3 unique ID (the CID from the
    plugin's factory) anywhere on the parent side — CrateSandboxBridge only
    ever holds a hardcoded absolute path to the test plugin. Step 13 uses
    that path AS the pluginUID surrogate. It's stable enough to prove out
    the registry's read/write/query behavior end-to-end, but it is NOT yet
    the real VST3 CID this class's own name implies. Swapping in a real
    juce::PluginDescription::createIdentifierString() (or the raw VST3 CID)
    later is a drop-in replacement — every method here just takes a String.
*/
class PluginHealthRegistry
{
public:
    static PluginHealthRegistry& getInstance()
    {
        static PluginHealthRegistry instance;
        return instance;
    }

    void recordSuccessfulLoad (const juce::String& pluginUID)
    {
        const juce::ScopedLock sl (lock);

        auto entry = findOrCreateEntry (pluginUID);
        entry->setProperty ("successfulLoads", (int) entry->getProperty ("successfulLoads") + 1);
        save();
    }

    void recordCrash (const juce::String& pluginUID)
    {
        const juce::ScopedLock sl (lock);

        auto entry = findOrCreateEntry (pluginUID);
        entry->setProperty ("crashCount", (int) entry->getProperty ("crashCount") + 1);
        save();
    }

    bool requiresSolitaryConfinement (const juce::String& pluginUID) const
    {
        const juce::ScopedLock sl (lock);

        if (auto entry = findEntry (pluginUID))
            return (int) entry->getProperty ("crashCount") >= 3;

        return false;
    }

    int getCrashCount (const juce::String& pluginUID) const
    {
        const juce::ScopedLock sl (lock);

        if (auto entry = findEntry (pluginUID))
            return (int) entry->getProperty ("crashCount");

        return 0;
    }

    // Step 21 (The Watchdog & Graceful Fallback) directive: a SEPARATE flag
    // from crashCount/requiresSolitaryConfinement() — a plugin that hangs
    // the lookahead pipeline (LookaheadWorkerThread's processBlock() call
    // never returning, confirmed via direct testing with Rift Filter Lite)
    // hasn't necessarily crashed the sandbox or misbehaved in real-time
    // playback at all; it specifically can't be trusted with the
    // non-real-time, arbitrary-thread calling pattern lookahead mode uses.
    // Disk-persisted for the same reason crashCount is: a plugin that hung
    // the pipeline yesterday still will today, and every future load
    // (including in a brand-new session) should skip attempting lookahead
    // for it without needing to rediscover the hang the hard way again.
    void recordLookaheadHang (const juce::String& pluginUID)
    {
        const juce::ScopedLock sl (lock);

        auto entry = findOrCreateEntry (pluginUID);
        entry->setProperty ("unsafeForLookahead", true);
        save();
    }

    bool isUnsafeForLookahead (const juce::String& pluginUID) const
    {
        const juce::ScopedLock sl (lock);

        if (auto entry = findEntry (pluginUID))
            return (bool) entry->getProperty ("unsafeForLookahead");

        return false;
    }

    // Step 22 (The Profiling Database / The Warden) directive: resolved by
    // the CHILD's own scan (juce::PluginDescription::manufacturerName —
    // see ControlBlock::vendorName's own doc comment for why it's never
    // re-scanned in the PARENT), cached here the same way recordResolvedUID()
    // caches the authentic UID — so SandboxRouter's routing decision, which
    // has to run BEFORE any child process exists, can consult a plugin's
    // vendor from a PAST load without needing to scan it again.
    void recordResolvedVendor (const juce::String& pluginUID, const juce::String& vendorName)
    {
        const juce::ScopedLock sl (lock);

        auto entry = findOrCreateEntry (pluginUID);
        entry->setProperty ("vendorName", vendorName);
        save();
    }

    // Empty string means "not yet known" — a plugin that's never been
    // loaded at all, or has loaded but not yet reached the point in
    // loadHostedPlugin() where the scan resolves it. SandboxRouter treats
    // an empty vendor name as "unrecognized" (Rule B), same as any vendor
    // not on the trusted whitelist.
    juce::String getVendorName (const juce::String& pluginUID) const
    {
        const juce::ScopedLock sl (lock);

        if (auto entry = findEntry (pluginUID))
            return entry->getProperty ("vendorName").toString();

        return {};
    }

    // Step 14 (The Crate Brain) directive: the router needs to know a
    // plugin's crash history BEFORE it has spawned anything for THIS
    // load — but the authentic UID (Step 13.5) can only ever be resolved
    // by a CHILD process that has actually scanned the file. Rather than
    // booting a disposable child on every single load just to identify
    // it (real cost for a large sample library, and the exact "penalize
    // slow-loading plugins" mistake Step 13.5 already fixed once), this
    // records a persistent FILE PATH -> UID mapping the first time any
    // real launch resolves it, so every subsequent load of the same file
    // can look its history up immediately, no scan required. A brand-new
    // plugin this registry has never seen simply has no cached mapping
    // yet — see SandboxManager's own handling of that case.
    void recordResolvedUID (const juce::String& pluginPath, const juce::String& pluginUID)
    {
        const juce::ScopedLock sl (lock);

        auto entry = findOrCreateEntry (pluginUID);
        entry->setProperty ("lastKnownPath", pluginPath);
        save();
    }

    juce::String lookupCachedUID (const juce::String& pluginPath) const
    {
        const juce::ScopedLock sl (lock);

        if (auto* pluginsArray = root->getProperty ("plugins").getArray())
        {
            for (auto& entryVar : *pluginsArray)
            {
                if (auto obj = entryVar.getDynamicObject())
                    if (obj->getProperty ("lastKnownPath").toString() == pluginPath)
                        return obj->getProperty ("pluginUID").toString();
            }
        }

        return {};
    }

    juce::File getRegistryFile() const
    {
        return registryFile;
    }

private:
    PluginHealthRegistry()
        : registryFile (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("The Crate Studio")
                             .getChildFile ("CratePluginHealth.json"))
    {
        load();
    }

    juce::DynamicObject::Ptr findEntry (const juce::String& pluginUID) const
    {
        if (auto* pluginsArray = root->getProperty ("plugins").getArray())
        {
            for (auto& entryVar : *pluginsArray)
            {
                if (auto obj = entryVar.getDynamicObject())
                    if (obj->getProperty ("pluginUID").toString() == pluginUID)
                        return obj;
            }
        }

        return nullptr;
    }

    juce::DynamicObject::Ptr findOrCreateEntry (const juce::String& pluginUID)
    {
        if (auto existing = findEntry (pluginUID))
            return existing;

        auto* pluginsArray = root->getProperty ("plugins").getArray();
        jassert (pluginsArray != nullptr);

        juce::DynamicObject::Ptr newEntry = new juce::DynamicObject();
        newEntry->setProperty ("pluginUID", pluginUID);
        newEntry->setProperty ("successfulLoads", 0);
        newEntry->setProperty ("crashCount", 0);

        pluginsArray->add (juce::var (newEntry.get()));
        return newEntry;
    }

    void load()
    {
        root = new juce::DynamicObject();
        root->setProperty ("plugins", juce::var (juce::Array<juce::var>()));

        if (! registryFile.existsAsFile())
            return;

        auto parsed = juce::JSON::parse (registryFile);

        if (auto* obj = parsed.getDynamicObject())
            if (obj->getProperty ("plugins").isArray())
                root = obj;
    }

    void save()
    {
        registryFile.getParentDirectory().createDirectory();
        registryFile.replaceWithText (juce::JSON::toString (juce::var (root.get())));
    }

    juce::File registryFile;
    juce::DynamicObject::Ptr root;
    mutable juce::CriticalSection lock;

    JUCE_DECLARE_NON_COPYABLE (PluginHealthRegistry)
};
