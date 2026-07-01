# Lulada -- Sampler

This page documents the Sampler: a multi-instrument sample player built on the vendored ft2-clone mixer DSP, its FT2/Renoise instrument model (keymap, envelopes, auto-vibrato), the four-bus output, and the session-global bank pool that many sampler nodes share.

## Table of Contents

1. [The FT2-clone mixer](#1-the-ft2-clone-mixer)
2. [The SamplerNode](#2-the-samplernode)
3. [Instruments and slots](#3-instruments-and-slots)
4. [Envelopes and auto-vibrato](#4-envelopes-and-auto-vibrato)
5. [Voices, interpolation, buses](#5-voices-interpolation-buses)
6. [The session-global bank pool](#6-the-session-global-bank-pool)
7. [Two-phase sample loading](#7-two-phase-sample-loading)
8. [Persistence](#8-persistence)

---

## 1. The FT2-clone mixer

The per-voice DSP is the mixer core of [`8bitbubsy/ft2-clone`](https://github.com/8bitbubsy/ft2-clone) (BSD-3-Clause), vendored under `src/engine/sampler/`. Only the mixer, interpolation, and instrument-shape bits are taken; the XM replayer, module loaders, sample loaders (JUCE's `AudioFormatManager` covers WAV/AIFF/FLAC/OGG), and the SDL UI are all dropped.

| File | Role |
| --- | --- |
| `ft2_mix.c` / `ft2_mix.h` | The voice mixer -- resample + accumulate into the output |
| `ft2_mix_interpolation.c/.h` | Interpolation kernels (linear, cubic, windowed-sinc) |
| `ft2_mix_simd.c` | AVX2 SIMD mix kernel |
| `ft2_mix_macros.h` | Shared mixing macros |
| `ft2_audio.h` | Audio-side declarations |

## 2. The SamplerNode

`SamplerNode` (`src/nodes/sampler.hpp`) is a `BaseProcessor`: **MIDI in, stereo audio out**, registered as `EL_NODE_ID_SAMPLER`. It hosts a `juce::Synthesiser` voice pool whose voices render through the ft2 mixer. MIDI channel selects the instrument (default: channel N -> bank N, remappable and updated by program-change); the sampler is a *consumer* of the shared bank pool rather than the owner of its instruments (see §6).

It is a `juce::ChangeBroadcaster` so the editor and the Disk Op sample-bank pane can refresh when slot contents change. The editor is paged -- **Bank / Instrument / Sample / FX** -- with the four bus-gain sliders under the sample preview on the Bank page.

## 3. Instruments and slots

A `SamplerInstrument` follows the FT2/Renoise model: up to `kNumSlots = 32` sample slots plus a 128-entry MIDI keymap and per-instrument envelopes.

A `SamplerSampleSlot` holds decoded `int16` sample data (mono, or stereo when `data16R` is non-null) and its play parameters:

| Field | Meaning |
| --- | --- |
| `data16L` / `data16R` | Decoded int16 buffers (R null for mono) |
| `sourceSampleRate` | Original file rate; the mixer resamples per note |
| `rootNote` | MIDI note that plays at native pitch |
| `finetune` / `relativeNote` | Fine offset and XM-style semitone offset |
| `volume` / `panning` | Per-slot gain and pan |
| `loopMode` | `kNone` / `kForward` / `kPingpong` + `loopStart` / `loopLength` |
| `busIndex` | Which of the 4 output buses this slot feeds |
| `sourceFile` | Absolute POSIX path of the loaded file (for reload -- see §8) |

The keymap (`slotForNote` / `setSlotForNote` / `autoSpreadKeymap`) maps each of the 128 MIDI notes to a slot, so one instrument can be a multisampled range.

## 4. Envelopes and auto-vibrato

Each instrument carries two FT2-style envelopes -- volume and pan -- as `FT2Envelope`: up to 12 points (`x` = tick offset 0..324, `y` = 0..64) with `Enabled` / `Sustain` / `Loop` flags, a sustain point, and loop start/end. Envelope ticks run at the FT2 nominal rate (`kEnvTickRateHz = 50`); `getSamplesPerEnvTick()` converts to the current sample rate.

Two behaviours refine the raw FT2 model for modern use:

- **`envSampleRelative`** (default on) -- at note-on, envelope ticks are scaled so the envelope's last point lines up with the *end of the playing sample*, instead of running in absolute 50 Hz ticks (which max out around 6.5 s and make envelopes unusable for short one-shots).
- **Global ADSR fallback** -- when an instrument has no volume envelope enabled, a global `AdsrParams` (attack/decay/sustain/release) shapes the voice instead; when the FT2 volume envelope's `kEnabled` flag is set, the ADSR is bypassed in its favour.

`AutoVibParams` adds FT2 auto-vibrato per instrument (type sine/square/ramp, sweep-in, depth, rate). Instruments can also be `mono` -- one voice per channel binding, with last-note-priority pitch glide over `portamentoTimeMs`.

## 5. Voices, interpolation, buses

| Parameter | Values | Default |
| --- | --- | --- |
| Voice count | `setNumVoices(n)` | 16 |
| Interpolation | `kInterpNone` / `kInterpLinear` / `kInterpCubic` / `kInterpSinc16` | Linear |
| Output buses | `kNumBuses = 4` stereo aux outputs | -- |

Each slot routes to a bus (`busIndex`); the voice's audio is multiplied by `busGain[busIndex]` (per-bus master gain, 0..2.0, 1.0 = unity) before being summed into that bus's output channels. Interpolation mode and loop state pick the specific ft2 mix function via `getMixFuncIndexForCurrentMode(loop, pingpong)`.

## 6. The session-global bank pool

Instruments do **not** live on the sampler node. They live in a process-lifetime singleton, `SampleBankPool` (`src/services/samplebankpool.hpp`) -- the FT2/Buzz "bank table" model. Multiple `SamplerNode`s in one session all reference the same pool; each picks which bank(s) it plays via a per-channel binding (`channelBinding[MIDI channel] -> bank index`).

| Aspect | Detail |
| --- | --- |
| Capacity | `kNumBanks = 128` (matches Disk Op's 128-row grid) |
| Ownership | Singleton; sampler nodes are consumers, not owners |
| Mutation UI | Disk Op's Sample Bank pane (load / rename / clear) |
| Growth | Lazy via `ensureInstrumentExists` / `addInstrument` |
| Notifications | `juce::ChangeBroadcaster`, deferred to the message thread |

Thread safety: all public accessors take an internal lock, but the **audio thread never touches it** -- a voice captures a `SamplerInstrument::Ptr` (ref-counted) at note-on time, so the instrument stays alive even if the message thread later reallocates the bank vector. This is the same "hold a ref-counted snapshot across the block" discipline used elsewhere on the realtime path.

## 7. Two-phase sample loading

Loading a sample must not stall the audio thread or hold a lock across file I/O. `SamplerInstrument` splits it:

1. **`prepareSlot(file, fmt)`** -- the UI thread does the file read + decode into a fresh `SamplerSampleSlot`, holding **no** sampler-side lock. Returns null on read failure.
2. **`commitSlot(slot, data)`** -- publishes the prepared slot into the instrument under a brief `SamplerNode::sampleLock`.

So the expensive work (decode) happens lock-free, and only the pointer publish is guarded. `clear()` / `clearSlot()` give index-stable "empty this bank/slot" semantics for the Disk Op pane's fixed 128-row table.

## 8. Persistence

Sessions serialise the bank table through `getStateInformation` / `setStateInformation`, and `clearAll()` resets the pool on session-new / session-load. Crucially, the decoded `int16` buffers are **never embedded** in the session XML -- only each slot's `sourceFile` path is stored, and the audio is re-decoded from disk on load. The sampler node persists its per-channel bindings; the banks themselves belong to the pool. See [Graph Engine](graph-engine.gen.html) for how the node sits in the graph and [Tracker](tracker.gen.html) for the MIDI source that typically drives it.
