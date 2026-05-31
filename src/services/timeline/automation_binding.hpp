// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>
#include <element/juce/graphics.hpp>   // juce::Colour

namespace element {

/** Presentation record for one automation lane/overlay shown in the
 *  arrangement.  PRESENTATION ONLY -- the curve data, target key and
 *  mode live in the song-owned engine AutomationTrack referenced by
 *  `trackId` (persisted separately under tags::automationTracks via
 *  AutomationEngine::save/loadFromValueTree, so timeline automation
 *  plays regardless of which view is active).  This struct just records
 *  WHERE + HOW the arrangement draws that track.
 *
 *    ownerLaneId.isNull()  -> standalone "dedicated" automation lane
 *                             (its own row in the arrangement).
 *    ownerLaneId != null   -> overlay nested under the lane with that
 *                             id (Ableton/Bitwig per-track automation).
 *
 *  Keeping presentation here (view domain) and curve data in the engine
 *  (song domain) is the timeline/song-owned split: see memory
 *  project_automation_timeline_vs_clip_semantics.  The dedicated-lane
 *  vs overlay rendering choice is Layer 3 / UI -- this data supports
 *  either without change.
 *
 *  Bindings are stored as a list under tags::arrangement, a peer to the
 *  <lanes> child -- additive, so existing lane serialization + undo are
 *  untouched. */
struct AutomationBinding
{
    juce::Uuid    id;
    juce::Uuid    trackId     { juce::Uuid::null() };  // -> engine AutomationTrack
    juce::Uuid    ownerLaneId { juce::Uuid::null() };  // null => dedicated lane
    juce::String  name;                                // display cache (target param name)
    juce::Colour  colour      { 0xff'48'9a'c8 };
    int           heightPx     { 48 };
    bool          expanded     { true };

    /** True when this binding renders as an overlay under a parent lane
     *  rather than as its own dedicated lane row. */
    bool isOverlay() const noexcept { return ! ownerLaneId.isNull(); }

    /** Sparse-write XML (only non-default fields). */
    juce::ValueTree toValueTree() const;
    static AutomationBinding fromValueTree (const juce::ValueTree&);
};

} // namespace element
