// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/midi_note.hpp"
#include "services/timeline/midi_cc_lane.hpp"

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>
#include <element/juce/graphics.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace element {

/** A contiguous span of MIDI notes on the timeline.  Peer to Region
 *  (services/timeline/region.hpp) -- the audio + tracker-sequence
 *  region kind -- in the sense that it is itself a region data type,
 *  but stored separately inside Playlist (midiRegions_) because
 *  MidiNoteRegion is non-copyable (atomic snapshot ptr + trash deque)
 *  and cannot live in std::vector<Region> by value.
 *
 *  Thread model -- copies the AutomationRegion (Phase 1) discipline
 *  verbatim:
 *
 *    ONE writer (UI / message thread), ONE reader (audio thread).
 *    The note list is published via a raw pointer atomic swap.  UI
 *    thread builds a new immutable NoteList, atomic_exchanges the
 *    live pointer, and pushes the displaced pointer onto a UI-thread
 *    trash deque drained on AsyncUpdater (or any message-thread tick).
 *    Audio thread atomic_loads once per block and uses the snapshot
 *    for the entire render.
 *
 *  Why leaked-pointer + trash-bin instead of std::atomic<shared_ptr>:
 *  libstdc++ implements atomic<shared_ptr> with an internal spinlock
 *  -- not wait-free and not PI-aware on PREEMPT_RT.  Raw atomic ptr
 *  swap + deferred reclaim is wait-free for the audio reader and
 *  alloc-free for everyone except the UI-thread edit/sweep path.
 *
 *  Phase 2 status: data type only.  The audio thread does NOT yet
 *  consume MidiNoteRegion snapshots (Phase 3 piano-roll lands the
 *  playback path).  The snapshot infrastructure is in place now so
 *  Phase 3 doesn't refactor the storage shape -- just adds the read
 *  side. */
class MidiNoteRegion
{
public:
    using NoteList = std::vector<MidiNote>;

    /** Construct an empty region.  An empty NoteList snapshot is
     *  allocated up-front so the audio thread never observes a null
     *  active-notes pointer. */
    MidiNoteRegion();

    /** Destructor.  Frees the live snapshot + drains the trash deque.
     *  Caller's responsibility to ensure no audio thread is reading
     *  this region at destruction time (e.g. remove from the Playlist
     *  on the message thread before destroying). */
    ~MidiNoteRegion();

    MidiNoteRegion (const MidiNoteRegion&)            = delete;
    MidiNoteRegion& operator= (const MidiNoteRegion&) = delete;

    /** Deep-clone -- builds a fresh MidiNoteRegion with the same id,
     *  placement, and active note snapshot.  The clone has its own
     *  atomic snapshot pointer + epoch counter + empty trash deque;
     *  publishing or sweeping on one does not affect the other.
     *  Message-thread only.  Used by Playlist's copy-construction
     *  path (juce::Array<Lane> undo snapshots in ArrangementView). */
    std::unique_ptr<MidiNoteRegion> clone() const;

    //==========================================================================
    // Identity + timeline placement.  Message-thread mutated; audio thread
    // reads as plain copies on its snapshot tick.  Mirrors the Region struct
    // shape so callers can pattern-match across kinds (paint/select/move
    // share a code path on the UI side).

    juce::Uuid    id;

    /** Optional pointer at a MidiSource (services/sources/midi_source.hpp)
     *  the region was imported from.  Null Uuid == authored in-place
     *  (Phase 3 piano-roll edits will create regions without a source).
     *  Recorded-MIDI regions (deferred to a later phase) will also
     *  point at a MidiSource.
     *
     *  Default-init explicitly to the null Uuid -- juce::Uuid's
     *  default ctor would otherwise generate a fresh random uuid,
     *  which here would falsely imply a source on every new region. */
    juce::Uuid    sourceId { juce::Uuid::null() };
    double        positionBeats { 0.0 };
    double        lengthBeats   { 0.0 };

    /** Offset INTO the source note list at which playback begins, in
     *  beats.  Mirrors Region::startBeats (the audio kind).  Default 0
     *  means "play from the beginning of the source", which matches
     *  pre-Q5 behaviour.
     *
     *  Left-edge trim advances startBeats by the trim delta INSTEAD of
     *  rewriting the note list -- so notes hidden by the trim survive
     *  the gesture and reappear if the user drags the left edge back.
     *  Notes whose onBeat falls outside [startBeats, startBeats +
     *  lengthBeats) are PRESERVED but never played; the audio thread
     *  uses startBeats to gate the emit window. */
    double        startBeats    { 0.0 };

    bool          looped        { false };

    /** Loop period in beats for the audio thread's modulo-into-source
     *  wrap.  Mirrors how the audio kind's PlaybackDS wraps on the
     *  underlying source file length rather than on the region's
     *  drawn duration.
     *
     *  Default 0.0 means "use lengthBeats" -- preserves pre-fix
     *  sessions that didn't carry this field.  When the user toggles
     *  Loop on, ArrangementView snapshots `loopLengthBeats =
     *  lengthBeats` at that moment so subsequent right-edge drags
     *  extend the region (number of repeats) without stretching the
     *  loop itself.  Sparse-write -- omitted when 0.
     *
     *  Audio-thread access is via the immutable RegionEntry; no torn
     *  reads. */
    double        loopLengthBeats { 0.0 };

    juce::Colour  colour        { 0xff'5a'8a'5a }; // muted green; distinct from audio (blue) + tracker (orange)
    juce::String  name;

    //==========================================================================
    // Audio-thread-safe snapshot interface.  Phase 2 has no audio-thread
    // consumer; Phase 3 piano-roll wires playback through these.

    /** Load the currently-active immutable NoteList snapshot.  Audio
     *  thread holds the returned pointer for the duration of one render
     *  block; releases by simply dropping the local copy.  Lifetime
     *  guaranteed by the epoch-counter trash discipline -- see
     *  advanceAudioEpoch() + sweepTrash().  Never returns null after
     *  construction (the ctor publishes an empty list up front). */
    const NoteList* loadSnapshot() const noexcept
    {
        return activeNotes_.load (std::memory_order_acquire);
    }

    /** Audio thread: call once at the START of each render block in
     *  which this region might be sampled.  Advances the internal
     *  epoch counter, telling subsequent sweepTrash() calls that any
     *  trash items stamped at OR BEFORE the prior epoch are safe to
     *  reclaim -- the audio thread has loaded a fresh snapshot since.
     *  Lock-free: single atomic fetch_add per block. */
    void advanceAudioEpoch() noexcept
    {
        audioEpoch_.fetch_add (1, std::memory_order_acq_rel);
    }

    /** Returns the number of notes in the active snapshot.  Wait-free.
     *  Cheap UI helper for the arrangement-view note-count badge; the
     *  paint code in ArrangementView calls this once per repaint. */
    std::size_t noteCount() const noexcept
    {
        const auto* snap = activeNotes_.load (std::memory_order_acquire);
        return (snap != nullptr) ? snap->size() : 0;
    }

    //==========================================================================
    // UI-thread mutators.  All build a new NoteList and publish via
    // publishSnapshot().  Audio thread sees the swap atomically.

    /** Replace the entire note list.  The input vector is sorted by
     *  (onBeat, pitch) ascending; callers can pass an unsorted vector
     *  and this method will sort the local copy before publishing.
     *  Sorting by (onBeat, pitch) gives a stable ordering that the
     *  paint code can rely on without re-sorting per frame. */
    void setNotes (NoteList newNotes);

    /** Append a note + re-sort + publish.  If `n.id` is 0 the region
     *  stamps a fresh monotonic id before insertion; otherwise the
     *  supplied id is preserved (allows bulk insert paths -- SMF
     *  import, undo redo -- to pre-assign ids and have them survive
     *  the publish).  Returns the final id of the inserted note. */
    std::uint64_t addNote (MidiNote n);

    /** Remove notes whose (pitch, channel, onBeat-within-eps) match
     *  the example.  Used by tests and legacy callers; the piano-roll
     *  edit path prefers id-keyed removal (removeNoteById) so
     *  selection state survives mutation. */
    void removeNotesMatching (const MidiNote& example) noexcept;

    /** Remove a single note by stable id.  Returns true if a note was
     *  removed.  Used by the piano-roll edit gestures (delete key,
     *  marquee-erase, undo redo). */
    bool removeNoteById (std::uint64_t noteId) noexcept;

    /** Replace a single note's mutable fields (pitch / velocity /
     *  channel / onBeat / lengthBeats) by stable id.  Used by the
     *  piano-roll Move / Resize / Velocity gestures so each edit
     *  publishes a fresh snapshot with the same note ids.  Returns
     *  true if a note with this id was found. */
    bool updateNoteById (std::uint64_t noteId, const MidiNote& replacement) noexcept;

    /** Bulk replace -- like setNotes but stamps fresh ids for entries
     *  with id == 0.  Preferred over setNotes for callers that want
     *  the region's id allocator to take responsibility for filling
     *  in missing ids (SMF import path uses this). */
    void setNotesAssigningIds (NoteList newNotes);

    /** Allocator hook -- next monotonic id.  Used by piano-roll edit
     *  paths that want to pre-stamp ids for undo before publishing. */
    std::uint64_t nextNoteId() noexcept;

    //==========================================================================
    // Clip-local MIDI CC automation lanes.  Parallel COW snapshot to the
    // note list (own atomic ptr + trash deque, sharing audioEpoch_).  The
    // MidiPlayerNode emits these alongside notes; the piano-roll CC lane
    // editor mutates them.  Reuses dsp::automation curve points so the
    // editor shares the timeline-overlay curve paint/eval code.

    using CcLaneList = std::vector<MidiCcLane>;

    /** Load the active immutable CC-lane snapshot.  Audio thread holds it
     *  for one render block.  Never null after construction. */
    const CcLaneList* loadCcSnapshot() const noexcept
    {
        return activeCc_.load (std::memory_order_acquire);
    }

    /** Replace the entire CC-lane set (each lane's points sorted by
     *  tBeats).  Message-thread mutator; publishes a fresh snapshot. */
    void setCcLanes (CcLaneList lanes);

    /** Upsert one (ccNumber, channel) lane's point list -- replaces the
     *  matching lane in place, or appends a new lane.  Empty `points`
     *  removes the lane.  This is the piano-roll CC editor's primary
     *  mutate path (one lane edited at a time). */
    void setCcLane (int ccNumber, int channel, MidiCcLane::PointList points);

    /** Remove the (ccNumber, channel) lane, if present. */
    void removeCcLane (int ccNumber, int channel) noexcept;

    /** Param-target counterparts of setCcLane / removeCcLane (#11): upsert
     *  / remove the clip-local lane that drives node `nodeId`'s parameter
     *  `paramId`.  Identity is (nodeId, paramId); a param lane never
     *  collides with a CC lane.  Empty `points` removes the lane.  Message
     *  thread only -- same COW publish discipline as the CC variants. */
    void setParamLane (const juce::Uuid& nodeId, const juce::String& paramId,
                       MidiCcLane::PointList points);
    void removeParamLane (const juce::Uuid& nodeId, const juce::String& paramId) noexcept;

    /** Drain the trash deque.  Called on the message thread by the
     *  arrangement view's AsyncUpdater tick (Phase 4 wiring) or by
     *  tests directly.  Safe to call from any thread that isn't the
     *  audio thread.  Idempotent.  Drains BOTH the note and CC trash. */
    void sweepTrash() noexcept;

    //==========================================================================
    // ValueTree persistence -- sparse-write per the Element convention
    // (only attributes that differ from defaults are emitted).  Mirrors
    // Region::toValueTree's shape so the session XML stays uniform
    // across region kinds.

    juce::ValueTree toValueTree() const;
    static std::unique_ptr<MidiNoteRegion> fromValueTree (const juce::ValueTree&);

private:
    /** Atomic-exchange a new NoteList into activeNotes_ and push the
     *  displaced pointer onto trash_ for deferred reclaim.  Called only
     *  from UI / message thread. */
    void publishSnapshot (std::unique_ptr<NoteList> newSnap);

    /** Publish a new snapshot constructed via the in-place lambda.
     *  Snapshot the active list, hand a mutable copy to `mutate`, then
     *  publish.  The lambda runs on the UI thread.  Convenience for
     *  addNote / removeNotesMatching. */
    template <typename Mutator>
    void mutateAndPublish (Mutator&& mutate);

    /** Live snapshot pointer.  ALWAYS non-null after construction;
     *  ctor publishes an empty NoteList up front so the audio thread
     *  never observes a null and never has to null-check. */
    std::atomic<const NoteList*> activeNotes_ { nullptr };

    /** Trash entry: displaced snapshot + the audioEpoch_ value at the
     *  moment of publish.  Reclaimed only when audioEpoch_ has STRICTLY
     *  advanced past this stamp. */
    struct TrashEntry
    {
        std::unique_ptr<const NoteList> ptr;
        std::uint64_t                   stampEpoch;
    };

    /** UI-thread-owned deque of displaced snapshots awaiting reclaim. */
    std::deque<TrashEntry> trash_;

    /** Live CC-lane snapshot (parallel to activeNotes_).  Non-null after
     *  ctor; published via raw-ptr atomic swap, trash drained under the
     *  same epoch gate as the note list. */
    std::atomic<const CcLaneList*> activeCc_ { nullptr };

    struct CcTrashEntry
    {
        std::unique_ptr<const CcLaneList> ptr;
        std::uint64_t                     stampEpoch;
    };
    std::deque<CcTrashEntry> ccTrash_;

    /** Publish a new CC-lane snapshot; old goes to ccTrash_ stamped at the
     *  current epoch. */
    void publishCcSnapshot (std::unique_ptr<CcLaneList> newSnap);

    template <typename Mutator>
    void mutateCcAndPublish (Mutator&& mutate);

    /** Per-region epoch counter.  Audio thread advances at block start. */
    std::atomic<std::uint64_t> audioEpoch_ { 0 };

    /** Per-region monotonic note-id allocator.  Starts at 1 (0 is the
     *  "unassigned" sentinel) and advances each time the region needs
     *  to stamp an id.  Lives on the message thread; never accessed
     *  from the audio thread. */
    std::uint64_t noteIdAllocator_ { 0 };
};

} // namespace element
