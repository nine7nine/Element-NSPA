// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/midi_cc_lane.hpp"

#include "dsp/automation/curve.hpp"

#include <algorithm>
#include <cmath>

namespace element {

namespace {
const juce::Identifier kCcTag      { "cc" };
const juce::Identifier kCcNumAttr  { "n" };
const juce::Identifier kCcChanAttr { "ch" };
const juce::Identifier kParamNidAttr { "nid" };  // param target nodeId (param lane)
const juce::Identifier kParamPidAttr { "pid" };  // param target paramId token
const juce::Identifier kPointTag   { "p" };
const juce::Identifier kPtTAttr    { "t" };   // tBeats (region-local)
const juce::Identifier kPtVAttr    { "v" };   // valueNormalized
const juce::Identifier kPtCotAttr  { "cot" }; // bend handle X (0.25..0.75)
const juce::Identifier kPtCovAttr  { "cov" }; // bend handle value offset
} // namespace

double MidiCcLane::valueAtBeats (double localBeats) const noexcept
{
    /* Mirror AutomationRegion::sampleAtBeats so the painted/edited curve
     * and the emitted CC stream agree on interpolation. */
    if (points.empty())
        return 0.5;
    if (points.size() == 1)
        return points.front().valueNormalized;

    if (localBeats <= points.front().tBeats)
        return points.front().valueNormalized;
    if (localBeats >= points.back().tBeats)
        return points.back().valueNormalized;

    const auto cmp = [] (const dsp::automation::AutomationPoint& a, double t) noexcept
    {
        return a.tBeats < t;
    };
    auto it = std::lower_bound (points.begin(), points.end(), localBeats, cmp);
    if (it == points.begin()) return points.front().valueNormalized;
    if (it == points.end())   return points.back().valueNormalized;

    const auto& from = *(it - 1);
    const auto& to   = *it;
    const double span = to.tBeats - from.tBeats;
    if (span <= 0.0) return to.valueNormalized;

    const double xNorm = (localBeats - from.tBeats) / span;
    /* evaluateSegment returns the actual value directly (value-space 2D
     * Bezier), matching AutomationRegion::sampleAtBeats. */
    return dsp::automation::evaluateSegment (
        xNorm, from.valueNormalized, to.valueNormalized, from.curve);
}

int MidiCcLane::ccValueAtBeats (double localBeats) const noexcept
{
    const double v = juce::jlimit (0.0, 1.0, valueAtBeats (localBeats));
    return juce::jlimit (0, 127, (int) std::lround (v * 127.0));
}

juce::ValueTree MidiCcLane::toValueTree() const
{
    juce::ValueTree v (kCcTag);
    if (isParam())
    {
        /* Param lane: persist the target node + paramId; cc/channel are
         * meaningless here so they are sparse-skipped. */
        v.setProperty (kParamNidAttr, paramNodeId.toString(), nullptr);
        v.setProperty (kParamPidAttr, paramId, nullptr);
    }
    else
    {
        v.setProperty (kCcNumAttr, ccNumber, nullptr);
        if (channel != 1)
            v.setProperty (kCcChanAttr, channel, nullptr);
    }

    for (const auto& p : points)
    {
        juce::ValueTree pv (kPointTag);
        pv.setProperty (kPtTAttr, p.tBeats,          nullptr);
        pv.setProperty (kPtVAttr, p.valueNormalized, nullptr);
        /* Curve defaults (centred handle) are sparse-skipped. */
        if (p.curve.offsetT != 0.5)
            pv.setProperty (kPtCotAttr, p.curve.offsetT, nullptr);
        if (p.curve.offsetV != 0.0)
            pv.setProperty (kPtCovAttr, p.curve.offsetV, nullptr);
        v.appendChild (pv, nullptr);
    }
    return v;
}

MidiCcLane MidiCcLane::fromValueTree (const juce::ValueTree& v)
{
    MidiCcLane lane;
    if (! v.hasType (kCcTag))
        return lane;

    const juce::String pid = v.getProperty (kParamPidAttr, juce::String()).toString();
    if (pid.isNotEmpty())
    {
        lane.paramId     = pid;
        lane.paramNodeId = juce::Uuid (v.getProperty (kParamNidAttr, juce::String()).toString());
    }
    else
    {
        lane.ccNumber = (int) v.getProperty (kCcNumAttr, 1);
        lane.channel  = (int) v.getProperty (kCcChanAttr, 1);
    }

    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        const auto pv = v.getChild (i);
        if (! pv.hasType (kPointTag)) continue;
        dsp::automation::AutomationPoint p;
        p.tBeats          = (double) pv.getProperty (kPtTAttr, 0.0);
        p.valueNormalized = (double) pv.getProperty (kPtVAttr, 0.0);
        p.curve.offsetT = (double) pv.getProperty (kPtCotAttr, 0.5);
        p.curve.offsetV = (double) pv.getProperty (kPtCovAttr, 0.0);
        lane.points.push_back (p);
    }
    std::sort (lane.points.begin(), lane.points.end(),
               [] (const dsp::automation::AutomationPoint& a,
                   const dsp::automation::AutomationPoint& b) noexcept
               { return a.tBeats < b.tBeats; });
    return lane;
}

} // namespace element
