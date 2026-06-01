// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/timeline/midi_cc_lane.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "dsp/automation/automation_point.hpp"

#include <element/juce/core.hpp>

#include <cmath>

using element::MidiCcLane;
using element::MidiNote;
using element::MidiNoteRegion;
using element::dsp::automation::AutomationPoint;

namespace {

inline bool nearly (double a, double b, double tol = 1e-9)
{
    return std::abs (a - b) <= tol;
}

/* offsetT defaults to 0.5 (centred handle = straight chord); pass a
 * non-default offsetT/offsetV to bend the segment after this point. */
AutomationPoint pt (double t, double v,
                    double offsetT = 0.5,
                    double offsetV = 0.0)
{
    AutomationPoint p;
    p.tBeats          = t;
    p.valueNormalized = v;
    p.curve.offsetT   = offsetT;
    p.curve.offsetV   = offsetV;
    return p;
}

} // namespace

BOOST_AUTO_TEST_SUITE (MidiCcLaneTests)

//==============================================================================
// MidiCcLane sampling.

BOOST_AUTO_TEST_CASE (empty_lane_is_neutral)
{
    MidiCcLane lane;
    BOOST_CHECK (nearly (lane.valueAtBeats (0.0), 0.5));
    BOOST_CHECK_EQUAL (lane.ccValueAtBeats (3.0), 64);   // round(0.5*127) = 64
}

BOOST_AUTO_TEST_CASE (single_point_is_flat)
{
    MidiCcLane lane;
    lane.points = { pt (0.0, 0.25) };
    BOOST_CHECK (nearly (lane.valueAtBeats (-1.0), 0.25));
    BOOST_CHECK (nearly (lane.valueAtBeats (5.0),  0.25));
    BOOST_CHECK_EQUAL (lane.ccValueAtBeats (5.0), (int) std::lround (0.25 * 127.0));
}

BOOST_AUTO_TEST_CASE (linear_interpolation_and_clamp)
{
    MidiCcLane lane;
    lane.points = { pt (0.0, 0.0), pt (4.0, 1.0) };

    BOOST_CHECK (nearly (lane.valueAtBeats (-1.0), 0.0));   // clamp left
    BOOST_CHECK (nearly (lane.valueAtBeats (0.0),  0.0));
    BOOST_CHECK (nearly (lane.valueAtBeats (2.0),  0.5));   // midpoint
    BOOST_CHECK (nearly (lane.valueAtBeats (4.0),  1.0));
    BOOST_CHECK (nearly (lane.valueAtBeats (9.0),  1.0));   // clamp right

    BOOST_CHECK_EQUAL (lane.ccValueAtBeats (2.0), 64);      // round(0.5*127)
    BOOST_CHECK_EQUAL (lane.ccValueAtBeats (4.0), 127);
    BOOST_CHECK_EQUAL (lane.ccValueAtBeats (0.0), 0);
}

BOOST_AUTO_TEST_CASE (descending_segment_slopes_down)
{
    /* Regression: evaluate() is value-normalised, so a descending segment
     * must slope DOWN, not reverse into a rising "sawtooth" (the bug the
     * naive from + yn*(to-from) mapping produced). */
    MidiCcLane lane;
    lane.points = { pt (0.0, 1.0), pt (4.0, 0.0) };   // full decline

    BOOST_CHECK (nearly (lane.valueAtBeats (0.0), 1.0));
    BOOST_CHECK (nearly (lane.valueAtBeats (1.0), 0.75));
    BOOST_CHECK (nearly (lane.valueAtBeats (2.0), 0.5));
    BOOST_CHECK (nearly (lane.valueAtBeats (3.0), 0.25));
    BOOST_CHECK (nearly (lane.valueAtBeats (4.0), 0.0));
    /* Monotonic non-increasing across the whole segment. */
    double prev = 2.0;
    for (double b = 0.0; b <= 4.0; b += 0.5)
    {
        const double v = lane.valueAtBeats (b);
        BOOST_CHECK (v <= prev + 1e-9);
        prev = v;
    }
}

BOOST_AUTO_TEST_CASE (mixed_incline_then_decline)
{
    MidiCcLane lane;
    lane.points = { pt (0.0, 0.2), pt (2.0, 0.8), pt (4.0, 0.4) };
    BOOST_CHECK (nearly (lane.valueAtBeats (0.0), 0.2));
    BOOST_CHECK (nearly (lane.valueAtBeats (1.0), 0.5));   // rising
    BOOST_CHECK (nearly (lane.valueAtBeats (2.0), 0.8));   // peak (point)
    BOOST_CHECK (nearly (lane.valueAtBeats (3.0), 0.6));   // falling
    BOOST_CHECK (nearly (lane.valueAtBeats (4.0), 0.4));
}

BOOST_AUTO_TEST_CASE (value_tree_round_trip_preserves_points_and_curve)
{
    MidiCcLane lane;
    lane.ccNumber = 11;
    lane.channel  = 3;
    lane.points   = { pt (0.0, 0.1),
                      pt (2.0, 0.9, 0.65, 0.2),   // bent handle
                      pt (4.0, 0.4) };

    const auto vt = lane.toValueTree();
    const auto back = MidiCcLane::fromValueTree (vt);

    BOOST_CHECK_EQUAL (back.ccNumber, 11);
    BOOST_CHECK_EQUAL (back.channel,  3);
    BOOST_REQUIRE_EQUAL (back.points.size(), (size_t) 3);
    BOOST_CHECK (nearly (back.points[1].tBeats, 2.0));
    BOOST_CHECK (nearly (back.points[1].valueNormalized, 0.9));
    BOOST_CHECK (nearly (back.points[1].curve.offsetT, 0.65));
    BOOST_CHECK (nearly (back.points[1].curve.offsetV, 0.2));
    /* Default centred handle survives the sparse-write. */
    BOOST_CHECK (nearly (back.points[0].curve.offsetT, 0.5));
    BOOST_CHECK (nearly (back.points[0].curve.offsetV, 0.0));
}

//==============================================================================
// MidiNoteRegion CC-lane snapshot interface.

BOOST_AUTO_TEST_CASE (region_starts_with_no_cc_lanes)
{
    MidiNoteRegion r;
    const auto* snap = r.loadCcSnapshot();
    BOOST_REQUIRE (snap != nullptr);   // never null after construction
    BOOST_CHECK (snap->empty());
}

BOOST_AUTO_TEST_CASE (set_cc_lane_upsert_replace_remove)
{
    MidiNoteRegion r;

    r.setCcLane (1, 1, { pt (0.0, 0.0), pt (1.0, 1.0) });
    {
        const auto* s = r.loadCcSnapshot();
        BOOST_REQUIRE_EQUAL (s->size(), (size_t) 1);
        BOOST_CHECK_EQUAL ((*s)[0].ccNumber, 1);
        BOOST_CHECK_EQUAL ((*s)[0].points.size(), (size_t) 2);
    }

    /* Same (cc, channel) replaces points in place, not appends. */
    r.setCcLane (1, 1, { pt (0.0, 0.5) });
    {
        const auto* s = r.loadCcSnapshot();
        BOOST_REQUIRE_EQUAL (s->size(), (size_t) 1);
        BOOST_CHECK_EQUAL ((*s)[0].points.size(), (size_t) 1);
    }

    /* Different cc appends. */
    r.setCcLane (11, 1, { pt (0.0, 0.2), pt (2.0, 0.8) });
    BOOST_CHECK_EQUAL (r.loadCcSnapshot()->size(), (size_t) 2);

    /* Empty point set removes the lane. */
    r.setCcLane (1, 1, {});
    {
        const auto* s = r.loadCcSnapshot();
        BOOST_REQUIRE_EQUAL (s->size(), (size_t) 1);
        BOOST_CHECK_EQUAL ((*s)[0].ccNumber, 11);
    }

    r.removeCcLane (11, 1);
    BOOST_CHECK (r.loadCcSnapshot()->empty());
}

BOOST_AUTO_TEST_CASE (region_round_trip_preserves_notes_and_cc)
{
    MidiNoteRegion r;
    r.id            = juce::Uuid();
    r.positionBeats = 4.0;
    r.lengthBeats   = 8.0;

    MidiNote n;
    n.pitch = 64; n.onBeat = 1.0; n.lengthBeats = 0.5; n.velocity = 90;
    r.addNote (n);

    r.setCcLane (74, 2, { pt (0.0, 0.1), pt (4.0, 0.9, 0.4, -0.15) });

    const auto vt = r.toValueTree();
    auto back = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (back != nullptr);

    /* Notes survive. */
    BOOST_CHECK_EQUAL (back->loadSnapshot()->size(), (size_t) 1);

    /* CC lanes survive with curve metadata. */
    const auto* cc = back->loadCcSnapshot();
    BOOST_REQUIRE_EQUAL (cc->size(), (size_t) 1);
    BOOST_CHECK_EQUAL ((*cc)[0].ccNumber, 74);
    BOOST_CHECK_EQUAL ((*cc)[0].channel,  2);
    BOOST_REQUIRE_EQUAL ((*cc)[0].points.size(), (size_t) 2);
    BOOST_CHECK (nearly ((*cc)[0].points[1].curve.offsetT, 0.4));
    BOOST_CHECK (nearly ((*cc)[0].points[1].curve.offsetV, -0.15));
}

BOOST_AUTO_TEST_CASE (clone_copies_cc_lanes)
{
    MidiNoteRegion r;
    r.setCcLane (1, 1, { pt (0.0, 0.0), pt (2.0, 1.0) });

    auto c = r.clone();
    BOOST_REQUIRE (c != nullptr);
    const auto* cc = c->loadCcSnapshot();
    BOOST_REQUIRE_EQUAL (cc->size(), (size_t) 1);
    BOOST_CHECK_EQUAL ((*cc)[0].points.size(), (size_t) 2);

    /* Independent storage: mutating the clone doesn't touch the original. */
    c->removeCcLane (1, 1);
    BOOST_CHECK (c->loadCcSnapshot()->empty());
    BOOST_CHECK_EQUAL (r.loadCcSnapshot()->size(), (size_t) 1);
}

BOOST_AUTO_TEST_SUITE_END()
