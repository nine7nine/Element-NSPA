// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/timeline/automation_binding.hpp"

using element::AutomationBinding;

BOOST_AUTO_TEST_SUITE (AutomationBindingTests)

BOOST_AUTO_TEST_CASE (dedicated_binding_round_trips_presentation_and_trackid)
{
    AutomationBinding b;
    b.id        = juce::Uuid();
    b.trackId   = juce::Uuid();
    b.name      = "Cutoff";
    b.colour    = juce::Colour (0xff'12'34'56);
    b.heightPx  = 72;
    b.expanded  = true;

    BOOST_CHECK (! b.isOverlay());   // null owner -> dedicated lane

    const auto restored = AutomationBinding::fromValueTree (b.toValueTree());

    BOOST_CHECK (restored.id      == b.id);
    BOOST_CHECK (restored.trackId == b.trackId);     // link to engine track preserved
    BOOST_CHECK (restored.ownerLaneId.isNull());
    BOOST_CHECK (! restored.isOverlay());
    BOOST_CHECK_EQUAL (restored.name.toStdString(), "Cutoff");
    BOOST_CHECK_EQUAL (restored.colour.toString().toStdString(),
                       juce::Colour (0xff'12'34'56).toString().toStdString());
    BOOST_CHECK_EQUAL (restored.heightPx, 72);
    BOOST_CHECK (restored.expanded);
}

BOOST_AUTO_TEST_CASE (overlay_binding_preserves_owner_lane)
{
    const juce::Uuid ownerLane;

    AutomationBinding b;
    b.id          = juce::Uuid();
    b.trackId     = juce::Uuid();
    b.ownerLaneId = ownerLane;

    BOOST_CHECK (b.isOverlay());

    const auto restored = AutomationBinding::fromValueTree (b.toValueTree());
    BOOST_CHECK (restored.isOverlay());
    BOOST_CHECK (restored.ownerLaneId == ownerLane);
}

BOOST_AUTO_TEST_CASE (sparse_defaults_round_trip)
{
    /* A binding left at defaults (height 90, expanded, null owner)
     * must omit those attrs yet reload to the same defaults. */
    AutomationBinding b;
    b.id      = juce::Uuid();
    b.trackId = juce::Uuid();

    const auto tree = b.toValueTree();
    BOOST_CHECK (! tree.hasProperty (juce::Identifier ("ownerLaneId")));
    BOOST_CHECK (! tree.hasProperty (juce::Identifier ("heightPx")));
    BOOST_CHECK (! tree.hasProperty (juce::Identifier ("expanded")));

    const auto restored = AutomationBinding::fromValueTree (tree);
    BOOST_CHECK (restored.ownerLaneId.isNull());
    BOOST_CHECK_EQUAL (restored.heightPx, 90);
    BOOST_CHECK (restored.expanded);
}

BOOST_AUTO_TEST_CASE (collapsed_flag_round_trips)
{
    AutomationBinding b;
    b.id       = juce::Uuid();
    b.expanded = false;

    const auto tree = b.toValueTree();
    BOOST_CHECK (tree.hasProperty (juce::Identifier ("expanded")));

    const auto restored = AutomationBinding::fromValueTree (tree);
    BOOST_CHECK (! restored.expanded);
}

BOOST_AUTO_TEST_CASE (null_track_id_placeholder_round_trips)
{
    /* A freshly created, not-yet-targeted binding has a null trackId;
     * it must round-trip with the null preserved. */
    AutomationBinding b;
    b.id = juce::Uuid();
    BOOST_CHECK (b.trackId.isNull());

    const auto restored = AutomationBinding::fromValueTree (b.toValueTree());
    BOOST_CHECK (restored.id == b.id);
    BOOST_CHECK (restored.trackId.isNull());
}

BOOST_AUTO_TEST_CASE (invalid_tree_yields_default_binding)
{
    juce::ValueTree wrong ("notABinding");
    const auto b = AutomationBinding::fromValueTree (wrong);
    BOOST_CHECK (b.ownerLaneId.isNull());
    BOOST_CHECK (b.trackId.isNull());

    const auto b2 = AutomationBinding::fromValueTree (juce::ValueTree());
    BOOST_CHECK (b2.trackId.isNull());
}

BOOST_AUTO_TEST_SUITE_END()
