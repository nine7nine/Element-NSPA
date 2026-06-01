// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/automation_point.hpp"

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>

#include <vector>

namespace element {

/** One clip-local automation lane inside a MidiNoteRegion.
 *
 *  A piano-roll automation lane edits a curve that rides WITH the clip --
 *  it loops with the region and plays only when the region/clip plays.
 *  This is distinct from timeline parameter automation (song-owned,
 *  AutomationEngine).  See memory project_automation_timeline_vs_clip_semantics.
 *
 *  ## Target (CC or plugin/node param) -- #11 unification
 *
 *  A lane targets EITHER a MIDI continuous controller OR a plugin/node
 *  parameter, mirroring the arranger's AutomationTargetKey:
 *   - CC lane   (default): isParam()==false.  Identity = (ccNumber,
 *     channel).  The MidiPlayerNode emits a controllerEvent alongside the
 *     region's notes (rides the clip's MIDI output -- works with external
 *     gear too).
 *   - Param lane: paramId non-empty.  Identity = (paramNodeId, paramId).
 *     The MidiPlayerNode applies the sampled value to the resolved node
 *     parameter while the region plays (clip-local + looped).  paramId is
 *     the stable index token (automation::encodeNodeParamId).
 *  The curve points + sampling are identical either way -- only emission
 *  differs.  ccNumber/channel are ignored when isParam().
 *
 *  The curve reuses dsp::automation::AutomationPoint verbatim -- tBeats
 *  is local to the region start, valueNormalized is [0, 1] (mapped to a
 *  0..127 CC value at emit: round(v * 127), or applied directly to a
 *  normalized param), and each point carries the per-segment CurveOptions.
 *  Reusing AutomationPoint lets the piano-roll editor share the exact
 *  curve paint + evaluate code the timeline automation overlays use. */
struct MidiCcLane
{
    using PointList = std::vector<dsp::automation::AutomationPoint>;

    int       ccNumber { 1 };   // 0..127       (CC lane only)
    int       channel  { 1 };   // 1..16        (CC lane only)

    /** Param-target fields.  When paramId is non-empty the lane targets a
     *  node parameter (paramNodeId + paramId) instead of a MIDI CC. */
    juce::Uuid   paramNodeId;
    juce::String paramId;

    PointList points;           // sorted by tBeats ascending (region-local)

    /** True when this lane drives a node/plugin parameter (vs a MIDI CC). */
    bool isParam() const noexcept { return paramId.isNotEmpty(); }

    /** Two lanes share identity (collide) when they drive the same
     *  destination: same (nodeId, paramId) for param lanes, same
     *  (cc, channel) for CC lanes.  A param lane and a CC lane never
     *  collide even if their cc/channel fields happen to match. */
    bool sameTargetAs (const MidiCcLane& o) const noexcept
    {
        if (isParam() != o.isParam()) return false;
        if (isParam())
            return paramNodeId == o.paramNodeId && paramId == o.paramId;
        return ccNumber == o.ccNumber && channel == o.channel;
    }

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
