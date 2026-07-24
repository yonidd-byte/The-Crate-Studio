#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "PluginWindowHeader.h"

namespace te = tracktion::engine;

class CrateSandboxBridge;

/**
    Native editor host window for a te::Plugin. TE deliberately leaves window
    creation to the host app (PluginWindowState::showWindow() calls out to
    Engine::getUIBehaviour().createPluginWindow(), whose default implementation
    returns nullptr) — this is that implementation, adapted from TE's own
    examples/common/PluginWindow.h reference.
*/
class PluginWindow : public juce::DocumentWindow
{
public:
    explicit PluginWindow (te::Plugin&);
    ~PluginWindow() override;

    static std::unique_ptr<juce::Component> create (te::Plugin&);

    void show();

private:
    // Step 87 (Single Taskbar Icon) directive: each floating plugin
    // window is a genuine top-level juce::DocumentWindow (see the base
    // class list above), and JUCE's own TopLevelWindow::
    // getDesktopWindowStyleFlags() defaults to including
    // ComponentPeer::windowAppearsOnTaskbar — meaning every open plugin
    // window got its own separate Windows taskbar entry alongside the
    // main app, exactly as a user would expect from an independent
    // top-level application window, not a child/tool window of a single
    // professional DAW. Overridden here to strip that one flag; see the
    // .cpp's own doc comment on the constructor for why this ALSO
    // requires NOT letting the base class's own constructor call
    // addToDesktop() for us (a virtual call made during base-class
    // construction always resolves to the base class's own
    // implementation, never a derived override — a plain C++ rule, not
    // a JUCE quirk — so this override would otherwise never actually run
    // for the window's real, initial creation).
    int getDesktopWindowStyleFlags() const override;


    // Step 39 (The Universal Plugin Header) directive, Task 1: wraps
    // [PluginWindowHeader][editor] as ONE Component so PluginWindow's own
    // setContentNonOwned(..., true) still has exactly one thing to track
    // for resizeToFitWhenContentChangesSize — see the .cpp's own doc
    // comment for the full geometry contract this preserves.
    //
    // ONLY ever constructed when the plugin being hosted is a
    // CrateSandboxBridge (see setEditor()'s own dynamic_cast gate) — a
    // native, non-sandboxed plugin's editor is set as PluginWindow's
    // content DIRECTLY, exactly as before Step 39, with this class never
    // even instantiated.
    class PluginWindowContent : public juce::Component,
                               private juce::ComponentListener
    {
    public:
        // Step 41 (Geometry Loop Fix & Header Toggle) directive: takes the
        // OWNING PluginWindow itself — see applyGeometry()'s own doc
        // comment for why this class is now the ONE AND ONLY authority
        // that ever resizes the outer window, with JUCE's own implicit
        // resizeToFitWhenContentChangesSize mechanism turned OFF entirely
        // (see setEditor()'s own setContentNonOwned() call).
        PluginWindowContent (PluginWindow& ownerWindowToUse, te::Plugin::EditorComponent& editorToUse, CrateSandboxBridge& bridgeToUse);
        ~PluginWindowContent() override;

        void paint (juce::Graphics&) override;
        void resized() override;

        // Forwards straight to the wrapped editor — PluginWindow.cpp's own
        // setEditor() still queries these against editor directly (see its
        // own code), but keeping these here too means this class remains a
        // fully transparent te::Plugin::EditorComponent-shaped wrapper for
        // anything that might address it generically in the future.
        bool allowWindowResizing() { return editor.allowWindowResizing(); }
        juce::ComponentBoundsConstrainer* getBoundsConstrainer() { return editor.getBoundsConstrainer(); }

        // Step 41 (Geometry Loop Fix) directive: called ONCE, right after
        // construction (from setEditor(), replacing what JUCE's own
        // implicit resize-to-fit cascade used to do at this exact moment)
        // — establishes the OUTER window's correct initial size before it's
        // ever shown.
        void applyInitialGeometry();

        // Step 53 (Defensive Header Geometry / Persistent Fixed-Size Lock)
        // directive: public (like applyInitialGeometry() above) so
        // PluginWindow::setEditor() can call it directly, AFTER its own
        // setConstrainer(editor->getBoundsConstrainer()) call — that
        // ordering matters: this method targets whatever constrainer is
        // ACTUALLY active via ownerWindow.getConstrainer() (see its own
        // doc comment in the .cpp for why setResizeLimits() itself is
        // inert here), and the real one isn't installed until that swap
        // happens, which is AFTER this class's own constructor (and its
        // one synchronous startup call) already ran.
        //
        // Step 56 (Kill The Polling Timer) directive: ALSO called from
        // bridge.onResizeLimitsChanged (registered in this class's own
        // constructor, cleared in its destructor) — CrateSandboxBridge
        // fires that callback exactly once per editor lifetime, the
        // instant its own probe values actually become valid (or change,
        // after a recreation) — see CrateSandboxBridge::onResizeLimitsChanged's
        // own doc comment. This replaces a 4Hz polling Timer that used to
        // live on this class: re-running the exact same idempotent work
        // (setSizeLimits() with unchanged numbers, walking children to
        // re-hide an already-hidden grip) on a fixed cadence regardless of
        // whether anything had changed — a real contributor to
        // "random invalidation flashes" uncorrelated with the user's own
        // drag frames, on top of being wasted work.
        void enforceResizeLimits();

    private:
        // Step 63 (Break The Host-Side Async Loop) directive — QA finding,
        // confirmed via the Step 62 stack-audit instrumentation: NOT a C++
        // recursion/stack overflow (reentrancy depth stayed at 1 in every
        // captured log line), but a genuine, non-converging 2-value
        // oscillation between the outer window's own current size and a
        // CHILD-reported size 2px off from it in both dimensions —
        // consistent with DocumentWindow::getContentComponentBorder()'s
        // own 1px-per-side border round-tripping through
        // setContentComponentSize()/ResizableWindow::resized()'s bounds
        // inset without ever being checked for "did this already happen."
        // applyGeometry()'s own comment explains the actual root cause:
        // ownerWindow.setContentComponentSize() was called UNCONDITIONALLY
        // on every single call, with no memory of what was last actually
        // applied — this is that memory.
        int lastAppliedGeometryWidth  = -1;
        int lastAppliedGeometryHeight = -1;


        // Step 41 directive, Task 4 (The Gear Toggle): collapses the full
        // control strip down to just enough room for the gear button
        // itself — NOT all the way to 0. A true 0-height strip would put
        // the gear button's own area flush against (or inside) the
        // reparented native HWND's own bounds, and a JUCE-drawn Component
        // is NOT guaranteed to paint above a real Win32 child window in
        // the same screen region regardless of JUCE's own internal z-order
        // (the exact "airspace problem" this session already solved once,
        // the hard way, for HWND reparenting itself — see
        // CrateSandboxBridge's own doc comment on juce::HWNDComponent).
        // Reserving a small strip that's ALWAYS pure JUCE-drawn space,
        // never shared with the HWND's own region, sidesteps that problem
        // entirely rather than reopening it. Sized to comfortably fit the
        // Step 42 gear button (28px, matching bypassButton's own height)
        // plus a little padding — still shrinks the window by the majority
        // (64px -> 32px, a 32px reduction) of the full header's height,
        // matching the spirit of "shrink away that gap" even though a
        // small sliver deliberately remains.
        static constexpr int collapsedStripHeight = 32;

        void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;

        // Step 41 (Geometry Loop Fix) directive: the ONE place that ever
        // computes a new target size and applies it to BOTH this wrapper
        // and the outer PluginWindow — see its own doc comment in the .cpp
        // for the dual-authority race this replaces.
        void applyGeometry();

        void setHeaderVisible (bool shouldBeVisible);

        // Step 53 (Persistent Fixed-Size Lock) directive: once the probe
        // has EVER shown min==max in both dimensions for this window's
        // hosted plugin, this latches true FOREVER (never re-evaluated
        // back to false) — see enforceResizeLimits()'s own doc comment
        // for the state-load-triggered re-probe this guards against.
        bool everProbedFixedSize = false;
        int latchedFixedWidth = 0, latchedFixedHeight = 0;

        // Step 44 (Gear Icon Flat Styling, Round 2) directive: colour IDs
        // alone can't make gearButton truly flat — TheCrateLookAndFeel::
        // drawButtonBackground() hardcodes fill = accent whenever
        // shouldDrawButtonAsDown is true, REGARDLESS of buttonColourId
        // (confirmed by reading its own source), which is what produced a
        // solid cyan block on press despite gearButton's colours being set
        // to transparentBlack. A dedicated LookAndFeel is the only way to
        // guarantee "flat icon, never a coloured block."
        //
        // Step 48 (Header Button UX) directive: "flat" doesn't mean "no
        // feedback at all" — a subtle semi-transparent white wash on hover,
        // slightly more opaque on press, gives the tactile confirmation a
        // real icon button needs without reintroducing TheCrateLookAndFeel's
        // own hardcoded accent-block behaviour this class exists to avoid.
        struct FlatIconLookAndFeel : public juce::LookAndFeel_V4
        {
            void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                        bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
            {
                if (! shouldDrawButtonAsHighlighted && ! shouldDrawButtonAsDown)
                    return; // rest state: genuinely flat, nothing painted at all

                const float alpha = shouldDrawButtonAsDown ? 0.22f : 0.12f;
                g.setColour (juce::Colours::white.withAlpha (alpha));
                g.fillRoundedRectangle (button.getLocalBounds().toFloat(), 4.0f);
            }
        };

        FlatIconLookAndFeel gearButtonLookAndFeel;

        PluginWindow& ownerWindow;
        te::Plugin::EditorComponent& editor;
        CrateSandboxBridge& bridge;
        PluginWindowHeader header;
        juce::TextButton gearButton;

        // Step 47 (The Unified Title Bar) directive, Task 1: JUCE's native
        // title bar (and its own close button) is fully collapsed for a
        // sandboxed window now (see PluginWindow's own constructor) — this
        // is what actually closes the window instead. Same FlatIconLookAndFeel
        // and always-visible-regardless-of-headerVisible treatment as
        // gearButton, for the same "must still be reachable even collapsed"
        // reason (see collapsedStripHeight's own comment) — you can't close
        // a window whose only close button just got hidden.
        juce::TextButton closeButton;
        bool headerVisible = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindowContent)
    };

    void moved() override;
    void userTriedToCloseWindow() override         { plugin.windowState->closeWindowExplicitly(); }
    void closeButtonPressed() override              { userTriedToCloseWindow(); }
    float getDesktopScaleFactor() const override    { return 1.0f; }

    void setEditor (std::unique_ptr<te::Plugin::EditorComponent>);

    std::unique_ptr<te::Plugin::EditorComponent> editor;
    std::unique_ptr<PluginWindowContent> content; // only non-null when editor is wrapped with a header — see setEditor()'s own comment
    te::Plugin& plugin;
    bool updateStoredBounds = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

/** Wires TE's plugin-window extension point to PluginWindow above. Pass an instance
    of this to the te::Engine constructor. */
class CrateUIBehaviour : public te::UIBehaviour
{
public:
    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& pws) override
    {
        if (auto ws = dynamic_cast<te::Plugin::WindowState*> (&pws))
            return PluginWindow::create (ws->plugin);

        return {};
    }
};
