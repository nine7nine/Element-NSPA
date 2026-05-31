// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/automation_point.hpp"

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>

#include <vector>

namespace element {

/** One clip-local MIDI CC automation lane inside a MidiNoteRegion.
 *
 *  A piano-roll "CC lane" edits a continuous controller (mod wheel = 1,
 *  expression = 11, etc.) that rides WITH the clip -- it loops with the
 *  region and plays only when the region/clip plays.  This is distinct
 *  from timeline parameter automation (song-owned, AutomationEngine):
 *  CC lanes are MIDI data emitted by the MidiPlayerNode alongside the
 *  region's notes.  See memory project_automation_timeline_vs_clip_semantics.
 *
 *  The curve reuses dsp::automation::AutomationPoint verbatim -- tBeats
 *  is local to the region start, valueNormalized is [0, 1] (mapped to a
 *  0..127 CC value at emit: round(v * 127)), and each point carries the
 *  per-segment CurveOptions.  Reusing AutomationPoint lets the piano-roll
 *  CC editor share the exact curve paint + evaluate code the timeline
 *  automation overlays use.
 *
 *  Identity is (ccNumber, channel): a region holds at most one lane per
 *  (cc, channel) pair. */
struct MidiCcLane
{
    using PointList = std::vector<dsp::automation::AutomationPoint>;

    int       ccNumber { 1 };   // 0..127
    int       channel  { 1 };   // 1..16
    PointList points;           // sorted by tBeats ascending (region-local)

    /** Sample the lane's normalized value at a region-local beat,
     *  mirroring AutomationRegion::sampleAtBeats: clamps to the endpoint
     *  values outside the point range, interpolates the bracketing pair
     *  through the from-point's curve.  Empty => 0.5 (neutral). */
    double valueAtBeats (double localBeats) const noexcept;

    /** CC value 0..127 at a region-local beat (round of valueAtBeats*127). */
    int ccValueAtBeats (double localBeats) const noexcept;

    /** Sparse-write XML (point attrs omit defaults). */
    juce::ValueTree toValueTree() const;
    static MidiCcLane fromValueTree (const juce::ValueTree&);
};

} // namespace element
