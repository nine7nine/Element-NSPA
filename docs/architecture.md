# Lulada -- Architecture & Design

Lulada is a fork of Kushview [Element](https://github.com/kushview/element) turned into a full DAW and rebuilt as a single winelib process that hosts Win32 plugins in-process on [Wine-NSPA](https://github.com/nine7nine) at realtime priority. This page is the system map: what changes from stock Element, the thread model, and the one idea that ties the DAW surfaces to the audio engine.

## Table of Contents

1. [What Lulada is](#1-what-lulada-is)
2. [Stock Element vs Lulada](#2-stock-element-vs-lulada)
3. [One process, a few threads](#3-one-process-a-few-threads)
4. [Everything is a graph node](#4-everything-is-a-graph-node)
5. [Crossing the thread boundary](#5-crossing-the-thread-boundary)
6. [Source layout](#6-source-layout)
7. [Design rules](#7-design-rules)
8. [Document index](#8-document-index)

---

## 1. What Lulada is

Element is a modular audio plugin host: a graph of nodes you wire together, with plugin editors embedded in the canvas. Lulada keeps that core intact and adds two things on top.

- **A realtime winelib runtime.** Compiled with `wineg++`, the one binary is native Linux ELF that also speaks the Win32 ABI, so it loads **VST2 / VST3 / CLAP plugins directly in-process** through Wine-NSPA -- no bridge process, no IPC audio hop -- and runs its graph at SCHED_FIFO priority under a PREEMPT_RT kernel.
- **A DAW layer.** A pattern tracker, a piano-roll, an Ableton/Bitwig Session View, a linear Arrangement timeline, an FT2-style sampler, and a sample-accurate automation engine -- all built as graph nodes and views, not a separate application.

The through-line is that the DAW is *not* bolted onto the side of the host. Each surface drives ordinary graph nodes, so a tracker pattern can play a hosted VST synth whose audio flows into the internal sampler's bus and out through a hosted CLAP reverb, in one render pass.

## 2. Stock Element vs Lulada

Every Lulada change is additive: stock Element still builds and runs (see [Building](building-packaging.gen.html)). The winelib runtime and the DAW surfaces are what the fork adds.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 940 560" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg    { fill: #1a1b26; }
    .van   { fill: #24283b; stroke: #565f89; stroke-width: 1.2; }
    .add   { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .keep  { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.3; }
    .lbl     { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-blu { fill: #7aa2f7; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-mtd { fill: #565f89; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .col     { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .div     { stroke: #6b7398; stroke-width: 1; stroke-dasharray: 7,4; }
  </style>
  <rect x="0" y="0" width="940" height="560" class="bg"/>
  <text x="235" y="28" text-anchor="middle" class="lbl-mtd">Stock Element</text>
  <text x="700" y="28" text-anchor="middle" class="col">Lulada</text>
  <line x1="470" y1="40" x2="470" y2="540" class="div"/>

  <!-- left: stock -->
  <rect x="40" y="52" width="390" height="52" class="van"/>
  <text x="235" y="74" text-anchor="middle" class="lbl">Native Linux build (cmake/clang)</text>
  <text x="235" y="90" text-anchor="middle" class="lbl-mut">SCHED_OTHER; no Win32 plugin hosting</text>

  <rect x="40" y="116" width="390" height="52" class="van"/>
  <text x="235" y="138" text-anchor="middle" class="lbl">Modular graph host</text>
  <text x="235" y="154" text-anchor="middle" class="lbl-mut">nodes, routing, sub-graphs, Lua, undo</text>

  <rect x="40" y="180" width="390" height="52" class="van"/>
  <text x="235" y="202" text-anchor="middle" class="lbl">Plugin formats</text>
  <text x="235" y="218" text-anchor="middle" class="lbl-mut">native LV2 / VST3 / LADSPA / CLAP</text>

  <rect x="40" y="244" width="390" height="52" class="van"/>
  <text x="235" y="266" text-anchor="middle" class="lbl">Windowing</text>
  <text x="235" y="282" text-anchor="middle" class="lbl-mut">X11</text>

  <rect x="40" y="308" width="390" height="52" class="van"/>
  <text x="235" y="330" text-anchor="middle" class="lbl">Authoring</text>
  <text x="235" y="346" text-anchor="middle" class="lbl-mut">graph editor + node editors</text>

  <!-- right: lulada -->
  <rect x="510" y="52" width="390" height="52" class="add"/>
  <text x="705" y="74" text-anchor="middle" class="lbl-grn">Winelib build (wineg++)</text>
  <text x="705" y="90" text-anchor="middle" class="lbl-mut">SCHED_FIFO under PREEMPT_RT; in-process Win32 host</text>

  <rect x="510" y="116" width="390" height="52" class="keep"/>
  <text x="705" y="138" text-anchor="middle" class="lbl-blu">Modular graph host  (kept from Element)</text>
  <text x="705" y="154" text-anchor="middle" class="lbl-mut">same nodes/routing/sub-graphs/Lua/undo, now RT-safe</text>

  <rect x="510" y="180" width="390" height="52" class="add"/>
  <text x="705" y="202" text-anchor="middle" class="lbl-grn">Plugin formats</text>
  <text x="705" y="218" text-anchor="middle" class="lbl-mut">Win32 VST2 / VST3 / CLAP via Wine-NSPA + internal nodes</text>

  <rect x="510" y="244" width="390" height="52" class="add"/>
  <text x="705" y="266" text-anchor="middle" class="lbl-grn">Windowing</text>
  <text x="705" y="282" text-anchor="middle" class="lbl-mut">Wayland (subsurface plugin embed) + X11 fallback</text>

  <rect x="510" y="308" width="390" height="52" class="add"/>
  <text x="705" y="330" text-anchor="middle" class="lbl-grn">Authoring  --  the DAW layer</text>
  <text x="705" y="346" text-anchor="middle" class="lbl-mut">tracker, piano-roll, session, arrangement, sampler, automation</text>

  <!-- legend -->
  <rect x="40" y="404" width="16" height="16" class="van"/>
  <text x="64" y="417" class="lbl-mut">stock Element</text>
  <rect x="230" y="404" width="16" height="16" class="keep"/>
  <text x="254" y="417" class="lbl-mut">kept, made RT-safe</text>
  <rect x="470" y="404" width="16" height="16" class="add"/>
  <text x="494" y="417" class="lbl-mut">added by Lulada</text>

  <rect x="40" y="448" width="860" height="80" class="keep"/>
  <text x="60" y="470" class="lbl-blu">The fork's leverage</text>
  <text x="60" y="490" class="lbl-mut">Element already models everything as a graph node with typed audio/CV/MIDI ports.</text>
  <text x="60" y="506" class="lbl-mut">Lulada reuses that: surfaces are node drivers; Win32 plugins are just more nodes.</text>
  <text x="60" y="522" class="lbl-mut">One graph -- tracker MIDI, sampler audio, a hosted VST all compose without special cases.</text>
</svg>
</div>

## 3. One process, a few threads

`element_app` is a single winelib executable. Everything runs in-process on a small set of threads with strict ownership.

| Thread | Priority | Owns |
| --- | --- | --- |
| Message thread | SCHED_OTHER | Component tree, all painting/input, session load/save, undo, launch *requests* |
| Audio thread | SCHED_FIFO (audio ceiling) | The graph render: `RootGraph` walks nodes in render order each block |
| Graph workers | SCHED_FIFO 70 (one rung below audio) | Parallel plugin `processBlock` for independent render layers |
| Disk-IO threads | SCHED_FIFO | Fill/drain streaming ringbuffers; woken by a PI-boosted futex |
| Background | low / SCHED_OTHER | Auth refresh, out-of-process plugin-scanner worker |

The audio thread never blocks on the message thread and never allocates. Where a plugin-heavy graph has independent work, the audio thread fans it out to a small pool of `GraphWorker` threads (self-promoted to SCHED_FIFO 70, synchronised by `WaitableEvent`, zero allocations on the render path) and joins before the block ends -- see [Graph Engine](graph-engine.gen.html#6-parallel-layers).

<div class="diagram-container">
<svg width="100%" viewBox="0 0 940 470" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg      { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .blu { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.5; }
    .pur { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.5; }
    .cy  { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl     { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy  { fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-grn  { stroke: #9ece6a; stroke-width: 1.4; fill: none; }
    .ln-cy   { stroke: #7dcfff; stroke-width: 1.4; fill: none; }
    .ln-yel  { stroke: #e0af68; stroke-width: 1.4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="940" height="470" class="bg"/>
  <text x="470" y="26" text-anchor="middle" class="title">Threads and the data that crosses between them</text>

  <!-- message thread -->
  <rect x="30" y="46" width="300" height="150" class="grn"/>
  <text x="48" y="66" class="lbl-grn">Message thread  (SCHED_OTHER)</text>
  <text x="48" y="84" class="lbl-mut">Component tree, paint, input</text>
  <rect x="48" y="94" width="264" height="28" class="box"/>
  <text x="180" y="112" text-anchor="middle" class="lbl-sm">DAW surfaces + node editors</text>
  <rect x="48" y="128" width="264" height="28" class="box"/>
  <text x="180" y="146" text-anchor="middle" class="lbl-sm">session load / save + undo</text>
  <rect x="48" y="162" width="264" height="28" class="box"/>
  <text x="180" y="180" text-anchor="middle" class="lbl-mut">edits build new immutable snapshots</text>

  <!-- audio thread -->
  <rect x="610" y="46" width="300" height="150" class="hot"/>
  <text x="628" y="66" class="lbl-yel">Audio thread  (SCHED_FIFO ceiling)</text>
  <text x="628" y="84" class="lbl-mut">wait-free, no locks, no alloc</text>
  <rect x="628" y="94" width="264" height="28" class="box"/>
  <text x="760" y="112" text-anchor="middle" class="lbl-sm">RootGraph render (ordered ops)</text>
  <rect x="628" y="128" width="264" height="28" class="box"/>
  <text x="760" y="146" text-anchor="middle" class="lbl-mut">loads snapshot ptr once per block</text>
  <rect x="628" y="162" width="264" height="28" class="box"/>
  <text x="760" y="180" text-anchor="middle" class="lbl-mut">applies launches at target beat</text>

  <!-- SPSC + snapshot channel -->
  <rect x="360" y="70" width="220" height="46" class="box"/>
  <text x="470" y="89" text-anchor="middle" class="lbl-sm">lock-free SPSC FIFOs</text>
  <text x="470" y="104" text-anchor="middle" class="lbl-mut">launch requests, region tables</text>
  <rect x="360" y="126" width="220" height="46" class="box"/>
  <text x="470" y="145" text-anchor="middle" class="lbl-sm">atomic snapshot pointers</text>
  <text x="470" y="160" text-anchor="middle" class="lbl-mut">COW notes / curves, epoch reclaim</text>

  <line x1="330" y1="93" x2="360" y2="93" class="ln-grn"/>
  <line x1="580" y1="93" x2="610" y2="93" class="ln-yel"/>
  <line x1="330" y1="149" x2="360" y2="149" class="ln-grn"/>
  <line x1="580" y1="149" x2="610" y2="149" class="ln-yel"/>

  <!-- workers -->
  <rect x="610" y="214" width="300" height="70" class="hot"/>
  <text x="628" y="234" class="lbl-yel">Graph workers  (SCHED_FIFO 70)</text>
  <text x="628" y="252" class="lbl-mut">parallel plugin processBlock for</text>
  <text x="628" y="266" class="lbl-mut">independent render layers, joined per block</text>
  <line x1="760" y1="196" x2="760" y2="214" class="ln-yel"/>

  <!-- disk io -->
  <rect x="360" y="214" width="220" height="70" class="cy"/>
  <text x="378" y="234" class="lbl-cy">Disk-IO threads</text>
  <text x="378" y="252" class="lbl-mut">Playback_DS / Record_DS rings</text>
  <text x="378" y="266" class="lbl-mut">woken by a PI-boosted futex</text>
  <line x1="610" y1="119" x2="580" y2="119" class="ln-cy"/>
  <line x1="470" y1="172" x2="470" y2="214" class="ln-cy"/>

  <!-- runtime -->
  <rect x="30" y="214" width="300" height="70" class="pur"/>
  <text x="48" y="234" class="lbl-pur">Wine-NSPA runtime</text>
  <text x="48" y="252" class="lbl-mut">hosted Win32 plugins load here;</text>
  <text x="48" y="266" class="lbl-mut">RT + OLE set up before main()</text>

  <rect x="30" y="308" width="880" height="140" class="box"/>
  <text x="50" y="330" class="lbl-cy">The rule</text>
  <text x="50" y="352" class="lbl-mut">A UI edit never mutates engine state in place. It builds a new immutable snapshot</text>
  <text x="50" y="368" class="lbl-mut">(notes, a curve, a region table) and hands the pointer across atomically, or enqueues</text>
  <text x="50" y="384" class="lbl-mut">a launch request on an SPSC FIFO. The audio thread loads the snapshot once per block</text>
  <text x="50" y="400" class="lbl-mut">and reads it all render -- never a half-written edit, never a wait on a UI-held lock.</text>
  <text x="50" y="424" class="lbl-grn">Green = UI-side data down;  cyan = streamed audio;  amber = the realtime render side.</text>
</svg>
</div>

## 4. Everything is a graph node

The single most important architectural fact is that the DAW surfaces are **drivers of internal graph nodes**, registered in `src/engine/nodefactory.cpp` right alongside Element's originals. There is no parallel "song engine"; the timeline and the clip launcher schedule the same nodes the graph editor shows.

| Surface | Drives | How |
| --- | --- | --- |
| Tracker | `TrackerNode` (vendored vht engine) | Advances vht sequences; MIDI out |
| Piano-roll | `MidiPlayerNode` + `MidiNoteRegion` | Walks a copy-on-write note snapshot |
| Session View | `TrackerNode` / `MidiPlayerNode` columns | Per-clip launch on a lock-free FIFO |
| Arrangement | `AudioClipNode` / `MidiPlayerNode` / `TrackerNode` lanes | Beat-scheduled regions, same FIFO |
| Sampler | `SamplerNode` + `SampleBankPool` | MIDI-in, ft2 mixer, audio-out |
| Automation | `AutomationEngine` (song-owned) | Wait-free per-block param/CC dispatch |

<div class="diagram-container">
<svg width="100%" viewBox="0 0 940 340" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.4; }
    .pur { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.4; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.4; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-grn  { stroke: #9ece6a; stroke-width: 1.4; fill: none; }
    .ln-pur  { stroke: #bb9af7; stroke-width: 1.4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="940" height="340" class="bg"/>
  <text x="470" y="24" text-anchor="middle" class="title">Surfaces drive nodes; MIDI and audio flow through one graph</text>

  <!-- surfaces -->
  <rect x="30" y="44" width="150" height="30" class="grn"/><text x="105" y="63" text-anchor="middle" class="lbl-grn">Tracker</text>
  <rect x="30" y="86" width="150" height="30" class="grn"/><text x="105" y="105" text-anchor="middle" class="lbl-grn">Piano-roll</text>
  <rect x="30" y="128" width="150" height="30" class="grn"/><text x="105" y="147" text-anchor="middle" class="lbl-grn">Session View</text>
  <rect x="30" y="170" width="150" height="30" class="grn"/><text x="105" y="189" text-anchor="middle" class="lbl-grn">Arrangement</text>
  <rect x="30" y="212" width="150" height="30" class="pur"/><text x="105" y="231" text-anchor="middle" class="lbl-pur">Automation</text>

  <!-- nodes -->
  <rect x="300" y="60" width="180" height="34" class="box"/><text x="390" y="81" text-anchor="middle" class="lbl-sm">TrackerNode (MIDI out)</text>
  <rect x="300" y="110" width="180" height="34" class="box"/><text x="390" y="131" text-anchor="middle" class="lbl-sm">MidiPlayerNode (MIDI out)</text>
  <rect x="300" y="160" width="180" height="34" class="box"/><text x="390" y="181" text-anchor="middle" class="lbl-sm">AudioClipNode (audio)</text>

  <!-- graph sinks -->
  <rect x="560" y="70" width="160" height="40" class="box"/><text x="640" y="88" text-anchor="middle" class="lbl-sm">SamplerNode</text><text x="640" y="102" text-anchor="middle" class="lbl-mut">or hosted VST/CLAP synth</text>
  <rect x="560" y="150" width="160" height="40" class="box"/><text x="640" y="168" text-anchor="middle" class="lbl-sm">hosted FX (VST3/CLAP)</text><text x="640" y="182" text-anchor="middle" class="lbl-mut">reverb, EQ, ...</text>
  <rect x="760" y="110" width="150" height="40" class="hot"/><text x="835" y="128" text-anchor="middle" class="lbl-yel">RootGraph out</text><text x="835" y="142" text-anchor="middle" class="lbl-mut">audio device</text>

  <!-- surface->node (green = MIDI-authoring surfaces) -->
  <line x1="180" y1="59" x2="300" y2="77" class="ln-grn"/>
  <line x1="180" y1="101" x2="300" y2="127" class="ln-grn"/>
  <line x1="180" y1="143" x2="300" y2="127" class="ln-grn"/>
  <line x1="180" y1="185" x2="300" y2="177" class="ln-grn"/>
  <!-- automation -> nodes (purple) -->
  <line x1="180" y1="227" x2="300" y2="177" class="ln-pur"/>
  <line x1="180" y1="227" x2="300" y2="127" class="ln-pur"/>

  <!-- node -> sink -->
  <line x1="480" y1="77" x2="560" y2="90" class="ln-grn"/>
  <line x1="480" y1="127" x2="560" y2="90" class="ln-grn"/>
  <line x1="480" y1="177" x2="760" y2="130" class="ln-grn"/>
  <line x1="720" y1="90" x2="760" y2="122" class="ln-grn"/>
  <line x1="720" y1="170" x2="760" y2="138" class="ln-grn"/>

  <text x="300" y="270" class="lbl-grn">green = MIDI / audio through the graph</text>
  <text x="300" y="288" class="lbl-pur">purple = automation dispatch (param / CC writes into the same nodes)</text>
  <text x="300" y="306" class="lbl-mut">The graph editor shows these very nodes -- nothing the surfaces schedule is hidden.</text>
</svg>
</div>

## 5. Crossing the thread boundary

Because the audio thread must stay wait-free, every piece of data it reads is published, not mutated. Three patterns recur across the codebase (each documented in detail in the relevant subsystem page):

- **Lock-free SPSC FIFO** -- message thread enqueues a small record (a clip launch, a record request); the audio thread drains it at block start. Latest request per target wins, giving free cancellation. Used by the tracker/session/arrangement launch scheduler and the audio-clip player.
- **Copy-on-write snapshot** -- the UI builds a new immutable list (notes, CC points, region tables), `atomic_exchange`s the live pointer, and defers freeing the old one until the audio thread has advanced past it (epoch-gated trash). Used by `MidiNoteRegion` and the automation curve model. It deliberately avoids `std::atomic<shared_ptr>`, whose libstdc++ implementation takes an internal spinlock -- not wait-free, not PI-aware on PREEMPT_RT.
- **Priority-inherited signalling** -- the disk streamer's audio-thread `signal()` boosts the IO-thread waiter through a librtpi PI futex, so a lower-priority IO thread can't invert against the RT audio thread. See [Winelib Runtime](winelib-runtime.gen.html#5-priority-inheritance-on-the-hot-paths).

## 6. Source layout

| Path | Purpose |
| --- | --- |
| `src/main.cc` | Winelib entry: RT + OLE init, wineserver warm-up, exit diagnostics |
| `src/application.cpp` | `Application` / `Startup`: audio, plugins, MIDI, scripting bring-up |
| `src/engine` | Modular graph (`GraphManager`, `GraphNode`, `RootGraph`, `GraphBuilder`), transport; vendored `tracker/` + `sampler/` C cores |
| `src/nodes` | Internal node implementations (Tracker, MidiPlayer, Sampler, AudioClip, MIDI tools, Script, ...) |
| `src/services` | Service layer + `timeline/`, `automation/`, `audiostreaming/`, `sources/` |
| `src/ui` | All views incl. `pianoroll/`; arrangement, session, graph editor, mixer, Disk Op |
| `src/dsp` | Shared DSP: quantize ops, `automation/` curve + track model |
| `src/el`, `src/lua`, `src/scripting` | Element's Lua bindings (sol2), the Lua runtime, the Script node host |
| `include/element` | Public / internal headers |

## 7. Design rules

- Keep rendering and input on the message thread; keep the graph render wait-free on the audio thread.
- Cross the thread boundary with lock-free FIFOs and epoch-gated snapshots, never a lock on the audio path.
- Host plugins **in-process** via Wine-NSPA; never split the audio path across a bridge process.
- One graph, one kind of node: internal nodes and Win32 plugins are peers you can route freely.
- Tracker and piano-roll are equal first-class surfaces; the same MIDI clip appears in Session *and* Arrangement.
- Keep song-domain data (automation curves, clip contents) separate from view-domain presentation.
- Wayland vs X11 is an all-or-nothing choice per session -- no mixed-backend plugin embedding.

## 8. Document index

| Document | Covers |
| --- | --- |
| [Winelib Runtime](winelib-runtime.gen.html) | RT + OLE startup, the wineserver warm-up race, PI on the hot paths, plugin formats |
| [JUCE-NSPA](juce-nspa.gen.html) | The JUCE fork: winelib plugin hosting, the per-plugin Win32 worker, PI primitives, embedding |
| [Threading & RT Safety](threading-rt-safety.gen.html) | The priority ladder, the three wait-free hand-off patterns, PI locking, the worker pool |
| [Wayland Embedding](wayland-embedding.gen.html) | The all-wayland stack, shared `wl_display`, subsurface plugin embed, two-reader loop |
| [Graph Engine](graph-engine.gen.html) | Nodes, the node factory, buffer-reuse render ordering, parallel layers, threading |
| [Tracker](tracker.gen.html) | The vht engine, two-node design, FX columns, the launch scheduler |
| [Piano-roll](pianoroll.gen.html) | The COW note region, diff-command undo, velocity + CC lanes, quantize/humanize |
| [Session View](session-view.gen.html) | Clip grid, launch quantisation, the queued-state machine, follow actions |
| [Arrangement & Timeline](arrangement-timeline.gen.html) | Lanes, regions, one dispatch path, disk streaming, record, fades |
| [Automation](automation.gen.html) | COW curve model, the wait-free engine, targets/resolver, the overlay UI |
| [Sampler](sampler.gen.html) | ft2 mixer DSP, the instrument model, four buses, the session-global bank pool |
| [Building](building-packaging.gen.html) | The winelib configuration, the two forks, launch requirements |
