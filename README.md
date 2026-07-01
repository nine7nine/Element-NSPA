# Lulada

**Lulada is a fork of [Element](https://github.com/kushview/element)** -- the modular
audio plugin host -- extended into a full DAW and built to run on
[Wine-NSPA](https://github.com/nine7nine), a real-time Wine fork for low-latency audio.

> **Status: work in progress.** Under active development, not yet release-ready --
> features land fast and things may break.

### A Winelib / Wayland DAW

Lulada is a winelib application -- native Linux code that hosts **Win32 plugins
directly on Linux** through Wine-NSPA, at realtime priority.

* **Plugin hosting** -- Win32 **VST2 / VST3 / CLAP** (loaded through Wine-NSPA),
  plus **native Element plugins** (its built-in nodes: sampler, tracker, audio FX,
  MIDI tools, ...); plugin editor UIs embed directly into the graph
* **Real-time** -- runs at realtime priority on Wine-NSPA; librtpi PI-mutex sync on
  the audio / disk-streaming hot paths (no priority inversion)
* **Wayland-native windowing** -- experimental libdecor/EGL path for the main window
  and plugin embedding, alongside the standard X11 backend

_LV2 / LADSPA / AU are not built in the winelib configuration._

### DAW surfaces

Tracker and piano-roll are equal first-class MIDI authoring surfaces; clips live in
both a clip-launcher and a linear timeline.

* **Tracker** -- vendored `vht` engine: pattern grid, QWERTY note entry, per-cell FX
  columns, multi-pattern sequences, MUTE/SOLO, MIDI recording
* **Piano-roll** -- viewport/zoom/tools, velocity lane, snap divisions, and
  quantize / humanize / scale-snap with live preview
* **Session View** -- Ableton/Bitwig-style clip grid: scenes, follow actions,
  per-scene tempo/sig master column, sample-accurate audio-thread launch scheduler
* **Arrangement / Timeline** -- audio + MIDI lanes, disk-streamed record/playback,
  region authoring (move/resize/split/snap), looped regions, gain + Bezier fades
* **MIDI clips are first-class in both surfaces** -- one graph node = one clip
  source, shown in Session *and* Arrangement at once

### Engine

* **Sampler** -- FT2-clone mixer DSP, multi-instrument with vol/pan envelopes, paged
  editor (Bank / Inst / Sample / FX), AVX2 SIMD mix kernel, session-global
  128-bank x 32-slot sample pool
* **Automation** -- COW data model, sample-accurate MIDI CC, touch-record, session
  save/load round-trip
* **Modular graph core** (from Element) -- route audio/MIDI anywhere, nest
  sub-graphs, embed plugin UIs, Lua scripting, multi-undo

### Documentation
Full architecture and technical reference lives in [`docs/`](./docs) and is published
as GitHub Pages at **[nine7nine.github.io/Lulada](https://nine7nine.github.io/Lulada/)**.
The published pages include the hand-authored SVG diagrams; the source `.md` files below
render on GitHub too.

**Design &amp; internals** &mdash;
[Architecture](./docs/architecture.md) ·
[Winelib Runtime](./docs/winelib-runtime.md) ·
[JUCE-NSPA](./docs/juce-nspa.md) ·
[Threading &amp; RT Safety](./docs/threading-rt-safety.md) ·
[Wayland Embedding](./docs/wayland-embedding.md) ·
[Graph Engine](./docs/graph-engine.md)

**DAW surfaces** &mdash;
[Tracker](./docs/tracker.md) ·
[Piano-roll](./docs/pianoroll.md) ·
[Session View](./docs/session-view.md) ·
[Arrangement &amp; Timeline](./docs/arrangement-timeline.md) ·
[Automation](./docs/automation.md) ·
[Sampler](./docs/sampler.md)

**Build** &mdash; [Building &amp; Packaging](./docs/building-packaging.md)

### Building
Lulada builds against Wine-NSPA (winelib), not upstream Element's standard CMake
flow. See [Building & Packaging](./docs/building-packaging.md) for the winelib
configuration, or [building.md](./docs/building.md) for Element's base dependencies.

### Upstream
Lulada tracks [Element](https://github.com/kushview/element) by Kushview (docs and
upstream binaries at [kushview.net](https://kushview.net/element/)). Element is
GPL-3.0-or-later; Lulada inherits the same license.
