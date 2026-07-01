# Lulada -- Winelib Runtime

This page documents how Lulada runs as a winelib process: the startup sequence that promotes it to realtime and initialises OLE before any plugin loads, the wineserver warm-up race it works around, priority inheritance on the hot paths, and which plugin formats the winelib configuration actually hosts.

## Table of Contents

1. [Why winelib](#1-why-winelib)
2. [Startup sequence](#2-startup-sequence)
3. [The wineserver warm-up race](#3-the-wineserver-warm-up-race)
4. [Realtime priority and OLE](#4-realtime-priority-and-ole)
5. [Priority inheritance on the hot paths](#5-priority-inheritance-on-the-hot-paths)
6. [Plugin formats](#6-plugin-formats)
7. [Exit diagnostics](#7-exit-diagnostics)
8. [What to check when a plugin will not load](#8-what-to-check-when-a-plugin-will-not-load)

---

## 1. Why winelib

A winelib binary is native Linux code (ELF, linked with `wineg++`) that can also call the Win32 ABI in the same address space. That is what lets Lulada be a Linux-native JUCE application *and* an in-process Win32 plugin host at once -- the audio graph, the plugin instances, and the UI all share one process, with no bridge and no IPC audio hop.

The tradeoff is that the process now depends on the Wine runtime being ready before the first Win32 call, and on the realtime primitives behaving correctly across the native/Win32 seam. Both are handled at startup.

## 2. Startup sequence

Two things happen *before* `main()`: the winelib init static initializers in `src/main.cc`, guarded by `#if defined (__WINE__)`, then JUCE's `START_JUCE_APPLICATION`. The application-level bring-up in `src/application.cpp` runs after that.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 940 470" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .hot  { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .cy   { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .grn  { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .lbl  { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy  { fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="940" height="470" class="bg"/>
  <text x="470" y="24" text-anchor="middle" class="title">Startup order (src/main.cc, before main())</text>

  <rect x="40" y="44" width="860" height="30" class="cy"/>
  <text x="55" y="63" class="lbl-cy">WinelibExitDiagnostics -- install signal / terminate / atexit hooks</text>

  <rect x="40" y="86" width="860" height="70" class="hot"/>
  <text x="55" y="105" class="lbl-yel">WinelibPluginHostInit (static ctor)</text>
  <text x="55" y="122" class="lbl-mut">1. ensure_wineserver_warm()  -- fast-path if a socket already exists for this uid</text>
  <text x="55" y="136" class="lbl-mut">2. SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)</text>
  <text x="55" y="150" class="lbl-mut">3. OleInitialize(nullptr)</text>

  <line x1="470" y1="156" x2="470" y2="180" class="ln"/>

  <rect x="40" y="182" width="860" height="26" class="box"/>
  <text x="55" y="199" class="lbl">START_JUCE_APPLICATION (element::Application)</text>

  <line x1="470" y1="208" x2="470" y2="230" class="ln"/>

  <rect x="40" y="232" width="860" height="30" class="grn"/>
  <text x="55" y="251" class="lbl">Application::initialise -> Startup::launchApplication</text>

  <rect x="60" y="274" width="180" height="34" class="box"/>
  <text x="150" y="295" text-anchor="middle" class="lbl-sm">setupAudioEngine</text>
  <rect x="252" y="274" width="180" height="34" class="box"/>
  <text x="342" y="295" text-anchor="middle" class="lbl-sm">setupPlugins</text>
  <rect x="444" y="274" width="180" height="34" class="box"/>
  <text x="534" y="295" text-anchor="middle" class="lbl-sm">setupMidiEngine</text>
  <rect x="636" y="274" width="244" height="34" class="box"/>
  <text x="758" y="295" text-anchor="middle" class="lbl-sm">setupScripting / setupRepos</text>

  <line x1="470" y1="308" x2="470" y2="332" class="ln"/>

  <rect x="40" y="334" width="860" height="30" class="box"/>
  <text x="55" y="353" class="lbl">finishedLaunching -> Application::finishLaunching</text>

  <rect x="60" y="378" width="270" height="34" class="box"/>
  <text x="195" y="399" text-anchor="middle" class="lbl-sm">scan plugins (if enabled)</text>
  <rect x="342" y="378" width="270" height="34" class="box"/>
  <text x="477" y="399" text-anchor="middle" class="lbl-sm">set content factory + services.run()</text>
  <rect x="624" y="378" width="256" height="34" class="box"/>
  <text x="752" y="399" text-anchor="middle" class="lbl-sm">AuthStartupThread (low prio)</text>

  <rect x="40" y="428" width="860" height="26" class="box"/>
  <text x="55" y="445" class="lbl-mut">Order matters: the RT / OLE class must be live before any plugin's static init runs</text>
</svg>
</div>

## 3. The wineserver warm-up race

Every Win32 call needs a live `wineserver`. `WinelibPluginHostInit` calls **`ensure_wineserver_warm()`** first, mirroring yabridge's `ensure_wineserver_warm`:

- **Fast path** -- `wineserver_socket_present()` scans `/tmp/.wine-<uid>/server-*/socket`; if one exists, return immediately.
- **Cold path** -- `fork()` + `execlp("wineserver", "-p")` (persistent/daemonize), `waitpid` the direct child, then poll up to ~2 s (40 x 50 ms) for the master socket to appear.

`wineserver -p` is idempotent: if one is already running, the second instance's `bind()` on the master socket fails and it exits cleanly.

The reason this exists at all is an NSPA-RT-specific cold-spawn race. On a PREEMPT_RT kernel, the first Wine client launched from Element's process tree could exit *silently* against an un-initialised wineserver -- vanilla Wine's slower init hides the window, but RT-accelerated startup exposes it. The warm-up is a **defensive workaround, not a root-cause fix**; two leading hypotheses (recorded in the code comments) are (a) JUCE's static-init bursting the first Wine call out faster than wineserver's own init window, and (b) the `SetPriorityClass` promotion below starving wineserver's startup by moving the parent to SCHED_FIFO too early.

> **Note:** every other DAW tested loads plugins fine against a cold wineserver -- the race is triggered specifically by Element's startup shape. The warm-up is idempotent and cheap, so it ships regardless.

## 4. Realtime priority and OLE

After the warm-up, two Win32 calls run, in this exact order:

```cpp
SetPriorityClass (GetCurrentProcess(), REALTIME_PRIORITY_CLASS); // 0x00000100
OleInitialize (nullptr);
```

The order is deliberate and matches yabridge's `wine-host`:

- **`SetPriorityClass` first** so that any thread a plugin spawns *during* `OleInitialize` (or during its own COM static-init) already inherits the realtime priority class. Without it, a plugin's internal `boost::thread` pool can fail `pthread_create` for its SCHED_FIFO threads because the parent's class was never promoted -- the failure surfaces downstream as a Wine RPC exception (`0x6bf`) on a plugin-internal thread.
- **`OleInitialize` next** so the OLE/COM apartment exists before any Windows VST3 plugin's static init runs against it. u-he plugins (among others) initialise COM state eagerly and crash against an uninitialised OLE subsystem.

Both calls are done at namespace scope so the static initializer fires before `main()` and before any JUCE static init that might construct plugin scanners. The `ole32` link is forced in `CMakeLists.txt` when building under Wine so `OleInitialize` resolves.

## 5. Priority inheritance on the hot paths

Realtime scheduling is only safe if a high-priority thread never blocks waiting on a lower-priority one that then gets preempted (priority inversion). Wine-NSPA and JUCE-NSPA close that gap with **librtpi PI-mutexes** on the hot paths:

- The **audio thread** and the **disk-IO thread** synchronise through JUCE-NSPA primitives that, when compiled with `__WINE__`, swap their underlying implementation to librtpi `PiMutex` + `PiCond` (`FUTEX_WAIT_REQUEUE_PI`). The audio thread's `signal()` boosts the IO-thread waiter directly via a Linux kernel futex syscall -- no wineserver involvement.
- The automation engine's message-thread/MIDI-thread lookup lock is a `juce::CriticalSection`, which is PI-correct on PREEMPT_RT for the same reason.

This is why the disk streamer (`src/services/audiostreaming`, adapted from NON-DAW) replaces NON's POSIX `sem_t` wakeup with a `juce::WaitableEvent`: under the winelib build that maps to the PI-boosted futex path. See [Arrangement & Timeline](arrangement-timeline.gen.html) for the streamer.

## 6. Plugin formats

The winelib configuration hosts Win32 plugins through Wine's PE loader, plus Element's own internal nodes. It does **not** build the Linux-native plugin formats.

| Format | Hosted? | Notes |
| --- | --- | --- |
| VST2 (Win32) | Yes | Loaded via Wine-NSPA |
| VST3 (Win32) | Yes | Loaded via Wine-NSPA; needs OLE up first |
| CLAP (Win32) | Yes | Via `deps/clap-juce-extensions` host path |
| Element internal nodes | Yes | Registered in `nodefactory.cpp`; see [Graph Engine](graph-engine.gen.html) |
| LV2 / LADSPA / AU | **No** | Not built in the winelib configuration |

Hosted plugin **editor UIs embed directly into the graph** -- their windows are reparented (X11) or composited as `wl_subsurface`s (Wayland) into Lulada's own window. See [Wayland Embedding](wayland-embedding.gen.html).

## 7. Exit diagnostics

`WinelibExitDiagnostics` (also in `src/main.cc`, `__WINE__`-only) is a retained bring-up aid. It installs handlers for `SIGSEGV`/`SIGABRT`/`SIGTERM`/`SIGHUP`/`SIGINT`/`SIGFPE`/`SIGBUS`, a `std::set_terminate` hook, and an `atexit` hook, and writes one tagged line to `/tmp/element-exit.log` (line-buffered) describing how the process died:

| Tag | Meaning |
| --- | --- |
| `START` | Process launched |
| `SIGNAL <name>` | Killed by a unix signal (then re-raised with the default handler) |
| `TERMINATE <what>` | C++ unhandled exception -- logs `e.what()` before `abort()` |
| `ATEXIT` | Clean return from `main` |

`SIGPIPE` is intentionally *not* caught -- Wine and JACK both use it normally.

## 8. What to check when a plugin will not load

When verifying a winelib change is actually live, or diagnosing a plugin failure:

- **Confirm the binary is winelib** -- the RT/OLE init only compiles under `#if defined (__WINE__)`; a stock CMake build of Element does not include it.
- **Confirm wineserver is warm** -- `ls /tmp/.wine-$(id -u)/server-*/socket`. A stale socket left by a crashed server can fool the fast path; the launcher hardens against this by also requiring a live wineserver process.
- **Read `/tmp/element-exit.log`** -- a `TERMINATE`/`SIGNAL` line tells you exception vs signal vs clean exit.
- **Check the RT class actually took** -- a plugin thread failing `pthread_create` for SCHED_FIFO usually means the process was not promoted (or lacks `RLIMIT_RTPRIO`), surfacing as a Wine RPC fault on a plugin-internal thread.
