// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/playlist.hpp"

#include <memory>

namespace element {

class TimelineAdapter;     // Phase 2: per-kind dispatcher

/** One row in the arrangement view.  Binds a Playlist to a target
 *  graph node via uuid; the Lane is NOT a graph node itself.  Adding
 *  / removing lanes doesn't change graph topology -- you add a node
 *  separately and then point a Lane at it.
 *
 *  Lane is value-typed at the data layer (this struct), wrapped by
 *  ArrangementView's UI components.  Lifetime: owned by the
 *  arrangement session-state container (Phase 1d serialises into
 *  tags::arrangement).
 *
 *  See timeline-audio-design.md Section 1.3.
 */
struct Lane
{
    /** Coarse lane kind for UI dispatch.  Phase 2 introduces this to
     *  distinguish piano-roll-MIDI lanes from audio/tracker lanes at
     *  paint + Add-Lane time.  Tracker lanes are NOT a separate kind
     *  here; they share Kind::Audio + are runtime-distinguished by
     *  the resolved target-node type (ArrangementView caches an
     *  audioClipCache OR trackerCache pointer per lane).  This keeps
     *  Phase 2 minimal -- only the truly new affordance (MIDI lane)
     *  needs an explicit kind.
     *
     *  Sparse-write XML: defaults to Audio so older sessions load
     *  unchanged. */
    enum class Kind : int { Audio = 0, Midi = 1 };

    juce::Uuid    id;
    juce::String  name;
    Kind          kind       { Kind::Audio };
    juce::Colour  colour     { 0xff'30'30'30 };
    /** Per-lane clip-strip height in pixels, OR 0 = "follow the arranger's
     *  global vertical zoom" (the default).  Set non-zero only when the
     *  user drags a lane's bottom edge to give it a custom height; sparse-
     *  written so existing sessions (which never carried a height) keep
     *  following the global zoom.  ArrangementView clamps to its
     *  [kLaneHMin, kLaneHMax] range on read. */
    int           heightPx   { 0 };
    bool          muted      { false };
    bool          soloed     { false };
    bool          armed      { false };

    /** Uuid of the target node in the live graph.  Resolved at adapter
     *  bind time -- when the target node is missing (e.g. user
     *  deleted it from the graph editor), the lane stays in the
     *  arrangement but doesn't dispatch.  ArrangementView surfaces
     *  this as a greyed lane with a "rebind" affordance.
     *
     *  Phase 2 MIDI lanes have a null targetNodeUuid -- MIDI playback
     *  is wired in a later phase. */
    juce::Uuid    targetNodeUuid;

    /** Dispatch contents.  Owned by the Lane; copy-on-write semantics
     *  would be a v2 optimisation if we want lane duplication to
     *  share regions until the user mutates one.  Phase 2: also holds
     *  per-lane MidiNoteRegion list (Playlist.midiRegions). */
    Playlist      playlist;

    /** Sparse-write XML.  Only writes non-default fields. */
    juce::ValueTree toValueTree() const;
    static Lane     fromValueTree (const juce::ValueTree&);
};

} // namespace element
