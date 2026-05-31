// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/automation_binding.hpp"

namespace element {

namespace {
const juce::Identifier kBindingTag    ("automationBinding");
const juce::Identifier kIdAttr        ("id");
const juce::Identifier kTrackIdAttr   ("trackId");
const juce::Identifier kOwnerAttr     ("ownerLaneId");
const juce::Identifier kNameAttr      ("name");
const juce::Identifier kColourAttr    ("colour");
const juce::Identifier kHeightAttr    ("heightPx");
const juce::Identifier kExpandedAttr  ("expanded");

constexpr int kDefaultHeightPx = 48;
} // namespace

juce::ValueTree AutomationBinding::toValueTree() const
{
    juce::ValueTree v (kBindingTag);
    v.setProperty (kIdAttr, id.toString(), nullptr);

    /* trackId is the link to the engine's AutomationTrack -- always
     * written (a binding with a null trackId is a not-yet-targeted
     * placeholder, still round-trips so the null reloads). */
    v.setProperty (kTrackIdAttr, trackId.toString(), nullptr);

    /* Sparse-write: a null owner (dedicated lane) is the common case. */
    if (! ownerLaneId.isNull())
        v.setProperty (kOwnerAttr, ownerLaneId.toString(), nullptr);

    if (name.isNotEmpty())
        v.setProperty (kNameAttr, name, nullptr);

    v.setProperty (kColourAttr, colour.toString(), nullptr);

    if (heightPx != kDefaultHeightPx)
        v.setProperty (kHeightAttr, heightPx, nullptr);

    /* expanded defaults true -- only emit when collapsed. */
    if (! expanded)
        v.setProperty (kExpandedAttr, false, nullptr);

    return v;
}

AutomationBinding AutomationBinding::fromValueTree (const juce::ValueTree& v)
{
    AutomationBinding b;
    if (! v.isValid() || ! v.hasType (kBindingTag))
        return b;

    b.id = juce::Uuid (v.getProperty (kIdAttr).toString());

    {
        const juce::String t = v.getProperty (kTrackIdAttr, juce::String()).toString();
        b.trackId = t.isNotEmpty() ? juce::Uuid (t) : juce::Uuid::null();
    }
    {
        const juce::String owner = v.getProperty (kOwnerAttr, juce::String()).toString();
        b.ownerLaneId = owner.isNotEmpty() ? juce::Uuid (owner) : juce::Uuid::null();
    }

    b.name = v.getProperty (kNameAttr, juce::String()).toString();

    {
        const juce::String s = v.getProperty (kColourAttr).toString();
        if (s.isNotEmpty()) b.colour = juce::Colour::fromString (s);
    }

    b.heightPx = (int)  v.getProperty (kHeightAttr,  kDefaultHeightPx);
    b.expanded = (bool) v.getProperty (kExpandedAttr, true);

    return b;
}

} // namespace element
