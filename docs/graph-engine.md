# Lulada -- Graph Engine

The modular audio graph is Lulada's core: a directed graph of nodes processed in dependency order every block. This page documents how nodes and connections are modelled, how internal nodes and hosted Win32 plugins become the same kind of citizen, how `GraphBuilder` turns a topology into a buffer-reusing (and parallelisable) op list, and how the render pass runs -- including the worker pool that fans out independent plugin work.

## Table of Contents

1. [The graph model](#1-the-graph-model)
2. [Processors and ports](#2-processors-and-ports)
3. [The node factory](#3-the-node-factory)
4. [Hosted plugins as nodes](#4-hosted-plugins-as-nodes)
5. [Building the render order](#5-building-the-render-order)
6. [Parallel layers](#6-parallel-layers)
7. [The render pass](#7-the-render-pass)
8. [Sub-graphs](#8-sub-graphs)
9. [Threading model](#9-threading-model)

---

## 1. The graph model

A Lulada session is, at bottom, a `GraphNode` -- a directed graph of processing nodes wired by connections. `RootGraph` is the top-level graph the audio device drives; nested `GraphNode`s appear as sub-graph nodes inside it. Editing (add/remove node, connect/disconnect) is mediated by `GraphManager` on the message thread; the audio thread only ever executes a pre-computed render plan.

| Concept | Type | Role |
| --- | --- | --- |
| Graph | `GraphNode` (`src/engine/graphnode.hpp`) | Owns nodes + connections; is itself a `Processor` so graphs nest |
| Node | `Processor` (`include/element/processor.hpp`) | A ref-counted processing unit with typed ports |
| Connection | `GraphNode::Connection : Arc` | `(sourceNode, sourcePort) -> (destNode, destPort)` |
| Manager | `GraphManager` (`src/engine/graphmanager.hpp`) | Message-thread topology edits, plugin instantiation, persistence |
| Top graph | `RootGraph` (`src/engine/rootgraph.hpp`) | The graph the audio callback renders |

## 2. Processors and ports

Every node is a `Processor`. Ports are typed -- audio, CV, and MIDI live in separate namespaces (`PortType`) -- so a node can carry any mix of audio, control-voltage, and MIDI I/O. The per-block work is handed a `RenderContext`:

```cpp
struct RenderContext {
    juce::AudioSampleBuffer audio;   // audio ports
    juce::AudioSampleBuffer cv;      // control-voltage ports
    MidiPipe                midi;    // one or more shared MIDI buffers, by index
};
```

A `MidiPipe` is a view over shared `MidiBuffer`s selected by index -- how one buffer pool is shared across the whole render without per-node allocation. Nodes come in two implementation flavours, and the graph treats them identically:

- **Native `Processor` subclasses** -- e.g. `TrackerNode`, `MidiPlayerNode`, the MIDI tools. They implement `render(RenderContext&)` directly.
- **`juce::AudioProcessor`-backed nodes** -- wrapped by `AudioProcessorNode` (`NodeFactory::wrap`). This is how both Element's DSP nodes (EQ, compressor, reverb, ...) and every hosted plugin plug into the same graph.

## 3. The node factory

`NodeFactory` (`src/engine/nodefactory.cpp`) registers the **internal** node providers -- always available regardless of installed plugins. Each is a `SingleNodeProvider<T>` under an `EL_NODE_ID_*` identifier:

| Node ID | Class | Kind |
| --- | --- | --- |
| `EL_NODE_ID_MIDI_SEQUENCER` | `TrackerNode` | Pattern tracker (MIDI out) |
| `EL_NODE_ID_MIDI_PLAYER` | `MidiPlayerNode` | Timeline MIDI clip player |
| `EL_NODE_ID_SAMPLER` | `SamplerNode` | FT2-style sampler instrument |
| `EL_NODE_ID_AUDIO_CLIP` | `AudioClipNode` | Disk-streamed audio region player/recorder |
| `EL_NODE_ID_AUDIO_ROUTER` | `AudioRouterNode` | Matrix audio routing |
| `EL_NODE_ID_MIDI_CHANNEL_SPLITTER` / `_ROUTER` / `_PROGRAM_MAP` / `_MONITOR` | MIDI tools | MIDI processing |
| `EL_NODE_ID_OSC_SENDER` / `_RECEIVER` | OSC nodes | OSC I/O |
| `EL_NODE_ID_SCRIPT` | `ScriptNode` | Lua-scripted DSP/MIDI node |

The DAW additions register here alongside Element's originals -- the Tracker, MIDI Player, Sampler, and Audio Clip nodes are ordinary providers, which is exactly what lets the Session View and Arrangement treat them as first-class graph nodes rather than bolted-on subsystems. A separate set of `juce::AudioProcessor`-backed DSP nodes (EQ, compressor, reverb, volume, ...) is provided through the audio-processor factory and can be hidden from the palette via a deny list.

## 4. Hosted plugins as nodes

A hosted Win32 VST2 / VST3 / CLAP plugin is instantiated (through the plugin manager and the winelib format host in [JUCE-NSPA](juce-nspa.gen.html#2-the-winelib-plugin-hosting-module)) as a `juce::AudioProcessor`, then wrapped by `AudioProcessorNode` -- **the same wrapper** the internal DSP nodes use. From the graph's point of view there is no difference between an internal node and a plugin: both are `Processor`s with ports, both are scheduled by `GraphBuilder`, both render in one pass.

That uniformity is the whole point of the modular core: a Tracker's MIDI output can drive a hosted VST synth, whose stereo output flows into an internal Sampler bus and then a hosted CLAP reverb, all wired in the graph editor and rendered in dependency order in one block. Under the hood, a plugin's `processBlock` is a marshalled call onto that plugin's dedicated Win32 worker (see [JUCE-NSPA](juce-nspa.gen.html#3-the-per-plugin-win32-worker)); the plugin's editor window embeds into the graph's plugin window.

## 5. Building the render order

When the topology changes, `GraphBuilder` (`src/engine/graphbuilder.hpp`) turns the graph into a linear list of **render ops** (`GraphOp`) on the message thread. It does classic shared-buffer allocation: walk the nodes in dependency order, and for each emit copy / clear / process ops that reuse a small pool of shared audio/CV/MIDI buffers instead of a buffer per edge. `buffersNeeded(PortType)` reports how many the plan needs; node latency is compensated with delay ops so parallel signal paths stay phase-aligned.

The op list is deliberately frugal. Cheap housekeeping (copy/clear/delay) is separated from the expensive `ProcessBufferOp`s (real plugin/node `processBlock` calls), and the builder avoids needless work -- for instance it does not insert an audio delay op for a control port on a single connection. The result is an `Array<GraphOp>` the audio thread executes in order against the shared buffer pool.

## 6. Parallel layers

Beyond the linear list, `GraphBuilder` post-processes the ops into **layers** for parallel dispatch (`computeRenderingLayers`). Two ops share a layer only if their buffer dependencies do not conflict -- write/read, read/write, and write/write are checked independently on the audio and MIDI buffer namespaces. Layer order is honoured at execution; ops *within* a layer are independent and may run concurrently. `countExpensiveOpsPerLayer` then counts the real `processBlock`s per layer, which gates whether a layer is worth fanning out at all.

At render time, a layer with more than one expensive op is dispatched across the **`GraphWorker` pool** (`src/engine/graphnode.cpp`): a set of `juce::Thread`s that each self-promote to **SCHED_FIFO 70** -- one rung below the audio ceiling -- synchronise via `WaitableEvent`, hold only raw op-index arrays (never a reference to the `GraphNode`, so lifetime stays trivial), and allocate nothing on the render path. If RT self-promotion fails (no `RLIMIT_RTPRIO`), a worker runs at SCHED_OTHER and the render still completes -- correctness never depends on the promotion. A single-expensive-op layer runs inline on the audio thread; the fan-out only pays off when several plugin `processBlock`s can overlap.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 320" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-blu { fill: #7aa2f7; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-yel  { stroke: #e0af68; stroke-width: 1.4; fill: none; }
    .ln-blu  { stroke: #7aa2f7; stroke-width: 1.4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="320" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="title">Independent layers; only multi-plugin layers fan out to workers</text>

  <text x="40" y="66" class="lbl-blu">Layer 0</text>
  <rect x="120" y="50" width="150" height="34" class="box"/><text x="195" y="71" text-anchor="middle" class="lbl-sm">Tracker render</text>
  <rect x="290" y="50" width="160" height="34" class="box"/><text x="370" y="71" text-anchor="middle" class="lbl-sm">MidiPlayer render</text>
  <text x="470" y="71" class="lbl-mut">no buffer conflict -- run together (cheap, inline)</text>
  <line x1="450" y1="90" x2="450" y2="106" class="ln-blu"/>

  <text x="40" y="134" class="lbl-blu">Layer 1</text>
  <rect x="120" y="118" width="150" height="34" class="hot"/><text x="195" y="139" text-anchor="middle" class="lbl-yel">VST synth A</text>
  <rect x="290" y="118" width="150" height="34" class="hot"/><text x="365" y="139" text-anchor="middle" class="lbl-yel">VST synth B</text>
  <rect x="460" y="118" width="150" height="34" class="hot"/><text x="535" y="139" text-anchor="middle" class="lbl-yel">Sampler</text>
  <text x="640" y="139" class="lbl-mut">3 expensive -> GraphWorkers @70</text>
  <line x1="450" y1="158" x2="450" y2="174" class="ln-blu"/>

  <text x="40" y="202" class="lbl-blu">Layer 2</text>
  <rect x="120" y="186" width="220" height="34" class="hot"/><text x="230" y="207" text-anchor="middle" class="lbl-yel">mix bus -> CLAP reverb</text>
  <text x="360" y="207" class="lbl-mut">depends on layer 1 -- runs after the join barrier</text>

  <rect x="40" y="244" width="820" height="60" class="box"/>
  <text x="60" y="266" class="lbl-yel">Worker discipline</text>
  <text x="60" y="284" class="lbl-mut">Self-promote SCHED_FIFO 70, WaitableEvent sync, raw index arrays only, zero allocation.</text>
  <text x="60" y="298" class="lbl-mut">Audio thread joins before the block ends. See Threading &amp; RT Safety for the ladder.</text>
</svg>
</div>

## 7. The render pass

Each audio block, the callback drives `RootGraph`, which executes the current plan:

1. Read the transport (playhead beats, tempo, playing state) for this block.
2. Execute the ordered/layered `GraphOp` list against the shared buffer pool -- clears, copies, delays, and `processBlock`/`render` calls, fanning out multi-plugin layers.
3. Each `Processor` gets its slice of the shared buffers as a `RenderContext` and works in place.
4. MIDI produced by a node (Tracker, MIDI Player) lands in the shared MIDI buffers, time-sorted, and is visible to downstream nodes in the same pass.

Transport-synchronised subsystems hang off this pass: the automation engine's `applyForBlock` runs from the graph render using the block's beat range, and clip launches queued on the SPSC FIFOs are applied at their target beat within the block. Nothing on this path takes a lock or allocates.

## 8. Sub-graphs

Because a `GraphNode` is itself a `Processor`, graphs nest: an `EL_NODE_ID_GRAPH` node is a full graph embedded as a single node in its parent. This is how Element groups routing and how the DAW builds internal plumbing -- e.g. `EL_NODE_ID_ARRANGEMENT_TRACKS`, the subgraph that holds the arrangement's per-lane audio/MIDI player nodes. A sub-graph renders as one op in its parent's plan, with its own internal render order. Port counts are preserved across session load so nested routing survives a save/reopen.

## 9. Threading model

The graph obeys a strict split (the disciplines are detailed in [Threading & RT Safety](threading-rt-safety.gen.html)):

- **Message thread** owns topology: `GraphManager` adds/removes nodes and connections, instantiates plugins, and rebuilds the render plan. JUCE `Component`s are message-thread-only, so editor UI lives here too.
- **Audio thread** only executes the pre-built plan -- never edits topology, never allocates, never takes a non-PI lock on the hot path. A new plan built on the message thread is swapped in atomically at a block boundary.
- **Graph workers** run fanned-out layers at SCHED_FIFO 70 (§6).

Cross-thread data -- clip launch requests, region tables, the automation track snapshot -- moves over lock-free SPSC FIFOs and epoch-gated pointer snapshots, never a shared mutex on the audio path.
