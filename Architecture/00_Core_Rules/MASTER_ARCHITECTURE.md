# THE CRATE STUDIO — MASTER ARCHITECTURE

**Version:** 1.0
**Stack:** C++ / JUCE (GUI, device management) + Tracktion Engine (audio graph, DSP, VST3 hosting, ARA2)
**Document owner:** Lead UI/UX Architect & Senior C++ Audio Developer
**Status:** Canonical reference. Supersedes prior PRD drafts. All phase work is validated against this document.

---

## 0. Vision & UX Philosophy

### 0.1 The Thesis

We are building the most feature-complete DAW in the world — the routing depth of Pro Tools, the MIDI arsenal of Cubase, the piano roll and automation physics of FL Studio, and the immediacy of Ableton Live — **inside a UI that feels lighter than any of them.**

This is not a contradiction. It is an engineering constraint.

Every DAW that has attempted the "do everything" mission has collapsed under its own toolbar. The failure mode is always the same: features earn permanent screen real estate the moment they ship, and after 200 features the user is navigating a cockpit instead of making music. **We reject this outright.**

> **The Prime Directive:** A feature's power must be inversely proportional to its idle visual footprint. The deeper the tool, the more aggressively it must hide until summoned.

### 0.2 The Four Laws of the Interface

These are non-negotiable. Any PR that violates one is rejected on sight, regardless of how good the DSP behind it is.

#### **Law I — Strict Single-Window Paradigm**
There are no "Workspaces." There is no "Mix Mode" vs "Edit Mode" vs "Beatmaking Mode." The environment **never reloads, never flashes, never re-lays-out.** A user moving from writing a hook to mixing the master bus experiences panels *sliding*, not a screen *changing*. State is continuous. The mental model is a single physical studio you turn around inside — not a set of rooms you walk between.

*Engineering consequence:* All top-level views are persistent `juce::Component` instances in a single window hierarchy. Panels animate visibility; they are never destroyed and rebuilt. No modal dialogs for anything a user does more than once a week.

#### **Law II — Progressive Disclosure via Collapsible Panels**
Advanced modules — Control Room, Setlist Manager, Cloud Studio, Spectrogram, Video Engine — live in **edge-docked side panels**, toggled by minimal, unlabeled-until-hover edge buttons. Default state for every advanced panel is **collapsed**. A first-run user sees a Browser, an Arrangement, a Mixer, and a Device Chain. That's it. The other 80% of the DAW is one click away and zero pixels wide.

#### **Law III — Context-Aware Menus Over Toolbars**
Deep features are summoned **where they apply**, not from a global tool rack. ARA2/Melodyne, Warping, Slicing, Harmonic Editing, Consolidate, Auto-Tag — all of these are right-click actions on the **specific clip or track** they operate on. The top transport bar holds transport, tempo, MIDI activity, and the Live Mode toggle. Nothing else. Ever.

*Rule of thumb:* If a tool only makes sense when something is selected, it does not belong in a toolbar.

#### **Law IV — The Hybrid Device & Mixer Paradigm**
Production and mixing require different mental models. We support both natively without mode-switching lag. The Prime Directive dictates that signal processing surfaces where the user expects it for their current task.

*   **Production (Arrangement View):** The Universal Bottom Device Chain. Holds *everything* for the selected track: MIDI FX, Instruments, and Audio Effects (Ableton style).
*   **Mixing (Mixer View):** Traditional Console Inserts. The Mixer strips hold *only Audio Effects* in a vertical insert rack (Pro Tools/SSL style).

*Engineering consequence:* These are two views of the exact same underlying te::Edit DSP graph. Adding an EQ to the Mixer Insert immediately populates it in the Device Chain, and vice versa. Instruments and MIDI FX do not render in the Mixer strips. The bottom panel dynamically adapts its content based on the active main view.

### 0.3 Screen Anatomy

```
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
│        │                                                      │  ▸Control│
│        │                                                      │   Room   │
│        │                                                      │  ▸Setlist│
│        │                                                      │  ▸Cloud  │
│        │                                                      │  ▸Video  │
├────────┴──────────────────────────────────────────────────────┴──────────┤
│  UNIVERSAL DEVICE CHAIN  ▸ [Synth]▸[Rack ▾]▸[EQ]▸[Comp]▸[+]   [fold all] │
└──────────────────────────────────────────────────────────────────────────┘
```

Four zones. Two of them collapse to zero. That is the whole DAW.

---

## 1. The Crate Brain — Plugin & Sample Intelligence

*The smartest plugin and sample engine in the world. This is our moat.*

### Features

| Feature | Description |
|---|---|
| **VST Master Scanner & Ontology DB** | Multi-threaded background scan of VST/VST3/CLAP directories, matched against a curated ontology of **9,000+ plugins**. Automatic deep-tree categorization (`Dynamics > Compressors > VCA`) with **zero manual tagging**. |
| **Semantic Smart Browser** | Intent-based search, not string matching. Typing `Opto` instantly surfaces every optical compressor installed on the machine — regardless of vendor naming. |
| **Auto-Routing Logic** | The system *understands* plugin function. Dragging a sidechain-capable compressor onto a track auto-proposes a sidechain route from the Kick bus. |
| **Smart Tiers (1/2/3)** | Usage-habit learning. Daily drivers stay one keystroke away; forgotten plugins are visually archived, not deleted. |
| **Right-Click FAV Engine** | Right-click any mixer channel → floating menu with the **top 5 favorite plugins for that specific category context**. |
| **Lightning Sample Indexing & Smart Preview** | Instant indexing of sample libraries. Browser previews are **auto-synced to project BPM and key** in real time. |
| **Auto-Tagging** | Samples classified (`Kick`, `808`, `Foley`) from filename *and* content analysis. |
| **"The Crate Favorites"** | One unified virtual folder aggregating every FAV sample across every drive. |

### How this fits the clean UI

The Crate Brain is the **left dock — and nothing else.** No wizard, no settings modal, no separate "plugin manager" application.

- **Idle state:** a single search field and a tree. Semantic search means the tree is usually irrelevant — the user types, not browses.
- **The scanner has no UI.** It runs on background threads at startup and reports only via a thin progress line at the bottom of the dock. It never blocks the GUI thread. Ever.
- **Auto-Routing surfaces as a proposal, never an action.** Drop a sidechain comp → a subtle inline prompt appears *on the device itself*: `Sidechain from ▸ Kick?`. Ignoring it costs zero clicks.
- **Smart Tiers are invisible ranking, not visible UI.** Tier 3 plugins simply sort last and render dimmed. No "tier" label pollutes the screen.
- **Instantiation is drag-and-drop only** — from the browser directly onto a mixer strip or into the bottom Device Chain. Double-click loads onto the selected track. There is no "Add Plugin" dialog in this product.

---

## 2. Racks & Macros — The Chain & Map System

*Containerization: turn arbitrary complexity into one object with 16 knobs.*

### Features

| Feature | Description |
|---|---|
| **Instrument / Drum / Effect Racks** | Build "containers" in the Device Chain that wrap synths and FX into a **single savable object** (preset-able as one unit). |
| **Parallel Chains** | Split audio/MIDI inside a Rack into multiple parallel paths (e.g. Low/High split into separate distortion stages). |
| **16 Macro Mapping** | 16 macro knobs per Rack. **One knob → many parameters**, simultaneously, across any device in the rack. |
| **Min/Max Value Scaling** | Full range control per mapping, including **Inverted Mapping** — one knob raising one parameter while lowering another. |

### How this fits the clean UI

Racks are the **primary anti-bloat weapon** in the entire architecture.

- A Rack renders in the bottom Device Chain as **one device-width header** with 16 knobs. Folded, it's a single title strip. The user's 14-device parallel monster occupies the same footprint as a single EQ.
- **Rack internals open *in-place*, inside the chain**, as an inset sub-chain — not in a new window, not in a new tab. Law I holds.
- **Macro mapping is a mode, not a screen.** Click the Macro icon on the Rack header → every automatable parameter in the chain glows. Click one to bind. Click Macro again to exit. Min/Max/Invert live in a tiny popover on the macro knob's right-click — never in a properties panel.

---

## 3. Universal Automation — Hybrid Automation Manager

*FL Studio's physics engine, Ableton's accessibility, Pro Tools' lane discipline.*

### Features

| Feature | Description |
|---|---|
| **FL Studio Physics** | Power curves, **Tension**, and advanced shapes (`Wave`, `Pulse`, stairs, S-curve) with phantom points for surgical slicing — computed and **baked directly to DSP**. |
| **Ableton-Style Parameter Dropdown** | A dropdown on every track that **auto-reads every exposed parameter of every loaded VST3**. No manual "parameter capture" ritual. |
| **Show Automated Only** | One filter button that hides dead parameters and displays exclusively what has actually been drawn. |
| **In-Track Automation Overlay** | Automation curves render **directly over the clips on the main track itself** (Ableton style) — never an expanded lane pushing content beneath the track. |
| **Automation Clips** | Automation is an **object** — sliceable, movable, copyable — plus Ableton-style block scale/offset dragging. |

### How this fits the clean UI

- **The dropdown solves the discoverability problem that would otherwise require a toolbar.** Every parameter of every plugin is one click from the track header. That's thousands of automatable targets with a zero-pixel idle cost.
- **`Show Automated Only` is the single most important UX button in this module.** A track with a 200-parameter synth is unreadable without it. It is **ON by default.**
- **The track header carries a dedicated automation-overlay toggle button**, sitting alongside `Show Automated Only`. Clicking it draws the selected parameter's curve **directly on top of the track's own clips** — no separate lane, no vertical growth, no rescale of the arrangement. Clicking it again removes the overlay; the track returns to showing only clip content. This is a strict Ableton-style overlay, not a Pro Tools expandable lane — the track's height never changes because automation is visible.
- Tension/curve editing happens **on the curve itself** — drag the midpoint handle, directly on the overlay. No curve-type dropdown, no properties inspector. Right-click the segment for exotic shapes (Wave/Pulse).

> **✅ Resolved (was blocking debt):** the prior version of this document flagged automation as hardcoded to a single Volume parameter with a `nullptr` `UndoManager`. Verified against current source — both are fixed. `AutomationLaneComponent::refreshParameterList()` iterates `track->getAllAutomatableParams()` and populates a real parameter-selector dropdown (any plugin parameter, disambiguated by device name; Volume is only the *initial* default when a lane is first toggled on), and every curve mutation routes through the real `edit.getUndoManager()`. No remaining `nullptr` UndoManager in this path.
>
> **⚠ Still open — spec vs. code:** the two rows/bullets above (`In-Track Automation Overlay`, the header toggle drawing curves *over* clips) describe the **target** design, not what's built. Current code (`ArrangementComponent`'s `setAutomationVisible()`) still shows/hides a genuine expandable lane beneath the track — toggling it changes the track row's height and reports that change up to the header column, the Pro Tools model this document is explicitly moving away from. `Show Automated Only` does not exist anywhere in source yet either. See Section 11 for the honest phase status.

---

## 4. Audio Engine, Pitch & Sample Editor

*Surgical audio manipulation without leaving the arrangement.*

### Features

| Feature | Description |
|---|---|
| **Smart Warping** | Automatic transient detection → anchor generation → surgical time-stretch **directly from the Arrangement**. |
| **zplane élastique Pro** | Industry-reference time-stretch core. Zero-artifact stretching. |
| **Vocal Tuning on the Piano Roll** | Monophonic audio pitch/formant editing rendered **as note blocks in the piano roll** — the same editor, not a separate tool. |
| **Native ARA 2** | Deep VocAlign / Melodyne integration via Tracktion Engine. No routing gymnastics. |
| **Harmonic Editing** | Seamless bidirectional Audio ⇄ MIDI conversion. |
| **Advanced Sample Editor / Slicing** | Loop slicing to peaks with automatic keyboard mapping to MIDI (Edison / Simpler behavior). |
| **Spectrogram View & Sample-Level MPE** | Frequency view for surgical repair; polyphonic Glide support inside the sampler. |

### How this fits the clean UI

This module is **100% context-menu driven.** Not one pixel of it exists on screen until a clip is right-clicked.

- Right-click an audio clip → `Warp`, `Slice to MIDI`, `Edit with Melodyne (ARA)`, `Convert to MIDI`, `Open Spectrogram`.
- **Vocal tuning does not open a new editor.** It reuses the Piano Roll component — audio pitch renders as blocks in the exact same view the user already knows. One editor, two data types. This is the single biggest cognitive-load saving in the DAW.
- **The Spectrogram is a view toggle on the clip**, not a separate window — the waveform crossfades into spectral rendering in place.
- Warp anchors appear **only when Warp mode is armed on that clip**, and vanish on exit. Arrangement clips are never permanently cluttered with anchor markers.

---

## 5. Arrangement Architecture & Project Workflow

*Structure without chaos. Explicit hierarchy, never an open canvas.*

### Features

| Feature | Description |
|---|---|
| **Arrangement Blocks (Nested Clips)** | Build "Chorus 1" blocks containing both MIDI and audio, with **Ghosting** — edit one instance, every copy in the song updates. |
| **Modern Swipe-to-Comp** | Assemble a master take by swiping across multi-layer loop recordings (Playlists / sub-lanes auto-generated per loop pass). |
| **Scratch Pads** | A secondary timeline for testing edits and drops **without destroying the main Arrangement**. |
| **Global Chord Track** | A master chord lane that mathematically governs every MIDI and audio item synced to it. |
| **Strict Track Hierarchy** | Explicit Audio / MIDI / Group-Bus tracks. **No chaotic open-canvas routing.** |

### How this fits the clean UI

- **Arrangement Blocks collapse the timeline's visual density by an order of magnitude.** A 40-track chorus renders as one titled block. Double-click to enter (in-place zoom, not a new window); Escape to exit.
- **Swipe-to-Comp requires zero mode-switching.** Loop-record generates sub-lanes automatically; the user simply drags across the take they want. The promoted take highlights; the rest dim. No "comping tool" is ever selected from a toolbar.
- **Scratch Pads are a collapsible bottom-adjacent strip** — a drawer above the Device Chain, toggled by one edge button. Closed by default.
- **The Global Chord Track is a single pinned lane at the top of the arrangement**, collapsible to a 4px strip. Always available, almost never in the way.

---

## 6. The MIDI Suite — Editor & Generative Arsenal

*The FL piano roll, plus a Cubase-grade logical engine, plus Bitwig-grade modulation.*

### Features

| Feature | Description |
|---|---|
| **FL-Grade Piano Roll** | Porta/Slide notes, Ghost Channels, and critical tooling (`Strum`, `Chop`, smart grid snapping, flawless mouse physics). |
| **MIDI Devices — Arpeggiator** | Advanced: step control, Gate, Velocity. |
| **MIDI Devices — Scale & Chord** | Live scale locking; note→chord conversion. |
| **MIDI Devices — Note Length / Random / Note Echo** | Controlled randomness and command-driven MIDI delay. |
| **Unified Modulation System** | Attach an LFO, Envelope, or Envelope Follower to **any parameter in the Device Chain in one click** (Bitwig style). |
| **Expression Maps** | Articulation management for strings and symphonic libraries (Legato, Pizzicato…). |
| **The Logical Editor** | Visual programming for complex conditional MIDI rules. |
| **Deep MPE** | Core-level MIDI Polyphonic Expression passed natively to VST3. |
| **MIDI Feedback LEDs** | Ableton-style LED blinks in the transport bar and on track meters on incoming MIDI. |
| **CTRL+M Mapping Mode** | Assign any physical controller to any UI parameter, **with Min/Max value scaling** (e.g. CC knob → Delay Time, clamped to 20–300ms). |

### How this fits the clean UI

- **MIDI Devices are just devices.** The Arpeggiator, Scale & Chord, and Note Echo live in the **same bottom Device Chain** as plugins, upstream of the instrument. No "MIDI FX rack," no separate panel, no new concept for the user to learn. Signal flows left→right; MIDI things sit on the left of the chain. Done.
- **The Unified Modulation System is a drag, not a menu.** Drag an LFO's output dot onto any knob in the chain. A colored ring appears around the knob showing modulation depth. This replaces what other DAWs bury in a modulation matrix window.
- **CTRL+M is a mode, not a screen** — press it, the UI enters mapping tint, click a control, move a knob, done. Min/Max clamping is a right-click popover on the mapping.
- **The Logical Editor is the one genuinely complex UI in the product**, and it therefore lives *exclusively* in a collapsible right-side panel, off by default, opened from a piano-roll right-click. Power users find it; nobody else ever sees it.
- **MIDI LEDs are 6px.** They are the only always-visible element this module contributes.

---

## 7. Tempo Intelligence

| Feature | Description |
|---|---|
| **Detected Tempo & Auto-Stretch** | Automatic BPM analysis of dragged loops; seamless conform to project tempo on drop. |
| **Tempo Tap Analyzer** | Filters human timing deviation for maximum tap accuracy. |
| **Time Signature Automation** | Dynamic meter changes (4/4 → 3/4) with **full visual grid adaptation.** |

### How this fits the clean UI

Tempo intelligence is defined by what the user *doesn't* do. Dragging a 92 BPM loop into a 140 BPM project **just works** — no dialog, no "conform?" prompt, no warp-mode selection. The only visible artifact is a small `⟲` glyph on the clip indicating it was stretched, clickable to revert. Time signature changes are entered in the top ruler in-line; the grid redraws. Zero new panels contributed by this module.

---

## 8. The Live Performance Engine

*This is a stage instrument, not just a studio tool.*

| Feature | Description |
|---|---|
| **Action Markers** | Markers that execute commands when the playhead hits them: `Auto-Pause`, `Auto-Stop`, `Jump to Next Section`, `Loop Region`. |
| **Setlist Manager** | Markers presented as a song list, controllable via external MIDI controllers mid-performance. |
| **Bulletproof Live Mode** | Hard "stage lockdown" for MacBook M1 / ARM. **Locks samples to RAM**, suspends all non-critical background processing, pre-allocates buffers, guarantees zero dropouts under extreme load. |

### How this fits the clean UI

- **Action Markers are just markers** — created in the existing ruler, right-click → assign action. Zero new UI surface.
- **The Setlist Manager is a collapsible right panel.** For 95% of users it never opens. For touring users it becomes the entire screen — and it can be, because Law II lets a panel expand to full width.
- **Live Mode is one toggle in the transport bar** — the *only* advanced feature promoted to permanent top-bar residence, because a performer must be able to hit it without hunting. Engaging it visually locks the UI (dims non-essential chrome), which is itself the confirmation the user is safe.

> **Engineering invariant:** Live Mode is not a GUI state. It is an audio-engine state: buffer pre-allocation, RAM sample lock, background thread suspension, and a hard ban on allocation in the audio callback. The GUI dimming is a *symptom*, not the feature.

---

## 9. Pro Industry Standards — Modern Studio Environment

| Feature | Description |
|---|---|
| **Dolby Atmos & Spatial Audio** | Native 7.1.4 routing in the mixer with a **3D Object Panner**, full immersive mixing out of the box. |
| **Video Engine** | Video track with frame-based thumbnails, SMPTE Timecode support, and Replace Audio export for composers. |
| **The Cloud Studio (Real-Time Sync)** | Projects saved as `.crate_cloud`. Remote musicians' recordings and parts sync **directly into the timeline**, with channel-locking to prevent collisions. |
| **Dynamic CPU Allocation** | Load distribution via **Anticipative FX Pre-rendering** — non-record-armed tracks pre-rendered across all cores (Reaper model). |
| **Retrospective MIDI Record** | Permanent silent capture buffer. One button recovers the take you played while not in record. |
| **The Control Room** | Mixer zone for room calibration (Sonarworks) and headphone management — **never printed to Export.** |
| **Hardware Insert Ping** | Internal plugin that measures external analog hardware round-trip delay and auto-compensates (PDC). |
| **The Endgame Ecosystem** | Infrastructure to host large first-party/partner audio bundles as integral software components. |
| **API Wrapper** | Sandboxed Lua/Python scripting for community tools and MIDI extensions — **without touching core C++ source.** |

### How this fits the clean UI

This is the module most likely to destroy the interface, so it is governed by the harshest rules in the document:

- **The Control Room is a collapsible mixer zone**, docked far-right of the mixer, off by default. Its calibration chain is architecturally *after* the master export tap — it is physically incapable of printing to a render.
- **The Video Engine is a floating, resizable thumbnail** (not a docked panel) plus one optional timeline lane. Composers dock it; producers never enable it.
- **Cloud Studio is a right-side panel** showing collaborators and channel locks. A locked channel simply renders with a small padlock in the track header — the *only* footprint the feature has in the main view.
- **Anticipative FX and Dynamic CPU Allocation have NO UI.** They are pure engine behavior, surfaced solely as a lower number in the transport-bar CPU meter. This is the ideal shape of a feature.
- **Retrospective MIDI Record is one small button in the transport bar** (`⟲ MIDI`). It has no settings. It cannot be configured. It just saves you.
- **The Atmos 3D Object Panner replaces the standard pan control** in the mixer strip when the project is in immersive mode. It does not add a control — it *swaps* one.
- **The Scripting API surfaces as a console panel** (collapsible, bottom-right, off by default) and as user-defined entries in existing context menus. Community scripts appear where the user already right-clicks. They **cannot** add toolbar buttons. This is enforced at the API level, not by convention.

---

## 10. Non-Negotiable Engineering Invariants

These sit beneath every module above. They are not features; violating them is a defect.

1. **Strict thread separation.** Absolute isolation between the lock-free audio thread and the GUI thread. **Zero dynamic memory allocation on the audio thread.** No exceptions, no "just this once," no `std::string` in a `processBlock`.
2. **Everything is undoable.** Every mutation of the Edit passes a real `UndoManager`. *Status: implemented — the main `Edit`'s `UndoManager` is wired end to end through `CrateWorkflowManager` and the UI.* A `nullptr` UndoManager in any future commit is an automatic rejection.
3. **Everything persists.** Tracks, plugin state, and automation curves (including custom `crateAnchors` curve data) round-trip through `.crate` `ValueTree` serialization via `CrateWorkflowManager`. *Status: core persistence implemented.* Racks, macros, comps, and scratch pads must be brought under the same serialization path as those modules land — a DAW that loses work on quit is a toy, not a product.
4. **Frictionless I/O.** Last-used ASIO/CoreAudio device is remembered and auto-connected on startup with no dialog.
5. **The GUI never blocks.** Scanning, indexing, analysis, and pre-render all run on background threads. The user can always keep playing.
6. **No orphaned components.** Replaced UI code is deleted in the same commit, not left as dead weight.

---

## 11. Roadmap & Reality Check

Current state, honestly assessed against the gap analysis:

| Phase | Scope | Status |
|---|---|---|
| **P0 — Foundation Repair** | **Project save/load, full Undo/Redo, no orphaned components, level meters wired.** | ✅ **Done** — see below |
| **P0.5 — Shell/Transport/Mixer Chrome Overhaul** | **Zone 1–3 visual + interaction pass: icon transport, LCD, zoomable arrangement grid, waveform clips, drag-to-move, Pro Tools-grade mixer chrome, dB readouts, Spacebar transport.** | ✅ **Done** — see below |
| 1 | Skeleton: JUCE + TE, device auto-connect | ✅ Done |
| 2 | Timeline & Tracks (arrangement, headers, faders) | 🟡 Mostly done — real waveforms, drag-to-move, zoom, and playhead/ruler scrub landed this pass; Arrangement Blocks/Ghosting, Swipe-to-Comp, Scratch Pads, and the Global Chord Track are still 0% |
| 3 | Hybrid Mixer & Crate Browser | 🟡 Well advanced — scan/host, **drag-and-drop plugin instantiation from the Browser (real, wired to `te::Edit`)**, Device Chain (macro knobs, XY pad, focus sync), a Pro Tools-grade mixer strip (fader/pan/meter/routing/sends), the dynamic context-aware `BottomPanelContainer` (Law IV), and full Mixer↔Inspector parity/live-sync are all done; semantic (ontology-based) search and sample indexing/auto-tagging are still a plain substring filter and an un-indexed file tree respectively — both still 0% against their Section 1 spec |
| 4 | Piano Roll & MIDI Suite / MPE | 🟡 Core editor real and wired (add/move/resize/delete notes, velocity, chord-stamp, undo — all through the real `te::MidiClip` sequence); MPE, the Articulation lane's persistence/playback wiring, and every Section 6 MIDI Device (Arpeggiator, Scale & Chord, Note Length/Random/Echo) are still 0% |
| 5 | Automation / MIDI Map / ARA2 / Anticipative FX | 🟡 Curve physics done (deep), and the parameter dropdown + undo integration this document previously listed as blocking debt are **confirmed fixed** (any plugin parameter, real `UndoManager`) — but the In-Track Automation Overlay redesign (Section 3) is still an old-model expandable lane in code, not an overlay, and `Show Automated Only` doesn't exist yet; MIDI Map, ARA2, multi-core = 0% |
| 6 | Live Mode & Scripting API | 🔴 Not started |

**P0 implementation detail:** persistence is handled by `CrateWorkflowManager`, which serializes the project to `.crate` via JUCE `ValueTree`, safely tears down and rebuilds `te::Edit` instances on load, and round-trips the custom automation curve anchors (`crateAnchors`) so power-curve/tension/Wave/Pulse shapes survive a restart intact. Undo/Redo is fully wired to the main `Edit`'s `UndoManager` and reaches the UI — no commit path passes `nullptr` for it anymore.

**P0.5 implementation detail:** the transport is icon-driven (`juce::Path` primitives, not text) with a compact LCD (bars.beats + time) and a draggable BPM field; Spacebar Play/Stop and the Stop button both route through `CrateWorkflowManager::startPlayback()`/`stopAndReturnToStart()`, which remembers where playback started so Stop rewinds there instead of just pausing. The Arrangement grid is a real `pixelsPerSecond` zoom model (Ctrl/Cmd+scroll, debounced), the ruler computes only the visible bar/beat range and scrubs the transport on click-drag, and clips are real `ClipComponent`s with async `te::SmartThumbnail` waveforms (drawing is clip-bounds-limited so deep zoom doesn't stall the message thread) and undo-wrapped drag-to-move. The Mixer strip's fader/meter/pan knob are drawn by a dedicated `CrateMixerLookAndFeel` (bipolar pan fill, dB tick marks, gradient peak-hold meter with a numerical dB readout), Inserts/Sends are real `PluginSlotComponent`s, and Mute is TRUE polarity (ON = muted) synced identically between the Arrangement header and the Mixer strip via `ValueTree::Listener`.

**Phase 3 implementation detail — drag-and-drop:** confirmed real and working, corrected from a prior draft of this document that had it listed as still missing. `BrowserComponent::PluginRow::mouseDrag` starts a genuine `DragAndDropContainer` drag; `PluginSlotComponent::itemDropped`, `UniversalDeviceChainComponent::itemDropped`, and `TrackHeaderComponent::itemDropped` are all real drop targets, and all three ultimately call `CrateWorkflowManager::loadPluginOntoTrack()`, which does `edit->getPluginCache().createNewPlugin(...)` followed by `targetTrack.pluginList.insertPlugin(...)` inside a real undo transaction — not a visual-only highlight. `InsertsRackComponent` (the Mixer's own vertical insert list) is worth a direct naming-collision callout: it is **not** the Racks & Macros system Section 2 describes (no macro-knob mapping, no multi-device container-as-one-savable-object) — it's a plain per-track insert-slot list that happens to share the word "Rack."

**Phase 3 implementation detail (Law IV, Hybrid Device & Mixer Paradigm):** `BottomPanelContainer` is fully implemented as a context-aware swap host — it flawlessly swaps between the real `UniversalDeviceChainComponent` (Arrangement view), a `MasterAnalyzerComponent` (Mixer view, Tonal Balance / Insight placeholder), and a MIDI FX placeholder (Piano Roll view), driven by the active main view with zero destroy/recreate — only `setVisible()` toggles, per the "no orphaned components, nothing rebuilt" invariant. The dynamic Track Inspector (`InspectorStrip`) has been brought to full UI/UX and lifecycle parity with `MixerStrip`: identical live meter + GR meter wiring, identical Ableton-Mute name-plate behavior (with its own `UndoableAction` fixing a `CachedValue`-with-nullptr-`UndoManager` gap in the engine's own `setMute()`/`setSolo()`), and identical `ValueTree::Listener` registration — re-bound on every `setTrack()` call (unlike `MixerStrip`, which binds once for its whole lifetime) so a routing change made in *either* view instantly rebuilds the *other*. Sends/Routing is now robust end to end: the "+" menu lists every bus that exists anywhere in the project and greys out (rather than hides) ones the current track already sends to — visual confirmation the routing graph is intact, not a silent gap — shared verbatim between `MixerStrip` and `InspectorStrip` via one `SendBusUtils::scanBuses()` function; the Master strip's Sends section is omitted entirely (the summing bus has no valid "send to a bus" target); and both Sends viewports use a click-and-drag-only scrollbar (auto-hide styled thumb, mouse wheel and empty-space drag both deliberately disabled) so a dense mixing session can never suffer an accidental routing change from a stray scroll.

**Phase 4 implementation detail:** the Piano Roll's core note editor is real, not a mockup — notes are added/moved/resized/deleted directly against `activeMidiClip->getSequence()`, velocity edits go through `PianoRollExpressionLane` into the same sequence, and every edit is wrapped in `edit.getUndoManager()`. Two gaps this document previously glossed over: (1) MPE is explicitly, deliberately OFF — notes are created with `tracktion::engine::MPESourceID {}`, commented `// 0 == not MPE` in the source itself — this is a real architectural placeholder, not an oversight; (2) `PianoRollArticulationLane` is visual only — its `articulations` list is a local `std::vector` that is never written back into the clip's sequence and never read during playback, so drawing an articulation block currently has zero audible effect. Section 6's MIDI Devices (Arpeggiator, Scale & Chord, Note Length/Random/Echo) have no code anywhere yet.

### The Brutal Take

Curve-tension math is excellent, and it's no longer sitting on a foundation that could erase it. The engine now has a floor: plugin loads, tracks, and automation curves — including their custom anchor data — survive a full restart, and edits are undoable end to end. **P0, the precondition for this being a product rather than a demo, is complete.**

The Zone 1–3 chrome pass closes the other credibility gap: the DAW no longer *looks* like a placeholder. Transport, browser, arrangement, and mixer all read as intentional, industry-referenced UI, with real interaction underneath (drag-to-move, zoom, scrub, waveform, spacebar) rather than static mockup. That was necessary but it is still surface depth, not new capability — no new audio-processing module shipped in this pass.

**Law IV's hybrid scaling architecture is now functionally proven, not just theoretical.** `BottomPanelContainer` genuinely swaps three real, distinct views on the active main view with zero teardown, and the Mixer/Inspector are no longer two independently-built surfaces that happen to look similar — they share the exact same `CrateSendSlot`, the exact same bus-scan function, and the exact same `ValueTree::Listener`-driven sync, so a routing or mute change made in one is *guaranteed* to reach the other, not just usually reach it. That guarantee — verified by tracing the actual engine source (`EditItem::edit`, `CachedValue`'s `UndoManager` binding) rather than assumed from the UI — is the difference between a demo of the dual-view concept and a real implementation of it.

The parts that *are* built (device I/O, VST3 hosting, curve automation with durable persistence, arrangement UI, Device Chain, mixer chrome, the hybrid Mixer/Inspector pairing) are genuinely solid, and the TE integration patterns learned (plugin window hosting, curve baking, modal-dialog pitfalls, `ValueTree`/`Edit` lifecycle management, JUCE `dynamic_cast`-and-private-inheritance gotchas, `Viewport` subclassing for scroll sync, `Component::setBounds()`'s silent same-size-is-a-no-op trap) transfer directly forward. The real remaining gap is still breadth, not durability or polish: MPE, ARA2, Live Mode, the Crate Brain's actual intelligence layer (ontology scanning, semantic search, auto-tagging), and the scripting API are all still zero — and the Piano Roll's core editor, the automation parameter dropdown, and drag-and-drop instantiation are all further along than earlier drafts of this document credited them for. That's the next several phases, not a rescue mission.

**This section was corrected against a live source audit, not memory of past conversation.** A prior pass of this document claimed drag-and-drop instantiation was still missing and that automation was hardcoded to Volume with a `nullptr` UndoManager — both false by the time of this audit; the doc had drifted from the code, not the other way around. Every status above (Phases 3–5 specifically) was re-verified line-by-line against current source rather than carried forward from the previous revision. The one lesson worth keeping: **this document is a claim about the code, and claims decay — re-verify against source at every significant milestone, not just when someone catches a discrepancy.**

---

## 12. The One-Sentence Test

Before any feature ships, it must answer this:

> **"Where does this live when the user isn't using it?"**

If the answer is *"in the toolbar"* — redesign it.
If the answer is *"a collapsed panel," "a right-click menu," "a folded device header,"* or *"nowhere, it's just engine behavior"* — ship it.

---

*The Crate Studio — Built for producers, by producers.*
