#pragma once

// Step 73 (Airlock HWND) directive — QA finding, confirmed by reading
// JUCE's own juce_HWNDComponent_windows.cpp directly rather than guessing:
// HWNDComponent::Pimpl::componentMovedOrResized()/componentPeerChanged()
// call SetParent()/SetWindowPos()/InvalidateRect() against the reparented
// child HWND, and those calls run on WHATEVER THREAD triggers a JUCE
// Component bounds/peer change — in this codebase, unambiguously the JUCE
// message thread (CrateEditorComponent::resized()/timerCallback(), both
// message-thread-only). SetWindowPos/SetParent are NOT fire-and-forget
// when the target HWND belongs to a DIFFERENT thread (here: a different
// PROCESS entirely) — Windows synchronously delivers WM_WINDOWPOSCHANGING/
// CHANGED-class notifications to the TARGET window's own owning thread as
// part of the call, and the CALLER blocks until that thread's message
// pump services them. A busy or wedged CHILD (Melda's own heavy
// synchronous grid recalculation, established fact this whole session)
// not pumping promptly means these calls block the JUCE message thread
// directly — a real, documented Win32 hazard, independent of whatever
// exact OTHER thread the captured .dmp's MessageManagerLock wait
// ultimately traces back to.
//
// This class exists to make sure the JUCE message thread NEVER again
// issues a synchronous Win32 call against a cross-process child HWND.
// Every such call (SetParent, SetWindowPos, InvalidateRect) is moved onto
// a single dedicated OS thread (raw _beginthreadex, not juce::Thread —
// this thread's entire reason to exist is owning its own private Win32
// message queue, decoupled from JUCE's), which owns one small invisible
// "slot" window per reparented plugin editor. The JUCE side only ever
// POSTS a request to this thread's queue and returns immediately;
// whatever a wedged plugin makes this thread wait on stays confined here,
// never propagating back to the real UI thread.
//
// One exception, deliberately: createSlot() below blocks its OWN caller
// (the JUCE message thread) for a BOUNDED wait — the caller needs the
// resulting slot id back before it can do anything else, and creating a
// window is normally fast. Bounded, not indefinite, per the user's own
// explicit "graceful teardown, no deadlock" requirement.

#include <windows.h>
#include <objbase.h>
#include <process.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include "FlightRecorder.h"

class SandboxAirlock
{
public:
    static SandboxAirlock& getInstance()
    {
        static SandboxAirlock instance;
        return instance;
    }

    // Step 79 (Pre-Emptive Native Parenting) directive — QA finding: the
    // Step 75/76 approach (create the CHILD's editor as its own floating
    // top-level window, THEN reparent + forcibly strip its styles
    // afterward) survived the lifetime bug fix but never actually solved
    // the domain problem — a heavy VST3's own Direct2D/OpenGL swap chain
    // is set up against whatever window it was ORIGINALLY created for,
    // and a later SetParent() doesn't retroactively fix that. The correct
    // fix is upstream of all of it: give the CHILD the slot's HWND value
    // BEFORE it ever creates its editor, so JUCE's own
    // AudioProcessorEditor::addToDesktop(0, slotHwnd) calls CreateWindowEx
    // with the slot as hWndParent from the very first frame (confirmed by
    // reading juce_Windowing_windows.cpp directly: parentToAddTo != nullptr
    // makes JUCE itself set WS_CHILD, with no WS_POPUP/WS_CAPTION/
    // WS_THICKFRAME/WS_SYSMENU ever applied in the first place — nothing
    // to strip afterward because the wrong styles are never set at all).
    // This slot is consequently now created EMPTY, with nothing to
    // reparent into it — see CrateSandboxBridge::publishHostSlotHandle()
    // for how its raw HWND value reaches the CHILD over IPC, and
    // Main.cpp's own addToDesktop() call for the CHILD side of this.
    struct PendingSlotResult
    {
        std::atomic<bool> ready { false };
        std::atomic<uint64_t> slotId { 0 };
        std::atomic<int64_t> slotHwndValue { 0 };
    };

    std::shared_ptr<PendingSlotResult> requestSlotCreationAsync (void* hostPeerHwnd)
    {
        auto result = std::make_shared<PendingSlotResult>();

        if (! ensureThreadRunning())
        {
            result->ready.store (true, std::memory_order_release); // slotId/slotHwndValue stay 0 — "ready, and it failed"
            return result;
        }

        // Step 76 (Airlock Lifetime Safety) directive: still shared-
        // ownership across the PostThreadMessageW boundary — see
        // handleCreateSlot()'s own doc comment. Necessary since there's
        // no bounded wait left to guarantee anything about when (or
        // whether, from this caller's own point of view) the airlock
        // thread actually gets to this request.
        auto request = std::make_shared<CreateSlotRequest>();
        request->hostPeerHwnd = (HWND) hostPeerHwnd;
        request->result       = result;

        auto* messageOwnedRef = new std::shared_ptr<CreateSlotRequest> (request);
        PostThreadMessageW (threadId, MSG_CREATE_SLOT, 0, (LPARAM) messageOwnedRef);

        return result;
    }

    // Fire-and-forget. Never blocks the caller. Repositions/resizes the
    // slot window ONLY — the CHILD's own editor, being a genuine WS_CHILD
    // of the slot created from birth (Step 79), resizes ITSELF via its
    // own same-process JUCE Component::setBounds() call whenever the
    // plugin's own internal handle is dragged; no cross-process
    // SetWindowPos against the child HWND is needed for that any more.
    void requestBounds (uint64_t slotId, int x, int y, int width, int height)
    {
        if (slotId == 0 || threadId == 0)
            return;

        auto* bounds = new BoundsRequest { slotId, x, y, width, height };
        PostThreadMessageW (threadId, MSG_SET_BOUNDS, 0, (LPARAM) bounds);
    }

    // Fire-and-forget. The airlock thread detaches/destroys the slot
    // window (and whatever's still reparented under it) on its own
    // thread, in its own time — the caller doesn't wait for this, matching
    // every other teardown path in this codebase's own "never block on a
    // possibly-wedged resource" discipline.
    void destroySlot (uint64_t slotId)
    {
        if (slotId == 0 || threadId == 0)
            return;

        PostThreadMessageW (threadId, MSG_DESTROY_SLOT, 0, (LPARAM) slotId);
    }

    // Called once, at app shutdown (see CrateWorkflowManager's own
    // shutdown sequence). Posts a quit request and waits, BOUNDED, for the
    // thread to exit cleanly; TerminateThread is an absolute last resort,
    // logged as such, never a silent hang of process exit.
    void shutdown()
    {
        if (threadHandle == nullptr)
            return;

        PostThreadMessageW (threadId, WM_QUIT, 0, 0);

        if (WaitForSingleObject (threadHandle, 3000) != WAIT_OBJECT_0)
            TerminateThread (threadHandle, 1); // last resort — the airlock thread itself is wedged, not a sandboxed plugin; nothing left to do but stop waiting

        CloseHandle (threadHandle);
        threadHandle = nullptr;
        threadId = 0;
    }

    ~SandboxAirlock() { shutdown(); }

private:
    SandboxAirlock() = default;
    SandboxAirlock (const SandboxAirlock&) = delete;
    SandboxAirlock& operator= (const SandboxAirlock&) = delete;

    enum : UINT
    {
        MSG_CREATE_SLOT  = WM_USER + 1,
        MSG_SET_BOUNDS   = WM_USER + 2,
        MSG_DESTROY_SLOT = WM_USER + 4
    };

    // Step 76 (Airlock Lifetime Safety) directive: owned via shared_ptr on
    // both sides of the PostThreadMessageW crossing — see
    // requestSlotCreationAsync()'s own doc comment. Step 78 (Async Slot
    // Creation) removed the resultEvent HANDLE entirely along with the
    // blocking wait it existed to support — result (below) is how
    // handleCreateSlot() reports back now, with no HANDLE lifetime left
    // to manage at all. Step 79 (Pre-Emptive Native Parenting) removed
    // childPluginHwnd — the slot is created empty now; there is no child
    // HWND to reparent into it at creation time any more.
    struct CreateSlotRequest
    {
        HWND hostPeerHwnd = nullptr;
        std::shared_ptr<PendingSlotResult> result;
    };

    struct BoundsRequest { uint64_t slotId; int x, y, width, height; };

    // Step 79 directive: no more childHwnd — the slot is a plain empty
    // container the CHILD embeds its own editor into directly (see this
    // class's own top-of-file doc comment); nothing left for this class
    // to track about what's inside it.
    struct Slot { HWND slotHwnd = nullptr; };

    bool ensureThreadRunning()
    {
        if (threadHandle != nullptr)
            return true;

        std::lock_guard<std::mutex> lock (startupMutex);

        if (threadHandle != nullptr)
            return true;

        threadReady.store (false, std::memory_order_release);

        threadHandle = (HANDLE) _beginthreadex (nullptr, 0, &SandboxAirlock::threadEntryTrampoline,
                                                 this, 0, nullptr);

        if (threadHandle == nullptr)
            return false;

        // Bounded — same "never wait forever on our own infrastructure"
        // discipline as createSlot()'s own wait, below. The thread does
        // nothing but register a window class and pump messages before
        // signalling ready; if THAT hangs, the airlock itself is broken,
        // not a sandboxed plugin, and every caller needs to be able to
        // give up rather than freeze.
        const auto startMs = GetTickCount64();

        while (! threadReady.load (std::memory_order_acquire))
        {
            if (GetTickCount64() - startMs > 2000)
                return false;

            Sleep (1);
        }

        return true;
    }

    static unsigned __stdcall threadEntryTrampoline (void* self)
    {
        static_cast<SandboxAirlock*> (self)->runMessageLoop();
        return 0;
    }

    void runMessageLoop()
    {
        threadId = GetCurrentThreadId();

        // Step 77 (Thread Environment Initialization) directive, Task 1:
        // COM apartment initialization for this thread. Checked against
        // this codebase's own actual design before adding: the Airlock
        // thread itself never creates a COM object — its slot window is a
        // bare WS_CHILD container with a trivial DefWindowProc (no
        // drag-drop target, no Shell API, no Direct2D/WIC device of its
        // own) — and the CHILD's own Direct2D/OLE usage runs entirely on
        // the CHILD's OWN process and thread, unaffected by this
        // process's COM state. Added anyway as standard, low-risk hygiene
        // for any thread that creates and owns Win32 windows meant to
        // host third-party UI: COM apartment state is genuinely per-
        // thread (unlike DPI awareness — see Task 2's own comment), so a
        // thread spawned via raw _beginthreadex (bypassing whatever
        // ambient CoInitializeEx JUCE's own message thread already has)
        // is the one place in this codebase that could ever matter.
        // CoUninitialize() is paired at the bottom of this SAME function,
        // right before the thread exits — COM requires the init/uninit
        // calls to run on the exact same thread.
        const HRESULT comResult = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        const bool comInitialized = SUCCEEDED (comResult);

        if (! comInitialized)
            CRATE_FR_LOGF ("AIRLOCK", "CoInitializeEx FAILED, hr=0x%08lX", (unsigned long) comResult);

        // Step 77 directive, Task 2: checked against this process's OWN
        // actual startup sequence before adding — JUCE's own
        // juce_Windowing_windows.cpp already calls
        // SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        // at PROCESS scope during this app's own startup (confirmed by
        // reading that file directly). Per-thread DPI awareness on
        // Windows 10 1607+ inherits the PROCESS-WIDE default unless a
        // thread explicitly opts OUT via its own call — nothing in this
        // codebase does that — so this Airlock thread was already
        // running at Per-Monitor-V2, same as every other thread in this
        // process, with no explicit call required. This line is
        // consequently closer to an explicit guarantee than a behavioural
        // fix for the reported symptom, but it's what protects this
        // thread's own window-creation calls if the process-wide default
        // were ever changed for an unrelated reason later.
        SetThreadDpiAwarenessContext (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (WNDCLASSEXW);
        wc.lpfnWndProc = &SandboxAirlock::slotWndProc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.lpszClassName = L"CrateSandboxAirlockSlot";

        // Step 76 (Airlock Lifetime Safety) directive: this call was
        // never actually the cause of the "createSlot FAILED" symptom
        // (RegisterClassExW here already runs exactly once, before the
        // thread ever signals ready or processes a single message — see
        // createSlot()'s own doc comment for the REAL root cause, a
        // stack-lifetime race), but its return value was never checked
        // either. Logging the real Win32 error here means a genuinely
        // different future failure is diagnosable from the flight
        // recorder instead of guessed at again.
        if (RegisterClassExW (&wc) == 0)
            CRATE_FR_LOGF ("AIRLOCK", "RegisterClassExW FAILED, GetLastError=%lu", GetLastError());

        // Forces the thread's own message queue into existence before
        // threadReady is signalled — PostThreadMessageW (used by every
        // caller above) silently fails if the target thread has never
        // called a message-retrieving function at least once.
        MSG msg;
        PeekMessage (&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        threadReady.store (true, std::memory_order_release);

        while (GetMessage (&msg, nullptr, 0, 0) > 0)
        {
            switch (msg.message)
            {
                case MSG_CREATE_SLOT:  handleCreateSlot  ((std::shared_ptr<CreateSlotRequest>*) msg.lParam); break;
                case MSG_SET_BOUNDS:   handleSetBounds   ((BoundsRequest*) msg.lParam);     break;
                case MSG_DESTROY_SLOT: handleDestroySlot ((uint64_t) msg.lParam);           break;
                default: break;
            }
        }

        // WM_QUIT (shutdown()) — tear down every slot still open rather
        // than leaking them when this thread exits. Step 79 directive: no
        // more child HWND to unparent first — the CHILD's own editor
        // (reparented into nothing, it just goes away as its own process
        // tears down) was never OUR responsibility to detach.
        std::lock_guard<std::mutex> lock (slotsMutex);

        for (auto& [id, slot] : slots)
        {
            if (slot.slotHwnd != nullptr)
                DestroyWindow (slot.slotHwnd);
        }

        slots.clear();

        // Step 77 directive, Task 1: paired with CoInitializeEx() at the
        // top of this same function, on this same thread, right before it
        // exits.
        if (comInitialized)
            CoUninitialize();
    }

    void handleCreateSlot (std::shared_ptr<CreateSlotRequest>* messageOwnedRef)
    {
        // Step 76 (Airlock Lifetime Safety) directive: reclaim this
        // thread's own share of ownership immediately — request now keeps
        // the underlying CreateSlotRequest alive for the rest of this
        // function regardless of whatever createSlot()'s own caller does
        // (already returned via timeout, still waiting, or already woken
        // up) — see createSlot()'s own doc comment for the full lifetime
        // reasoning this replaces.
        std::shared_ptr<CreateSlotRequest> request = *messageOwnedRef;
        delete messageOwnedRef; // just the wrapper pointer used to cross PostThreadMessageW — the shared_ptr's own refcount is untouched

        HWND slotHwnd = CreateWindowExW (0, L"CrateSandboxAirlockSlot", L"", WS_CHILD,
                                          0, 0, 1, 1, request->hostPeerHwnd, nullptr,
                                          GetModuleHandleW (nullptr), nullptr);

        if (slotHwnd != nullptr)
        {
            // Step 79 (Pre-Emptive Native Parenting) directive: the slot
            // is created empty — no reparenting, no style stripping, no
            // SWP_FRAMECHANGED needed here any more. The CHILD embeds its
            // OWN editor into this slot from birth (JUCE's own
            // AudioProcessorEditor::addToDesktop(0, slotHwnd) call —
            // Main.cpp's own doc comment on this) via a normal, correct,
            // same-process CreateWindowEx with WS_CHILD already set by
            // JUCE itself. This slot window's only remaining job is being
            // a visible, correctly-positioned container the CHILD's
            // editor happens to be a child of.
            ShowWindow (slotHwnd, SW_SHOWNA);

            const uint64_t id = nextSlotId.fetch_add (1, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lock (slotsMutex);
                slots[id] = { slotHwnd };
            }

            // Step 78 (Async Slot Creation) directive: written BEFORE
            // ready is flagged — a poller (AirlockHWNDComponent) that
            // observes ready==true is guaranteed to see these values too,
            // same happens-before contract release/acquire on ready
            // always provided.
            request->result->slotId.store (id, std::memory_order_relaxed);
            request->result->slotHwndValue.store ((int64_t) (intptr_t) slotHwnd, std::memory_order_relaxed);

            // Step 74 (The Flight Recorder) directive: the actual slot
            // creation event — exactly what a future investigation needs
            // to see happened, and when.
            CRATE_FR_LOGF ("AIRLOCK", "slot %llu created — hwnd 0x%p, awaiting the CHILD's own pre-emptive embed",
                            (unsigned long long) id, slotHwnd);
        }
        else
        {
            // Step 76 directive: the real Win32 error, not just the fact
            // of failure — this was never actually a missing
            // RegisterClassExW (that runs exactly once, well before this
            // point — see runMessageLoop()'s own comment); it was this
            // exact request's own hostPeerHwnd being torn-down stack
            // garbage by the time this ran, which CreateWindowExW
            // correctly rejected as an invalid parent handle. GetLastError()
            // here means a genuinely different future failure is
            // diagnosable instead of guessed at again.
            CRATE_FR_LOGF ("AIRLOCK", "createSlot FAILED - CreateWindowExW returned null, GetLastError=%lu, "
                                      "hostPeerHwnd=0x%p", GetLastError(), request->hostPeerHwnd);
        }

        // Step 78 directive: the release-store that actually publishes
        // this result to whichever thread is polling it (see
        // AirlockHWNDComponent::tryCreateSlotAndPushBounds()'s own
        // acquire-load) — replaces the old SetEvent()/WaitForSingleObject
        // pairing entirely; there is no HANDLE left to signal or close.
        request->result->ready.store (true, std::memory_order_release);

        // request (this function's own shared_ptr) goes out of scope at
        // the end of this function — see requestSlotCreationAsync()'s own
        // doc comment for why it's safe for either side to be the one
        // whose release actually destroys the underlying object.
    }

    void handleSetBounds (BoundsRequest* boundsRequest)
    {
        std::unique_ptr<BoundsRequest> owned (boundsRequest); // always free, even on an unknown/already-destroyed slot

        std::lock_guard<std::mutex> lock (slotsMutex);
        auto it = slots.find (owned->slotId);

        if (it == slots.end())
            return;

        // Step 79 (Pre-Emptive Native Parenting) directive: repositions
        // ONLY the slot window itself. The CHILD's own editor — a genuine
        // WS_CHILD of this slot from birth — resizes ITSELF via its own
        // same-process JUCE Component::setBounds() call whenever the
        // plugin's own internal handle is dragged; no second SetWindowPos
        // against a cross-process child HWND is needed here any more.
        SetWindowPos (it->second.slotHwnd, nullptr, owned->x, owned->y, owned->width, owned->height,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }

    void handleDestroySlot (uint64_t slotId)
    {
        std::lock_guard<std::mutex> lock (slotsMutex);
        auto it = slots.find (slotId);

        if (it == slots.end())
            return;

        // Step 74 (The Flight Recorder) directive: the teardown event,
        // matching createSlot's own log above.
        CRATE_FR_LOGF ("AIRLOCK", "slot %llu destroyed", (unsigned long long) slotId);

        // Step 79 directive: no child HWND to unparent first — destroying
        // the slot destroys whatever real WS_CHILD editor window the
        // CHILD embedded into it along with it (standard Win32 behaviour:
        // DestroyWindow recursively destroys child windows), same as any
        // other JUCE-owned native window tree tearing down.
        if (it->second.slotHwnd != nullptr)
            DestroyWindow (it->second.slotHwnd);

        slots.erase (it);
    }

    static LRESULT CALLBACK slotWndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return DefWindowProcW (hwnd, msg, wParam, lParam);
    }

    HANDLE threadHandle = nullptr;
    DWORD threadId = 0;
    std::atomic<bool> threadReady { false };
    std::mutex startupMutex;

    std::atomic<uint64_t> nextSlotId { 1 };
    std::mutex slotsMutex;
    std::unordered_map<uint64_t, Slot> slots;
};
