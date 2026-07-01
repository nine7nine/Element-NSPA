# Lulada -- Building & Packaging

This page documents how Lulada is built: the winelib configuration that distinguishes it from upstream Element's stock CMake flow, the two forks it depends on (Wine-NSPA and JUCE-NSPA), the CMake edits that enable in-process plugin hosting and Wayland, and what the launcher must set up at runtime.

## Table of Contents

1. [The winelib configuration](#1-the-winelib-configuration)
2. [Dependencies](#2-dependencies)
3. [What the build enables](#3-what-the-build-enables)
4. [Building against the JUCE-NSPA fork](#4-building-against-the-juce-nspa-fork)
5. [Runtime launch requirements](#5-runtime-launch-requirements)
6. [Base Element build](#6-base-element-build)
7. [Packaging](#7-packaging)

---

## 1. The winelib configuration

Lulada's canonical build is a **winelib** build -- compiled with `wineg++` / `winegcc`, not a native Linux CMake build. This is not an optional variant: it is the whole point. A native-Linux Element cannot load Windows VST/VST3/CLAP plugins, which is the reason the fork exists. The winelib binary is native Linux ELF that can also call the Win32 ABI in-process (see [Winelib Runtime](winelib-runtime.gen.html)), so one `element_app` is both the Linux-native JUCE DAW and the in-process Win32 plugin host.

The source tree here (`pkgbuilds/Wine-NSPA/element`) is compiled by an out-of-tree winelib build directory; edits to these sources are picked up by an incremental rebuild there. Upstream Element's native/macOS/Windows CMake flows still work for the base app, but they produce a plain Element without Wine plugin hosting.

## 2. Dependencies

The winelib build sits on top of two sibling forks plus Element's own base dependencies:

| Dependency | Role |
| --- | --- |
| **Wine-NSPA** | Realtime Wine fork -- the Win32 runtime the plugins load into; NTSync + io_uring fast paths, `winewayland.drv` host-mode embed, librtpi PI-mutexes |
| **JUCE-NSPA** | Element's JUCE fork -- winelib glue, `CriticalSection`/waitable PI on `__WINE__`, `WineHWNDEmbedComponent`, the Wayland windowing layer |
| **Boost** | `>= 1.74` (Element requirement) |
| **Sol2 / Lua** | Lua scripting (`FindSol2.cmake`) |
| **CLAP JUCE extensions** | `deps/clap-juce-extensions` -- CLAP host + plugin support |
| Base libs | freetype, X11/Xext/Xrandr/Xcomposite/Xinerama/Xrender/Xcursor, ALSA, JACK, curl |

The winelib link also forces `-lole32` (for the `OleInitialize` startup call) whenever the compiler is `wineg++` -- see `CMakeLists.txt`.

## 3. What the build enables

The Lulada delta over stock Element is small at the CMake level -- most of the work is in the two forks. `CMakeLists.txt` adds:

- **`JUCE_WAYLAND=1`** on `element_app` for `UNIX AND NOT APPLE` -- enables the JUCE-NSPA Wayland windowing path (main window + dialogs + popups + native node editors on Wayland surfaces, falling back to X11 at runtime when `WAYLAND_DISPLAY` is absent). See [Wayland Embedding](wayland-embedding.gen.html).
- **`ole32` link under Wine** -- so the `WinelibPluginHostInit` static initializer's `OleInitialize` resolves.
- Standard hidden-visibility properties, and the usual Element targets (`element_app`, optional plugins, tests).

The plugin-format matrix that results: **VST2 / VST3 / CLAP (Win32)** hosted through Wine-NSPA, plus Element's internal nodes. **LV2 / LADSPA / AU are not built** in the winelib configuration.

## 4. Building against the JUCE-NSPA fork

The single most important build fact: **the build compiles the in-tree JUCE it fetches, not a separate JUCE clone.** JUCE-NSPA is a fork of vanilla JUCE 8.0.12 with the NSPA modifications applied (winelib dispatch/path glue, `CriticalSection` PI, the `WineHWNDEmbedComponent`, and the Wayland layer). A change to JUCE only affects Lulada once it is present in the JUCE source the build actually consumes -- committing to a history clone alone does nothing to the binary. When reconfiguring from scratch, verify the fork's modifications survived (a `FetchContent` reset can revert them).

## 5. Runtime launch requirements

Because the process hosts Win32 plugins in-process, the launcher must prepare the Wine environment before the app starts:

- **`WINEPREFIX`** -- point at the prefix that holds the installed Windows plugins.
- **A warm `wineserver`** -- the app pre-warms it at startup (`ensure_wineserver_warm`), but the launcher should keep a persistent, `setsid`-detached `wineserver` alive so the daemon does not die when the launching shell exits. A stale socket left by a crashed server must not fool the warm check -- require a live wineserver *process*, not just the socket file.
- **`WINE_NSPA_WAYLAND_HOST=1`** -- opt into the host/guest Wayland model (unset = upstream Wine behaviour; standalone plugins still work). See [Wayland Embedding](wayland-embedding.gen.html#3-display-injection).
- **Realtime limits** -- the process needs `RLIMIT_RTPRIO` so `SetPriorityClass(REALTIME)` and plugin SCHED_FIFO threads can take effect on the PREEMPT_RT kernel.

## 6. Base Element build

For reference, Element's own dependencies (needed by the base app regardless of the winelib layer) are unchanged from upstream. On Arch:

```bash
sudo pacman -S git base-devel cmake ninja pkgconf boost \
    freetype2 fontconfig libx11 libxext libxrandr libxcomposite \
    libxinerama libxrender libxcursor alsa-lib jack2 \
    ladspa curl ttf-roboto clang
```

The stock (non-winelib) configure/build, useful for working on view code that does not need plugin hosting:

```bash
git submodule update --init --recursive
cmake -B build -G Ninja
cmake --build build
```

See the base [building.md](building.md) for Debian, macOS, Windows, and Docker instructions. The winelib configuration replaces the compiler and adds the flags in [§3](#3-what-the-build-enables); it does not change the source layout.

## 7. Packaging

Element's `CMakeLists.txt` carries CPack configuration (a `DEB` generator on Linux, `ZIP` elsewhere) and an `installer` target. Those target the base app; a winelib distribution additionally has to ship or depend on the Wine-NSPA runtime and set up the launcher environment from [§5](#5-runtime-launch-requirements). Lulada tracks upstream Element (GPL-3.0-or-later) and inherits its license; see the repository root for `AUTHORS.md`, `CHANGELOG.md`, and `LICENSES/`.
