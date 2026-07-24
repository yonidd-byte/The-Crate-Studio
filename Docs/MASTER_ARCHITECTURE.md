<!-- LLM DIRECTIVE: THIS FILE IS STRICTLY READ-ONLY. DO NOT MODIFY, SUMMARIZE, OR OVERWRITE ANY PART OF THIS DOCUMENT. -->
# THE CRATE STUDIO — MASTER ARCHITECTURE & ENGINE SPECIFICATION

**Version:** 3.0 (Unified Engine Phasing & UX Philosophy)
**Stack:** C++ / JUCE (GUI, device management) + Tracktion Engine (audio graph, DSP, VST3 hosting, ARA2)
**Status:** Canonical reference for Engine Phasing, Sandbox Architecture, and UX Directives. C++ Core Engine and Sandbox Architecture are FEATURE COMPLETE for V1 as of Phase 4. All future PRs are validated against this document.

---

## PART 1: VISION & UX PHILOSOPHY

### 1.1 The Thesis
We are building the most feature-complete DAW in the world — the routing depth of Pro Tools, the MIDI arsenal of Cubase, the piano roll and automation physics of FL Studio, and the immediacy of Ableton Live — **inside a UI that feels lighter than any of them.**

Every DAW that has attempted the "do everything" mission has collapsed under its own toolbar. The failure mode is always the same: features earn permanent screen real estate the moment they ship, and after 200 features the user is navigating a cockpit instead of making music. **We reject this outright.**

> **The Prime Directive:** A feature's power must be inversely proportional to its idle visual footprint. The deeper the tool, the more aggressively it must hide until summoned.

### 1.2 The Four Laws of the Interface
These are non-negotiable. Any PR that violates one is rejected on sight, regardless of how good the DSP behind it is.

*   **Law I — Strict Single-Window Paradigm:** There are no "Workspaces." The environment never reloads, never flashes, never re-lays-out. Panels animate visibility; they are never destroyed and rebuilt. No modal dialogs for anything a user does more than once a week.
*   **Law II — Progressive Disclosure via Collapsible Panels:** Advanced modules (Control Room, Setlist Manager, Cloud Studio) live in edge-docked side panels, toggled by minimal, unlabeled-until-hover edge buttons. Default state is collapsed. 
*   **Law III — Context-Aware Menus Over Toolbars:** Deep features are summoned where they apply, not from a global tool rack. ARA2, Warping, and Slicing are right-click actions on the specific clip/track.
*   **Law IV — The Hybrid Device & Mixer Paradigm:** We support production and mixing natively.
    *   **Production:** The Universal Bottom Device Chain holds MIDI FX, Instruments, and Audio Effects (Ableton style).
    *   **Mixing:** Traditional Console Inserts in the Mixer hold *only* Audio Effects (Pro Tools/SSL style). 
    *   These are two views of the exact same underlying `te::Edit` DSP graph.

### 1.3 Screen Anatomy
```text
┌──────────────────────────────────────────────────────────────────────────┐
│  TRANSPORT BAR  ▸ play/rec · tempo · MIDI LED · CPU · [LIVE MODE]        │
├────────┬──────────────────────────────────────────────────────┬──────────┤
│        │                                                      │          │
│ THE    │           ARRANGEMENT / MIXER                        │  ADVANCED│
│ CRATE  │           (crossfade view — never a reload)          │  PANELS  │
│ BROWSER│                                                      │  (all    │
│        │           · tracks, clips, automation lanes          │  collapsed│
│ (dock  │           · nested Arrangement Blocks                │  by      │
│  left) │           · scratch pads, global chord track         │  default)│
│        │                                                      │          │
├────────┴──────────────────────────────────────────────────────┴──────────┤
│  UNIVERSAL DEVICE CHAIN  ▸ [Synth]▸[Rack ▾]▸[EQ]▸[Comp]▸[+]   [fold all] │
└──────────────────────────────────────────────────────────────────────────┘
PART 2: CORE ENGINE & SANDBOX ARCHITECTURE (THE BACKEND)
2.0 Cross-Platform Strategy & macOS Porting
Current Target: Windows 10/11 (Win32/HWND) serves as the primary R&D environment.
macOS Directive: The core C++ engine, te::WaveNode extraction, and IPC logic are inherently cross-platform. Once the Windows version hits RC status, the macOS port will translate Win32 auto-reset events to POSIX Condition Variables, and replace HWND Reparenting with Apple-compliant NSView bridging via XPC Services.

Phase 1: Core Isolation & GUI/DSP Decoupling (100% DONE)
IPC & Audio Bridge: High-performance inter-process communication pipeline established.

Shared Memory Sync: Sample-accurate synchronization relying on OS-level shared memory to eliminate memory-copy overhead.

Scatter-Gather & Yielding: Proven stability running 50+ concurrent child processes.

GUI/DSP Decoupling (HWND Reparenting): Absolute segregation of rendering and processing. Plugin UIs are reparented and embedded natively into the DAW's window structure.

Phase 2: Nervous System, Time-Slip Engine & Immortality (100% DONE)
Continuous State Sync (Muscle Memory): The VST Chunk is extracted continuously to the Parent DAW upon endEdit triggers.

Isolated Lookahead Extraction (Alien Tech): A parallel te::WaveNode graph extracts raw audio blocks at future timestamps, bypassing the live te::Edit state.

SSD Ring Buffer (Time-Slip): Extracted future audio cached in a lock-free Ring Buffer.

The Watchdog (The Guillotine): Strict 1000ms timeout on the Lookahead pipeline. Hangs result in immediate Child process termination.

Auto-Demotion: Immediate failover to real-time Live Mode without audio dropouts upon Guillotine events.

Phase 3: The Crate Brain & Smart Confinement (100% DONE)
PluginHealthRegistry: Local, persistent JSON registry tracking crashCount, unsafeForLookahead, and vendorName.

The Warden (SandboxRouter) — Optimistic Routing:

Shared Sandbox: Plugins with crashCount == 0 route here by default.

Solitary Confinement: Forced isolation ONLY if the plugin has crashed on this machine.

Cryosleep Warm Pool: Pre-launched, idle CrateSandbox.exe processes wait fully booted but unclaimed, achieving zero process-creation latency on drag-and-drop.

Phase 4: Sandbox UI Integration & Graphic Polish (100% DONE)
Geometry Polish: Eradication of "gray boxes" via precise Client Area calculations (juce::HWNDComponent's pixel-precise SetWindowPos).

Dynamic Resize (IPlugView) & Snap-Back: Child continuously republishes editor bounds over IPC. Host-initiated resizing forces exact dimensions to the Child, applied through ComponentBoundsConstrainer, snapping back to confirmed applied dimensions.

Fixed-Size UX Lock: IPlugView::canResize() intercepts resize logic; disables host UI grip if false.

Workspace Bounds Clamping: Window explicitly clamped to Desktop::Displays::userBounds.

DPI Bridge: juce::NativeScaleFactorNotifier pushes real-time monitor scale to the Child via IPC.

Editor View Recovery Guard: Heuristics (Snap-Back fingerprinting & HWND Responsiveness Ping) to self-heal frozen UIs without interrupting DSP.

Phase 5: The Debt Sweep — Hardening & Optimization (100% DONE)
Kernel-Level Teardown (Zero-Zombies): Process-wide Win32 Job Object (CreateJobObjectW) ensures instant OS-level termination of the Cryosleep pool on Parent exit/crash.

Real-Time Thread Safety: Strict spinlock-acquire tracking in StateExtractionThread prevents data races with processBlock().

Cache-Line Alignment: alignas(64) separation of heartbeatCounter from IPC audio round-trip handshakes completely eliminates false sharing.

Zero-Allocation Scrub: LookaheadProducerThread utilizes pre-allocated persistent te::MidiMessageArray to eliminate dynamic memory allocation.

PART 3: DAW MODULES & WORKFLOW (THE FRONTEND)
3.1 The Crate Brain — Plugin & Sample Intelligence (Browser)
Ontology DB & Semantic Search: Background scan categorizes 9,000+ plugins. Typing "Opto" surfaces all optical compressors regardless of name.

Auto-Routing Logic: Dragging a sidechain comp auto-proposes a route.

Right-Click FAV Engine: Top 5 favorite plugins contextually appear on right-click.

UI Implementation: The left dock. No modal wizard. Instantiation is drag-and-drop only.

Lightning Sample Indexing: Instant indexing of sample libraries. Browser previews auto-sync to project BPM and key.

Auto-Tagging: Samples classified (Kick, Foley) from filename and content analysis.

The Crate Favorites: One unified virtual folder aggregating every FAV sample across every drive.

3.2 Racks & Macros — The Chain & Map System
Containers & Parallel Chains: Wrap synths and FX into a single savable object with internal parallel splits.

16 Macro Mapping: One knob controls multiple parameters with min/max value scaling and inversion.

UI Implementation: Racks render as one device-width header. Internals open in-place. Macro mapping is a mode, not a pop-up.

3.3 Universal Automation — Hybrid Manager
FL Physics & Ableton Accessibility: Power curves, Tension, and advanced shapes computed directly to DSP. Auto-reads every exposed VST3 parameter.

In-Track Automation Overlay: Curves render directly over clips (Ableton style) in CrateColors::NeonBlue, dimming underlying clips (0.35f alpha). Track height does not change.

Show Automated Only: One filter button to hide dead parameters.

3.4 Audio Engine, Pitch & Sample Editor
Smart Warping & zplane: Surgical time-stretch directly from the Arrangement.

Vocal Tuning (Piano Roll): Audio pitch/formant editing rendered as note blocks in the piano roll.

Native ARA 2: Deep integration via Tracktion Engine.

UI Implementation: 100% context-menu driven. Warping, slicing, and ARA2 appear on clip right-click.

Advanced Sample Editor (Slicing): Loop slicing to peaks with automatic keyboard mapping to MIDI (Edison/Simpler style).

Spectrogram View: Frequency view for surgical repair; polyphonic Glide support inside the sampler.

3.5 Arrangement Architecture
Arrangement Blocks: Nested clips with Ghosting (edit one, all update).

Swipe-to-Comp: Sub-lanes auto-generate on loop record.

Scratch Pads & Global Chord Track: Secondary timelines for testing, and a master lane governing MIDI/Audio sync.

3.6 The MIDI Suite
FL-Grade Piano Roll & MPE: Deep routing passed natively to VST3.

MIDI Devices: Arp, Scale, Note Echo live directly in the bottom Device Chain.

Unified Modulation: Drag an LFO's output dot onto any parameter in the chain (Bitwig style).

Logical Editor: Visual programming for MIDI rules (collapsible right-side panel).

MIDI Feedback LEDs: Ableton-style visual indicators (LED blinks) in the top transport bar and on individual track meters whenever MIDI data is received, ensuring absolute confidence during live routing.  

CTRL+M Mapping Mode: A dedicated mapping mode to assign physical MIDI controllers to any UI parameter. Strictly includes Value Scaling/Limiting (Min/Max boundaries, e.g., restricting a CC knob mapped to Delay Time to a 20ms-300ms range).  

Expression Maps: Articulation management for strings and symphonic libraries (Legato, Pizzicato).

3.7 Live Performance Engine
Action Markers & Setlist Manager: Markers execute commands (Auto-Pause, Loop Region).

Bulletproof Live Mode: Locks samples to RAM, suspends background processing, guarantees zero dropouts. Engaged via a single transport toggle.

3.8 Pro Industry Standards
Dolby Atmos: Native 7.1.4 routing with a 3D Object Panner.

Cloud Studio: Remote sync directly into the timeline with channel-locking.

Anticipative FX: Pre-rendering of non-armed tracks across all cores.

Retrospective MIDI Record: Permanent silent capture buffer.

API Wrapper (The Endgame Ecosystem): Exposes a safe, sandboxed scripting API (Python/Lua). Users can write custom scripts to manipulate project data, trigger renders, and build generative MIDI tools without altering the core C++ source code.

The Control Room: Mixer zone for room calibration (Sonarworks) and headphone management — never printed to Export.

Hardware Insert Ping: Internal plugin that measures external analog hardware round-trip delay and auto-compensates (PDC).

3.9 Tempo Intelligence
Detected Tempo & Auto-Stretch: Automatic BPM analysis of dragged loops; seamless conform to project tempo.

Tempo Tap Analyzer: Filters human timing deviation for maximum tap accuracy.

Time Signature Automation: Dynamic meter changes with full visual grid adaptation.

PART 4: NON-NEGOTIABLE INVARIANTS & ROADMAP
4.1 Strict Invariants
Strict thread separation: Zero dynamic memory allocation on the audio thread.

Everything is undoable: Every mutation passes a real UndoManager.

Everything persists: Tracks, plugin state, and curves round-trip through .crate ValueTree serialization via CrateWorkflowManager.

The GUI never blocks: Scanning, indexing, and analysis run on background threads.

Frictionless I/O: Auto-memory of the last used ASIO/CoreAudio interface, connecting seamlessly on startup with zero dialogs or user friction.  

4.2 Roadmap Reality Check
P0 — Foundation Repair: ✅ Done. Project save/load, Undo/Redo, serialization of crateAnchors.

P0.5 — Chrome Overhaul: ✅ Done. Icon transport, zoomable grid, real waveforms, drag-to-move, Pro Tools-grade mixer styling.

Phase 1-2 (Engine): ✅ Done. Core Sandbox and Time-Slip operations.

Phase 3 (Mixer/Browser): 🟡 Well advanced. Drag-and-drop instantiation, Universal Device Chain, Inspector/Mixer ValueTree synchronization, and SendBus routing are fully functional. Semantic search/auto-tagging pending.

Phase 4 (MIDI): 🟡 Core editor wired (activeMidiClip->getSequence()). Deep MPE and Articulation logic pending.

Phase 5 (Automation): 🟡 Curve physics and In-Track Overlay implemented. ARA2, Multi-Core pooling pending.

Phase 6 (Live/Scripting): 🔴 Not started.

4.3 The One-Sentence Test
Before any feature ships, it must answer this:

"Where does this live when the user isn't using it?"

If the answer is "in the toolbar" — redesign it.
If the answer is "a collapsed panel," "a right-click menu," "a folded device header," or "nowhere, it's just engine behavior" — ship it.

---
## PART 5: ENGINE GAP CLOSURE — MANDATORY FOR V1
### 5.1 Plugin Delay Compensation — Full-Graph Solver (P0, blocking)
* Graph-wide topological solve: Compute per-node cumulative latency over the entire `te::Edit` graph.
* Dynamic latency changes without dropout: Re-run and crossfade the graph swap rather than hard-switching when VST3s report `kLatencyChanged`.
* Sandbox latency accounting: IPC pipeline depth must be constant and included in the solve.
* Guillotine demotion is a latency event: Auto-demotion to Live Mode changes effective path latency and must trigger a crossfaded re-solve.
### 5.2 Multi-Core Audio Graph Scheduling (P0, blocking)
* Node-level thread pool: Adopt tracktion_graph's multi-threaded player.
* IPC must never block a pool thread: A sandboxed node parks on its own completion signal; the pool thread steals other ready nodes.
* Chain co-location: The Warden must prefer placing an entire track's chain in *one* shared sandbox process to minimize round-trips.
### 5.3 Time-Slip Cache Coherence & Freeze
* Invalidation rules: Any automation edit, parameter change, clip edit, or routing mutation invalidates the ring buffer from the mutation timestamp forward.
* Non-deterministic plugins: Plugins with free-running LFOs or sidechains must bypass Lookahead.
* Offline export never reads the Time-Slip cache.
* Track Freeze (P0): Offline render of the track through its chain to a temp file + chain suspension.
### 5.4 MIDI 2.0 / UMP & Project Durability
* Internal event representation = UMP (Universal MIDI Packet) with 32-bit controller resolution.
* Project Durability: Implement atomic saves, rotating timed autosaves, and a crash journal for recovery alongside continuous chunk extraction.
---
## PART 6: RENDERING PIPELINE DIRECTIVES (THE 120FPS LOCK-IN)
### 6.1 Backend Decision
* Windows: JUCE 8 Direct2D renderer. Locked. No `juce::OpenGLContext` attached to the window as a general renderer.
### 6.2 Repaint Discipline 
* `setOpaque(true)` on every panel, meter, and track lane.
* Playhead: Its own 2px overlay component.
* Meters: Dedicated opaque components fed by a lock-free atomic FIFO, repainted via a single shared `juce::VBlankAttachment`.
* `setBufferedToImage(true)` on static chrome (browser cards, device headers).
* No per-frame shadows or glows: Pre-render Neon Blue hover glows and shadows once into cached `juce::Image` sprites (`CrateSprites`).
* Waveforms: Multi-resolution peak pyramid rendered into per-clip tiled images.