# Lulada -- Wayland Embedding

This page documents Lulada's experimental Wayland windowing path: how the app owns one `wl_display` that the Wine graphics driver adopts as a guest, how Win32 plugin editor windows embed into the host window as `wl_subsurface`s, the two-reader event-loop discipline that keeps both JUCE and Wine dispatching, and how it all falls back to X11.

## Table of Contents

1. [All-or-nothing per session](#1-all-or-nothing-per-session)
2. [Host and guest](#2-host-and-guest)
3. [Display injection](#3-display-injection)
4. [Subsurface plugin embed](#4-subsurface-plugin-embed)
5. [Two coordinated readers](#5-two-coordinated-readers)
6. [Lessons that shaped the design](#6-lessons-that-shaped-the-design)
7. [X11 fallback](#7-x11-fallback)
8. [Where the pieces live](#8-where-the-pieces-live)

---

## 1. All-or-nothing per session

A Lulada session runs **either** all-Wayland **or** all-X11 -- never a hybrid of a Wayland main window with X11-embedded plugin windows. Wine's graphics-driver model assumes one coherent backend per process, and the mixed mode trips a wineserver process-death cleanup path that injects an uninterceptable `WM_CLOSE` into the desktop window.

So the rule is: any Wayland-direction rendering must terminate *inside* `winewayland.drv`, not `winex11.drv` via Xwayland. `winex11.drv` stays for X11 sessions; the user picks one stack at startup. Wayland support is enabled by compiling `element_app` with `JUCE_WAYLAND=1` (set in `CMakeLists.txt` for `UNIX AND NOT APPLE`) and is chosen at runtime -- the app uses Wayland surfaces when `WAYLAND_DISPLAY` is present and falls back to X11 when it is not.

## 2. Host and guest

The key inversion from a normal Wine session is who owns the Wayland connection:

- **Lulada is the host.** Its JUCE-NSPA layer connects to the compositor, owns the `wl_display`, and drives the Wayland event loop. The main window, dialogs, popups, and native node editors are all host-side Wayland surfaces.
- **`winewayland.drv` is a guest.** Instead of opening its own connection, it *adopts* the host's `wl_display` and uses it only to render plugin surfaces, binding globals on Wine's own event queue.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 430" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg  { fill: #1a1b26; }
    .grn { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .cy  { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .box { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .sys { fill: #1f2535; stroke: #565f89; stroke-width: 1; }
    .lbl  { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy  { fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln  { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="430" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="title">Host owns the wl_display; the Wine driver is a guest</text>

  <rect x="30" y="44" width="410" height="150" class="grn"/>
  <text x="48" y="64" class="lbl-grn">Lulada (host)  --  JUCE-NSPA</text>
  <rect x="48" y="76" width="374" height="30" class="box"/>
  <text x="235" y="95" text-anchor="middle" class="lbl-sm">owns wl_display + subcompositor + event loop</text>
  <rect x="48" y="112" width="180" height="30" class="box"/>
  <text x="138" y="131" text-anchor="middle" class="lbl-mut">main window surface</text>
  <rect x="242" y="112" width="180" height="30" class="box"/>
  <text x="332" y="131" text-anchor="middle" class="lbl-mut">dialogs / popups</text>
  <rect x="48" y="148" width="374" height="30" class="box"/>
  <text x="235" y="167" text-anchor="middle" class="lbl-mut">WaylandMessageLoop reads default queue around poll()</text>

  <rect x="460" y="44" width="410" height="150" class="cy"/>
  <text x="478" y="64" class="lbl-cy">winewayland.drv (guest)</text>
  <rect x="478" y="76" width="374" height="30" class="box"/>
  <text x="665" y="95" text-anchor="middle" class="lbl-sm">adopts host wl_display; binds on Wine's own queue</text>
  <rect x="478" y="112" width="374" height="30" class="box"/>
  <text x="665" y="131" text-anchor="middle" class="lbl-mut">plugin editor surface -> wl_subsurface of host window</text>
  <rect x="478" y="148" width="374" height="30" class="box"/>
  <text x="665" y="167" text-anchor="middle" class="lbl-mut">read_events thread dispatches Wine's own queue</text>

  <line x1="440" y1="91" x2="460" y2="91" class="ln"/>

  <rect x="30" y="220" width="840" height="30" class="box"/>
  <text x="450" y="239" text-anchor="middle" class="lbl-sm">libwayland prepare_read / read_events barrier -- two readers, two queues</text>

  <line x1="450" y1="250" x2="450" y2="278" class="ln"/>

  <rect x="30" y="280" width="840" height="40" class="sys"/>
  <text x="450" y="304" text-anchor="middle" class="lbl-mut">Wayland compositor (kwin / mutter / ...)  --  single connection, single client</text>

  <rect x="30" y="342" width="840" height="70" class="box"/>
  <text x="50" y="362" class="lbl-sm">Gated by launcher env WINE_NSPA_WAYLAND_HOST=1</text>
  <text x="50" y="380" class="lbl-mut">Unset  -> upstream Wine; winewayland.drv opens its own connection (standalone works)</text>
  <text x="50" y="396" class="lbl-mut">Set    -> host mode: driver defers its connect and adopts the host display instead</text>
</svg>
</div>

The whole guest path is gated behind the launcher environment variable **`WINE_NSPA_WAYLAND_HOST=1`**. Unset, Wine behaves exactly as upstream (it opens its own connection, so standalone Wine apps still work); set, `wayland_process_init` *defers* -- it does not connect -- and `wayland_ensure_init` lazily adopts the host display when it arrives.

## 3. Display injection

The host has to hand its `wl_display` pointer to the guest driver. This is done through an environment variable, **`WINE_NSPA_WAYLAND_DISPLAY`**, rather than a Wine unix-call, because timing matters: `winewayland.drv`'s `DllMain` runs `wl_display_connect` the instant the DLL loads, which loses a race against any later injection call. Reading the pointer from the environment at init sidesteps the race, and the value is PID-guarded so child Wine processes ignore an inherited value.

On `main`, this injection was moved out of the app code and **into the JUCE-NSPA layer** (JUCE publishes the display when it creates the host connection), so the Element-side delta is just the `JUCE_WAYLAND=1` compile definition. The Wine-side handshake messages are registered window messages `WM_WAYLANDDRV_NSPA_EMBED_WINDOW` / `_DONE` (`0x80001006` / `0x80001007`), which fall inside the `0x80001000`-`0x80001fff` range covered by Wine-NSPA's driver-message fast path.

## 4. Subsurface plugin embed

When a hosted plugin opens its editor, the JUCE `WineHWNDEmbedComponent` (Wayland branch) and the Wine driver cooperate to make the plugin's top-level window a **`wl_subsurface`** of Lulada's host window, so the editor renders *inside* the graph's plugin window rather than as a detached top-level.

The subtle part -- and a bug that was fixed during bring-up -- is that Wayland only adds and maps a subsurface on the **parent surface's next commit**. X11's `XReparent` is immediate and has no analog. So the embed handler must commit *both* the new child surface and the foreign parent surface after `get_subsurface`; committing only the child leaves the editor black and detached. This mirrors what JUCE's own `WaylandWindowSystem::createWindow` does for native surfaces.

## 5. Two coordinated readers

A live session has **two** threads reading the same Wayland connection, each dispatching a *different* event queue:

| Reader | Queue | Runs when |
| --- | --- | --- |
| JUCE `WaylandMessageLoop` | default queue | around the message thread's `poll()` (`prepareWaylandFd` / `processWaylandFd`) |
| Wine `read_events` thread | Wine's own queue | continuously, even while a Win32 modal loop is up |

They coexist through libwayland's `prepare_read` / `read_events` barrier and never handle each other's events. This two-reader shape is deliberate and matches how `winex11.drv` reads X11 independently of JUCE. In host mode Wine keeps its `read_events` thread (it waits for the deferred adopt, then dispatches its own queue); only the clipboard thread stays host-gated off.

The one hard rule: **the fd callback in `juce_WaylandWindowSystem.cpp` must stay a no-op** -- it must never read the fd, because JUCE already reads around `poll()`. Reading it there duplicates `prepareWaylandFd`'s read within one loop iteration and deadlocks.

## 6. Lessons that shaped the design

Two freezes during bring-up produced the rules above:

- **Duplicate reader = deadlock.** Adding a "single reader" in JUCE's fd callback read the fd a second time inside one loop iteration and hung the message loop. The fd callback stays empty; JUCE reads around `poll()`.
- **Sole reader breaks plugin menus.** Making JUCE the *only* reader broke plugin dropdown menus: a menu runs a modal Win32 loop (`TrackPopupMenu`) that bypasses JUCE's message loop entirely, so nothing read Wayland while a menu was open and the window went "Not Responding". Keeping Wine's own reader thread fixes it -- two *coordinated* readers is not the same as the duplicate-reader bug.

A third fix is unrelated to reading: in host mode the driver initially reported zero monitors, so window DPI computed to 0 and wineserver's `scale_dpi` hit a divide-by-zero (`SIGFPE`). `WAYLAND_UpdateDisplayDevices` now reports a fallback 1920x1080 monitor in host mode (gated; standalone is untouched).

## 7. X11 fallback

When `WAYLAND_DISPLAY` is absent, or the user chooses the X11 stack, the whole thing runs on `winex11.drv` with plugin editors reparented via X11 -- the long-standing path. On X11 the main window and plugin windows are both X11 surfaces (under a plain X server or Xwayland), so the coherent-backend rule holds without any of the host/guest machinery. The choice is per session; there is no runtime switch.

## 8. Where the pieces live

The Wayland path spans three repositories; Lulada (this repo) carries only the thin enabling edits.

| Piece | Repo | Role |
| --- | --- | --- |
| `JUCE_WAYLAND=1` + injection glue | Lulada (`CMakeLists.txt`) | Enable Wayland; publish the host display |
| `WaylandComponentPeer` / `WaylandMessageLoop` / `WineHWNDEmbedComponent` | JUCE-NSPA | Host surfaces, the reader around `poll()`, the subsurface embed component |
| `winewayland.drv` host-mode adopt + subsurface embed handler | Wine-NSPA | Guest driver: adopt the host display, embed plugin surfaces |

See [Winelib Runtime](winelib-runtime.gen.html) for the process this all runs inside, and [Architecture](architecture.gen.html) for how the embedded editor sits in the graph.
