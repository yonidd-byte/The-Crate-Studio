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
#include <functional>
#include <memory>

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

    // Step 9 (The Multi-Process Scalability Stress Test) directive: 50
    // CONCURRENT CrateSandboxBridge/CrateSandbox.exe pairs, each hosting a
    // real VST3, each running its own continuous Step 8 parameter sweep —
    // proving the Hybrid Sync architecture (and, as of this step, the
    // per-instance IPC naming fix it REQUIRED — see CrateSandboxBridge's own
    // doc comment) scales to a professional-session-sized plugin count
    // rather than the single-instance case every step up to now tested.
    static void runSandboxScaleTest (tracktion::engine::Edit& edit);

    // Step 74 (The Flight Recorder) directive, Task 2: loads a real, heavy
    // third-party plugin (hardcoded machine-local path — see the .cpp's
    // own doc comment), opens its actual editor (exercising SandboxAirlock's
    // reparenting path for real), and floods it with genuine parameter
    // changes plus rapid display-scale toggling for 10 seconds — a
    // deliberate, worst-case reproduction of the exact failure pattern
    // this whole session traced. Physically absent from a Release build,
    // same JUCE_DEBUG guard as every other method here — never reachable
    // outside a Debug build, by construction, not by a runtime flag
    // someone could forget to flip off.
    static void runAirlockDeadlockStressTest (tracktion::engine::Edit& edit);

private:
    // Step 74 directive: the self-perpetuating flood tick — public-facing
    // entry point above is runAirlockDeadlockStressTest(); this is its own
    // internal recursive step, exposed only because the self-holding
    // std::function it reschedules itself through needs a free function
    // to call back into (see the .cpp's own doc comment on why this can't
    // be a pure local lambda without the same shared_ptr-of-itself
    // ownership dance this file's other polling helpers already use).
    static void runAirlockDeadlockStressTickInternal (tracktion::engine::Plugin::Ptr bridgePlugin, double startMs,
                                                       std::shared_ptr<std::function<void()>> selfHolder);

    CrateStressTest() = delete; // static-only utility, never instantiated
};

#endif // JUCE_DEBUG
