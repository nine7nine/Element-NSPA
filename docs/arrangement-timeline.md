# Lulada -- Arrangement & Timeline

This page documents the Arrangement view: the linear, multi-lane timeline that is Lulada's song mode. It covers the lane/region/playlist data model, how audio and MIDI and tracker lanes dispatch through the same sample-accurate scheduler, the disk-streaming engine behind record and playback, and Bezier fades and volume envelopes.

## Table of Contents

1. [Lanes, regions, playlists](#1-lanes-regions-playlists)
2. [Region placement and the tempo map](#2-region-placement-and-the-tempo-map)
3. [Sources and the registry](#3-sources-and-the-registry)
4. [Dispatch](#4-dispatch)
5. [Disk streaming](#5-disk-streaming)
6. [Recording](#6-recording)
7. [MIDI lanes](#7-midi-lanes)
8. [Fades and volume envelopes](#8-fades-and-volume-envelopes)
9. [File drop, markers, resize](#9-file-drop-markers-resize)
10. [Persistence](#10-persistence)

---

## 1. Lanes, regions, playlists

`ArrangementView` (`src/ui/arrangementview.hpp`) is a multi-lane timeline. A **lane** is a row that binds to exactly one target graph node by uuid (`Lane.targetNodeUuid`); the lane is **not** itself a graph node. Adding or removing lanes does not change graph topology -- you add a node in the graph editor, then point a lane at it. When the target node is missing (deleted from the graph), the lane stays in the arrangement, greyed, with a "rebind" affordance.

| Type | File | Role |
| --- | --- | --- |
| `Lane` | `services/timeline/lane.hpp` | One timeline row; binds a `Playlist` to a target node |
| `Playlist` | `services/timeline/playlist.hpp` | Owns the lane's regions (audio/tracker + MIDI) |
| `Region` | `services/timeline/region.hpp` | Value-typed audio/tracker region (points at a Source by uuid) |
| `MidiNoteRegion` | `services/timeline/midi_note_region.hpp` | MIDI note span (non-copyable; stored separately) |

A lane's coarse `Kind` is `Audio` or `Midi`. Tracker lanes are **not** a separate kind -- they share `Kind::Audio` and are runtime-distinguished by the resolved target-node type (the view caches an `audioClipCache` or `trackerCache` pointer per lane). Only the genuinely new affordance, the MIDI lane, needs an explicit kind, so older sessions load unchanged.

## 2. Region placement and the tempo map

`Region` is pure data: a thin wrapper pointing at a `Source` by uuid, with **beat-domain** placement throughout. `positionBeats`, `startBeats`, `lengthBeats`, `fadeInBeats`, and `fadeOutBeats` are all beats relative to the session transport's tempo map; sample-accurate scheduling happens later, on the audio thread.

- `positionBeats` -- where the region starts on the timeline.
- `startBeats` -- the offset *into* the Source where playback begins. Audio regions use this to trim into the file; vht-sequence regions truncate it to 0 (patterns always start at row 0).
- `lengthBeats` -- how long the region plays; `looped` repeats the source to fill it.
- `sequenceIdx` -- for a tracker region, which vht sequence inside the owning `TrackerNode` this region plays (so "play TrackerX's pattern N" needs no Source-per-pattern entry).

`Region` is value-typed and owned by the `Playlist` on the message thread; the audio thread only ever sees **copies** passed through FIFO entries and never reads the source struct directly.

## 3. Sources and the registry

Regions reference their audio/MIDI content indirectly, through the `SourceRegistry` (`src/services/sources`). Two source kinds back timeline regions:

| Source | Backs | Notes |
| --- | --- | --- |
| `AudioFileSource` | audio regions | An on-disk audio file, streamed from disk |
| `VhtSequenceSource` | tracker regions | A vht sequence inside a `TrackerNode` |

The indirection means many regions can share one source (e.g. loops), and record commits register the freshly-captured file as a new `AudioFileSource` before appending a region that points at it.

## 4. Dispatch

Every lane kind launches through the **same** path the Session View uses: a UI-thread detection loop on the 30 Hz timer decides *what* should fire, and the actual sequence/clip flip is sample-accurate within one render block on the audio thread.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 250" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln  { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="250" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="title">Per-lane dispatch -- one path for audio, tracker, and MIDI</text>

  <rect x="30" y="44" width="230" height="60" class="grn"/>
  <text x="48" y="64" class="lbl-grn">Message thread (30 Hz)</text>
  <text x="48" y="82" class="lbl-mut">scheduler decides which regions</text>
  <text x="48" y="96" class="lbl-mut">enter / leave the playhead window</text>

  <rect x="300" y="44" width="260" height="60" class="box"/>
  <text x="430" y="64" text-anchor="middle" class="lbl-sm">TimelineAdapter (per kind)</text>
  <text x="430" y="82" text-anchor="middle" class="lbl-mut">Audio -> AudioClipNode::schedulePlay</text>
  <text x="430" y="96" text-anchor="middle" class="lbl-mut">Tracker -> TrackerNode::schedulePlaying</text>

  <rect x="600" y="44" width="270" height="60" class="hot"/>
  <text x="618" y="64" class="lbl-yel">Audio thread (render)</text>
  <text x="618" y="82" class="lbl-mut">per-node SPSC FIFO -> pending action</text>
  <text x="618" y="96" class="lbl-mut">fires at target beat, +/- one block</text>

  <line x1="260" y1="74" x2="300" y2="74" class="ln"/>
  <line x1="560" y1="74" x2="600" y2="74" class="ln"/>

  <rect x="30" y="130" width="840" height="90" class="box"/>
  <text x="50" y="152" class="lbl-sm">Region copy travels in the FIFO entry -- the audio thread never dereferences the Playlist</text>
  <text x="50" y="172" class="lbl-mut">Audio region -> Playback_DS streams the AudioFileSource from disk into the block</text>
  <text x="50" y="188" class="lbl-mut">Tracker region -> TrackerNode advances the referenced vht sequence</text>
  <text x="50" y="204" class="lbl-mut">MIDI region -> MidiPlayerNode walks the note snapshot, emits NoteOn/Off per block</text>
</svg>
</div>

## 5. Disk streaming

Audio playback and record stream from disk so arbitrarily long files never sit in RAM. The engine (`src/services/audiostreaming`) is adapted from **NON-DAW**'s `Disk_Stream` (Jonathan Moore Liles, GPLv2-or-later), with the JACK-specific machinery replaced by Lulada's realtime primitives:

- Per-channel `jack_ringbuffer_t` rings become **`juce::AbstractFifo` + `std::vector<sample_t>`** -- the same lock-free SPSC shape the tracker's launch FIFO uses, with no JACK runtime dependency.
- The POSIX `sem_t` IO-thread wakeup becomes a **`juce::WaitableEvent`**, which under the winelib build maps to librtpi `PiMutex` + `PiCond` (`FUTEX_WAIT_REQUEUE_PI`). The audio thread's `signal()` boosts the IO-thread waiter directly via a kernel futex -- **no wineserver involvement, PI-correct on PREEMPT_RT** (see [Winelib Runtime](winelib-runtime.gen.html#5-priority-inheritance-on-the-hot-paths)).

`Disk_Stream` is an abstract `juce::Thread`; `Playback_DS` and `Record_DS` are the direction-specific subclasses. The IO thread groups blocks into ~`disk_io_kbytes` chunks to keep the libsndfile call rate down, and buffers `seconds_to_buffer` (default 2.0 s) per channel. `xruns()` counts blocks where `process()` requested data but the ring was empty (playback) or full (record); `Playback_DS` throttles seeks against the ring's fill headroom.

## 6. Recording

Recording is per-lane and armed-driven:

1. The lane's record-arm toggle (`Lane.armed`) propagates to the bound `AudioClipNode::setArmed`.
2. Transport-record **and** an armed lane triggers capture inside `AudioClipNode`, which streams input to disk through a `Record_DS`.
3. On capture finish, `AudioClipNode` fires `onRecordingCommitted` with the new file; `ArrangementView` registers it as an `AudioFileSource` and appends a `Region` to the lane's `Playlist` at the captured position.

The working audio-record path is the template the MIDI-record work follows.

## 7. MIDI lanes

A MIDI lane spawns a `MidiPlayerNode` internally (just as an audio lane spawns an `AudioClipNode`), but the node is a **first-class graph citizen** -- you can wire its MIDI output anywhere, swap the bound synth, or route it through a MIDI tool chain. It publishes one MIDI output port.

The node tracks its bound `MidiNoteRegion`s through a lock-free pointer table (a `RegionEntry` array): the message thread publishes a new array on every playlist mutation, and the audio thread loads the current table once per block. Each block, for every region whose `[position, position+length)` overlaps the block's beat range, the node maps block beats to local region beats (handling loops), walks the region's note snapshot, and emits `NoteOn` at `sampleOffset = (onBeat - blockStart) * samplesPerBeat`, with matching `NoteOff` for notes ending in the block. On stop or teardown it emits all-notes-off; the message thread must publish an empty table *before* destroying regions so the audio thread observes it first.

## 8. Fades and volume envelopes

Each `Region` carries gain and fade metadata, all evaluated on the audio-thread copy:

- **Static gain** -- `gainDb`, a constant.
- **Bezier fades** -- `fadeInBeats` / `fadeOutBeats` with `fadeInCurve` / `fadeOutCurve` in `[-1, +1]`, mapped to a power-curve exponent `p = exp2(curve * 2)` (so `+1 -> p=4` concave-up, `-1 -> p=0.25` concave-down, `0` linear). Sparse-written -- only persisted when non-zero.
- **Volume envelope** -- an ordered list of `EnvelopePoint` breakpoints (`beatOffset`, `gainDb`, curve). Empty means "use static `gainDb`"; two or more points enable per-sample interpolated gain via `gainAtBeatOffset(localBeat)`. Segments are shaped with the same 2D Bezier handle (`curveOffsetT` / `curveOffsetDb`) the automation overlay and piano-roll CC lanes use -- one curve model across the whole DAW (see [Automation](automation.gen.html)).

## 9. File drop, markers, resize

- **External file drop** -- `ArrangementView` is a `juce::FileDragAndDropTarget`. Dropping an audio file on an existing audio lane appends a region at the drop X; dropping on a tracker lane or empty area creates a new audio lane (in the `ArrangementTracks` subgraph) and appends the region.
- **Markers** -- a `MarkerTrack` (`services/timeline/marker_track.hpp`) holds named timeline markers above the lanes.
- **Per-lane resize** -- `Lane.heightPx` gives a lane a custom height (0 = follow the arranger's global vertical zoom), set by dragging the lane's bottom edge; clamped to `[kLaneHMin, kLaneHMax]` on read and sparse-written so older sessions keep following the global zoom.

## 10. Persistence

Lanes serialise into the session under `tags::arrangement` (a `<lanes>` child), with **automation bindings** stored as a peer list rather than nested inside lanes -- additive, so existing lane serialisation and undo are untouched. Automation *curve* data itself lives in the song-owned engine, not the view (see [Automation](automation.gen.html)). MIDI regions round-trip through the same subtree; audio regions persist their source-file path via the registry so captures and imports survive save/reload.
