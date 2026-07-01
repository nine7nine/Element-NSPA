# Lulada -- Threading & RT Safety

Lulada runs its audio graph at SCHED_FIFO on a PREEMPT_RT kernel, so the audio thread must never block on a lower-priority thread, never invert priority, and never allocate. This page documents the disciplines that make that hold across the whole codebase: the priority ladder, the three wait-free hand-off patterns, priority-inherited locking, the parallel render pool, and the concrete hazards the design defends against.

## Table of Contents

1. [The realtime constraint](#1-the-realtime-constraint)
2. [The priority ladder](#2-the-priority-ladder)
3. [Pattern 1: lock-free SPSC FIFO](#3-pattern-1-lock-free-spsc-fifo)
4. [Pattern 2: copy-on-write snapshot](#4-pattern-2-copy-on-write-snapshot)
5. [Pattern 3: PI locking and signalling](#5-pattern-3-pi-locking-and-signalling)
6. [Parallel rendering](#6-parallel-rendering)
7. [Hazards the discipline defends against](#7-hazards-the-discipline-defends-against)
8. [Adding an RT-touching feature](#8-adding-an-rt-touching-feature)

---

## 1. The realtime constraint

The audio callback runs on a thread promoted to SCHED_FIFO near the top of the RT band (the process is put in `REALTIME_PRIORITY_CLASS` before `main()`; see [Winelib Runtime](winelib-runtime.gen.html)). Three failure modes would each cause an xrun:

- **Blocking on a lock a lower-priority thread holds** -- unbounded if that thread is preempted.
- **Priority inversion** -- a mid-priority thread starving the lock holder while the RT thread waits.
- **Allocation / syscalls with unbounded latency** on the render path -- `malloc`, page faults, file I/O, a wineserver round-trip.

Lulada's answer is to keep the audio thread **wait-free** wherever possible, and where a lock is unavoidable, to make it priority-inheriting (via JUCE-NSPA's librtpi primitives -- see [JUCE-NSPA](juce-nspa.gen.html#4-rt-safe-synchronization)). Every cross-thread data structure in the engine is one of three shapes below.

## 2. The priority ladder

Threads are placed so nothing the audio thread depends on can be starved by something less important.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 350" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.6; }
    .cy  { fill: #16242b; stroke: #7dcfff; stroke-width: 1.4; }
    .pur { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.4; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.4; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .red { fill: #2a1a1a; stroke: #f7768e; stroke-width: 1.2; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy  { fill: #7dcfff; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .col { fill: #565f89; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="350" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="title">SCHED_FIFO ladder -- higher = preempts everything below</text>

  <text x="120" y="52" text-anchor="middle" class="col">priority</text>
  <text x="500" y="52" text-anchor="middle" class="col">thread</text>

  <rect x="60" y="62" width="120" height="34" class="red"/><text x="120" y="83" text-anchor="middle" class="lbl-sm">99</text>
  <rect x="220" y="62" width="640" height="34" class="red"/><text x="240" y="83" class="lbl-mut">kernel threads only (reserved)</text>

  <rect x="60" y="102" width="120" height="34" class="grn"/><text x="120" y="123" text-anchor="middle" class="lbl-grn">88-89</text>
  <rect x="220" y="102" width="640" height="34" class="grn"/><text x="240" y="123" class="lbl-mut">JACK / PipeWire callback</text>

  <rect x="60" y="142" width="120" height="34" class="hot"/><text x="120" y="163" text-anchor="middle" class="lbl-yel">80 (ceiling)</text>
  <rect x="220" y="142" width="640" height="34" class="hot"/><text x="240" y="163" class="lbl-yel">Audio thread -- RootGraph render, wait-free</text>

  <rect x="60" y="182" width="120" height="34" class="hot"/><text x="120" y="203" text-anchor="middle" class="lbl-yel">70</text>
  <rect x="220" y="182" width="640" height="34" class="box"/><text x="240" y="203" class="lbl-sm">GraphWorker pool -- parallel plugin processBlock (one rung below audio)</text>

  <rect x="60" y="222" width="120" height="34" class="cy"/><text x="120" y="243" text-anchor="middle" class="lbl-cy">RT (mid)</text>
  <rect x="220" y="222" width="640" height="34" class="cy"/><text x="240" y="243" class="lbl-mut">Disk-IO threads -- Playback_DS / Record_DS, woken by a PI-boosted futex</text>

  <rect x="60" y="262" width="120" height="34" class="box"/><text x="120" y="283" text-anchor="middle" class="lbl-sm">SCHED_OTHER</text>
  <rect x="220" y="262" width="640" height="34" class="box"/><text x="240" y="283" class="lbl-mut">Message thread (UI), background workers, wineserver dispatchers</text>

  <text x="60" y="322" class="lbl-mut">A lock shared by an RT thread and a SCHED_OTHER thread must be PI-inheriting -- else</text>
  <text x="60" y="336" class="lbl-mut">the holder is starved by a mid-prio thread while the RT thread waits. PI prevents it.</text>
</svg>
</div>

## 3. Pattern 1: lock-free SPSC FIFO

For a message-thread action that must take effect on the audio thread -- launching a clip, arming a record, starting a `Playback_DS` -- the UI enqueues a small record on a single-producer/single-consumer FIFO (`juce::AbstractFifo` over a fixed array), and the audio thread drains it at block start. No lock is ever taken.

The tracker/session/arrangement launch scheduler is the canonical example. Its FIFO is 64 slots; the audio thread collapses drained requests into a per-target `PendingAction` table, so **the latest request per target wins** -- re-banging a queued clip cancels the prior request for free, and a full FIFO (far beyond human click rate) drops the newest request rather than blocking.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 220" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-grn  { stroke: #9ece6a; stroke-width: 1.4; fill: none; }
    .ln-yel  { stroke: #e0af68; stroke-width: 1.4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="220" class="bg"/>
  <text x="450" y="22" text-anchor="middle" class="title">One producer, one consumer, no lock</text>
  <rect x="30" y="44" width="220" height="70" class="grn"/>
  <text x="48" y="64" class="lbl-grn">Message thread</text>
  <text x="48" y="82" class="lbl-mut">enqueue LaunchReq</text>
  <text x="48" y="96" class="lbl-mut">{target, beat, wantPlaying}</text>
  <rect x="340" y="50" width="220" height="58" class="box"/>
  <text x="450" y="72" text-anchor="middle" class="lbl-sm">64-slot AbstractFifo</text>
  <text x="450" y="88" text-anchor="middle" class="lbl-mut">full -> drop newest (no block)</text>
  <rect x="650" y="44" width="220" height="70" class="hot"/>
  <text x="668" y="64" class="lbl-yel">Audio thread</text>
  <text x="668" y="82" class="lbl-mut">drain -> PendingAction[target]</text>
  <text x="668" y="96" class="lbl-mut">latest per target wins</text>
  <line x1="250" y1="79" x2="340" y2="79" class="ln-grn"/>
  <line x1="560" y1="79" x2="650" y2="79" class="ln-yel"/>
  <rect x="30" y="140" width="840" height="60" class="box"/>
  <text x="50" y="162" class="lbl-mut">The record carries a beat target: negative = fire at the next block start; otherwise fire</text>
  <text x="50" y="178" class="lbl-mut">in the block whose range contains it. Clips on the same beat flip together, zero skew.</text>
  <text x="50" y="194" class="lbl-mut">AudioClipNode uses the same shape to hand a Playback_DS to the render. See Tracker.</text>
</svg>
</div>

## 4. Pattern 2: copy-on-write snapshot

For data the audio thread must *read* every block while the UI keeps editing it -- a MIDI note list, an automation curve, a lane's region table -- Lulada publishes an immutable snapshot by atomic pointer swap and reclaims the old one only after the audio thread has provably moved past it.

The cycle (used by `MidiNoteRegion`, `AutomationTrack`/`AutomationRegion`, and the automation engine's track-list):

1. The UI builds a **new immutable** copy with the edit applied.
2. It `atomic_exchange`s the live pointer to the new copy.
3. It pushes the displaced pointer onto a UI-thread trash deque, stamped with the current audio epoch.
4. The audio thread bumps a per-owner epoch counter once per block and `atomic_load`s the live pointer, using that one snapshot for the whole render.
5. A message-thread sweep (`AsyncUpdater`) frees trashed pointers only once the audio epoch has advanced past their stamp -- guaranteeing no audio-thread reader still holds them.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 290" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-grn  { stroke: #9ece6a; stroke-width: 1.4; fill: none; }
    .ln-yel  { stroke: #e0af68; stroke-width: 1.4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="290" class="bg"/>
  <text x="450" y="22" text-anchor="middle" class="title">Leaked-pointer publish + epoch-gated reclaim</text>

  <rect x="30" y="42" width="250" height="96" class="grn"/>
  <text x="48" y="62" class="lbl-grn">Message thread (edit)</text>
  <text x="48" y="80" class="lbl-mut">1. build new immutable copy</text>
  <text x="48" y="94" class="lbl-mut">2. atomic_exchange live ptr</text>
  <text x="48" y="108" class="lbl-mut">3. old ptr -> trash (stamp epoch)</text>
  <text x="48" y="126" class="lbl-mut">5. sweep frees past-epoch trash</text>

  <rect x="330" y="60" width="220" height="60" class="box"/>
  <text x="440" y="82" text-anchor="middle" class="lbl-sm">atomic&lt;const List*&gt;</text>
  <text x="440" y="98" text-anchor="middle" class="lbl-mut">always non-null (empty pre-published)</text>
  <text x="440" y="112" text-anchor="middle" class="lbl-mut">+ epoch counter</text>

  <rect x="600" y="42" width="270" height="96" class="hot"/>
  <text x="618" y="62" class="lbl-yel">Audio thread (render)</text>
  <text x="618" y="80" class="lbl-mut">4a. fetch_add epoch (once/block)</text>
  <text x="618" y="94" class="lbl-mut">4b. atomic_load snapshot ptr</text>
  <text x="618" y="108" class="lbl-mut">read same snapshot all block</text>
  <text x="618" y="126" class="lbl-mut">never allocates, never frees</text>

  <line x1="280" y1="90" x2="330" y2="90" class="ln-grn"/>
  <line x1="600" y1="90" x2="550" y2="90" class="ln-yel"/>

  <rect x="30" y="164" width="840" height="106" class="box"/>
  <text x="50" y="186" class="lbl-grn">Why not std::atomic&lt;shared_ptr&gt;</text>
  <text x="50" y="206" class="lbl-mut">libstdc++ implements atomic&lt;shared_ptr&gt; with an internal spinlock -- not wait-free,</text>
  <text x="50" y="222" class="lbl-mut">not PI-aware on PREEMPT_RT. The raw atomic pointer swap is wait-free for the reader</text>
  <text x="50" y="238" class="lbl-mut">and alloc-free for all but the UI edit/sweep. Snapshot pre-published empty -- never null;</text>
  <text x="50" y="254" class="lbl-mut">the epoch stamp guarantees a freed pointer is never one the render is still reading.</text>
</svg>
</div>

## 5. Pattern 3: PI locking and signalling

Some paths genuinely need a lock or a wakeup between threads. These use JUCE-NSPA's priority-inheriting primitives so an RT waiter can never be starved:

- **`juce::CriticalSection`** is a librtpi recursive PI mutex on the winelib build (`FUTEX_LOCK_PI`). The tracker's `engineLock_` (advance vs editor read), the sampler's `sampleLock` (slot publish), and the automation engine's `lookupLock_` all rely on this: a UI thread holding one while the audio or MIDI thread waits is boosted to the waiter's priority until it releases.
- **`juce::WaitableEvent`** is a `PiMutex` + `PiCond` pair. The disk streamer's audio-thread `signal()` boosts the waiting IO thread through the kernel futex -- the IO thread refills the ring and the render never sees an empty buffer.
- **Tiered lock avoidance.** Even a PI lock costs something, so hot consultation paths short-circuit first. The automation engine's MIDI-thread mute lookup loads an atomic `activeTrackCount()` and returns immediately when it is zero (the common "no automation configured" case), taking the PI `CriticalSection` only when tracks actually exist. Teardown drains any in-flight lookups by acquiring and releasing that same lock after the engine pointer is cleared, so no handler is mid-dereference at destruction.

## 6. Parallel rendering

When a render layer contains more than one expensive plugin `processBlock`, the audio thread fans the layer out to a **`GraphWorker` pool** and joins before the block ends. Each worker (`src/engine/graphnode.cpp`) is a `juce::Thread` that self-promotes to **SCHED_FIFO 70** -- deliberately one rung below the audio ceiling -- is synchronised by `WaitableEvent`, holds only raw index arrays (never a reference to the graph, so lifetime stays trivial), and performs **zero allocations** on the render path. If the self-promotion fails (no `RLIMIT_RTPRIO`), a worker simply runs at SCHED_OTHER and the render still completes -- correctness never depends on the RT promotion. `GraphBuilder` decides which layers are worth fanning out (a layer with a single expensive op runs inline); see [Graph Engine](graph-engine.gen.html#6-parallel-layers).

## 7. Hazards the discipline defends against

The patterns above are not abstract -- each was hardened against a concrete failure found during development:

| Hazard | Where | How the design handles it |
| --- | --- | --- |
| Use-after-free of an edited list on the audio thread | MIDI regions, automation | Epoch-gated reclaim -- a pointer is freed only after the render advanced past it |
| Priority inversion on a shared lock | tracker/sampler/automation locks | `CriticalSection` is a librtpi PI mutex on winelib |
| Torn read of a half-written edit | note lists, curves, region tables | Immutable snapshot, single atomic pointer swap |
| Voice-loop / ping-pong reverse-base UAF, stale selection | sampler voices | Ref-counted `Ptr` captured at note-on; two-phase slot publish under `sampleLock` |
| Duplicate note ids corrupting selection + velocity | piano-roll | Id-stable notes; ids pre-assigned at record time so diff commands address them deterministically |
| Descending curve segment drawn as a rising sawtooth | automation / envelope | Fixed segment evaluation on the unified 2D-Bezier model |
| A launched-but-never-stopped "rogue" sequence emitting | tracker/session | Force-stop reconciliation + the RT-safety bundle around the launch scheduler |
| IO thread starved while the render waits for audio data | disk streaming | PI-boosted `WaitableEvent` wakeup; ring pre-buffers `seconds_to_buffer` |

## 8. Adding an RT-touching feature

When a new feature needs to move data between the UI and the audio thread, the question is always *which of the three patterns fits*:

- **A one-shot action** (start/stop/launch/arm)? Use an **SPSC FIFO**; make the latest request per target win.
- **A body of data read every block** (notes, curves, regions)? Use a **COW snapshot** with epoch-gated reclaim; never mutate in place.
- **An unavoidable rendezvous** (an IO refill, a brief consistency window)? Use a **PI primitive** (`CriticalSection` / `WaitableEvent`), and add a cheap atomic fast-path so the hot case never takes the lock.

And the invariants that hold everywhere: the audio thread never allocates, never frees, never takes a non-PI lock, and never makes a wineserver round-trip on the render path. If a feature can't be expressed within those, it belongs on the message thread with its result published across, not computed on the audio thread.
