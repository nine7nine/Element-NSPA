# Lulada -- Session View

This page documents the Session View: an Ableton/Bitwig-style clip-launch grid where each column is a graph source node, each scene is a row, and clicking a clip launches it through the same sample-accurate audio-thread scheduler the arrangement uses.

## Table of Contents

1. [The grid model](#1-the-grid-model)
2. [Clips, scenes, columns](#2-clips-scenes-columns)
3. [Launch quantisation](#3-launch-quantisation)
4. [Launch state machine](#4-launch-state-machine)
5. [Follow actions](#5-follow-actions)
6. [Scenes and the master column](#6-scenes-and-the-master-column)
7. [Reconciling UI to engine](#7-reconciling-ui-to-engine)
8. [Persistence and undo](#8-persistence-and-undo)

---

## 1. The grid model

`SessionView` (`src/ui/sessionview.hpp`) is a `ContentView` laid out Bitwig-style: column headers across the top, scene labels down the left, and a sparse grid of clips in between. Each **column maps 1:1 to a source node** in the active graph, and each **scene (row)** holds at most one clip per column.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 880 320" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .hdr { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .cell{ fill: #1f2535; stroke: #3b4261; stroke-width: 1; }
    .clip{ fill: #2a3550; stroke: #7aa2f7; stroke-width: 1.2; }
    .clipg{ fill: #23402a; stroke: #9ece6a; stroke-width: 1.2; }
    .scene{ fill: #2a2438; stroke: #bb9af7; stroke-width: 1; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .lbl-blu { fill: #7aa2f7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .title { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="880" height="320" class="bg"/>
  <text x="440" y="24" text-anchor="middle" class="title">columns = source nodes,  scenes = rows,  clips = sparse cells</text>

  <rect x="30" y="44" width="120" height="34" class="hdr"/>
  <text x="90" y="65" text-anchor="middle" class="lbl-mut">scene</text>
  <rect x="160" y="44" width="150" height="34" class="hdr"/>
  <text x="235" y="60" text-anchor="middle" class="lbl-blu">Tracker col</text>
  <text x="235" y="72" text-anchor="middle" class="lbl-mut">TrackerNode</text>
  <rect x="320" y="44" width="150" height="34" class="hdr"/>
  <text x="395" y="60" text-anchor="middle" class="lbl-grn" fill="#9ece6a" font-weight="bold">MIDI col</text>
  <text x="395" y="72" text-anchor="middle" class="lbl-mut">MidiPlayerNode</text>
  <rect x="480" y="44" width="150" height="34" class="hdr"/>
  <text x="555" y="60" text-anchor="middle" class="lbl-blu">Tracker col</text>
  <text x="555" y="72" text-anchor="middle" class="lbl-mut">TrackerNode</text>
  <rect x="640" y="44" width="150" height="34" class="hdr"/>
  <text x="715" y="65" text-anchor="middle" class="lbl-pur">master</text>

  <!-- scene 1 -->
  <rect x="30" y="86" width="120" height="40" class="scene"/>
  <text x="90" y="110" text-anchor="middle" class="lbl-sm">Scene A</text>
  <rect x="160" y="86" width="150" height="40" class="clip"/>
  <text x="235" y="110" text-anchor="middle" class="lbl-sm">pattern 1</text>
  <rect x="320" y="86" width="150" height="40" class="clipg"/>
  <text x="395" y="110" text-anchor="middle" class="lbl-sm">midi clip</text>
  <rect x="480" y="86" width="150" height="40" class="cell"/>
  <rect x="640" y="86" width="150" height="40" class="scene"/>
  <text x="715" y="110" text-anchor="middle" class="lbl-mut">120 bpm 4/4</text>

  <!-- scene 2 -->
  <rect x="30" y="130" width="120" height="40" class="scene"/>
  <text x="90" y="154" text-anchor="middle" class="lbl-sm">Scene B</text>
  <rect x="160" y="130" width="150" height="40" class="cell"/>
  <rect x="320" y="130" width="150" height="40" class="clipg"/>
  <text x="395" y="154" text-anchor="middle" class="lbl-sm">midi clip</text>
  <rect x="480" y="130" width="150" height="40" class="clip"/>
  <text x="555" y="154" text-anchor="middle" class="lbl-sm">pattern 3</text>
  <rect x="640" y="130" width="150" height="40" class="scene"/>
  <text x="715" y="154" text-anchor="middle" class="lbl-mut">140 bpm</text>

  <!-- scene 3 -->
  <rect x="30" y="174" width="120" height="40" class="scene"/>
  <text x="90" y="198" text-anchor="middle" class="lbl-sm">Scene C</text>
  <rect x="160" y="174" width="150" height="40" class="clip"/>
  <text x="235" y="198" text-anchor="middle" class="lbl-sm">pattern 2</text>
  <rect x="320" y="174" width="150" height="40" class="cell"/>
  <rect x="480" y="174" width="150" height="40" class="clip"/>
  <text x="555" y="198" text-anchor="middle" class="lbl-sm">pattern 4</text>
  <rect x="640" y="174" width="150" height="40" class="scene"/>

  <text x="30" y="248" class="lbl-sm">Click a clip -> schedulePlaying on its source node (sample-accurate at the boundary)</text>
  <text x="30" y="268" class="lbl-sm">Click a scene -> bang every clip on that row; apply the scene's tempo / sig overrides</text>
  <text x="30" y="288" class="lbl-mut">A column is one kind only: Tracker (vht sequences) or MIDI (per-clip region).</text>
</svg>
</div>

## 2. Clips, scenes, columns

A clip is polymorphic over its backing node kind (`ClipKind`):

| Kind | Backing | Bound by |
| --- | --- | --- |
| `Tracker` | one vht sequence inside a `TrackerNode` | `trackerNodeId` + `sequenceIdx` |
| `Midi` | a `MidiNoteRegion` owned by a `MidiPlayerNode` | `midiPlayerNodeId` + `midiSourceId` |

MIDI clips own their region per-clip (cloned, **not** shared with arrangement regions), so editing a session MIDI clip in the piano-roll affects only that clip. Node bindings are stored as graph node ids and resolved against the live graph, so a clip survives session save/load even as node pointers change.

A `SessionColumn` is locked to one kind at creation -- Tracker columns bind a `TrackerNode`, MIDI columns bind a `MidiPlayerNode`; there are no mixed-kind columns (one clip-source per column). `rescanColumns()` walks the active graph on activate and on graph-topology changes, rebuilding the column list from the current source nodes while preserving user-authored columns and dropping orphans.

## 3. Launch quantisation

Each clip carries a `LaunchQuant` that decides *when* a bang takes effect relative to the transport:

| `LaunchQuant` | Fires at |
| --- | --- |
| `Off` | Immediately (next render block) |
| `Beat` | Next beat |
| `Bar` | Next bar (`beatsPerBar`) -- the default |
| `TwoBars` | Next 2-bar boundary |
| `FourBars` | Next 4-bar boundary |

The view converts the quant into a transport beat target and passes it to the source node's scheduler (`TrackerNode::schedulePlaying` / `MidiPlayerNode::schedulePlayingClip`) -- the same lock-free SPSC FIFO the arrangement uses. All clips targeting the same beat flip together with zero inter-clip skew (see [Tracker](tracker.gen.html#6-the-launch-scheduler)).

## 4. Launch state machine

Each clip's visible state is an atomic `LiveState` driven by the message thread on a bang and reconciled toward engine truth by the UI timer:

```
Stopped  --bang-->  WaitingToStart  --(audio thread flips seq->playing
                                         at the quant boundary)-->  Playing
Playing  --bang-->  WaitingToStop   --(audio thread clears playing)-->  Stopped
```

`WaitingToStart` / `WaitingToStop` are the "queued" states you see blinking until the boundary arrives. The state is atomic because the message thread writes it on the bang and reads it on the 30 Hz timer while the audio thread independently flips the engine flag.

## 5. Follow actions

When a playing clip's sequence wraps past its end, its `FollowAction` decides what happens next -- detected via the tracker node's `sequenceWrappedSinceLastQuery` edge-trigger:

| `FollowAction` | Effect on wrap |
| --- | --- |
| `None` | Keep looping (vht loops naturally) -- the default |
| `Stop` | Schedule an immediate stop |
| `RestartClip` | Re-launch the same clip from row 0 |
| `NextClip` | Bang the clip on the next-greater scene row in the same column (else `Stop`) |
| `FirstClip` | Bang the lowest scene-row clip in the same column |

This gives simple generative chaining without a separate arrangement -- clips can hand off down a column or ping-pong on their own.

## 6. Scenes and the master column

A scene bang (`bangScene`) launches every clip on that row at once. Scenes also carry Ableton-style per-scene **transport overrides**, shown in a master column and applied when the scene launches:

- `tempoOverride` -- bpm, sentinel `-1` means "no override; inherit current".
- `beatsPerBar` / `beatDivisor` -- time signature, sentinel `0` means "no override".

So a scene can be a tempo/signature change as well as a set of clips, which is how the Session View doubles as a loose arrangement device.

## 7. Reconciling UI to engine

The message thread never assumes a launch happened; it *requests* one and then watches for the audio thread to make it real. On each timer tick the view reads each clip's atomic `LiveState` and the engine's actual playing/position, and repaints only what changed (diff-gated via `lastDrawnPlaying` / `lastDrawnPosRow`). This keeps the grid honest even though the flip is sample-accurate and happens asynchronously on the audio thread. `stopAllClips()` and the per-column force-stop path ("belt and suspenders") ensure no sequence is left playing when the user clears the grid or the graph changes.

## 8. Persistence and undo

Session View state (clips, scenes, columns) is serialised into the session `ValueTree`. Undo is **snapshot-based**: `writeToSession` generates a `SessionViewSnapshotAction`, and the global `GuiService` `UndoManager` calls `applySessionSnapshot(snap)` to swap the stored subtree back in, re-read it to rebuild the in-memory `clips_` / `scenes_` / `columns_` arrays, and repaint. An internal `applyingUndoAction_` guard stops the re-read from recursively pushing another action. This is a coarser granularity than the piano-roll's per-gesture diff commands, which suits the Session View's larger structural edits (add/remove clip, move, re-scene).
