// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/automation/automation_region.hpp"

#include <algorithm>
#include <cmath>

namespace element::dsp::automation {

namespace {

/* ValueTree property identifiers for sparse-write serialisation.  Kept
 * file-local; not part of the public API.  Short attribute names keep
 * round-tripped XML compact even with many points per region. */
const juce::Identifier kIdAttr            { "id" };
const juce::Identifier kPositionAttr      { "pos" };
const juce::Identifier kLengthAttr        { "len" };
const juce::Identifier kLoopedAttr        { "loop" };
const juce::Identifier kPointTag          { "p" };
const juce::Identifier kPointTAttr        { "t" };
const juce::Identifier kPointVAttr        { "v" };
const juce::Identifier kPointOffsetTAttr  { "cot" };   // bend handle X (0.25..0.75)
const juce::Identifier kPointOffsetVAttr  { "cov" };   // bend handle value offset
const juce::Identifier kAutomationRegionTag{ "automationRegion" };

/* Threshold for "matching" floating-point coordinates in
 * removePointsMatching.  Points are user-placed so the natural epsilon
 * is well above double precision; 1e-6 beats / 1e-6 normalized covers
 * any UI-grid quantisation we'll see. */
constexpr double kPointMatchEps = 1e-6;

} // namespace

AutomationRegion::AutomationRegion()
{
    /* Publish an empty snapshot up front so the audio thread never
     * observes a null active-points pointer.  Ownership: the live
     * snapshot is owned by the region until either replaced (old
     * goes to trash_) or the region is destroyed (handled in dtor). */
    activePoints_.store (new PointList(), std::memory_order_release);
}

AutomationRegion::~AutomationRegion()
{
    /* Reclaim the live snapshot.  trash_ unique_ptrs reclaim
     * themselves at deque destruction. */
    if (auto* live = activePoints_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
}

//==============================================================================

double AutomationRegion::sampleAtBeats (double localBeats) const noexcept
{
    /* Wait-free read of the published snapshot.  The pointer remains
     * valid for the entire render block because UI-thread reclaim only
     * runs on the message thread via sweepTrash(), and the message
     * thread doesn't run during the audio callback. */
    const auto* snap = activePoints_.load (std::memory_order_acquire);
    if (snap == nullptr || snap->empty())
        return 0.5;

    if (snap->size() == 1)
        return (*snap)[0].valueNormalized;

    /* Clamp to the range covered by the points -- callers handle
     * out-of-region behaviour upstream (region-not-active path).  No
     * extrapolation beyond endpoints. */
    if (localBeats <= snap->front().tBeats)
        return snap->front().valueNormalized;
    if (localBeats >= snap->back().tBeats)
        return snap->back().valueNormalized;

    /* Find the first point with tBeats > localBeats; the bracketing
     * pair is (it-1, it).  lower_bound is O(log n) on the sorted
     * snapshot; n is typically tens of points per region. */
    const auto cmp = [] (const AutomationPoint& a, double t) noexcept
    {
        return a.tBeats < t;
    };

    auto it = std::lower_bound (snap->begin(), snap->end(), localBeats, cmp);
    if (it == snap->begin())
        return snap->front().valueNormalized;
    if (it == snap->end())
        return snap->back().valueNormalized;

    const auto& from = *(it - 1);
    const auto& to   = *it;
    const double span = to.tBeats - from.tBeats;
    if (span <= 0.0)
        return to.valueNormalized;

    const double xNorm = (localBeats - from.tBeats) / span;
    /* evaluateSegment returns the actual value directly (value-space 2D
     * Bezier between the two endpoints), so no lo/hi remap is needed. */
    return evaluateSegment (xNorm, from.valueNormalized, to.valueNormalized, from.curve);
}

//==============================================================================

void AutomationRegion::setPoints (PointList newPoints)
{
    std::sort (newPoints.begin(), newPoints.end(),
               [] (const AutomationPoint& a, const AutomationPoint& b) noexcept
               { return a.tBeats < b.tBeats; });

    auto snap = std::make_unique<PointList> (std::move (newPoints));
    publishSnapshot (std::move (snap));
}

void AutomationRegion::addPoint (AutomationPoint p)
{
    mutateAndPublish ([&] (PointList& copy)
    {
        copy.push_back (p);
        std::sort (copy.begin(), copy.end(),
                   [] (const AutomationPoint& a, const AutomationPoint& b) noexcept
                   { return a.tBeats < b.tBeats; });
    });
}

void AutomationRegion::removePointsMatching (const AutomationPoint& example) noexcept
{
    mutateAndPublish ([&] (PointList& copy)
    {
        copy.erase (std::remove_if (copy.begin(), copy.end(),
                       [&] (const AutomationPoint& p) noexcept
                       {
                           return std::abs (p.tBeats - example.tBeats) < kPointMatchEps
                               && std::abs (p.valueNormalized - example.valueNormalized) < kPointMatchEps;
                       }),
                    copy.end());
    });
}

void AutomationRegion::sweepTrash() noexcept
{
    /* Message-thread reclaim with epoch-guarded grace period.  An
     * entry is safe to free only once audioEpoch_ has STRICTLY
     * advanced past the stamp at publish time -- that means the audio
     * thread loaded at least one new snapshot since the publish, so
     * the old pointer can't still be in flight on the audio path. */
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);
    while (! trash_.empty() && trash_.front().stampEpoch < safeEpoch)
        trash_.pop_front();
}

//==============================================================================

void AutomationRegion::publishSnapshot (std::unique_ptr<PointList> newSnap)
{
    const PointList* raw   = newSnap.release();
    const auto       stamp = audioEpoch_.load (std::memory_order_acquire);
    const PointList* old   = activePoints_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        trash_.push_back (TrashEntry { std::unique_ptr<const PointList> (old), stamp });
}

template <typename Mutator>
void AutomationRegion::mutateAndPublish (Mutator&& mutate)
{
    /* Snapshot the live points + apply the mutator to the copy.  The
     * load is acquire because we're about to read the contents on this
     * (UI) thread; the audio thread's last release-store ordered all
     * prior writes before. */
    const auto* live = activePoints_.load (std::memory_order_acquire);
    auto next = (live != nullptr)
                  ? std::make_unique<PointList> (*live)
                  : std::make_unique<PointList>();
    mutate (*next);
    publishSnapshot (std::move (next));
}

//==============================================================================

juce::ValueTree AutomationRegion::toValueTree() const
{
    juce::ValueTree v (kAutomationRegionTag);
    v.setProperty (kIdAttr, id.toString(), nullptr);
    if (positionBeats != 0.0) v.setProperty (kPositionAttr, positionBeats, nullptr);
    if (lengthBeats   != 0.0) v.setProperty (kLengthAttr,   lengthBeats,   nullptr);
    if (looped)               v.setProperty (kLoopedAttr,   true,          nullptr);

    if (const auto* snap = loadSnapshot())
    {
        for (const auto& p : *snap)
        {
            juce::ValueTree pv (kPointTag);
            pv.setProperty (kPointTAttr, p.tBeats, nullptr);
            pv.setProperty (kPointVAttr, p.valueNormalized, nullptr);
            if (p.curve.offsetT != 0.5)
                pv.setProperty (kPointOffsetTAttr, p.curve.offsetT, nullptr);
            if (p.curve.offsetV != 0.0)
                pv.setProperty (kPointOffsetVAttr, p.curve.offsetV, nullptr);
            v.appendChild (pv, nullptr);
        }
    }
    return v;
}

std::unique_ptr<AutomationRegion> AutomationRegion::fromValueTree (const juce::ValueTree& v)
{
    if (! v.hasType (kAutomationRegionTag))
        return nullptr;

    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid (v.getProperty (kIdAttr).toString());
    r->positionBeats = (double) v.getProperty (kPositionAttr, 0.0);
    r->lengthBeats   = (double) v.getProperty (kLengthAttr,   0.0);
    r->looped        = (bool)   v.getProperty (kLoopedAttr,   false);

    PointList pts;
    pts.reserve ((size_t) v.getNumChildren());
    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        auto pv = v.getChild (i);
        if (! pv.hasType (kPointTag))
            continue;
        AutomationPoint p;
        p.tBeats          = (double) pv.getProperty (kPointTAttr, 0.0);
        p.valueNormalized = (double) pv.getProperty (kPointVAttr, 0.0);
        p.curve.offsetT = (double) pv.getProperty (kPointOffsetTAttr, 0.5);
        p.curve.offsetV = (double) pv.getProperty (kPointOffsetVAttr, 0.0);
        pts.push_back (p);
    }
    r->setPoints (std::move (pts));
    return r;
}

} // namespace element::dsp::automation
