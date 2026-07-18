## 10. Non-Negotiable Engineering Invariants

These sit beneath every module above. They are not features; violating them is a defect.

1. **Strict thread separation.** Absolute isolation between the lock-free audio thread and the GUI thread. **Zero dynamic memory allocation on the audio thread.** No exceptions, no "just this once," no `std::string` in a `processBlock`.
2. **Everything is undoable.** Every mutation of the Edit passes a real `UndoManager`. *Status: implemented — the main `Edit`'s `UndoManager` is wired end to end through `CrateWorkflowManager` and the UI.* A `nullptr` UndoManager in any future commit is an automatic rejection.
3. **Everything persists.** Tracks, plugin state, and automation curves (including custom `crateAnchors` curve data) round-trip through `.crate` `ValueTree` serialization via `CrateWorkflowManager`. *Status: core persistence implemented.* Racks, macros, comps, and scratch pads must be brought under the same serialization path as those modules land — a DAW that loses work on quit is a toy, not a product.
4. **Frictionless I/O.** Last-used ASIO/CoreAudio device is remembered and auto-connected on startup with no dialog.
5. **The GUI never blocks.** Scanning, indexing, analysis, and pre-render all run on background threads. The user can always keep playing.
6. **No orphaned components.** Replaced UI code is deleted in the same commit, not left as dead weight.

---
