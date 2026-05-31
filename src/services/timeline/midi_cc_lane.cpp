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
const juce::Identifier kPointTag   { "p" };
const juce::Identifier kPtTAttr    { "t" };   // tBeats (region-local)
const juce::Identifier kPtVAttr    { "v" };   // valueNormalized
const juce::Identifier kPtAlgAttr  { "ca" };  // curve algorithm (enum int)
const juce::Identifier kPtCurvAttr { "cv" };  // curviness
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
    const double yNorm = dsp::automation::evaluate (
        xNorm, from.curve, from.valueNormalized > to.valueNormalized);
    return from.valueNormalized + yNorm * (to.valueNormalized - from.valueNormalized);
}

int MidiCcLane::ccValueAtBeats (double localBeats) const noexcept
{
    const double v = juce::jlimit (0.0, 1.0, valueAtBeats (localBeats));
    return juce::jlimit (0, 127, (int) std::lround (v * 127.0));
}

juce::ValueTree MidiCcLane::toValueTree() const
{
    juce::ValueTree v (kCcTag);
    v.setProperty (kCcNumAttr, ccNumber, nullptr);
    if (channel != 1)
        v.setProperty (kCcChanAttr, channel, nullptr);

    for (const auto& p : points)
    {
        juce::ValueTree pv (kPointTag);
        pv.setProperty (kPtTAttr, p.tBeats,          nullptr);
        pv.setProperty (kPtVAttr, p.valueNormalized, nullptr);
        /* Curve defaults (Linear / curviness 0) are sparse-skipped. */
        if (p.curve.algorithm != dsp::automation::CurveAlgorithm::Linear)
            pv.setProperty (kPtAlgAttr, (int) p.curve.algorithm, nullptr);
        if (p.curve.curviness != 0.0)
            pv.setProperty (kPtCurvAttr, p.curve.curviness, nullptr);
        v.appendChild (pv, nullptr);
    }
    return v;
}

MidiCcLane MidiCcLane::fromValueTree (const juce::ValueTree& v)
{
    MidiCcLane lane;
    if (! v.hasType (kCcTag))
        return lane;

    lane.ccNumber = (int) v.getProperty (kCcNumAttr, 1);
    lane.channel  = (int) v.getProperty (kCcChanAttr, 1);

    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        const auto pv = v.getChild (i);
        if (! pv.hasType (kPointTag)) continue;
        dsp::automation::AutomationPoint p;
        p.tBeats          = (double) pv.getProperty (kPtTAttr, 0.0);
        p.valueNormalized = (double) pv.getProperty (kPtVAttr, 0.0);
        p.curve.algorithm = (dsp::automation::CurveAlgorithm)
            (int) pv.getProperty (kPtAlgAttr, (int) dsp::automation::CurveAlgorithm::Linear);
        p.curve.curviness = (double) pv.getProperty (kPtCurvAttr, 0.0);
        lane.points.push_back (p);
    }
    std::sort (lane.points.begin(), lane.points.end(),
               [] (const dsp::automation::AutomationPoint& a,
                   const dsp::automation::AutomationPoint& b) noexcept
               { return a.tBeats < b.tBeats; });
    return lane;
}

} // namespace element
