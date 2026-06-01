// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "dsp/automation/curve.hpp"
#include "dsp/automation/parameter_change_tracker.hpp"
#include "dsp/automation/automation_point.hpp"
#include "dsp/automation/automation_region.hpp"
#include "dsp/automation/automation_track.hpp"

#include <element/juce/core.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using element::dsp::automation::AutomationMode;
using element::dsp::automation::AutomationPoint;
using element::dsp::automation::AutomationRecordMode;
using element::dsp::automation::AutomationRegion;
using element::dsp::automation::AutomationTrack;
using element::dsp::automation::CurveOptions;
using element::dsp::automation::evaluateSegment;
using element::dsp::automation::ParameterChangeTracker;

namespace {

inline bool nearly (double a, double b, double tol = 1e-9)
{
    return std::abs (a - b) <= tol;
}

} // namespace

BOOST_AUTO_TEST_SUITE (AutomationCurveTests)

/* ---------- evaluateSegment: 2D-Bezier curve model ---------- */

BOOST_AUTO_TEST_CASE (output_is_clamped_to_unit_interval)
{
    for (double ot : { 0.25, 0.4, 0.5, 0.6, 0.75 })
        for (double ov : { -0.6, -0.2, 0.0, 0.2, 0.6 })
            for (double v0 : { 0.0, 0.3, 1.0 })
                for (double v1 : { 0.0, 0.7, 1.0 })
                    for (double x : { 0.0, 0.25, 0.5, 0.75, 1.0 })
                    {
                        const double y = evaluateSegment (x, v0, v1, { ot, ov });
                        BOOST_REQUIRE_GE (y, 0.0);
                        BOOST_REQUIRE_LE (y, 1.0);
                    }
}

BOOST_AUTO_TEST_CASE (input_outside_unit_interval_clamps)
{
    const CurveOptions opts { 0.6, 0.2 };
    BOOST_CHECK (nearly (evaluateSegment (-0.5, 0.2, 0.8, opts),
                          evaluateSegment ( 0.0, 0.2, 0.8, opts)));
    BOOST_CHECK (nearly (evaluateSegment ( 1.5, 0.2, 0.8, opts),
                          evaluateSegment ( 1.0, 0.2, 0.8, opts)));
}

BOOST_AUTO_TEST_CASE (default_handle_is_straight_lerp)
{
    /* Centred handle (0.5, 0) == straight chord between the endpoints. */
    const CurveOptions opts { };
    BOOST_CHECK (nearly (evaluateSegment (0.0,  0.2, 0.8, opts), 0.2));
    BOOST_CHECK (nearly (evaluateSegment (0.5,  0.2, 0.8, opts), 0.5));
    BOOST_CHECK (nearly (evaluateSegment (1.0,  0.2, 0.8, opts), 0.8));
    /* Descending chord: still a straight lerp, no reflection artefacts. */
    BOOST_CHECK (nearly (evaluateSegment (0.25, 0.8, 0.0, opts), 0.6));
}

BOOST_AUTO_TEST_CASE (endpoints_always_pin)
{
    /* For ANY handle the curve must start at v0 and end at v1 so points
     * meet their neighbours with no visible gap. */
    for (double ot : { 0.25, 0.5, 0.75 })
        for (double ov : { -0.4, 0.0, 0.4 })
        {
            const CurveOptions opts { ot, ov };
            BOOST_CHECK (nearly (evaluateSegment (0.0, 0.3, 0.7, opts), 0.3, 1e-9));
            BOOST_CHECK (nearly (evaluateSegment (1.0, 0.3, 0.7, opts), 0.7, 1e-9));
        }
}

BOOST_AUTO_TEST_CASE (positive_offsetV_bulges_up_both_directions)
{
    /* offsetV > 0 must push the mid value ABOVE the straight chord
     * whether the segment rises or falls (value-space, not fraction). */
    const CurveOptions up { 0.5, 0.2 };
    const double chordAsc  = 0.5;                 // straight mid of 0.2..0.8
    const double chordDesc = 0.5;                 // straight mid of 0.8..0.2
    BOOST_CHECK_GT (evaluateSegment (0.5, 0.2, 0.8, up), chordAsc);
    BOOST_CHECK_GT (evaluateSegment (0.5, 0.8, 0.2, up), chordDesc);

    const CurveOptions down { 0.5, -0.2 };
    BOOST_CHECK_LT (evaluateSegment (0.5, 0.2, 0.8, down), chordAsc);
    BOOST_CHECK_LT (evaluateSegment (0.5, 0.8, 0.2, down), chordDesc);
}

BOOST_AUTO_TEST_CASE (curve_passes_through_the_bend_pin)
{
    /* The defining property of the 2D handle: the curve passes through
     * (offsetT, chordMid + offsetV).  At x == offsetT the value must be
     * the pin value (when it lands inside [0,1]). */
    const double v0 = 0.2, v1 = 0.6, chordMid = 0.4;
    for (double ot : { 0.3, 0.5, 0.7 })
        for (double ov : { -0.15, 0.0, 0.2 })
        {
            const double y = evaluateSegment (ot, v0, v1, { ot, ov });
            BOOST_CHECK (nearly (y, chordMid + ov, 1e-6));
        }
}

/* ---------- ParameterChangeTracker ---------- */

BOOST_AUTO_TEST_SUITE (ParameterChangeTrackerTests)

BOOST_AUTO_TEST_CASE (begin_block_seeds_all_fields_from_knob)
{
    ParameterChangeTracker t;
    t.beginBlock (0.42);
    BOOST_CHECK (nearly (t.baseValue,      0.42));
    BOOST_CHECK (nearly (t.automatedValue, 0.42));
    BOOST_CHECK (nearly (t.modulatedValue, 0.42));
    BOOST_CHECK (! t.changedThisBlock);
}

BOOST_AUTO_TEST_CASE (apply_automation_overrides_base)
{
    ParameterChangeTracker t;
    t.beginBlock (0.20);
    t.applyAutomation (0.80);
    BOOST_CHECK (nearly (t.baseValue,      0.20));   /* knob untouched */
    BOOST_CHECK (nearly (t.automatedValue, 0.80));
    BOOST_CHECK (nearly (t.modulatedValue, 0.80));
}

BOOST_AUTO_TEST_CASE (finalize_block_sets_changed_when_modulated_differs)
{
    ParameterChangeTracker t;
    t.beginBlock (0.5); t.finalizeBlock();      /* first block -- changed (was 0) */
    BOOST_CHECK (t.changedThisBlock);

    t.beginBlock (0.5); t.finalizeBlock();      /* same value -- unchanged */
    BOOST_CHECK (! t.changedThisBlock);

    t.beginBlock (0.6); t.finalizeBlock();      /* different -- changed */
    BOOST_CHECK (t.changedThisBlock);

    t.beginBlock (0.6);
    t.applyAutomation (0.9);                     /* automation kicks the value */
    t.finalizeBlock();
    BOOST_CHECK (t.changedThisBlock);
}

BOOST_AUTO_TEST_SUITE_END()  /* ParameterChangeTrackerTests */

/* ---------- AutomationRegion sampling ---------- */

BOOST_AUTO_TEST_SUITE (AutomationRegionTests)

BOOST_AUTO_TEST_CASE (empty_region_samples_neutral)
{
    AutomationRegion r;
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0),  0.5));
    BOOST_CHECK (nearly (r.sampleAtBeats (4.2),  0.5));
}

BOOST_AUTO_TEST_CASE (single_point_region_returns_constant)
{
    AutomationRegion r;
    AutomationRegion::PointList pts { { 0.0, 0.42, {} } };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0),  0.42));
    BOOST_CHECK (nearly (r.sampleAtBeats (5.0),  0.42));
}

BOOST_AUTO_TEST_CASE (linear_segment_interpolates)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 0.0, 0.0, { } },
        { 4.0, 1.0, { } }
    };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0), 0.0));
    BOOST_CHECK (nearly (r.sampleAtBeats (1.0), 0.25));
    BOOST_CHECK (nearly (r.sampleAtBeats (2.0), 0.5));
    BOOST_CHECK (nearly (r.sampleAtBeats (3.0), 0.75));
    BOOST_CHECK (nearly (r.sampleAtBeats (4.0), 1.0));
}

BOOST_AUTO_TEST_CASE (linear_segment_descending_reflects)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 0.0, 0.8, { } },
        { 2.0, 0.2, { } }
    };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (1.0), 0.5));
}

BOOST_AUTO_TEST_CASE (out_of_range_clamps_to_endpoints)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 1.0, 0.2, {} },
        { 3.0, 0.8, {} }
    };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0), 0.2));   /* before first */
    BOOST_CHECK (nearly (r.sampleAtBeats (5.0), 0.8));   /* after last */
}

BOOST_AUTO_TEST_CASE (any_bend_handle_endpoint_pins)
{
    /* For any bend handle, the value at t=0 must equal the from-point
     * value and at t=1 the to-point value (no visible gap). */
    for (double ot : { 0.25, 0.5, 0.75 })
        for (double ov : { -0.4, 0.0, 0.4 })
        {
            AutomationRegion r;
            AutomationRegion::PointList pts {
                { 0.0, 0.3, { ot, ov } },
                { 2.0, 0.7, { ot, ov } }
            };
            r.setPoints (pts);
            BOOST_CHECK (nearly (r.sampleAtBeats (0.0), 0.3, 1e-6));
            BOOST_CHECK (nearly (r.sampleAtBeats (2.0), 0.7, 1e-6));
        }
}

BOOST_AUTO_TEST_CASE (setpoints_sorts_unordered_input)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 4.0, 1.0, {} },
        { 0.0, 0.0, {} },
        { 2.0, 0.5, {} }
    };
    r.setPoints (pts);
    /* Linear interp must work correctly even though input was
     * shuffled.  If sort failed, sampleAtBeats(2.0) wouldn't be 0.5. */
    BOOST_CHECK (nearly (r.sampleAtBeats (2.0), 0.5));
}

BOOST_AUTO_TEST_CASE (cow_snapshot_swap_is_visible_atomically)
{
    /* Smoke test for the leaked-ptr-trash-bin + epoch-gated reclaim
     * pattern.  The UI side publishes a new snapshot; the audio side
     * -- a separate thread simulating the render callback -- bumps
     * the audio epoch + samples in a tight loop and sees ONE coherent
     * value, never a torn read.  Can't TSan-prove race-freedom from
     * inside the test but can confirm the published value materialises
     * atomically AND no in-flight reader observes a freed pointer. */
    AutomationRegion r;
    AutomationRegion::PointList initial { { 0.0, 0.25, {} } };
    r.setPoints (initial);

    std::atomic<bool> done { false };
    std::atomic<int>  seenA { 0 };
    std::atomic<int>  seenB { 0 };

    std::thread reader ([&] ()
    {
        while (! done.load (std::memory_order_acquire))
        {
            r.advanceAudioEpoch();           /* per-block epoch tick */
            const double v = r.sampleAtBeats (0.0);
            if (nearly (v, 0.25)) seenA.fetch_add (1, std::memory_order_relaxed);
            else if (nearly (v, 0.75)) seenB.fetch_add (1, std::memory_order_relaxed);
            else BOOST_FAIL ("torn snapshot read: " << v);
        }
    });

    /* UI thread: publish + sweep concurrent with the reader.  The
     * epoch gate must keep sweepTrash() from reclaiming any snapshot
     * the reader is currently using. */
    for (int i = 0; i < 1000; ++i)
    {
        AutomationRegion::PointList next { { 0.0, (i & 1) ? 0.75 : 0.25, {} } };
        r.setPoints (next);
        if ((i % 16) == 0)
            r.sweepTrash();
        std::this_thread::sleep_for (std::chrono::microseconds (10));
    }
    done.store (true, std::memory_order_release);
    reader.join();

    BOOST_CHECK_GT (seenA.load() + seenB.load(), 0);

    /* Final sweep -- audio thread is gone, all trash must be safely
     * reclaimable now. */
    r.advanceAudioEpoch();
    r.sweepTrash();
}

BOOST_AUTO_TEST_CASE (region_xml_round_trip)
{
    AutomationRegion r;
    r.id = juce::Uuid();
    r.positionBeats = 8.0;
    r.lengthBeats   = 4.0;
    r.looped        = true;
    AutomationRegion::PointList pts {
        { 0.0, 0.1, { } },
        { 2.0, 0.9, { 0.35, -0.2 } },
        { 4.0, 0.5, { 0.65,  0.15 } }
    };
    r.setPoints (pts);

    const auto vt = r.toValueTree();
    auto restored = AutomationRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);

    BOOST_CHECK (restored->id == r.id);
    BOOST_CHECK_EQUAL (restored->positionBeats, r.positionBeats);
    BOOST_CHECK_EQUAL (restored->lengthBeats,   r.lengthBeats);
    BOOST_CHECK (restored->looped);

    /* Sampling parity: the restored region must produce the same
     * values at the same beat offsets. */
    for (double t : { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0 })
        BOOST_CHECK (nearly (restored->sampleAtBeats (t), r.sampleAtBeats (t), 1e-9));
}

BOOST_AUTO_TEST_SUITE_END()  /* AutomationRegionTests */

/* ---------- AutomationTrack region resolution ---------- */

BOOST_AUTO_TEST_SUITE (AutomationTrackTests)

BOOST_AUTO_TEST_CASE (empty_track_returns_no_active_region)
{
    AutomationTrack t;
    BOOST_CHECK (t.findActiveRegion (0.0) == nullptr);
    BOOST_CHECK (t.findActiveRegion (100.0) == nullptr);
}

BOOST_AUTO_TEST_CASE (single_region_resolution)
{
    AutomationTrack t;
    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid();
    r->positionBeats = 4.0;
    r->lengthBeats   = 4.0;
    auto* raw = r.get();
    t.addRegion (std::move (r));

    BOOST_CHECK (t.findActiveRegion (3.999) == nullptr);
    BOOST_CHECK (t.findActiveRegion (4.0)   == raw);
    BOOST_CHECK (t.findActiveRegion (6.0)   == raw);
    BOOST_CHECK (t.findActiveRegion (8.0)   == nullptr);  /* end-exclusive */
}

BOOST_AUTO_TEST_CASE (multi_region_binary_search_with_cache)
{
    AutomationTrack t;
    AutomationRegion* a = nullptr;
    AutomationRegion* b = nullptr;
    AutomationRegion* c = nullptr;

    {
        auto r = std::make_unique<AutomationRegion>();
        r->positionBeats =  0.0; r->lengthBeats = 4.0;
        a = r.get();
        t.addRegion (std::move (r));
    }
    {
        auto r = std::make_unique<AutomationRegion>();
        r->positionBeats =  8.0; r->lengthBeats = 4.0;
        b = r.get();
        t.addRegion (std::move (r));
    }
    {
        auto r = std::make_unique<AutomationRegion>();
        r->positionBeats = 16.0; r->lengthBeats = 4.0;
        c = r.get();
        t.addRegion (std::move (r));
    }

    BOOST_CHECK (t.findActiveRegion ( 2.0) == a);
    BOOST_CHECK (t.findActiveRegion ( 5.0) == nullptr);   /* gap */
    BOOST_CHECK (t.findActiveRegion (10.0) == b);
    BOOST_CHECK (t.findActiveRegion (12.0) == nullptr);   /* end-exclusive */
    BOOST_CHECK (t.findActiveRegion (18.0) == c);

    /* Hit cache: repeated calls in the same region must reuse the
     * cached pointer.  We can't observe this directly without a
     * counter, but the call must continue returning the correct
     * region without crashing. */
    for (int i = 0; i < 100; ++i)
        BOOST_CHECK (t.findActiveRegion (18.0) == c);
}

BOOST_AUTO_TEST_CASE (remove_region_clears_cache)
{
    AutomationTrack t;
    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid();
    r->positionBeats = 0.0;
    r->lengthBeats   = 4.0;
    const auto id = r->id;
    auto* raw = r.get();
    t.addRegion (std::move (r));

    BOOST_CHECK (t.findActiveRegion (1.0) == raw);
    t.removeRegion (id);
    BOOST_CHECK (t.findActiveRegion (1.0) == nullptr);
    t.sweepTrash();   /* reclaims displaced snapshot + the region */
}

BOOST_AUTO_TEST_CASE (mode_atomic_round_trip)
{
    AutomationTrack t;
    BOOST_CHECK (t.getMode() == AutomationMode::Off);
    t.setMode (AutomationMode::Read);
    BOOST_CHECK (t.getMode() == AutomationMode::Read);
    t.setMode (AutomationMode::Record);
    BOOST_CHECK (t.getMode() == AutomationMode::Record);
    t.setMode (AutomationMode::Off);
    BOOST_CHECK (t.getMode() == AutomationMode::Off);
}

BOOST_AUTO_TEST_CASE (track_xml_round_trip_internal_target)
{
    AutomationTrack t;
    t.id = juce::Uuid();
    t.targetKey.nodeId  = juce::Uuid();
    t.targetKey.paramId = "volume";
    t.setMode (AutomationMode::Read);
    t.setRecordMode (AutomationRecordMode::Latch);

    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid();
    r->positionBeats = 0.0;
    r->lengthBeats   = 4.0;
    AutomationRegion::PointList pts { { 0.0, 0.0, {} }, { 4.0, 1.0, {} } };
    r->setPoints (pts);
    t.addRegion (std::move (r));

    const auto vt = t.toValueTree();
    auto restored = AutomationTrack::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (restored->id == t.id);
    BOOST_CHECK (restored->targetKey == t.targetKey);
    BOOST_CHECK (restored->getMode() == AutomationMode::Read);
    BOOST_CHECK (restored->getRecordMode() == AutomationRecordMode::Latch);
    BOOST_CHECK (restored->findActiveRegion (2.0) != nullptr);
}

BOOST_AUTO_TEST_CASE (write_fifo_push_drain_round_trip)
{
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;

    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 0);

    /* Push a handful of events from "the UI thread". */
    BOOST_REQUIRE (t.tryPushWriteEvent ({ 0.10, 1.0 }));
    BOOST_REQUIRE (t.tryPushWriteEvent ({ 0.20, 1.5 }));
    BOOST_REQUIRE (t.tryPushWriteEvent ({ 0.30, 2.0 }));
    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 3);

    /* Drain on "the audio thread" -- events come out in order. */
    AutomationWriteEvent buf[8];
    const int n = t.drainWriteEvents (buf, 8);
    BOOST_REQUIRE_EQUAL (n, 3);
    BOOST_CHECK_CLOSE (buf[0].valueNormalized, 0.10, 1e-9);
    BOOST_CHECK_CLOSE (buf[1].hostBeats,       1.5,  1e-9);
    BOOST_CHECK_CLOSE (buf[2].hostBeats,       2.0,  1e-9);
    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 0);
}

BOOST_AUTO_TEST_CASE (write_fifo_returns_false_when_full)
{
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;

    /* AbstractFifo's published capacity is N-1 usable slots (one
     * slot is reserved as the empty/full sentinel).  Push until
     * exhausted then assert overflow returns false. */
    int pushed = 0;
    while (t.tryPushWriteEvent ({ (double) pushed / 1000.0, (double) pushed }))
        ++pushed;
    BOOST_CHECK_GT (pushed, 0);
    BOOST_CHECK_EQUAL (t.tryPushWriteEvent ({ 0.0, 0.0 }), false);

    /* Drain partial and confirm we can push again. */
    AutomationWriteEvent buf[16];
    const int drained = t.drainWriteEvents (buf, 16);
    BOOST_CHECK_GT (drained, 0);
    BOOST_CHECK (t.tryPushWriteEvent ({ 0.5, 99.0 }));
}

BOOST_AUTO_TEST_CASE (write_fifo_drain_with_zero_max_out_is_safe_noop)
{
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;
    t.tryPushWriteEvent ({ 0.5, 1.0 });

    AutomationWriteEvent buf[1];
    BOOST_CHECK_EQUAL (t.drainWriteEvents (buf,     0), 0);
    BOOST_CHECK_EQUAL (t.drainWriteEvents (nullptr, 8), 0);
    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 1);   /* untouched */
}

BOOST_AUTO_TEST_CASE (write_fifo_cross_thread_smoke)
{
    /* SPSC stress: one thread pushes, one thread drains in a loop.
     * Verifies no event tearing + no event loss + no leak. */
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;

    constexpr int kTotal = 5000;
    std::atomic<int> drainedSum  { 0 };
    std::atomic<bool> producerDone { false };

    std::thread consumer ([&] ()
    {
        AutomationWriteEvent buf[32];
        while (true)
        {
            const int n = t.drainWriteEvents (buf, 32);
            for (int i = 0; i < n; ++i)
                drainedSum.fetch_add ((int) buf[i].valueNormalized,
                                      std::memory_order_relaxed);
            if (n == 0 && producerDone.load (std::memory_order_acquire)
                && t.getNumPendingWriteEvents() == 0)
                return;
        }
    });

    int expectedSum = 0;
    for (int i = 0; i < kTotal; ++i)
    {
        /* Spin-wait if FIFO temporarily full -- production-side
         * back-pressure rather than dropping. */
        while (! t.tryPushWriteEvent ({ (double) i, 0.0 }))
            std::this_thread::yield();
        expectedSum += i;
    }
    producerDone.store (true, std::memory_order_release);
    consumer.join();
    BOOST_CHECK_EQUAL (drainedSum.load(), expectedSum);
}

BOOST_AUTO_TEST_CASE (track_xml_round_trip_midi_target)
{
    AutomationTrack t;
    t.id = juce::Uuid();
    t.targetKey.midiCcChannel = 3;
    t.targetKey.midiCcNumber  = 74;
    t.setMode (AutomationMode::Read);

    const auto vt = t.toValueTree();
    auto restored = AutomationTrack::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (restored->targetKey.isMidi());
    BOOST_CHECK_EQUAL (restored->targetKey.midiCcChannel, 3);
    BOOST_CHECK_EQUAL (restored->targetKey.midiCcNumber,  74);
}

BOOST_AUTO_TEST_SUITE_END()  /* AutomationTrackTests */

BOOST_AUTO_TEST_SUITE_END()  /* AutomationCurveTests */
