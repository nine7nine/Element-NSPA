// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/lane.hpp"

namespace element {

namespace {
const juce::Identifier kIdAttr        ("id");
const juce::Identifier kNameAttr      ("name");
const juce::Identifier kKindAttr      ("kind");
const juce::Identifier kColourAttr    ("colour");
const juce::Identifier kHeightAttr    ("heightPx");
const juce::Identifier kMutedAttr     ("muted");
const juce::Identifier kSoloedAttr    ("soloed");
const juce::Identifier kArmedAttr     ("armed");
const juce::Identifier kTargetAttr    ("targetNodeUuid");
} // namespace

juce::ValueTree Lane::toValueTree() const
{
    juce::ValueTree v ("lane");
    v.setProperty (kIdAttr,     id.toString(),             nullptr);
    if (name.isNotEmpty())  v.setProperty (kNameAttr,   name,                 nullptr);
    /* Sparse-write: Kind::Audio is the default, only emit attr when
     * MIDI.  Keeps existing sessions byte-identical on round-trip. */
    if (kind != Kind::Audio) v.setProperty (kKindAttr, (int) kind, nullptr);
    v.setProperty (kColourAttr, colour.toString(),         nullptr);
    /* 0 == "follow global zoom" (default); only persist a real override. */
    if (heightPx != 0)      v.setProperty (kHeightAttr, heightPx,             nullptr);
    if (muted)              v.setProperty (kMutedAttr,  true,                 nullptr);
    if (soloed)             v.setProperty (kSoloedAttr, true,                 nullptr);
    if (armed)              v.setProperty (kArmedAttr,  true,                 nullptr);
    v.setProperty (kTargetAttr, targetNodeUuid.toString(), nullptr);
    v.appendChild (playlist.toValueTree(), nullptr);
    return v;
}

Lane Lane::fromValueTree (const juce::ValueTree& v)
{
    Lane l;
    if (! v.isValid() || v.getType() != juce::Identifier ("lane"))
        return l;

    l.id             = juce::Uuid (v.getProperty (kIdAttr).toString());
    l.name           = v.getProperty (kNameAttr,   juce::String()).toString();
    {
        const int k = (int) v.getProperty (kKindAttr, (int) Kind::Audio);
        l.kind = (k == (int) Kind::Midi) ? Kind::Midi : Kind::Audio;
    }
    {
        const juce::String s = v.getProperty (kColourAttr).toString();
        if (s.isNotEmpty()) l.colour = juce::Colour::fromString (s);
    }
    l.heightPx       = (int)  v.getProperty (kHeightAttr, 0);
    l.muted          = (bool) v.getProperty (kMutedAttr,  false);
    l.soloed         = (bool) v.getProperty (kSoloedAttr, false);
    l.armed          = (bool) v.getProperty (kArmedAttr,  false);
    l.targetNodeUuid = juce::Uuid (v.getProperty (kTargetAttr).toString());

    /* Playlist is a single child under the lane node.  Missing child
     * leaves the lane with an empty playlist (legal for fresh
     * lanes). */
    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        const auto child = v.getChild (i);
        if (child.getType() != juce::Identifier ("playlist")) continue;
        l.playlist = Playlist::fromValueTree (child);
        break;
    }
    return l;
}

} // namespace element
