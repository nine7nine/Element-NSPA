// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/timeline/midi_note.hpp"
#include "services/timeline/midi_note_region.hpp"

#include <element/juce/core.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include <thread>

using element::MidiNote;
using element::MidiNoteRegion;

namespace {

inline bool nearly (double a, double b, double tol = 1e-9)
{
    return std::abs (a - b) <= tol;
}

MidiNote makeNote (int pitch, double onBeat, double lengthBeats = 0.25,
                   int velocity = 100, int channel = 1)
{
    MidiNote n;
    n.pitch       = pitch;
    n.onBeat      = onBeat;
    n.lengthBeats = lengthBeats;
    n.velocity    = velocity;
    n.channel     = channel;
    return n;
}

} // namespace

BOOST_AUTO_TEST_SUITE (MidiNoteRegionTests)

/* ---------- Construction + empty-state invariants ---------- */

BOOST_AUTO_TEST_CASE (default_constructed_region_publishes_empty_snapshot)
{
    /* The ctor must publish an empty snapshot up front so the audio
     * thread never observes a null active-notes pointer.  This mirrors
     * AutomationRegion's invariant and is load-bearing for the wait-
     * free audio-thread read path. */
    MidiNoteRegion r;
    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE (snap != nullptr);
    BOOST_CHECK_EQUAL (snap->size(), (size_t) 0);
    BOOST_CHECK_EQUAL (r.noteCount(), (size_t) 0);
}

BOOST_AUTO_TEST_CASE (default_fields_match_documented_defaults)
{
    MidiNoteRegion r;
    BOOST_CHECK_EQUAL (r.positionBeats, 0.0);
    BOOST_CHECK_EQUAL (r.lengthBeats,   0.0);
    BOOST_CHECK_EQUAL (r.startBeats,    0.0);
    BOOST_CHECK (! r.looped);
    BOOST_CHECK (r.sourceId.isNull());
}

BOOST_AUTO_TEST_CASE (start_beats_round_trips_through_value_tree)
{
    MidiNoteRegion r;
    r.id            = juce::Uuid();
    r.positionBeats = 4.0;
    r.lengthBeats   = 2.0;
    r.startBeats    = 0.75;
    r.setNotes ({ makeNote (60, 0.0), makeNote (62, 1.0), makeNote (64, 1.5) });

    const auto vt = r.toValueTree();
    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK_EQUAL (restored->startBeats,    0.75);
    BOOST_CHECK_EQUAL (restored->positionBeats, 4.0);
    BOOST_CHECK_EQUAL (restored->lengthBeats,   2.0);
    BOOST_CHECK_EQUAL (restored->noteCount(),   (size_t) 3);
}

BOOST_AUTO_TEST_CASE (start_beats_omitted_when_zero)
{
    /* Sparse-write contract: default startBeats stays out of the
     * serialised tree so older sessions and freshly authored regions
     * don't bloat. */
    MidiNoteRegion r;
    r.id            = juce::Uuid();
    r.positionBeats = 1.0;
    r.lengthBeats   = 1.0;
    const auto vt = r.toValueTree();
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("start")));
}

BOOST_AUTO_TEST_CASE (clone_propagates_start_beats)
{
    MidiNoteRegion r;
    r.startBeats = 1.25;
    auto c = r.clone();
    BOOST_REQUIRE (c != nullptr);
    BOOST_CHECK_EQUAL (c->startBeats, 1.25);
}

/* ---------- loopLengthBeats (A.1 fix: MIDI loop period decoupled
 *            from drawn length so right-edge drag extends the
 *            number of repeats rather than stretching the loop) */

BOOST_AUTO_TEST_CASE (loop_length_beats_defaults_to_zero)
{
    MidiNoteRegion r;
    BOOST_CHECK_EQUAL (r.loopLengthBeats, 0.0);
}

BOOST_AUTO_TEST_CASE (loop_length_beats_round_trips_through_value_tree)
{
    MidiNoteRegion r;
    r.id              = juce::Uuid();
    r.lengthBeats     = 8.0;
    r.looped          = true;
    r.loopLengthBeats = 2.0;

    const auto vt = r.toValueTree();
    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK_EQUAL (restored->loopLengthBeats, 2.0);
    BOOST_CHECK_EQUAL (restored->lengthBeats,     8.0);
    BOOST_CHECK       (restored->looped);
}

BOOST_AUTO_TEST_CASE (loop_length_beats_omitted_when_zero)
{
    MidiNoteRegion r;
    r.id          = juce::Uuid();
    r.lengthBeats = 4.0;
    /* loopLengthBeats default 0 -- sparse-write should skip it so
     * pre-fix sessions stay byte-identical. */
    const auto vt = r.toValueTree();
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("loopLen")));
}

BOOST_AUTO_TEST_CASE (clone_propagates_loop_length_beats)
{
    MidiNoteRegion r;
    r.looped          = true;
    r.loopLengthBeats = 3.0;
    auto c = r.clone();
    BOOST_REQUIRE (c != nullptr);
    BOOST_CHECK_EQUAL (c->loopLengthBeats, 3.0);
    BOOST_CHECK       (c->looped);
}

BOOST_AUTO_TEST_CASE (loaded_looped_region_without_loop_len_migrates_to_length)
{
    /* Pre-fix sessions carry looped=true but no loopLen attribute.
     * The audio thread used lengthBeats as the loop period back then;
     * fromValueTree locks that period in by setting loopLengthBeats
     * to the loaded lengthBeats, so a subsequent right-edge drag in
     * the new build doesn't silently change the playback behaviour. */
    juce::ValueTree vt (juce::Identifier ("midiNoteRegion"));
    vt.setProperty (juce::Identifier ("id"),   juce::Uuid().toString(), nullptr);
    vt.setProperty (juce::Identifier ("len"),  4.0,                       nullptr);
    vt.setProperty (juce::Identifier ("loop"), true,                      nullptr);
    /* deliberately NO "loopLen" property -- simulating a pre-fix file */

    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (restored->looped);
    BOOST_CHECK_EQUAL (restored->loopLengthBeats, 4.0);
}

BOOST_AUTO_TEST_CASE (non_looped_region_does_not_auto_migrate_loop_length)
{
    /* Sanity: a non-looped legacy region must NOT acquire a loop
     * length on load.  The migration is gated on looped=true. */
    juce::ValueTree vt (juce::Identifier ("midiNoteRegion"));
    vt.setProperty (juce::Identifier ("id"),  juce::Uuid().toString(), nullptr);
    vt.setProperty (juce::Identifier ("len"), 4.0,                       nullptr);
    /* No loop, no loopLen */

    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (! restored->looped);
    BOOST_CHECK_EQUAL (restored->loopLengthBeats, 0.0);
}

/* ---------- setNotes ---------- */

BOOST_AUTO_TEST_CASE (set_notes_publishes_snapshot)
{
    MidiNoteRegion r;
    MidiNoteRegion::NoteList notes {
        makeNote (60, 0.0),
        makeNote (62, 1.0),
        makeNote (64, 2.0)
    };
    r.setNotes (notes);

    BOOST_CHECK_EQUAL (r.noteCount(), (size_t) 3);
    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE (snap != nullptr);
    BOOST_CHECK_EQUAL ((*snap)[0].pitch, 60);
    BOOST_CHECK_EQUAL ((*snap)[1].pitch, 62);
    BOOST_CHECK_EQUAL ((*snap)[2].pitch, 64);
}

BOOST_AUTO_TEST_CASE (set_notes_sorts_unordered_input)
{
    MidiNoteRegion r;
    MidiNoteRegion::NoteList notes {
        makeNote (60, 2.0),
        makeNote (62, 0.0),
        makeNote (64, 1.0)
    };
    r.setNotes (notes);

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE (snap != nullptr);
    BOOST_CHECK (nearly ((*snap)[0].onBeat, 0.0));
    BOOST_CHECK (nearly ((*snap)[1].onBeat, 1.0));
    BOOST_CHECK (nearly ((*snap)[2].onBeat, 2.0));
}

BOOST_AUTO_TEST_CASE (set_notes_sort_breaks_ties_by_pitch)
{
    /* Notes at the same onBeat must be deterministically ordered by
     * pitch so the paint code can rely on a stable iteration order. */
    MidiNoteRegion r;
    MidiNoteRegion::NoteList notes {
        makeNote (72, 1.0),
        makeNote (60, 1.0),
        makeNote (66, 1.0)
    };
    r.setNotes (notes);

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 3);
    BOOST_CHECK_EQUAL ((*snap)[0].pitch, 60);
    BOOST_CHECK_EQUAL ((*snap)[1].pitch, 66);
    BOOST_CHECK_EQUAL ((*snap)[2].pitch, 72);
}

/* ---------- addNote ---------- */

BOOST_AUTO_TEST_CASE (add_note_appends_and_resorts)
{
    MidiNoteRegion r;
    r.setNotes ({ makeNote (60, 0.0), makeNote (64, 2.0) });
    r.addNote (makeNote (62, 1.0));

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 3);
    BOOST_CHECK_EQUAL ((*snap)[0].pitch, 60);
    BOOST_CHECK_EQUAL ((*snap)[1].pitch, 62);
    BOOST_CHECK_EQUAL ((*snap)[2].pitch, 64);
}

BOOST_AUTO_TEST_CASE (add_note_does_not_mutate_prior_snapshot)
{
    /* COW invariant: the snapshot captured before an edit must not be
     * mutated by the edit.  Equivalent to the AutomationRegion COW
     * test -- ensures readers holding an older snapshot still see the
     * old data, not the new. */
    MidiNoteRegion r;
    r.setNotes ({ makeNote (60, 0.0) });
    const auto* before = r.loadSnapshot();
    const auto  size0  = before->size();

    r.addNote (makeNote (64, 1.0));

    /* The captured `before` snapshot still has the original size.
     * Note: by Phase 2 design the audio thread holds its captured
     * pointer for one render block; this test simulates that hold. */
    BOOST_CHECK_EQUAL (before->size(), size0);
    BOOST_CHECK_EQUAL (r.noteCount(), (size_t) 2);
}

/* ---------- removeNotesMatching ---------- */

BOOST_AUTO_TEST_CASE (remove_notes_matching_strips_by_pitch_channel_onbeat)
{
    MidiNoteRegion r;
    r.setNotes ({
        makeNote (60, 0.0, 0.25, 100, 1),
        makeNote (60, 1.0, 0.25, 100, 1),
        makeNote (60, 1.0, 0.25, 100, 2),  /* different channel -- keep */
        makeNote (64, 1.0, 0.25, 100, 1)
    });

    MidiNote example = makeNote (60, 1.0, 0.25, 100, 1);
    r.removeNotesMatching (example);

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 3);
    /* Removed entry was (60, on=1.0, ch=1).  The (60, on=0.0) and the
     * (60, on=1.0, ch=2) and (64, on=1.0) should survive. */
    for (const auto& n : *snap)
    {
        const bool isRemovedMatch = (n.pitch == 60 && n.channel == 1
                                  && nearly (n.onBeat, 1.0));
        BOOST_CHECK (! isRemovedMatch);
    }
}

/* ---------- Trash discipline ---------- */

BOOST_AUTO_TEST_CASE (sweep_trash_is_safe_with_no_swaps)
{
    /* An idle region (no edits since construction) has nothing on
     * trash_.  sweepTrash is idempotent and must not crash. */
    MidiNoteRegion r;
    r.sweepTrash();
    r.advanceAudioEpoch();
    r.sweepTrash();
    BOOST_CHECK_EQUAL (r.noteCount(), (size_t) 0);
}

BOOST_AUTO_TEST_CASE (sweep_trash_reclaims_after_epoch_advance)
{
    /* Publish a snapshot, advance the audio epoch, sweep -- the old
     * snapshot must be reclaimed.  Indirect check via a follow-up
     * sweep that also succeeds. */
    MidiNoteRegion r;
    r.setNotes ({ makeNote (60, 0.0) });
    r.setNotes ({ makeNote (62, 0.0) });  /* publishes a 2nd snapshot,
                                             pushes 1st to trash */
    r.advanceAudioEpoch();
    r.sweepTrash();
    /* Trash deque is now drained -- there's no public size accessor,
     * but a follow-up sweep that doesn't crash + a live snapshot that
     * still works confirms the path. */
    BOOST_REQUIRE (r.loadSnapshot() != nullptr);
    BOOST_CHECK_EQUAL (r.noteCount(), (size_t) 1);
}

BOOST_AUTO_TEST_CASE (cow_snapshot_swap_is_visible_atomically)
{
    /* Smoke test for the COW + epoch-gated reclaim pattern: a reader
     * thread (simulating the audio render callback) bumps the audio
     * epoch + samples in a tight loop and must see ONE coherent state,
     * never a torn read or a freed pointer.  The UI thread publishes
     * + sweeps concurrently.  Mirrors AutomationRegion's analogous
     * test verbatim. */
    MidiNoteRegion r;
    r.setNotes ({ makeNote (60, 0.0) });

    std::atomic<bool> done { false };
    std::atomic<int>  seenA { 0 };
    std::atomic<int>  seenB { 0 };

    std::thread reader ([&] ()
    {
        while (! done.load (std::memory_order_acquire))
        {
            r.advanceAudioEpoch();           /* per-block epoch tick */
            const auto* snap = r.loadSnapshot();
            BOOST_REQUIRE (snap != nullptr);
            BOOST_REQUIRE (! snap->empty());
            const int pitch = (*snap)[0].pitch;
            if      (pitch == 60) seenA.fetch_add (1, std::memory_order_relaxed);
            else if (pitch == 62) seenB.fetch_add (1, std::memory_order_relaxed);
            else BOOST_FAIL ("torn snapshot read: pitch=" << pitch);
        }
    });

    /* UI thread: publish + sweep concurrent with the reader.  The
     * epoch gate must keep sweepTrash() from reclaiming any snapshot
     * the reader is currently using. */
    for (int i = 0; i < 1000; ++i)
    {
        MidiNoteRegion::NoteList next { makeNote ((i & 1) ? 62 : 60, 0.0) };
        r.setNotes (next);
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

/* ---------- ValueTree round-trip ---------- */

BOOST_AUTO_TEST_CASE (region_xml_round_trip_basic)
{
    MidiNoteRegion r;
    r.id            = juce::Uuid();
    r.positionBeats = 8.0;
    r.lengthBeats   = 4.0;
    r.looped        = true;
    r.name          = juce::String ("intro");
    r.setNotes ({
        makeNote (60, 0.0, 0.5, 100, 1),
        makeNote (64, 1.0, 0.5,  80, 2),
        makeNote (67, 2.0, 1.0, 127, 1)
    });

    const auto vt = r.toValueTree();
    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);

    BOOST_CHECK (restored->id == r.id);
    BOOST_CHECK_EQUAL (restored->positionBeats, r.positionBeats);
    BOOST_CHECK_EQUAL (restored->lengthBeats,   r.lengthBeats);
    BOOST_CHECK (restored->looped);
    BOOST_CHECK (restored->name == r.name);

    BOOST_REQUIRE_EQUAL (restored->noteCount(), r.noteCount());
    const auto* a = r.loadSnapshot();
    const auto* b = restored->loadSnapshot();
    for (size_t i = 0; i < a->size(); ++i)
    {
        BOOST_CHECK_EQUAL ((*a)[i].pitch,    (*b)[i].pitch);
        BOOST_CHECK_EQUAL ((*a)[i].velocity, (*b)[i].velocity);
        BOOST_CHECK_EQUAL ((*a)[i].channel,  (*b)[i].channel);
        BOOST_CHECK (nearly ((*a)[i].onBeat,      (*b)[i].onBeat));
        BOOST_CHECK (nearly ((*a)[i].lengthBeats, (*b)[i].lengthBeats));
    }
}

BOOST_AUTO_TEST_CASE (region_xml_preserves_sourceid_when_set)
{
    MidiNoteRegion r;
    r.id       = juce::Uuid();
    r.sourceId = juce::Uuid();
    r.setNotes ({ makeNote (60, 0.0) });

    const auto vt = r.toValueTree();
    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (restored->sourceId == r.sourceId);
}

BOOST_AUTO_TEST_CASE (region_xml_sparse_writes_default_fields)
{
    /* Empty/default fields should NOT appear in the serialised XML --
     * sparse-write keeps the session file compact for the common case
     * of many small regions sharing defaults.  Verify via attribute
     * presence on the ValueTree, not text matching. */
    MidiNoteRegion r;
    r.id = juce::Uuid();
    /* Leave looped=false, lengthBeats=0, positionBeats=0, name empty,
     * sourceId null, colour default. */
    const auto vt = r.toValueTree();
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("pos")));
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("len")));
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("loop")));
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("src")));
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("name")));
    BOOST_CHECK (! vt.hasProperty (juce::Identifier ("col")));
}

BOOST_AUTO_TEST_CASE (region_xml_round_trip_empty_notes_survives)
{
    /* An empty region (no notes) should round-trip cleanly to noteCount=0. */
    MidiNoteRegion r;
    r.id = juce::Uuid();
    const auto vt = r.toValueTree();
    auto restored = MidiNoteRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK_EQUAL (restored->noteCount(), (size_t) 0);
}

BOOST_AUTO_TEST_CASE (region_xml_default_channel_omitted)
{
    /* Channel 1 is the default; sparse-write must NOT emit a channel
     * attribute for default-channel notes.  Verify via attribute
     * presence on the first child <n> element. */
    MidiNoteRegion r;
    r.id = juce::Uuid();
    r.setNotes ({ makeNote (60, 0.0, 0.25, 100, 1) });
    const auto vt = r.toValueTree();
    BOOST_REQUIRE_EQUAL (vt.getNumChildren(), 1);
    const auto nv = vt.getChild (0);
    BOOST_CHECK (! nv.hasProperty (juce::Identifier ("ch")));

    /* A non-default channel SHOULD emit the attribute. */
    MidiNoteRegion r2;
    r2.id = juce::Uuid();
    r2.setNotes ({ makeNote (60, 0.0, 0.25, 100, 7) });
    const auto vt2 = r2.toValueTree();
    BOOST_REQUIRE_EQUAL (vt2.getNumChildren(), 1);
    const auto nv2 = vt2.getChild (0);
    BOOST_CHECK (nv2.hasProperty (juce::Identifier ("ch")));
    BOOST_CHECK_EQUAL ((int) nv2.getProperty (juce::Identifier ("ch")), 7);
}

/* ---------- setNotesAssigningIds: unique-id invariant ---------- */

/* Selection/highlight/per-note edit all key on the note id, so every
 * note MUST end up with a unique non-zero id.  The old single-pass logic
 * could hand a 0-id note a fresh id that collided with a preset id later
 * in the (position-sorted) list -- making distinct notes alias under
 * selection ("grab one note, others light up elsewhere"). */
BOOST_AUTO_TEST_CASE (assign_ids_no_collision_when_zero_precedes_preset)
{
    MidiNoteRegion r;
    /* Position order puts the 0-id note first; a preset id of 1 would be
     * exactly what nextNoteId() hands out on a fresh region (allocator 0). */
    MidiNoteRegion::NoteList notes;
    MidiNote a = makeNote (60, 0.0); a.id = 0;   // gets a fresh id
    MidiNote b = makeNote (62, 1.0); b.id = 1;   // preset, appears later
    notes.push_back (a);
    notes.push_back (b);
    r.setNotesAssigningIds (std::move (notes));

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE (snap != nullptr);
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 2);
    BOOST_CHECK ((*snap)[0].id != 0);
    BOOST_CHECK ((*snap)[1].id != 0);
    BOOST_CHECK ((*snap)[0].id != (*snap)[1].id);   // no collision
}

BOOST_AUTO_TEST_CASE (assign_ids_repairs_duplicate_preset_ids)
{
    /* A session corrupted by the old bug (two notes persisted with the
     * same id) must be repaired on load, not preserved. */
    MidiNoteRegion r;
    MidiNoteRegion::NoteList notes;
    MidiNote a = makeNote (60, 0.0); a.id = 5;
    MidiNote b = makeNote (62, 1.0); b.id = 5;   // duplicate
    MidiNote c = makeNote (64, 2.0); c.id = 5;   // triplicate
    notes.push_back (a); notes.push_back (b); notes.push_back (c);
    r.setNotesAssigningIds (std::move (notes));

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 3);
    std::set<std::uint64_t> ids;
    for (const auto& n : *snap)
    {
        BOOST_CHECK (n.id != 0);
        ids.insert (n.id);
    }
    BOOST_CHECK_EQUAL (ids.size(), (size_t) 3);   // all distinct after repair
}

BOOST_AUTO_TEST_SUITE_END()  /* MidiNoteRegionTests */
