# Lulada -- Piano-roll

This page documents the piano-roll editor: the copy-on-write MIDI note region it edits, the Ardour-style diff commands that make every gesture a first-class undoable action, the velocity and CC controller lanes, and the quantize / humanize / scale tools with live preview.

## Table of Contents

1. [Where the piano-roll fits](#1-where-the-piano-roll-fits)
2. [The MidiNoteRegion](#2-the-midinoteregion)
3. [The editor dock](#3-the-editor-dock)
4. [Tools and gestures](#4-tools-and-gestures)
5. [Diff commands and undo](#5-diff-commands-and-undo)
6. [Velocity and CC lanes](#6-velocity-and-cc-lanes)
7. [Quantize, humanize, scale](#7-quantize-humanize-scale)
8. [Rendering](#8-rendering)

---

## 1. Where the piano-roll fits

The piano-roll is a **peer** to the tracker -- both are first-class MIDI authoring surfaces. Where the tracker edits vht patterns, the piano-roll edits a `MidiNoteRegion`: a contiguous span of notes that lives on a timeline lane and is played back by a `MidiPlayerNode` (see [Arrangement & Timeline](arrangement-timeline.gen.html)). The same region shows up in both the Session View and the Arrangement, so one MIDI clip is authored once and used everywhere.

The editor itself (`PianoRollView`, `src/ui/pianoroll/`) is a bottom-attached dock: a header with the region label and tool palette, a fixed-width `PianoRollKeyboard` column on the left, and a horizontally-scrolling `PianoRollGrid` in a `Viewport` on the right. It binds to a region by uuid and resolves the live pointer through a `RegionResolver` on each paint -- so a region removed and re-added across an undo boundary re-resolves correctly.

## 2. The MidiNoteRegion

`MidiNoteRegion` (`src/services/timeline/midi_note_region.hpp`) is the shared data type behind the surface. Notes are id-stable (`MidiNote` carries a `std::uint64_t id`), not tree-index-based, which is what makes deterministic diff-and-undo possible.

Its thread discipline is the copy-on-write, wait-free pattern used across the DAW's realtime data:

- **One writer** (message thread), **one reader** (audio thread).
- The note list is an immutable `NoteList` published by a **raw pointer atomic swap**. The UI builds a new list, `atomic_exchange`s the live pointer, and pushes the displaced pointer onto a UI-thread trash deque drained on `AsyncUpdater`.
- The audio thread `atomic_load`s once per block and uses that snapshot for the whole render.

It deliberately does **not** use `std::atomic<shared_ptr>`: libstdc++ implements that with an internal spinlock, which is neither wait-free nor PI-aware on PREEMPT_RT. Raw atomic pointer swap plus deferred (epoch-gated) reclaim is wait-free for the audio reader and allocation-free for everyone except the UI edit/sweep path. `clone()` deep-copies a region with its own snapshot pointer, epoch counter, and trash deque, which is how the arrangement's `juce::Array<Lane>` undo snapshots copy regions that are otherwise non-copyable.

## 3. The editor dock

`PianoRollView` owns the per-editor state that outlives a single region binding -- the active tool, the last-used quantize/humanize/scale parameters, and the docked Q/H/S panel visibility. The grid is re-bound per region, so anything that must survive a re-bind lives on the view, not the grid.

| Element | Type | Role |
| --- | --- | --- |
| Keyboard column | `PianoRollKeyboard` | Pitch labels + click-to-audition |
| Note grid | `PianoRollGrid` | Note drawing, hit-testing, gesture dispatch |
| Velocity lane | `VelocityLane` | Per-note velocity bars |
| CC lanes | `CcLane` | Tabbed multi-CC controller overlay |
| Q/H/S panel | docked, 290 px | Quantize / Humanize / Scale, right edge |

## 4. Tools and gestures

The tool palette is a radio toggle on the header, default **Select**. The grid's mouse handlers branch on `PianoRollView::getActiveTool()`:

| Tool | Behaviour |
| --- | --- |
| Select | Marquee select + move/resize existing notes |
| Pencil | Single-click adds one note; drag resizes it |
| Erase | Click/drag removes notes |
| Brush | Drag paints one snap-division note per visited (beat-cell, pitch), committed as a single command on mouseUp |

Note drag/move/resize is handled by `NoteDrag` (`note_drag.cpp`), which samples the pointer against the drawn note geometry so a grab tracks the cursor exactly. A gesture mutates nothing on the region until `mouseUp`, at which point the accumulated change is committed as one diff command.

## 5. Diff commands and undo

Every edit gesture becomes a `MidiNoteDiffCommand` (`midi_note_diff_command.hpp`), a `juce::UndoableAction` modelled on Ardour's `NoteDiffCommand` but built on Element's UUID + id-stable note model. A command records three kinds of ops:

| Op | Records | Undo re-creates |
| --- | --- | --- |
| Add | The new note (id pre-assigned via `region->nextNoteId()`) | Removes it |
| Remove | The **full** note state | Re-adds with the same id/pitch/velocity/channel/timing |
| Update | Before **and** after for a mutable-field change (move/resize/velocity) | Restores "before" |

Commands are built empty and populated during the gesture's `mouseUp` commit, then handed to the `GuiService` `UndoManager`, which `perform()`s them. Region resolution is by uuid at perform/undo time, so an undo that crosses a region remove/re-add still targets the right region (and is a safe no-op if the region can't be resolved). An `onApplied` hook fires at the end of *both* `perform()` and `undo()` so the session `ValueTree` and view-side caches stay in sync in both directions -- without it, Ctrl+Z would leave stale notes in the session XML.

## 6. Velocity and CC lanes

Below the grid sit controller lanes that share the grid's horizontal scroll and zoom:

- **Velocity lane** (`VelocityLane`) draws one bar per note; dragging edits velocity, which records `Update` ops through the same diff-command path.
- **CC lanes** (`CcLane`) are a tabbed multi-CC overlay -- clip-local MIDI CC automation stored per region as a `MidiCcLane` (which reuses the same `AutomationPoint` curve primitive as timeline automation). Points are shaped with the unified 2D Bezier handle, identical to the volume envelope and timeline overlay (see [Automation](automation.gen.html)). At playback, `MidiPlayerNode` emits the CC values sample-accurately alongside the notes.

The lanes track the live playhead surgically: while transport rolls, `updateControllerLanePlayheads()` repaints only the old and new playhead strips, not the whole lane.

## 7. Quantize, humanize, scale

The Q/H/S tools live in a docked panel (toggled per-tab by the toolbar buttons or Ctrl+Q) and operate on the shared quantize primitives in `src/dsp` (`quantize_ops`, `quantize_options`). Last-used settings live on the view and survive dialog open/close, so Ctrl+Q and the toolbar buttons replay the user's most recent parameters (falling back to defaults derived from the current snap division until the user has adjusted them).

The key detail is **live preview parity**: the preview highlight is driven from `MidiNoteDiffCommand::touchedIds()` -- the exact same diff the Apply path will commit. So what the user sees highlighted before applying is precisely the set of notes Apply writes, with no semantic drift between preview and commit.

## 8. Rendering

The piano-roll is on the DAW's rendering hot path when transport rolls, so it uses the same surgical-repaint strategy as the arrangement: the playhead is drawn opaque and updated by repainting only the old/new strips, driven off a `VBlankAttachment` rather than a 30 Hz timer. The static content -- background, grid, ruler, and the note layer -- is cached to an image and blitted, keyed off the region snapshot pointer, since a note drag mutates nothing until `mouseUp`. The velocity and CC lanes are *not* image-cached (they mutate live on every mouse-move) but are opaque, surgically repainted, and culled to the viewport.
