# Lulada -- Automation

This page documents the automation subsystem: the wait-free, song-owned engine that applies parameter curves on the audio thread, the three kinds of target it can drive, the unified 2D-Bezier curve model shared with the volume envelope, and how the overlay UI presents it on both the arrangement and the piano-roll.

## Table of Contents

1. [Song-owned engine, view-owned presentation](#1-song-owned-engine-view-owned-presentation)
2. [Tracks, regions, modes](#2-tracks-regions-modes)
3. [Targets](#3-targets)
4. [The wait-free engine](#4-the-wait-free-engine)
5. [The per-block apply](#5-the-per-block-apply)
6. [Mapping-engine arbitration](#6-mapping-engine-arbitration)
7. [The curve model](#7-the-curve-model)
8. [Touch record](#8-touch-record)
9. [The overlay UI](#9-the-overlay-ui)
10. [Persistence](#10-persistence)

---

## 1. Song-owned engine, view-owned presentation

Automation is split into two domains, and keeping them apart is the central design decision:

- **Song domain** -- the `AutomationEngine` owns the `AutomationTrack`s: their curve data, target keys, and modes. This is what *plays*, regardless of which view is open. It persists under `tags::automationTracks`.
- **View domain** -- an `AutomationBinding` (`services/timeline/automation_binding.hpp`) records only *where and how* the arrangement draws a track (its lane, colour, height, overlay-vs-dedicated). It references the engine track by `trackId` and persists as a peer list under `tags::arrangement`.

So timeline automation plays whether or not the arrangement view is active, and the presentation can change (overlay under a lane, or its own dedicated row) without touching the curve data. A binding is an overlay when `ownerLaneId` is set and a dedicated lane when it is null.

## 2. Tracks, regions, modes

An `AutomationTrack` (`src/dsp/automation`) holds a target key and a set of `AutomationRegion`s (curve segments placed on the timeline). Its `AutomationMode` mirrors Ardour/Zrythm:

| Mode | Engine behaviour |
| --- | --- |
| `Off` | Track ignored entirely -- no events, mute table not set |
| `Read` | Sample the active region each block, dispatch through the target, set the mute table |
| `Record` | Drain the touch-record FIFO into a soft buffer; UI materialises it on touch release |

`Read` and `Record` coexist: parts of the timeline not being touch-overwritten keep playing back while the touched region records. Record mode is further `Touch` (ends on knob release) or `Latch` (records until transport stop).

## 3. Targets

An `AutomationTrack`'s persistent target *key* resolves to a live `AutomationTarget` (`services/automation/automation_target.hpp`) -- the actual writable destination on the graph. There are three kinds:

| Kind | Destination | Timing |
| --- | --- | --- |
| `PluginParam` | a `juce::AudioProcessorParameter` on a hosted VST/CLAP/AU | coarse (one `setValue` per block) |
| `NodeParam` | an `element::Parameter` on an internal node (Tracker BPM, Sampler mix, clip gain, ...) | coarse; sample-accurate is the node's responsibility |
| `MidiCc` | a `(channel, ccNumber)` pair | sample-accurate by construction |

The engine rebuilds targets whenever graph topology changes (nodes added/removed, plugins re-scanned). Plugin/node pointers are **raw non-owning**; the engine invalidates a target (`kind = Invalid`) *before* the backing object is destroyed, on topology callbacks, so the audio thread never dereferences a dangling target.

## 4. The wait-free engine

`AutomationEngine` (`services/automation/automation_engine.hpp`) runs one instance per graph and is engineered so **every audio-thread path is wait-free** -- no locks, no allocations. All UI-side mutation uses a leaked-pointer + epoch-gated trash-bin pattern:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 300" xmlns="http://www.w3.org/2000/svg">
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
  <rect x="0" y="0" width="900" height="300" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="title">Leaked-pointer + epoch-gated reclaim -- wait-free for the audio reader</text>

  <rect x="30" y="44" width="330" height="110" class="grn"/>
  <text x="48" y="64" class="lbl-grn">Message thread (UI / session load)</text>
  <text x="48" y="84" class="lbl-mut">addTrack / removeTrack / bindXxx / unbindTarget</text>
  <text x="48" y="100" class="lbl-mut">builds a NEW track-pointer snapshot,</text>
  <text x="48" y="114" class="lbl-mut">atomic-stores liveTracks_, old snapshot -&gt; trash</text>
  <text x="48" y="134" class="lbl-mut">sweepTrash() reclaims once audioEpoch_ passes</text>

  <rect x="540" y="44" width="330" height="110" class="hot"/>
  <text x="558" y="64" class="lbl-yel">Audio thread (applyForBlock)</text>
  <text x="558" y="84" class="lbl-mut">1. fetch_add audioEpoch_ (gates reclaim)</text>
  <text x="558" y="100" class="lbl-mut">2. atomic_load liveTracks_ snapshot</text>
  <text x="558" y="114" class="lbl-mut">3. per track: advance epoch, sample region,</text>
  <text x="558" y="128" class="lbl-mut">   dispatch via bound target</text>

  <line x1="360" y1="99" x2="540" y2="99" class="ln"/>

  <rect x="30" y="180" width="840" height="90" class="box"/>
  <text x="50" y="202" class="lbl-sm">Empty state (zero tracks) = ~3 atomic ops: one load + one fetch_add + one null check</text>
  <text x="50" y="222" class="lbl-mut">Retired targets + removed tracks + snapshots carry the epoch they were trashed at</text>
  <text x="50" y="238" class="lbl-mut">sweepTrash() frees them only after the audio thread advances past that epoch -- no UAF</text>
  <text x="50" y="254" class="lbl-mut">Why not atomic&lt;shared_ptr&gt;: libstdc++ uses a spinlock -- not wait-free / PI-aware</text>
</svg>
</div>

Target rebinds work the same way: a freshly-built `AutomationTarget` is published via atomic store onto the track, and the old target moves to `retiredTargets_` for epoch-gated reclaim -- so the audio thread can hold a target pointer across a block boundary without use-after-free.

## 5. The per-block apply

`applyForBlock(currentBeats, beatsPerBlock, numSamples, sampleRate, outMidi)` is the single audio-thread entry point, called from the graph render with the block's beat range:

1. Bump the per-engine audio epoch (gates trash reclaim).
2. Snapshot-load the live track-pointer vector.
3. For each track: advance its epoch, update its mute slot, and -- if `mode == Read`, a target is bound, and an active region covers the playhead -- sample the region and dispatch via the target.

Emission granularity differs by target kind. **MIDI CC** targets are sampled at a sub-block stride (`kAutomationSubBlockStride = 64` samples), giving ~16 sample-accurate `controllerEvent`s per block at 48 kHz/1024 -- well under any MIDI bandwidth ceiling. **Plugin and node params** are written coarse (one `setValue` at frame 0 per block) in the current phase; sample-accurate plugin emission is deferred. Passing `beatsPerBlock = 0` (or `bpm <= 0`) disables sample-accurate MIDI emission and falls back to a single write at offset 0 -- useful for test benches with no transport.

## 6. Mapping-engine arbitration

Automation and MIDI-CC mapping can target the same parameter, so they must not fight. The engine maintains a fixed table of **mute slots** (`kMaxMuteSlots = 256`, atomic bools): when a track is actively driving a parameter in `Read` mode, its mute slot is set, and the `MappingEngine`'s MIDI handlers check `isMappingMuted(slot)` before writing -- so the automation curve wins.

The lookup is tiered for the MIDI thread's benefit:

- **Fast path** -- an atomic `activeTrackCount()` load short-circuits to "not muted" when zero tracks are bound (the common "no automation configured" case), so MIDI handlers skip the lock entirely.
- **Slow path** -- a brief `juce::CriticalSection` (PI-correct on PREEMPT_RT) guards an O(N) walk of the bound tracks. The same lock is taken by UI-thread bind/unbind, so the two tolerate racing. `drainPendingLookups()` lets the engine owner flush in-flight MIDI-thread lookups during teardown, after the `MappingEngine`'s engine pointer is cleared, guaranteeing no handler is mid-dereference when the engine is destroyed.

The audio thread itself never takes this lock -- it consults mute slots by index with a single atomic load.

## 7. The curve model

Every automation segment uses **one** curve primitive, `CurveOptions` (`src/dsp/automation/curve.hpp`), the same 2D draggable-handle Bezier the audio-clip volume envelope uses. This unification replaced an older algorithmic 1D "curviness" family so that the timeline overlay, the piano-roll CC lanes, and the volume envelope all bend curves identically -- drag a handle in 2D.

| Field | Meaning |
| --- | --- |
| `offsetT` | Normalised X of the bend handle within the segment, clamped `[0.25, 0.75]` (outside, the Bezier's `x(u)` goes non-monotonic) |
| `offsetV` | Normalised value offset of the handle from the chord midpoint -- positive bulges up, negative down, regardless of segment direction |

Defaults `(0.5, 0.0)` put the handle on the straight chord = linear. `evaluateSegment(x, v0, v1, opts)` returns the normalised value: with default handle it is the straight lerp `v0..v1`; bent, it evaluates the value-space quadratic Bezier passing through `(offsetT, 0.5*(v0+v1) + offsetV)` at `u = 0.5` -- identical to the volume envelope's construction. It is POD/trivially-copyable so vectors of points stay cheap.

## 8. Touch record

In `Record` mode, UI/control-thread parameter touches are pushed into the track's `writeFifo_` as `AutomationWriteEvent`s (POD, so `AbstractFifo` storage needs no per-slot construction). The audio thread drains the FIFO at block start into a soft buffer; on touch release the UI materialises that buffer into a hard `AutomationRegion` snapshot. `Touch` ends recording on release; `Latch` keeps recording until transport stop.

## 9. The overlay UI

Automation is edited on the same 2D-Bezier curve surface on both authoring views:

- **Arrangement** -- automation renders as a **superimposed overlay** under a lane (not stacked lanes), with a chip rail listing the lane's bindings; each binding gets its own colour and the overlay area is vertically resizable. Editing is gated on a dedicated **Env tool** so envelope/automation point edits don't collide with region gestures. Points support add / move / delete / type and 2D handle bends, snapping to the cursor on move.
- **Piano-roll** -- the same curve editor drives clip-local MIDI CC lanes as a tabbed multi-CC overlay.

The target picker is **graph-wide** (any node's parameter, grouped per node), because a modular DAW has no automatic track-to-parameter mapping -- you bind an automation track to any parameter anywhere in the graph. Timeline automation is song-owned and absolute (plays against the transport tempo map); clip-local automation is local and loops with its clip -- same engine primitives, different ownership.

## 10. Persistence

`saveToValueTree` writes all owned tracks to a `tags::automationTracks` child (no child when zero tracks -- legacy sessions load as if no automation existed). `loadFromValueTree` reconstructs the tracks but deliberately does **not** re-bind targets: resolving each track's target key to a live `AutomationTarget` (and allocating its mute slot) happens in a separate session-load hook after the graph exists, via the `bindPluginParam` / `bindNodeParam` / `bindMidiCc` calls. The presentation `AutomationBinding`s persist separately under `tags::arrangement`, referencing tracks by `trackId`.
