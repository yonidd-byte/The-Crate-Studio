#pragma once

// JUCE_DEBUG itself is only DEFINED by juce_core's own headers (see
// juce_TargetPlatform.h/juce_PlatformDefs.h) — this include MUST happen
// unconditionally, BEFORE the #if below, or an "#if JUCE_DEBUG" that's the
// very first line a translation unit sees treats the undefined macro as 0
// and silently strips this class even in a Debug build (exactly backwards).
// Including <JuceHeader.h> here guarantees the macro is always defined by
// the time the guard is evaluated, regardless of what order a .cpp includes
// headers in.
#include <JuceHeader.h>

// Debug-Only QA Harness directive: the ENTIRE file is wrapped in this guard,
// not just the invocation site — JUCE_DEBUG is 0 in every Release build
// (juce_recommended_config_flags / the standard JUCE Debug/Release
// distinction), so the compiler physically never sees this class outside a
// Debug build. There is no runtime flag to forget to flip off and no "delete
// before shipping" step to rely on human memory for — it cannot ship by
// construction.
#if JUCE_DEBUG

#include <tracktion_engine/tracktion_engine.h>

/**
    Internal QA stress harness — generates a worst-case session (100 tracks,
    a built-in EQ/Compressor + several MIDI clips + a real volume automation
    curve on every one of them, all inside one UndoManager transaction),
    times the generation, verifies the backend actually did what was asked,
    and dumps a performance report to the debug console.

    NEVER shown to, or reachable by, an end user — see the hidden Debug-only
    keyboard shortcut wired up in MainComponent::keyPressed().
*/
class CrateStressTest
{
public:
    static void runExtremeLoadTest (tracktion::engine::Edit& edit);

private:
    CrateStressTest() = delete; // static-only utility, never instantiated
};

#endif // JUCE_DEBUG
