# Lulada -- JUCE-NSPA

Lulada runs on a fork of JUCE (JUCE-NSPA) that turns the framework into a realtime, winelib-native plugin host. This page documents what the fork changes: the in-process Win32 plugin-hosting module, the per-plugin Win32 worker that keeps plugin code on a thread it recognises, the priority-inheriting synchronization primitives, and the plugin-editor embedding.

## Table of Contents

1. [Why a JUCE fork](#1-why-a-juce-fork)
2. [The winelib plugin-hosting module](#2-the-winelib-plugin-hosting-module)
3. [The per-plugin Win32 worker](#3-the-per-plugin-win32-worker)
4. [RT-safe synchronization](#4-rt-safe-synchronization)
5. [Priority inheritance in practice](#5-priority-inheritance-in-practice)
6. [Plugin-editor embedding](#6-plugin-editor-embedding)
7. [The Wayland windowing layer](#7-the-wayland-windowing-layer)
8. [How it is built](#8-how-it-is-built)

---

## 1. Why a JUCE fork

Stock JUCE targets native platforms: on Linux it opens X11 windows, its `CriticalSection` is a plain `pthread_mutex`, and its plugin formats `dlopen` native `.so`/`.vst3` bundles. None of that fits a realtime winelib DAW that has to load **Windows** plugins in-process under a PREEMPT_RT scheduler. JUCE-NSPA (a fork of JUCE 8.0.12) adds four capabilities, all gated on the winelib build (`__WINE__` / `JUCE_LINUX`) so the vanilla behaviour is untouched off that path:

| Area | Stock JUCE | JUCE-NSPA |
| --- | --- | --- |
| Plugin hosting | native `.so`/VST3 bundles | Win32 VST2 / VST3 / CLAP as PE modules, in-process |
| Plugin thread identity | any pthread | one dedicated **Win32** worker per plugin instance |
| `CriticalSection` | `pthread_mutex` (no PI) | librtpi `pi_mutex_t` (FUTEX_LOCK_PI) |
| `WaitableEvent` | pthread cond/futex | librtpi `PiMutex` + `PiCond` (requeue-PI) |
| Plugin editor window | X11 top-level | embedded `HWND` via X11 reparent **or** `wl_subsurface` |

## 2. The winelib plugin-hosting module

Plugin hosting lives in a dedicated module, `juce_audio_processors_headless`, which carries winelib variants of the VST2, VST3, and CLAP format hosts (`juce_VST3PluginFormatImpl.h`, `juce_VSTPluginFormatImpl.h`, `juce_CLAPPluginFormatImpl.h`) plus the winelib ABI shims (`juce_winelib_dispatch.h`, `juce_winelib_path.h`, `juce_clap_winelib_abi.h`). Because the host binary is itself a PE-aware winelib ELF, it loads a Windows plugin through Wine's own PE loader rather than `dlopen` -- the plugin sees a normal Win32 process. The resulting `AudioPluginInstance` is an ordinary `juce::AudioProcessor`, which Element wraps as a graph node exactly like an internal DSP node (see [Graph Engine](graph-engine.gen.html#4-hosted-plugins-as-nodes)).

## 3. The per-plugin Win32 worker

This is the subtlest and most important part of the fork. When a winelib ELF (compiled in `JUCE_LINUX` mode) calls into Win32 PE plugin code, **every call must originate from a thread created by Win32 `CreateThread`, not a JUCE-promoted pthread.** PE libraries assume a per-thread Win32 identity -- a TEB, Win32 TLS slots, a COM apartment. A JUCE pthread has none of that, so when plugin code does a TLS lookup, a COM apartment check, or a thread-local allocation against missing state, it eventually hits an unrecoverable condition and calls `ExitProcess()`. During bring-up this showed up as the process dying via `LdrShutdownProcess` *from inside the plugin*, with no prior signal, abort, or C++ exception -- a silent, baffling exit.

The fix (`juce_winelib_dispatch.h`) is a **dedicated Win32 worker thread per plugin instance**, owned by the format's per-instance holder. Every plugin-touching call for that instance marshals onto its worker, so the plugin sees one stable Win32 thread identity for its whole lifetime.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 920 380" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .pur { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.5; }
    .hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .bad { fill: #2a1a1a; stroke: #f7768e; stroke-width: 1.4; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-red { fill: #f7768e; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-grn  { stroke: #9ece6a; stroke-width: 1.4; fill: none; }
    .ln-pur  { stroke: #bb9af7; stroke-width: 1.4; fill: none; }
    .ln-red  { stroke: #f7768e; stroke-width: 1.4; fill: none; stroke-dasharray: 5,3; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="920" height="380" class="bg"/>
  <text x="460" y="24" text-anchor="middle" class="title">Plugin calls marshal onto a Win32 worker with a real TEB / COM apartment</text>

  <!-- callers -->
  <rect x="30" y="52" width="200" height="40" class="grn"/>
  <text x="130" y="70" text-anchor="middle" class="lbl-grn">Message thread</text>
  <text x="130" y="84" text-anchor="middle" class="lbl-mut">load / setActive / editor</text>
  <rect x="30" y="110" width="200" height="40" class="hot"/>
  <text x="130" y="128" text-anchor="middle" class="lbl-yel" fill="#e0af68" font-weight="bold">Audio thread</text>
  <text x="130" y="142" text-anchor="middle" class="lbl-mut">process (per block)</text>

  <!-- marshal -->
  <rect x="300" y="70" width="180" height="60" class="box"/>
  <text x="390" y="92" text-anchor="middle" class="lbl-sm">per-instance dispatch</text>
  <text x="390" y="108" text-anchor="middle" class="lbl-mut">marshal call onto the</text>
  <text x="390" y="121" text-anchor="middle" class="lbl-mut">instance's Win32 worker</text>

  <!-- worker -->
  <rect x="560" y="52" width="330" height="98" class="pur"/>
  <text x="580" y="72" class="lbl-pur">Win32 worker (CreateThread)  --  one per instance</text>
  <text x="580" y="90" class="lbl-mut">stable TEB + Win32 TLS + COM apartment</text>
  <rect x="580" y="100" width="290" height="40" class="box"/>
  <text x="725" y="118" text-anchor="middle" class="lbl-sm">createInstance / initialize / setActive</text>
  <text x="725" y="132" text-anchor="middle" class="lbl-mut">process / terminate  --  all on this thread</text>

  <line x1="230" y1="80" x2="300" y2="90" class="ln-grn"/>
  <line x1="230" y1="120" x2="300" y2="110" class="ln-red"/>
  <line x1="480" y1="100" x2="560" y2="100" class="ln-pur"/>

  <!-- the alternative -->
  <rect x="30" y="200" width="860" height="70" class="bad"/>
  <text x="50" y="222" class="lbl-red">Without the worker: plugin runs on a JUCE pthread with no Win32 identity</text>
  <text x="50" y="242" class="lbl-mut">First TLS / COM / thread-local access on missing state -> plugin calls ExitProcess()</text>
  <text x="50" y="258" class="lbl-mut">Symptom: LdrShutdownProcess from inside the plugin -- no signal / abort / exception</text>

  <rect x="30" y="290" width="860" height="70" class="box"/>
  <text x="50" y="312" class="lbl-pur">Consequence for the graph</text>
  <text x="50" y="332" class="lbl-mut">N instances run on N parallel Win32 workers. A plugin's processBlock is a marshalled</text>
  <text x="50" y="348" class="lbl-mut">call onto that worker -- so parallel layers overlap plugins across worker threads.</text>
</svg>
</div>

## 4. RT-safe synchronization

Under a PREEMPT_RT kernel with the audio thread at SCHED_FIFO, a plain `pthread_mutex` (priority `PRIO_NONE`) is a latency hazard: if a low-priority thread holds it and gets preempted, the RT thread blocks unboundedly -- classic priority inversion. JUCE-NSPA replaces the two primitives that matter with **priority-inheriting** ones, vendored from librtpi (`native/juce_winelib_rtpi.h`, wrapped by `native/juce_winelib_pi_sync.h`):

- **`CriticalSection` becomes a librtpi recursive PI mutex** (`pi_mutex_t`, `NSPA_RTPI_MUTEX_RECURSIVE`). It locks with a **direct `FUTEX_LOCK_PI`** syscall -- no Wine ntdll surface, no wineserver round-trip. It is the same kernel mechanism as glibc's `PTHREAD_PRIO_INHERIT`, minus the per-op user-space tax (TEB lookup, CS struct bookkeeping, spin-count loop). This one change makes **every `juce::CriticalSection` in the codebase PI-correct**, which is why the tracker's `engineLock_`, the sampler's `sampleLock`, and the automation engine's lookup lock are all safe to touch from a non-RT thread while the audio thread runs.
- **`WaitableEvent` becomes a `PiMutex` + `PiCond` pair.** `PiCond` uses `FUTEX_WAIT_REQUEUE_PI` / `FUTEX_CMP_REQUEUE_PI`, so the wake-and-reacquire is atomic with respect to PI ordering. This is what the disk streamer's IO-thread wakeup rides: the audio thread's `signal()` boosts the waiting IO thread through the kernel futex, no wineserver involved.

> **Why not `std::mutex` / `std::condition_variable`:** `std::mutex` is `PRIO_NONE` -- no inheritance, so it inverts under SCHED_FIFO. And a plain condition variable leaves a window between wake and reacquire where the woken thread can be preempted, breaking the PI chain until it manages to relock. `PiCond`'s requeue-PI closes that window. The wrappers present the exact `std::mutex` + `std::condition_variable_any` surface, so calling code reads normally.

The wrappers also support a process-shared mode (`FUTEX_LOCK_PI`, placement-new into shmem) -- unused by the in-process host today, but it leaves the door open for a PI shmem rendezvous across the Element/winelib boundary at zero runtime cost (a flag bit).

## 5. Priority inheritance in practice

The payoff is a chain that never inverts: a low-priority thread holding a lock the RT audio thread needs is *temporarily boosted* to the audio thread's priority until it releases.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 920 300" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .bad { fill: #2a1a1a; stroke: #f7768e; stroke-width: 1.4; }
    .good{ fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.4; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #a9b1d6; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-red { fill: #f7768e; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln-red  { stroke: #f7768e; stroke-width: 1.4; fill: none; }
    .ln-grn  { stroke: #9ece6a; stroke-width: 1.4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="920" height="300" class="bg"/>
  <text x="230" y="24" text-anchor="middle" class="lbl-red">pthread_mutex (PRIO_NONE)</text>
  <text x="690" y="24" text-anchor="middle" class="lbl-grn">librtpi PI mutex (FUTEX_LOCK_PI)</text>

  <!-- inversion -->
  <rect x="30" y="44" width="400" height="34" class="bad"/><text x="230" y="65" text-anchor="middle" class="lbl-sm">RT audio @80 wants the lock</text>
  <rect x="30" y="92" width="400" height="34" class="box"/><text x="230" y="113" text-anchor="middle" class="lbl-sm">low-prio UI thread holds it</text>
  <rect x="30" y="140" width="400" height="34" class="bad"/><text x="230" y="161" text-anchor="middle" class="lbl-mut">a mid-prio thread preempts the UI holder</text>
  <rect x="30" y="188" width="400" height="52" class="bad"/><text x="230" y="208" text-anchor="middle" class="lbl-red">audio @80 blocked unboundedly</text><text x="230" y="224" text-anchor="middle" class="lbl-mut">xrun / dropout -- classic inversion</text>
  <line x1="230" y1="78" x2="230" y2="92" class="ln-red"/>
  <line x1="230" y1="126" x2="230" y2="140" class="ln-red"/>
  <line x1="230" y1="174" x2="230" y2="188" class="ln-red"/>

  <!-- PI -->
  <rect x="490" y="44" width="400" height="34" class="good"/><text x="690" y="65" text-anchor="middle" class="lbl-sm">RT audio @80 wants the lock</text>
  <rect x="490" y="92" width="400" height="34" class="box"/><text x="690" y="113" text-anchor="middle" class="lbl-sm">low-prio UI holder is boosted to @80</text>
  <rect x="490" y="140" width="400" height="34" class="good"/><text x="690" y="161" text-anchor="middle" class="lbl-mut">mid-prio thread cannot preempt the boosted holder</text>
  <rect x="490" y="188" width="400" height="52" class="good"/><text x="690" y="208" text-anchor="middle" class="lbl-grn">holder releases fast; audio proceeds</text><text x="690" y="224" text-anchor="middle" class="lbl-mut">bounded blocking = no dropout</text>
  <line x1="690" y1="78" x2="690" y2="92" class="ln-grn"/>
  <line x1="690" y1="126" x2="690" y2="140" class="ln-grn"/>
  <line x1="690" y1="174" x2="690" y2="188" class="ln-grn"/>

  <text x="460" y="270" text-anchor="middle" class="lbl-mut">Wait-free snapshots are preferred; PI is the safety net for the few locks that remain.</text>
</svg>
</div>

## 6. Plugin-editor embedding

A hosted plugin's editor is a Win32 `HWND`. `WineHWNDEmbedComponent` (`juce_gui_extra/embedding/`) makes that window a child of Lulada's own window so the editor renders inside the graph's plugin window instead of as a detached top-level. It has two backends selected by the session's windowing stack:

- **X11** -- reparent the plugin `HWND`'s X window under the host window (`XReparentWindow`), the long-standing path.
- **Wayland** -- turn the plugin surface into a `wl_subsurface` of the host surface (the branch in `juce_WineHWNDEmbedComponent_linux.cpp`), because Wayland has no cross-window reparent. See [Wayland Embedding](wayland-embedding.gen.html#4-subsurface-plugin-embed).

## 7. The Wayland windowing layer

JUCE-NSPA carries a Wayland windowing backend (adapted from the plugdata-team JUCE port and NSPA-tuned): a `WaylandComponentPeer` for host surfaces and a `WaylandMessageLoop` that reads the default event queue around the message thread's `poll()`. When the app runs all-Wayland, this backend owns the `wl_display` that the Wine graphics driver adopts as a guest -- the full host/guest model, the two-reader discipline, and its bring-up lessons are documented in [Wayland Embedding](wayland-embedding.gen.html).

## 8. How it is built

JUCE-NSPA is compiled **in-tree with Lulada**, not linked as an external prebuilt library -- the winelib build fetches the JUCE source and compiles it with the NSPA modifications applied. A JUCE change only affects Lulada once it is in the JUCE source the build actually consumes. See [Building](building-packaging.gen.html#4-building-against-the-juce-nspa-fork) for the two-tree layout and the reconfigure caveat.
