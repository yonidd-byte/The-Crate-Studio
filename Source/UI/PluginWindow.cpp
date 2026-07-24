#include "PluginWindow.h"
#include "../Engine/CrateSandboxBridge.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

int PluginWindow::getDesktopWindowStyleFlags() const
{
    // Step 87 (Single Taskbar Icon) directive: see this method's own
    // declaration in PluginWindow.h for the full reasoning — strips the
    // one flag responsible for every open plugin window getting its own
    // separate Windows taskbar entry.
    return DocumentWindow::getDesktopWindowStyleFlags() & ~juce::ComponentPeer::windowAppearsOnTaskbar;
}

PluginWindow::PluginWindow (te::Plugin& plug)
    // Step 87 directive: addToDesktop is now ALWAYS false here, on every
    // platform — see this constructor's own body for why, and this
    // class's own getDesktopWindowStyleFlags() override for what this
    // makes possible. DocumentWindow(name, backgroundColour,
    // requiredButtons, false) never touches the desktop at all during
    // base construction; the explicit addToDesktop() call below is what
    // actually creates the peer, at a point where a virtual call
    // correctly reaches THIS class's own override.
    : DocumentWindow (plug.getName(), juce::Colours::black, DocumentWindow::closeButton, false),
      plugin (plug)
{
    // Step 87 directive: replaces the OLD "pass true/false for
    // addToDesktop straight into the base constructor" approach — a
    // virtual call made from WITHIN a base class's own constructor
    // always resolves to the base class's own implementation, never a
    // derived override, regardless of the object's real final type
    // (ordinary C++ virtual-dispatch-during-construction behaviour, not
    // a JUCE quirk). getDesktopWindowStyleFlags() is called virtually
    // inside ResizableWindow::ResizableWindow() itself when addToDesktop
    // is true, at a point where the object is still only a
    // DocumentWindow/ResizableWindow as far as the vtable is concerned —
    // meaning the override above would silently never have applied to
    // the window's real, initial creation if left in the base
    // constructor's own hands. Calling it explicitly here, in THIS
    // (the most-derived) constructor's own body, is the one place a
    // virtual call is guaranteed to reach the real override. Runs first,
    // before anything below, matching where the base class's own
    // internal call used to happen (Windows previously passed `true`
    // straight into the base constructor) — nothing below has ever
    // needed to run without a peer already existing on this platform.
    addToDesktop (getDesktopWindowStyleFlags());

   #if JUCE_WINDOWS
    // Step 90 (Z-Order Lock) directive — QA finding: clicking/dragging a
    // macro control in the Host's own main Device Chain window was
    // burying the floating plugin window behind it. Normal Win32
    // behaviour for two independent top-level windows with no declared
    // relationship — activating one raises it above every OTHER
    // unrelated top-level window, plugin windows included.
    //
    // juce::Component::setAlwaysOnTop() (WS_EX_TOPMOST) would fix the
    // reported symptom but over-corrects: it pins this window above
    // EVERY app on the whole desktop, not just this one — a plugin
    // window still floating over a browser or a reference PDF after
    // alt-tabbing away is a different, equally real annoyance most
    // DAWs deliberately avoid. The actual Win32 idiom for "always stays
    // above MY OWN app's main window specifically, and nothing else" is
    // window OWNERSHIP (GWLP_HWNDPARENT) — distinct from WS_CHILD
    // parenting (an owned window still moves, resizes, and minimizes
    // independently) — which the OS itself enforces automatically
    // whenever the owner window is activated, which is exactly the
    // trigger described in the bug report.
    //
    // The main app window is found via JUCE's own TopLevelWindow
    // registry rather than threading a reference through this
    // constructor — the first registered TopLevelWindow that ISN'T
    // another PluginWindow is, by this app's own architecture (one
    // MainWindow, N plugin windows), the main window.
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
    {
        auto* candidate = juce::TopLevelWindow::getTopLevelWindow (i);

        if (candidate == nullptr || candidate == this || dynamic_cast<PluginWindow*> (candidate) != nullptr)
            continue;

        if (auto* mainPeer = candidate->getPeer())
            if (auto* myPeer = getPeer())
                SetWindowLongPtr ((HWND) myPeer->getNativeHandle(), GWLP_HWNDPARENT, (LONG_PTR) (HWND) mainPeer->getNativeHandle());

        break;
    }
   #endif

    getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
    setResizeLimits (100, 50, 4000, 4000);

    // Step 47 (The Unified Title Bar) directive, Task 1: collapses JUCE's
    // own native title bar entirely for a sandboxed window — checked here,
    // directly, rather than waiting for setEditor()'s own dynamic_cast,
    // because this MUST happen BEFORE setEditor() runs its initial
    // geometry math (DocumentWindow::getContentComponentBorder() adds
    // titleBarHeight to its top inset, so getting this wrong first would
    // size the window against the WRONG border for its very first layout).
    // A native, non-sandboxed plugin keeps the real title bar/close button
    // untouched — it has no PluginWindowHeader to take over that role.
    // setTitleBarHeight(0) alone is enough: LookAndFeel_V4::
    // drawDocumentWindowTitleBar() itself early-returns when w*h==0 (its
    // own source: "if (w * h == 0) return;"), so nothing is drawn, and the
    // title bar's own close button gets squeezed into zero usable space —
    // PluginWindowContent's own closeButton (see its own doc comment) is
    // what actually closes the window now.
    if (dynamic_cast<CrateSandboxBridge*> (&plugin) != nullptr)
    {
        setTitleBarHeight (0);

        // Step 50 (Header Layout & Minimum Width) directive: a hard floor
        // on the window's own width, computed from the SAME fixed-width
        // sums PluginWindowHeader::resized() actually lays out (see that
        // constant's own doc comment) — below this, JUCE's own resize
        // corner/border simply refuses to shrink the window further, so
        // the header's controls can never be dragged into crushing one
        // another. Height limits are untouched — only width was reported
        // as squashable.
        setResizeLimits (PluginWindowHeader::minimumHeaderContentWidth, 50, 4000, 4000);
    }

    setEditor (plugin.createEditor());
    setBoundsConstrained (getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());

   #if JUCE_LINUX
    // Step 87 directive: addToDesktop() itself now happens once,
    // unconditionally, at the very top of this constructor on every
    // platform (see that call site's own doc comment) — only the
    // Linux-specific always-on-top behaviour stays here.
    setAlwaysOnTop (true);
   #endif

    updateStoredBounds = true;
}

PluginWindow::~PluginWindow()
{
    updateStoredBounds = false;
    plugin.edit.flushPluginStateIfNeeded (plugin);
    setEditor (nullptr);
}

void PluginWindow::show()
{
    setVisible (true);
    toFront (false);
    setBoundsConstrained (getBounds());
}

void PluginWindow::setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
{
    setConstrainer (nullptr);
    content.reset();
    editor.reset();

    if (newEditor != nullptr)
    {
        editor = std::move (newEditor);

        // Step 39 (The Universal Plugin Header) directive, Task 1: ONLY a
        // CrateSandboxBridge-hosted plugin gets wrapped with a header — its
        // Dry/Wet mix, telemetry, and A/B controls call straight into
        // CrateSandboxBridge's own public API, which simply doesn't exist
        // for a native, non-sandboxed plugin. That plugin's own editor is
        // set as this window's content DIRECTLY, exactly as before Step 39
        // — zero behavioural change for anything not sandboxed.
        if (auto* bridge = dynamic_cast<CrateSandboxBridge*> (&plugin))
        {
            content = std::make_unique<PluginWindowContent> (*this, *editor, *bridge);

            // Step 41 (Geometry Loop Fix) directive: resizeToFitWhenContentChangesSize
            // is now FALSE, deliberately — see applyGeometry()'s own doc
            // comment in PluginWindowContent for the dual-authority
            // oscillation this removes. content->applyInitialGeometry()
            // (below) is what establishes the correct initial size instead.
            setContentNonOwned (content.get(), false);
            content->applyInitialGeometry();
        }
        else
        {
            setContentNonOwned (editor.get(), true);
        }
    }

    setResizable (editor == nullptr || editor->allowWindowResizing(), false);

    if (editor != nullptr && editor->allowWindowResizing())
        setConstrainer (editor->getBoundsConstrainer());

    // Step 53 (The Inert Constrainer Fix) directive: THIS is the point
    // where the constrainer actually wired to the native peer becomes
    // known — content's own constructor (above) already ran and seeded a
    // floor on whatever constrainer existed AT THAT moment (harmless, but
    // superseded the instant setConstrainer() just above swaps it out).
    // Re-applying here, immediately after the swap, closes that gap
    // completely rather than leaving the real constrainer with no
    // meaningful limits until PluginWindowContent's own Timer's first
    // tick (up to ~250ms later).
    if (content != nullptr)
        content->enforceResizeLimits();
}

PluginWindow::PluginWindowContent::PluginWindowContent (PluginWindow& ownerWindowToUse, te::Plugin::EditorComponent& editorToUse, CrateSandboxBridge& bridgeToUse)
    : ownerWindow (ownerWindowToUse), editor (editorToUse), bridge (bridgeToUse), header (bridgeToUse)
{
    addAndMakeVisible (header);
    addAndMakeVisible (editor);
    editor.addComponentListener (this);

    // Step 44 (Gear Icon Flat Styling, Round 2) directive: a dedicated
    // LookAndFeel (see its own doc comment in the header) rather than
    // colour IDs alone — TheCrateLookAndFeel's drawButtonBackground()
    // hardcodes a solid accent-coloured fill whenever the button is
    // "down," bypassing buttonColourId entirely, which is what produced a
    // solid cyan block on press. gearButtonLookAndFeel's own
    // drawButtonBackground() paints nothing, in any state, guaranteeing a
    // genuinely flat icon. No keyboard-focus ring for the same reason
    // Step 43 removed it from bypassButton (see PluginWindowHeader's own
    // comment on that).
    gearButton.setLookAndFeel (&gearButtonLookAndFeel);
    gearButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x9a\x99")); // U+2699 GEAR
    gearButton.setWantsKeyboardFocus (false);
    gearButton.onClick = [this] { setHeaderVisible (! headerVisible); };
    addAndMakeVisible (gearButton);

    // Step 47 (The Unified Title Bar) directive, Task 1: replaces JUCE's
    // own native title-bar close button (squeezed to zero usable space
    // now — see PluginWindow's own constructor). closeButtonPressed() is
    // PRIVATE on PluginWindow, but this nested class has the same access
    // rights as any other PluginWindow member per the standard, so calling
    // it directly here (rather than threading a std::function through
    // yet another constructor parameter) is both correct and the least
    // ceremony. Same FlatIconLookAndFeel/no-keyboard-focus/always-visible
    // treatment as gearButton, for the same reasons.
    closeButton.setLookAndFeel (&gearButtonLookAndFeel);
    closeButton.setButtonText (juce::CharPointer_UTF8 ("\xc3\x97")); // U+00D7 MULTIPLICATION SIGN, used as a close glyph
    closeButton.setWantsKeyboardFocus (false);
    closeButton.onClick = [this] { ownerWindow.closeButtonPressed(); };
    addAndMakeVisible (closeButton);

    // Step 56 (Kill The Polling Timer) directive: event-driven, not a 4Hz
    // Timer — CrateSandboxBridge fires this exactly once per editor
    // lifetime, the instant its own probe values actually become valid
    // (or change, after a recreation). See
    // CrateSandboxBridge::onResizeLimitsChanged's own doc comment for the
    // rising-edge detection that drives it, and enforceResizeLimits()'s
    // own doc comment (header) for the flicker this replaces.
    bridge.onResizeLimitsChanged = [this] { enforceResizeLimits(); };

    // Step 53 (Defensive Header Geometry) directive: seeds SOME floor
    // immediately rather than waiting for the callback above to ever
    // fire — harmless even though the constrainer actually wired to the
    // peer isn't installed until setEditor() calls setConstrainer() a
    // moment AFTER this constructor returns (see that call site's own
    // matching enforceResizeLimits() call, which is the one that
    // actually reaches the real, active constrainer).
    enforceResizeLimits();
}

PluginWindow::PluginWindowContent::~PluginWindowContent()
{
    // Step 56 (Kill The Polling Timer) directive: MUST clear before this
    // object finishes destructing — bridge outlives this Component (it's
    // owned by the plugin's own te::Plugin::Ptr lifetime, not this
    // window), so a stale callback left behind would fire into
    // already-freed memory the next time the CHILD's editor readiness
    // flips.
    bridge.onResizeLimitsChanged = nullptr;

    gearButton.setLookAndFeel (nullptr);  // standard JUCE ordering discipline — never leave a Component pointing at a LookAndFeel that's about to be destroyed
    closeButton.setLookAndFeel (nullptr);
    editor.removeComponentListener (this);
}

void PluginWindow::PluginWindowContent::paint (juce::Graphics& g)
{
    // Step 44 (Black Box Fix) directive: the gear button used to occupy a
    // column carved OUT of header's own bounds via removeFromRight() — a
    // slice of THIS component's own area that nothing ever painted (this
    // class had no paint() override at all), so the OUTER DocumentWindow's
    // background (plain black — see its own constructor) showed through
    // directly behind the gear button, and behind the ENTIRE collapsed
    // strip when the header is hidden (header.setVisible(false) means it
    // never paints at all, gear button included no more coverage there
    // either). Filling the current strip's full width here, BELOW where
    // header/gearButton paint themselves, removes both gaps at once with
    // one rectangle — same colour header itself already uses, so the two
    // are visually seamless when the header IS showing.
    const int stripHeight = headerVisible ? PluginWindowHeader::headerHeight : PluginWindowContent::collapsedStripHeight;
    g.setColour (juce::Colour (0xff2a2a2e));
    g.fillRect (0, 0, getWidth(), stripHeight);
}

void PluginWindow::PluginWindowContent::resized()
{
    auto bounds = getLocalBounds();

    // Step 41 (The Gear Toggle) directive: the strip is EITHER the full
    // header height (all controls) OR just enough for the gear button
    // (collapsedStripHeight) — see that constant's own doc comment for why
    // it's never truly 0.
    const int stripHeight = headerVisible ? PluginWindowHeader::headerHeight : collapsedStripHeight;
    auto stripArea = bounds.removeFromTop (stripHeight);

    // Step 45 (Gear/Telemetry Overlap Fix) directive: header's own CONTENT
    // width now stops short of the gear/close column — the Step 44 "black
    // box" fix gave header its full stripArea width again (needed so ITS
    // OWN background paint reaches the true right edge), but that meant
    // header's own right-aligned telemetryLabel ALSO extended under where
    // the gear/close buttons visually sit, since nothing told header to
    // leave room for them. PluginWindowContent's own paint() (above)
    // already covers the FULL strip's background regardless of header's
    // width, so narrowing header's content area here only affects layout,
    // not the seamless background continuity Step 44 fixed.
    constexpr int iconButtonSize = 28;
    constexpr int iconColumnWidth = iconButtonSize + 12;
    constexpr int cornerColumnWidth = iconColumnWidth * 2; // Step 47: gear + close, side by side

    if (headerVisible)
        header.setBounds (stripArea.withTrimmedRight (cornerColumnWidth));

    header.setVisible (headerVisible);

    // Step 47 (The Unified Title Bar) directive, Task 1: close sits at the
    // VERY far right (matching where a native title bar's own close
    // button would be), gear immediately to its left — both overlay the
    // top-right corner of whichever strip is showing, last children added
    // in the constructor, so JUCE's default z-order already draws them in
    // front of header without needing an explicit toFront() call. Matches
    // bypassButton's own height (28px) exactly — see this constant's own
    // history for the "massive clunky square" bug that fixed.
    juce::Rectangle<int> closeArea (getWidth() - iconColumnWidth, 0, iconColumnWidth, stripHeight);
    closeButton.setBounds (closeArea.withSizeKeepingCentre (iconButtonSize, iconButtonSize));

    juce::Rectangle<int> gearArea (getWidth() - cornerColumnWidth, 0, iconColumnWidth, stripHeight);
    gearButton.setBounds (gearArea.withSizeKeepingCentre (iconButtonSize, iconButtonSize));

    // Gives the editor exactly (our width, our height minus the strip) —
    // triggers the editor's OWN resized() override, which (for a
    // CrateSandboxBridge's CrateEditorComponent) publishes this as a
    // host-initiated resize request over IPC exactly as it did before this
    // wrapper existed, just against the strip-reduced size rather than
    // this window's full content area.
    editor.setBounds (bounds);
}

void PluginWindow::PluginWindowContent::componentMovedOrResized (juce::Component& comp, bool, bool wasResized)
{
    // The editor dictates its OWN size independently of us — the CHILD's
    // own reported geometry, the canResize lock, the Editor View Recovery
    // Guard's teardown/recreate cycle all resize `editor` directly, not
    // through our own resized() above. When that happens, this is the
    // trigger to re-sync — see applyGeometry()'s own doc comment for what
    // that actually does now.
    if (&comp != &editor || ! wasResized)
        return;

    applyGeometry();
}

void PluginWindow::PluginWindowContent::applyInitialGeometry()
{
    applyGeometry();
}

void PluginWindow::PluginWindowContent::setHeaderVisible (bool shouldBeVisible)
{
    if (headerVisible == shouldBeVisible)
        return;

    headerVisible = shouldBeVisible;
    applyGeometry(); // editor's own size hasn't changed, but the strip height backing it has — same single code path handles both cases identically
}

// Step 52 Task 2 (Strict VST3-Driven Resize Limits) directive: the user's
// own live-show-safety requirement — the OS window manager itself must
// physically refuse to shrink this window below what the hosted plugin's
// own VST3 view can actually support, not just something PluginWindowHeader's
// controls can fit in. bridge.getPluginMinWidth()/Height()/MaxWidth()/
// Height() are a PROBE result (Main.cpp's own checkSizeConstraint() query
// against the CHILD's real IPlugView — see that comment for why this is a
// probe, not a guaranteed direct API), 0 until the CHILD's editor has
// actually been created and probed — this method is a no-op until then.
//
// Step 56 (Kill The Polling Timer) directive: called from exactly three
// places now — this class's own constructor (seeds a floor immediately),
// PluginWindow::setEditor() (once the real constrainer is installed), and
// bridge.onResizeLimitsChanged (event-driven, fires once per editor
// lifetime when the probe result actually lands or changes) — never from
// a repeating Timer.
void PluginWindow::PluginWindowContent::enforceResizeLimits()
{
    const int probedMinWidth  = bridge.getPluginMinWidth();
    const int probedMinHeight = bridge.getPluginMinHeight();
    const int probedMaxWidth  = bridge.getPluginMaxWidth();
    const int probedMaxHeight = bridge.getPluginMaxHeight();

    // Step 53 (Persistent Fixed-Size Lock) directive — QA finding: after
    // Store A/B loaded a state chunk into VoxDucker, the user was able to
    // resize it again, reproducing the black void. Root cause: a state
    // load can make a VST3 internally recompute/re-publish its own view
    // size, and if that (or a fresh editor recreation, e.g. the Editor
    // View Recovery Guard's own teardown/rebuild cycle) triggers a fresh
    // probe that DOESN'T land on min==max this time, a PURELY REACTIVE
    // check would silently re-open the resize handle on the very next
    // tick. Fix: latch it — the FIRST time this window's plugin ever
    // proves itself fixed-size, that's authoritative for the rest of this
    // window's lifetime, never re-derived from whatever the bridge
    // happens to report afterward.
    if (probedMinWidth > 0 && probedMinHeight > 0
            && probedMinWidth == probedMaxWidth && probedMinHeight == probedMaxHeight)
    {
        everProbedFixedSize = true;
        latchedFixedWidth   = probedMinWidth;
        latchedFixedHeight  = probedMinHeight;
    }

    // Step 54 (Nuke the Resize Handle) directive — QA finding: even with
    // the constrainer's min==max lock in place, the OS window frame
    // itself was still active: the cursor still changed to a resize
    // arrow at the edges and the corner could still be grabbed and
    // dragged (the constrainer then fought the drag, which is exactly
    // the black-void-exposing tug-of-war the user can see). The
    // constrainer constrains WHERE a resize lands; only
    // setResizable(false, false) removes the resize AFFORDANCE itself —
    // on a desktop window it triggers recreateDesktopWindow() (confirmed
    // via direct JUCE source read, juce_ResizableWindow.cpp), rebuilding
    // the native HWND without the resizable style, so Windows itself
    // stops offering edge-resize hit zones and the cursor never changes.
    // Guarded by isResizable() so the (4Hz, repeating) tick only pays
    // for that native-window rebuild ONCE, not every 250ms forever.
    //
    // Belt-and-braces: also explicitly hide any live corner/border grip
    // component. setResizable(false, false) already resets both
    // internally, but ANOTHER code path (CrateEditorComponent's own
    // canResize tick in CrateSandboxBridge.h) also calls setResizable()
    // on this same window and could in principle re-create a grip later;
    // hiding whatever exists right now costs nothing and is re-asserted
    // every tick.
    if (everProbedFixedSize)
    {
        if (ownerWindow.isResizable())
            ownerWindow.setResizable (false, false);

        for (auto* child : ownerWindow.getChildren())
            if (dynamic_cast<juce::ResizableCornerComponent*> (child) != nullptr
                 || dynamic_cast<juce::ResizableBorderComponent*> (child) != nullptr)
                child->setVisible (false);
    }

    int minWidth, minHeight, maxWidth, maxHeight;

    if (everProbedFixedSize)
    {
        // Persistent lock: min == max == the FIRST-ever proven fixed
        // size, forever.
        minWidth  = juce::jmax (latchedFixedWidth, PluginWindowHeader::minimumHeaderContentWidth);
        minHeight = latchedFixedHeight + PluginWindowHeader::headerHeight;
        maxWidth  = minWidth;
        maxHeight = minHeight;
    }
    else
    {
        if (probedMinWidth <= 0 || probedMinHeight <= 0)
            return; // no probe result yet

        // minWidth = max(PluginMinWidth, HeaderMinimumWidth) — the window
        // can never be dragged narrower than whichever is actually
        // larger: the plugin's own real minimum, or the header's own
        // fixed-width controls (Step 50's minimumHeaderContentWidth).
        minWidth = juce::jmax (probedMinWidth, PluginWindowHeader::minimumHeaderContentWidth);

        // minHeight = PluginMinHeight + header height. Sized against
        // headerHeight (64px, the full uncollapsed strip) rather than the
        // smaller collapsedStripHeight (32px) — headerVisible is a user
        // toggle that can flip at any moment, and the floor must stay
        // valid for BOTH states, so it has to assume the larger one.
        minHeight = probedMinHeight + PluginWindowHeader::headerHeight;

        // Ceiling: a genuine probed maximum, or the existing 4000
        // fallback if checkSizeConstraint() didn't actually clamp the
        // probe's absurdly large candidate (meaning this plugin has no
        // real maximum).
        maxWidth  = probedMaxWidth  > 0 ? juce::jmax (probedMaxWidth,  minWidth)  : 4000;
        maxHeight = probedMaxHeight > 0 ? juce::jmax (probedMaxHeight, minHeight) : 4000;
    }

    // Step 53 (The Inert Constrainer Fix) directive — a SECOND, previously
    // undiscovered bug this QA round's "header still overlaps even when
    // resized narrow" report traced back to: PluginWindow::setEditor()
    // calls setConstrainer(editor->getBoundsConstrainer()) for a
    // sandboxed bridge, and CrateEditorComponent::getBoundsConstrainer()
    // always returns its OWN real, non-null ComponentBoundsConstrainer
    // member (CrateSandboxBridge.h) — confirmed via direct JUCE source
    // read (juce_ResizableWindow.cpp, ResizableWindow::setResizeLimits()):
    // "if you've set up a custom constrainer then these settings won't
    // have any effect" is backed by an actual jassert AND the real logic
    // (newMinimumWidth etc. are only ever written into defaultConstrainer,
    // which stops being the object wired to the native peer the instant
    // setConstrainer() swaps it out). Every previous call to
    // ownerWindow.setResizeLimits() here was therefore completely inert
    // for a sandboxed window — the floor was NEVER actually reaching the
    // OS-level drag constraint, which is the real reason the header could
    // still be crushed even though a floor had, in principle, already
    // been "set." Fix: reach whatever constrainer is ACTUALLY installed
    // (getConstrainer()) and call setSizeLimits() on IT directly —
    // setSizeLimits() is a plain, always-safe call on any
    // ComponentBoundsConstrainer instance, sidestepping the default-vs-
    // custom distinction entirely instead of trying to keep track of it.
    // Step 84 (White Box Diagnostics) directive, Round 2 — see
    // applyGeometry()'s own matching comment: getConstrainer() is
    // permanently null for a sandboxed window (allowWindowResizing()
    // always false — Step 65), so gating this log behind it (the first
    // version of this diagnostic did) meant it never printed regardless
    // of whether this method ran. setSizeLimits() itself genuinely IS a
    // no-op when there's no constrainer installed to call it on — logging
    // that fact unconditionally is itself the finding, not something to
    // hide behind the same null check that causes it.
    if (auto* activeConstrainer = ownerWindow.getConstrainer())
        activeConstrainer->setSizeLimits (minWidth, minHeight, maxWidth, maxHeight);

    CrateSandboxBridge::logToSharedLog (
        "DIAG WHITE BOX: enforceResizeLimits() probedMin=" + juce::String (probedMinWidth) + "x" + juce::String (probedMinHeight)
        + " probedMax=" + juce::String (probedMaxWidth) + "x" + juce::String (probedMaxHeight)
        + " everProbedFixedSize=" + juce::String ((int) everProbedFixedSize)
        + " wanted constrainer set to min=" + juce::String (minWidth) + "x" + juce::String (minHeight)
        + " max=" + juce::String (maxWidth) + "x" + juce::String (maxHeight)
        + " (constrainer=" + (ownerWindow.getConstrainer() != nullptr ? juce::String ("present") : juce::String ("NULL — call above was a no-op")) + ")");
}

// Step 41 (Geometry Loop Fix) directive — THE ROOT CAUSE of the reported
// "black void / infinite resize loop": Step 40's version of this had TWO
// INDEPENDENT authorities both trying to keep the OUTER PluginWindow sized
// correctly at once — (1) JUCE's own implicit resizeToFitWhenContentChangesSize
// cascade (still active, listening for THIS wrapper's own size changes) and
// (2) an explicit ownerWindow.setContentComponentSize() call made
// alongside it. Both compute "content size + some border," but JUCE's
// OWN internal cascade and setContentComponentSize() are NOT guaranteed to
// agree on which border figure to use — DocumentWindow overrides
// getContentComponentBorder() specifically to account for its title bar,
// while the base ResizableWindow's own resize-to-fit bookkeeping does not
// consistently route through that same override. Two authorities disagreeing
// by even the title-bar's own height is enough to oscillate forever: one
// mechanism grows the window, which resizes the content, which the OTHER
// mechanism reads as "wrong" and shrinks back, forever — exactly the
// "expanding wildly" / "black void" behaviour observed with a plugin
// (VoxDucker) that resizes ITSELF internally, repeatedly, in a way that
// keeps re-triggering this cycle.
//
// FIXED by removing one of the two authorities entirely: resizeToFitWhenContentChangesSize
// is now FALSE (see setEditor()'s own setContentNonOwned() call) — JUCE's
// implicit cascade never runs at all. THIS method is now the ONE AND ONLY
// place that ever computes a target size and applies it, to BOTH this
// wrapper (so it visually lays out correctly) AND the outer window
// (via setContentComponentSize(), which "adds on the borders" itself, per
// its own doc comment — no manual border arithmetic to get wrong here).
// Called from three places, all funnelled through this single path:
// applyInitialGeometry() (construction), componentMovedOrResized() (the
// editor resizing itself), and setHeaderVisible() (the Gear toggle).
void PluginWindow::PluginWindowContent::applyGeometry()
{
    const int stripHeight  = headerVisible ? PluginWindowHeader::headerHeight : collapsedStripHeight;
    const int wantedWidth  = editor.getWidth();
    const int wantedHeight = editor.getHeight() + stripHeight;

    // Step 84 (White Box Diagnostics) directive, Round 2 — QA finding on
    // the FIRST version of this instrumentation: it logged AFTER the
    // idempotence early-return below, AND gated behind
    // ownerWindow.getConstrainer() != nullptr. That constrainer is
    // permanently null for every sandboxed window — CrateEditorComponent::
    // allowWindowResizing() unconditionally returns false (Step 65's own
    // Pure Follower design: the Host never offers its own resize handle),
    // so PluginWindow::setEditor()'s own
    // "if (editor->allowWindowResizing()) setConstrainer(...)" never runs
    // for this class of window — setConstrainer(nullptr), called
    // unconditionally at the top of setEditor(), is the last word for the
    // window's entire lifetime. That silenced the log on EVERY call, not
    // just the first, regardless of whether applyGeometry()'s own resize
    // logic actually ran — zero DIAG WHITE BOX lines was evidence about
    // this diagnostic's own bug, not about applyGeometry() never being
    // called. Logging unconditionally, on entry, before either the
    // idempotence return or anything constrainer-related, closes that gap
    // for good.
    CrateSandboxBridge::logToSharedLog (
        "DIAG WHITE BOX: applyGeometry() ENTRY wanted=" + juce::String (wantedWidth) + "x" + juce::String (wantedHeight)
        + " lastApplied=" + juce::String (lastAppliedGeometryWidth) + "x" + juce::String (lastAppliedGeometryHeight)
        + " editor.real=" + juce::String (editor.getWidth()) + "x" + juce::String (editor.getHeight())
        + " ownerWindow.before=" + juce::String (ownerWindow.getWidth()) + "x" + juce::String (ownerWindow.getHeight()));

    // Step 63 (Break The Host-Side Async Loop) directive — the ACTUAL
    // root cause of the reported flicker/oscillation, confirmed via the
    // Step 62 stack-audit logs (a genuine, non-converging 2px ping-pong
    // between the outer window's size and a CHILD-reported one, NOT a
    // C++ stack overflow — reentrancy depth never exceeded 1 anywhere in
    // the captured log). ownerWindow.setContentComponentSize() below used
    // to run UNCONDITIONALLY on every single call to this method — every
    // time editor's own bounds changed even slightly (including a
    // CHILD-report round trip that itself only happened BECAUSE this
    // exact call previously nudged the outer window and its border
    // accounting landed a pixel or two off), this fired straight back
    // into the outer window's own resize cascade with no memory of
    // whether that exact target had already just been applied. A strict
    // idempotence check — mathematically identical wanted size means
    // return immediately, touch NOTHING — is what actually breaks a loop
    // like this: no setSize(), no setContentComponentSize(), no
    // re-entry into PluginWindowContent::resized()/editor.setBounds(),
    // regardless of how many times something upstream calls this with
    // the same numbers.
    if (wantedWidth == lastAppliedGeometryWidth && wantedHeight == lastAppliedGeometryHeight)
        return;

    lastAppliedGeometryWidth  = wantedWidth;
    lastAppliedGeometryHeight = wantedHeight;

    if (getWidth() != wantedWidth || getHeight() != wantedHeight)
        setSize (wantedWidth, wantedHeight); // triggers resized() above, laying out the strip/editor split

    ownerWindow.setContentComponentSize (wantedWidth, wantedHeight);

    const auto* activeConstrainer = ownerWindow.getConstrainer();

    juce::String nativeRectDiag = "n/a";

   #if JUCE_WINDOWS
    // Step 84 (White Box Diagnostics) directive, Round 3 — QA finding: the
    // user's own visual observation (the on-screen window looks far
    // bigger than what this log's ownerWindow.after= claims) means either
    // JUCE's Component-level bookkeeping has genuinely diverged from the
    // REAL native HWND, or something else entirely is being looked at
    // (e.g. a stale, orphaned PluginWindow from a previous
    // Watchdog-respawn/editor-recreation generation still sitting on
    // screen, never closed). Reading the actual native window rect via
    // Win32 GetWindowRect directly — bypassing JUCE's own bookkeeping
    // entirely — is the one measurement that can tell these apart: if it
    // matches ownerWindow.after=, the OS window genuinely is that size and
    // whatever looks bigger on screen is a SEPARATE window; if it doesn't
    // match, JUCE's logical bounds and the real native window have
    // actually diverged.
    if (auto* peer = ownerWindow.getPeer())
    {
        if (auto hwnd = (HWND) peer->getNativeHandle())
        {
            RECT windowRect {}, clientRect {};
            GetWindowRect (hwnd, &windowRect);
            GetClientRect (hwnd, &clientRect);

            nativeRectDiag = "windowRect=" + juce::String (windowRect.right - windowRect.left) + "x" + juce::String (windowRect.bottom - windowRect.top)
                                 + " clientRect=" + juce::String (clientRect.right - clientRect.left) + "x" + juce::String (clientRect.bottom - clientRect.top)
                                 + " at screen(" + juce::String (windowRect.left) + "," + juce::String (windowRect.top) + ")";
        }
    }
   #endif

    CrateSandboxBridge::logToSharedLog (
        "DIAG WHITE BOX: applyGeometry() APPLIED wanted=" + juce::String (wantedWidth) + "x" + juce::String (wantedHeight)
        + " thisContent.after=" + juce::String (getWidth()) + "x" + juce::String (getHeight())
        + " ownerWindow.after=" + juce::String (ownerWindow.getWidth()) + "x" + juce::String (ownerWindow.getHeight())
        + " nativeHWND[" + nativeRectDiag + "]"
        + " constrainer=" + (activeConstrainer != nullptr ? ("min=" + juce::String (activeConstrainer->getMinimumWidth()) + "x" + juce::String (activeConstrainer->getMinimumHeight())
                                                              + " max=" + juce::String (activeConstrainer->getMaximumWidth()) + "x" + juce::String (activeConstrainer->getMaximumHeight()))
                                                           : juce::String ("NULL")));
}

std::unique_ptr<juce::Component> PluginWindow::create (te::Plugin& plugin)
{
    if (auto externalPlugin = dynamic_cast<te::ExternalPlugin*> (&plugin))
        if (externalPlugin->getAudioPluginInstance() == nullptr)
            return nullptr;

    std::unique_ptr<PluginWindow> w;

    {
        // Blocks input to the rest of the app for the brief moment the plugin's
        // editor is being created, matching TE's own reference implementation.
        struct Blocker : public juce::Component { void inputAttemptWhenModal() override {} };
        Blocker blocker;
        blocker.enterModalState (false);

        w = std::make_unique<PluginWindow> (plugin);
    }

    if (w == nullptr || w->editor == nullptr)
        return {};

    w->show();
    return w;
}

void PluginWindow::moved()
{
    if (updateStoredBounds)
    {
        plugin.windowState->lastWindowBounds = getBounds();
        plugin.edit.pluginChanged (plugin);
    }
}
