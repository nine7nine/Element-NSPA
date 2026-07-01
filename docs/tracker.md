# Lulada -- Tracker

This page documents the Tracker: a pattern-based MIDI sequencer built on a vendored C engine (rdybka's `vht`) wrapped as an internal graph node, its two playback models (single-playhead for the arrangement, per-sequence flags for the clip launcher), and the sample-accurate launch scheduler that both surfaces share.

## Table of Contents

1. [Two-node architecture](#1-two-node-architecture)
2. [The vendored vht engine](#2-the-vendored-vht-engine)
3. [The TrackerNode wrapper](#3-the-trackernode-wrapper)
4. [Draining the engine to MIDI](#4-draining-the-engine-to-midi)
5. [Two playback models](#5-two-playback-models)
6. [The launch scheduler](#6-the-launch-scheduler)
7. [Mute and solo](#7-mute-and-solo)
8. [Undo](#8-undo)
9. [No song mode here](#9-no-song-mode-here)

---

## 1. Two-node architecture

The tracker is deliberately split into two graph nodes rather than one monolithic "sample tracker":

- **`TrackerNode`** -- pattern grid, MIDI **out only**.
- **`SamplerNode`** -- MIDI **in**, audio **out** (see [Sampler](sampler.gen.html)).

MIDI is the internal wire format between them. A "sample track" is just a MIDI track routed to the internal sampler -- same data underneath, different UX chrome. This keeps the tracker a pure sequencer that can drive *anything* (the internal sampler, a hosted VST/CLAP synth, a MIDI tool chain), and lets the sampler be a plain instrument that does not care where its notes come from.

## 2. The vendored vht engine

The pattern engine is the C core of [`rdybka/vht`](https://github.com/rdybka/vht) (GPL-3), vendored under `src/engine/tracker/` and driven directly. The Python/GTK UI and the JACK/ALSA drivers are dropped; Lulada supplies its own JUCE-side driver and UI.

| File(s) | Role |
| --- | --- |
| `module.c` | Top-level container: holds the sequences, transport, tempo |
| `sequence.c` | One pattern: a set of tracks sharing a row length + playhead |
| `track.c` | One column of cells within a sequence |
| `row.c` | A single cell: note, velocity, and control data |
| `midi_event.c` | The engine's MIDI event representation |
| `envelope.c` / `ctrlrow.c` | Per-cell control / envelope data |
| `midi_client.c` | Vendored client shim (JACK plumbing replaced by the node) |
| `timeline.c` | Left as a no-op stub -- Lulada does not use vht's song mode (see §9) |

The engine is C, so the node brackets its includes in `extern "C"` to keep C++ name mangling from breaking linking. The model nests `module -> sequence -> track -> row`; a module can hold many sequences, and each sequence carries its own `playing` flag and playhead.

> A wiring gotcha from bring-up: `sequence_new()` defaults `seq->playing = 0`. Without `sequence_set_playing(seq, 1)`, `sequence_advance` runs but skips `track_advance`, so no MIDI emits even when the module is playing. The two playback models below both drive that flag explicitly.

## 3. The TrackerNode wrapper

`TrackerNode` (`src/nodes/tracker.hpp`) is a `MidiFilterNode` that owns a `module*` and exposes it to the graph and the editor. It registers as `EL_NODE_ID_MIDI_SEQUENCER`, with zero audio channels and a MIDI output port. On first `prepareToRender` it calls `installDefaultPattern()` to build an empty valid module (2 tracks, 16 rows, no notes).

All engine access is serialised by an internal `juce::CriticalSection engineLock_`. The editor reads pattern state under `engineLock()`; the audio thread advances the engine under the same lock inside `render()`. Because the lock is a `juce::CriticalSection`, it is PI-correct on PREEMPT_RT, and the calls that take it from the message thread (UI clicks, the 30 Hz poll) are infrequent enough that contention with the audio thread is negligible.

State round-trips through `getState` / `setState` as a serialised blob, which is also the unit of undo (see §8).

## 4. Draining the engine to MIDI

vht originally flushed events to JACK ports in `jack_process.c`; `TrackerNode` replaces that with **`drainEngineToMidi`**. Each block, under `engineLock_`, the node advances the engine over `numSamples`, then drains every emitted event out of the engine's per-track output buffers into the node's per-port JUCE `MidiBuffer`s, time-sorted. When multiple sequences are playing concurrently (the clip-launcher case), all of their events land in the shared per-port buffer and are sorted together -- so several launched clips play in sample-aligned time.

## 5. Two playback models

`TrackerNode` supports two *distinct* ways to drive playback, used by the two surfaces:

| Model | Primitive | Playheads | Used by |
| --- | --- | --- | --- |
| Pattern-switch | `advanceToPattern(idx)` | One (`curr_seq`) | Arrangement view |
| Per-sequence flag | `setSequencePlaying(idx, on)` | Many (concurrent) | Session View clip launcher |

- **Pattern-switch** moves the single `curr_seq` playhead to a chosen pattern at the next buffer boundary -- classic tracker "jump to pattern N". `currentPatternIndex()` / `numPatterns()` let the arrangement view highlight the active block.
- **Per-sequence flags** flip an individual sequence's `playing` flag without touching `curr_seq`, so multiple sequences in one module can play at once. `module_advance` iterates all of them. Launching a clip rewinds that sequence *and all its tracks* to row 0 -- matching Ableton/Bitwig "launch restarts from the start".

The session-view model adds edge-detection helpers for follow-actions: `sequenceWrappedSinceLastQuery(idx)` returns true once per playhead wrap (consumed on read), and `getSequenceLengthBeats(idx)` lets the arrangement schedule region cutoffs for non-looped tracker regions.

## 6. The launch scheduler

Both surfaces launch clips through one sample-accurate scheduler, `schedulePlaying(sequenceIdx, beatTarget, wantPlaying)`. It is a **lock-free SPSC FIFO** from the message thread to the audio thread, so a UI click never touches `engineLock_`:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 260" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln  { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="260" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="title">schedulePlaying -- message thread to audio thread, no lock</text>

  <rect x="30" y="46" width="250" height="70" class="grn"/>
  <text x="48" y="66" class="lbl-grn">Message thread</text>
  <text x="48" y="86" class="lbl-mut">clip bang -> schedulePlaying(</text>
  <text x="48" y="100" class="lbl-mut">  seqIdx, beatTarget, wantPlaying)</text>

  <rect x="330" y="52" width="240" height="58" class="box"/>
  <text x="450" y="74" text-anchor="middle" class="lbl-sm">64-slot AbstractFifo</text>
  <text x="450" y="90" text-anchor="middle" class="lbl-mut">LaunchReq{seqIdx, beat, want}</text>
  <text x="450" y="103" text-anchor="middle" class="lbl-mut">full -> new request dropped</text>

  <rect x="620" y="46" width="250" height="70" class="hot"/>
  <text x="638" y="66" class="lbl-yel">Audio thread (render)</text>
  <text x="638" y="86" class="lbl-mut">drainLaunchFifo -> PendingAction[seqIdx]</text>
  <text x="638" y="100" class="lbl-mut">latest per seqIdx wins (implicit cancel)</text>

  <line x1="280" y1="81" x2="330" y2="81" class="ln"/>
  <line x1="570" y1="81" x2="620" y2="81" class="ln"/>

  <rect x="30" y="140" width="840" height="90" class="box"/>
  <text x="50" y="162" class="lbl-sm">applyPendingForBlock(blockStartBeat, blockEndBeat)</text>
  <text x="50" y="182" class="lbl-mut">beatTarget &lt; 0   -> fire at the start of the next render block (immediate)</text>
  <text x="50" y="198" class="lbl-mut">beatTarget &gt;= 0  -> fire in the block whose beat range contains it (+/- one block)</text>
  <text x="50" y="214" class="lbl-mut">clips targeting the same beat flip together -> zero inter-clip skew</text>
</svg>
</div>

Each block, before advancing the engine, the audio thread drains the FIFO into a per-sequence `PendingAction` table (indexed by `sequenceIdx`) and then applies any pending flip whose target beat falls in this block. Because the table keeps only the **latest** request per sequence, re-banging a queued clip cancels the prior request for free. A 64-slot FIFO at human click rate never fills; if it somehow did, the new request is dropped silently rather than blocking.

## 7. Mute and solo

Session-view mute/solo state lives on the node (`getUserMuted` / `setUserMuted`, `getSoloed` / `setSoloed`) so both the Session View and the tracker editor popup show one consistent state. These are *user intent*; the effective engine mute (`Processor::isMuted`) is reconciled by the Session View: when any tracker is soloed, non-soloed trackers get `setMuted(true)`; otherwise each falls back to `setMuted(getUserMuted())`.

## 8. Undo

The node keeps its own memento stacks: `pushUndo()` snapshots the whole module state (the same wire format as `getState`) onto a stack capped at 64 entries, with a matching redo stack. The editor calls `pushUndo()` before any mutation.

To integrate with the app-wide undo, the node exposes an `onPushUndo` callback. The `TrackerEditor` binds it to push a bridge `UndoableAction` into the global `GuiService` `UndoManager`, so Cmd+Z / Cmd+Shift+Z unwind tracker edits alongside graph, arrangement, and session-view edits. Local undo/redo remain the source of truth for the state mementos; the bridge action just delegates back to `TrackerNode::undo()` / `redo()`.

## 9. No song mode here

The tracker is intentionally **not** a song sequencer. vht's `timeline.c` (its per-slot order-list) is left as a no-op stub. The linear song role belongs to the polymorphic [Arrangement](arrangement-timeline.gen.html) view, which can host tracker *regions* (referencing vht sequences) on a timeline lane and drive them through the same `schedulePlaying` path. That keeps one song model for the whole DAW instead of two competing ones.
