// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/midi_note_region.hpp"

#include <algorithm>
#include <cmath>

namespace element {

namespace {

/* ValueTree property identifiers for sparse-write serialisation.
 * Short attribute names keep round-tripped XML compact even with
 * hundreds of notes per region. */
const juce::Identifier kIdAttr        { "id" };
const juce::Identifier kSourceAttr    { "src" };
const juce::Identifier kPositionAttr  { "pos" };
const juce::Identifier kLengthAttr    { "len" };
const juce::Identifier kStartAttr     { "start" };
const juce::Identifier kLoopedAttr    { "loop" };
const juce::Identifier kLoopLenAttr   { "loopLen" };
const juce::Identifier kColourAttr    { "col" };
const juce::Identifier kNameAttr      { "name" };
const juce::Identifier kNoteTag       { "n" };
const juce::Identifier kNoteIdAttr    { "i" };
const juce::Identifier kNotePitchAttr { "p" };
const juce::Identifier kNoteVelAttr   { "v" };
const juce::Identifier kNoteChanAttr  { "ch" };
const juce::Identifier kNoteOnAttr    { "t" };
const juce::Identifier kNoteLenAttr   { "l" };
const juce::Identifier kMidiNoteRegionTag { "midiNoteRegion" };

/* Threshold for "matching" floating-point coordinates in
 * removeNotesMatching.  Notes are user-placed so the natural epsilon
 * is well above double precision; 1e-6 beats covers any UI-grid
 * quantisation we will see in practice. */
constexpr double kNoteMatchEps = 1e-6;

/* Sort comparator -- (onBeat, pitch) ascending.  Stable ordering used
 * by both setNotes and the audio-thread paint loop. */
inline bool noteLess (const MidiNote& a, const MidiNote& b) noexcept
{
    if (a.onBeat != b.onBeat) return a.onBeat < b.onBeat;
    return a.pitch < b.pitch;
}

} // namespace

//==============================================================================

MidiNoteRegion::MidiNoteRegion()
{
    /* Publish an empty snapshot up front so the audio thread never
     * observes a null active-notes pointer.  Ownership: the live
     * snapshot is owned by the region until either replaced (old
     * goes to trash_) or the region is destroyed. */
    activeNotes_.store (new NoteList(), std::memory_order_release);
    activeCc_.store (new CcLaneList(), std::memory_order_release);
}

MidiNoteRegion::~MidiNoteRegion()
{
    /* Reclaim the live snapshots.  trash_ / ccTrash_ unique_ptrs reclaim
     * themselves at deque destruction. */
    if (auto* live = activeNotes_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
    if (auto* liveCc = activeCc_.exchange (nullptr, std::memory_order_acq_rel))
        delete liveCc;
}

std::unique_ptr<MidiNoteRegion> MidiNoteRegion::clone() const
{
    auto out = std::make_unique<MidiNoteRegion>();
    out->id            = id;
    out->sourceId      = sourceId;
    out->positionBeats = positionBeats;
    out->lengthBeats   = lengthBeats;
    out->startBeats    = startBeats;
    out->looped        = looped;
    out->loopLengthBeats = loopLengthBeats;
    out->colour        = colour;
    out->name          = name;

    if (const auto* snap = loadSnapshot())
        out->setNotes (*snap);    /* setNotes makes its own copy + sorts */
    if (const auto* ccSnap = loadCcSnapshot())
        out->setCcLanes (*ccSnap); /* copies the lane vector */
    return out;
}

//==============================================================================

void MidiNoteRegion::setNotes (NoteList newNotes)
{
    std::sort (newNotes.begin(), newNotes.end(), noteLess);
    auto snap = std::make_unique<NoteList> (std::move (newNotes));
    publishSnapshot (std::move (snap));
}

void MidiNoteRegion::setNotesAssigningIds (NoteList newNotes)
{
    /* Stamp fresh ids for any note with id == 0.  Preserve existing
     * ids so callers that pre-assigned (e.g. undo replay) get
     * round-trip stability.  Bumps noteIdAllocator_ past every
     * pre-assigned id to keep the monotonic invariant. */
    for (auto& n : newNotes)
    {
        if (n.id == 0)
            n.id = nextNoteId();
        else if (n.id > noteIdAllocator_)
            noteIdAllocator_ = n.id;
    }
    setNotes (std::move (newNotes));
}

std::uint64_t MidiNoteRegion::addNote (MidiNote n)
{
    if (n.id == 0)
        n.id = nextNoteId();
    else if (n.id > noteIdAllocator_)
        noteIdAllocator_ = n.id;

    const std::uint64_t finalId = n.id;

    mutateAndPublish ([&] (NoteList& copy)
    {
        copy.push_back (n);
        std::sort (copy.begin(), copy.end(), noteLess);
    });

    return finalId;
}

void MidiNoteRegion::removeNotesMatching (const MidiNote& example) noexcept
{
    mutateAndPublish ([&] (NoteList& copy)
    {
        copy.erase (std::remove_if (copy.begin(), copy.end(),
                       [&] (const MidiNote& nx) noexcept
                       {
                           return nx.pitch   == example.pitch
                               && nx.channel == example.channel
                               && std::abs (nx.onBeat - example.onBeat) < kNoteMatchEps;
                       }),
                    copy.end());
    });
}

bool MidiNoteRegion::removeNoteById (std::uint64_t noteId) noexcept
{
    if (noteId == 0) return false;
    bool removed = false;
    mutateAndPublish ([&] (NoteList& copy)
    {
        auto it = std::find_if (copy.begin(), copy.end(),
            [noteId] (const MidiNote& n) noexcept { return n.id == noteId; });
        if (it != copy.end())
        {
            copy.erase (it);
            removed = true;
        }
    });
    return removed;
}

bool MidiNoteRegion::updateNoteById (std::uint64_t noteId, const MidiNote& replacement) noexcept
{
    if (noteId == 0) return false;
    bool updated = false;
    mutateAndPublish ([&] (NoteList& copy)
    {
        for (auto& n : copy)
        {
            if (n.id == noteId)
            {
                /* Preserve the id (caller's replacement may have a
                 * stale or zero id from a temporary).  All other
                 * mutable fields take from replacement. */
                n.pitch       = replacement.pitch;
                n.velocity    = replacement.velocity;
                n.channel     = replacement.channel;
                n.onBeat      = replacement.onBeat;
                n.lengthBeats = replacement.lengthBeats;
                updated = true;
                break;
            }
        }
        if (updated)
            std::sort (copy.begin(), copy.end(), noteLess);
    });
    return updated;
}

std::uint64_t MidiNoteRegion::nextNoteId() noexcept
{
    return ++noteIdAllocator_;
}

void MidiNoteRegion::sweepTrash() noexcept
{
    /* Message-thread reclaim with epoch-guarded grace period.  An entry
     * is safe to free only once audioEpoch_ has STRICTLY advanced past
     * the stamp at publish time -- that means the audio thread loaded
     * at least one new snapshot since the publish, so the old pointer
     * cannot still be in flight on the audio path. */
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);
    while (! trash_.empty() && trash_.front().stampEpoch < safeEpoch)
        trash_.pop_front();
    while (! ccTrash_.empty() && ccTrash_.front().stampEpoch < safeEpoch)
        ccTrash_.pop_front();
}

//==============================================================================

namespace {
/* CC lanes are sorted by (ccNumber, channel) so identity lookup +
 * persistence order are stable. */
inline bool ccLaneLess (const MidiCcLane& a, const MidiCcLane& b) noexcept
{
    if (a.ccNumber != b.ccNumber) return a.ccNumber < b.ccNumber;
    return a.channel < b.channel;
}

inline void sortLanePoints (MidiCcLane& lane) noexcept
{
    std::sort (lane.points.begin(), lane.points.end(),
               [] (const dsp::automation::AutomationPoint& a,
                   const dsp::automation::AutomationPoint& b) noexcept
               { return a.tBeats < b.tBeats; });
}
} // namespace

void MidiNoteRegion::setCcLanes (CcLaneList lanes)
{
    for (auto& l : lanes)
        sortLanePoints (l);
    std::sort (lanes.begin(), lanes.end(), ccLaneLess);
    publishCcSnapshot (std::make_unique<CcLaneList> (std::move (lanes)));
}

void MidiNoteRegion::setCcLane (int ccNumber, int channel, MidiCcLane::PointList points)
{
    mutateCcAndPublish ([&] (CcLaneList& copy)
    {
        auto it = std::find_if (copy.begin(), copy.end(),
            [&] (const MidiCcLane& l) noexcept
            { return l.ccNumber == ccNumber && l.channel == channel; });

        if (points.empty())
        {
            /* Empty point set removes the lane entirely. */
            if (it != copy.end())
                copy.erase (it);
            return;
        }

        std::sort (points.begin(), points.end(),
                   [] (const dsp::automation::AutomationPoint& a,
                       const dsp::automation::AutomationPoint& b) noexcept
                   { return a.tBeats < b.tBeats; });

        if (it != copy.end())
        {
            it->points = std::move (points);
        }
        else
        {
            MidiCcLane lane;
            lane.ccNumber = ccNumber;
            lane.channel  = channel;
            lane.points   = std::move (points);
            copy.push_back (std::move (lane));
            std::sort (copy.begin(), copy.end(), ccLaneLess);
        }
    });
}

void MidiNoteRegion::removeCcLane (int ccNumber, int channel) noexcept
{
    mutateCcAndPublish ([&] (CcLaneList& copy)
    {
        copy.erase (std::remove_if (copy.begin(), copy.end(),
                       [&] (const MidiCcLane& l) noexcept
                       { return l.ccNumber == ccNumber && l.channel == channel; }),
                    copy.end());
    });
}

void MidiNoteRegion::publishCcSnapshot (std::unique_ptr<CcLaneList> newSnap)
{
    const CcLaneList* raw   = newSnap.release();
    const auto        stamp = audioEpoch_.load (std::memory_order_acquire);
    const CcLaneList* old   = activeCc_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        ccTrash_.push_back (CcTrashEntry { std::unique_ptr<const CcLaneList> (old), stamp });
}

template <typename Mutator>
void MidiNoteRegion::mutateCcAndPublish (Mutator&& mutate)
{
    const auto* live = activeCc_.load (std::memory_order_acquire);
    auto next = (live != nullptr)
                  ? std::make_unique<CcLaneList> (*live)
                  : std::make_unique<CcLaneList>();
    mutate (*next);
    publishCcSnapshot (std::move (next));
}

//==============================================================================

void MidiNoteRegion::publishSnapshot (std::unique_ptr<NoteList> newSnap)
{
    const NoteList* raw   = newSnap.release();
    const auto      stamp = audioEpoch_.load (std::memory_order_acquire);
    const NoteList* old   = activeNotes_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        trash_.push_back (TrashEntry { std::unique_ptr<const NoteList> (old), stamp });
}

template <typename Mutator>
void MidiNoteRegion::mutateAndPublish (Mutator&& mutate)
{
    const auto* live = activeNotes_.load (std::memory_order_acquire);
    auto next = (live != nullptr)
                  ? std::make_unique<NoteList> (*live)
                  : std::make_unique<NoteList>();
    mutate (*next);
    publishSnapshot (std::move (next));
}

//==============================================================================

juce::ValueTree MidiNoteRegion::toValueTree() const
{
    juce::ValueTree v (kMidiNoteRegionTag);
    v.setProperty (kIdAttr, id.toString(), nullptr);
    if (! sourceId.isNull())
        v.setProperty (kSourceAttr, sourceId.toString(), nullptr);
    if (positionBeats != 0.0) v.setProperty (kPositionAttr, positionBeats, nullptr);
    if (lengthBeats   != 0.0) v.setProperty (kLengthAttr,   lengthBeats,   nullptr);
    if (startBeats    != 0.0) v.setProperty (kStartAttr,    startBeats,    nullptr);
    if (looped)               v.setProperty (kLoopedAttr,   true,          nullptr);
    /* Sparse-write loop period: 0 means "use lengthBeats" (the
     * pre-fix default), so older sessions stay byte-identical and
     * fresh non-looped regions don't carry it. */
    if (loopLengthBeats != 0.0)
        v.setProperty (kLoopLenAttr, loopLengthBeats, nullptr);
    /* Default colour matches the ctor; only persist when overridden. */
    if (colour.getARGB() != juce::Colour (0xff'5a'8a'5a).getARGB())
        v.setProperty (kColourAttr, (juce::int64) colour.getARGB(), nullptr);
    if (name.isNotEmpty()) v.setProperty (kNameAttr, name, nullptr);

    if (const auto* snap = loadSnapshot())
    {
        for (const auto& n : *snap)
        {
            juce::ValueTree nv (kNoteTag);
            if (n.id != 0)
                nv.setProperty (kNoteIdAttr, (juce::int64) n.id, nullptr);
            nv.setProperty (kNotePitchAttr, n.pitch,       nullptr);
            nv.setProperty (kNoteVelAttr,   n.velocity,    nullptr);
            /* Channel 1 is the default; sparse-write skips it. */
            if (n.channel != 1)
                nv.setProperty (kNoteChanAttr, n.channel, nullptr);
            nv.setProperty (kNoteOnAttr,    n.onBeat,      nullptr);
            nv.setProperty (kNoteLenAttr,   n.lengthBeats, nullptr);
            v.appendChild (nv, nullptr);
        }
    }

    /* CC lanes serialize as <cc> children alongside the <n> note
     * children (MidiCcLane owns its own sparse-write); fromValueTree
     * discriminates by tag.  Additive: older readers ignore <cc>. */
    if (const auto* ccSnap = loadCcSnapshot())
        for (const auto& lane : *ccSnap)
            v.appendChild (lane.toValueTree(), nullptr);

    return v;
}

std::unique_ptr<MidiNoteRegion> MidiNoteRegion::fromValueTree (const juce::ValueTree& v)
{
    if (! v.hasType (kMidiNoteRegionTag))
        return nullptr;

    auto r = std::make_unique<MidiNoteRegion>();
    r->id = juce::Uuid (v.getProperty (kIdAttr).toString());
    if (v.hasProperty (kSourceAttr))
        r->sourceId = juce::Uuid (v.getProperty (kSourceAttr).toString());
    r->positionBeats = (double) v.getProperty (kPositionAttr, 0.0);
    r->lengthBeats   = (double) v.getProperty (kLengthAttr,   0.0);
    r->startBeats    = (double) v.getProperty (kStartAttr,    0.0);
    r->looped        = (bool)   v.getProperty (kLoopedAttr,   false);
    r->loopLengthBeats = (double) v.getProperty (kLoopLenAttr, 0.0);
    /* Migration for pre-loopLengthBeats sessions: a region saved as
     * looped without loopLengthBeats used to use lengthBeats as the
     * period.  Lock that period in now so a future right-edge drag
     * doesn't silently change the loop behaviour. */
    if (r->looped && r->loopLengthBeats == 0.0 && r->lengthBeats > 0.0)
        r->loopLengthBeats = r->lengthBeats;
    if (v.hasProperty (kColourAttr))
        r->colour = juce::Colour ((juce::uint32) (juce::int64) v.getProperty (kColourAttr));
    r->name          = v.getProperty (kNameAttr, juce::String()).toString();

    NoteList notes;
    notes.reserve ((size_t) v.getNumChildren());
    CcLaneList ccLanes;
    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        auto nv = v.getChild (i);
        if (nv.hasType (kNoteTag))
        {
            MidiNote n;
            n.id          = (std::uint64_t) (juce::int64) nv.getProperty (kNoteIdAttr, (juce::int64) 0);
            n.pitch       = (int)    nv.getProperty (kNotePitchAttr, 60);
            n.velocity    = (int)    nv.getProperty (kNoteVelAttr,   100);
            n.channel     = (int)    nv.getProperty (kNoteChanAttr,  1);
            n.onBeat      = (double) nv.getProperty (kNoteOnAttr,    0.0);
            n.lengthBeats = (double) nv.getProperty (kNoteLenAttr,   0.25);
            notes.push_back (n);
        }
        else if (nv.hasType (juce::Identifier ("cc")))
        {
            ccLanes.push_back (MidiCcLane::fromValueTree (nv));
        }
    }
    /* setNotesAssigningIds: pre-1.0 sessions without note ids get
     * fresh monotonic ids stamped; sessions with ids preserve them
     * AND bump the allocator past the maximum so subsequent
     * piano-roll edits stay unique. */
    r->setNotesAssigningIds (std::move (notes));
    if (! ccLanes.empty())
        r->setCcLanes (std::move (ccLanes));
    return r;
}

} // namespace element
