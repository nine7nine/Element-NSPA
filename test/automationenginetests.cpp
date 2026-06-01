// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/automation/automation_engine.hpp"

#include <element/juce/audio_basics.hpp>
#include <element/juce/audio_processors.hpp>
#include <element/parameter.hpp>

#include <atomic>
#include <memory>

using element::automation::AutomationEngine;
using element::automation::AutomationTarget;
using element::dsp::automation::AutomationMode;
using element::dsp::automation::AutomationPoint;
using element::dsp::automation::AutomationRegion;
using element::dsp::automation::AutomationTrack;

namespace {

class FakePluginParam : public juce::AudioProcessorParameter
{
public:
    std::atomic<float> last { -1.0f };
    int                setCallCount { 0 };
    float getValue() const override                            { return last.load(); }
    void  setValue (float v) override                          { last.store (v); ++setCallCount; }
    float getDefaultValue() const override                     { return 0.5f; }
    juce::String getName (int) const override                  { return "fake"; }
    juce::String getLabel() const override                     { return {}; }
    float getValueForText (const juce::String&) const override { return 0.f; }
};

class FakeNodeParam : public element::Parameter
{
public:
    std::atomic<float> last { -1.0f };
    int                setCallCount { 0 };
    int   getPortIndex() const noexcept override        { return 0; }
    int   getParameterIndex() const noexcept override   { return 0; }
    float getValue() const override                     { return last.load(); }
    void  setValue (float v) override                   { last.store (v); ++setCallCount; }
    float getDefaultValue() const override              { return 0.5f; }
    float getValueForText (const juce::String&) const override { return 0.f; }
    juce::String getName (int) const override           { return "fake"; }
    juce::String getLabel() const override              { return {}; }
};

std::unique_ptr<AutomationTrack> makeTrackWithLinearRegion (double regionStart,
                                                            double regionLen,
                                                            double startVal,
                                                            double endVal)
{
    auto track = std::make_unique<AutomationTrack>();
    track->id = juce::Uuid();
    auto region = std::make_unique<AutomationRegion>();
    region->id = juce::Uuid();
    region->positionBeats = regionStart;
    region->lengthBeats   = regionLen;
    AutomationRegion::PointList pts {
        { 0.0,        startVal, { } },   // default handle = straight chord
        { regionLen,  endVal,   { } }
    };
    region->setPoints (pts);
    track->addRegion (std::move (region));
    return track;
}

} // namespace

BOOST_AUTO_TEST_SUITE (AutomationEngineTests)

BOOST_AUTO_TEST_CASE (empty_engine_apply_is_safe_noop)
{
    AutomationEngine eng;
    BOOST_CHECK_EQUAL (eng.numTracks(), 0);

    juce::MidiBuffer midi;
    /* Zero-track call must not crash and must not allocate visibly. */
    eng.applyForBlock (0.0, 256, 48000.0, &midi);
    eng.applyForBlock (4.2, 256, 48000.0, &midi);
    BOOST_CHECK (midi.isEmpty());
}

BOOST_AUTO_TEST_CASE (plugin_param_read_mode_writes_coarse_value)
{
    AutomationEngine eng;
    FakePluginParam  param;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.2, 0.8));
    BOOST_REQUIRE (track != nullptr);

    eng.bindPluginParam (track, &param);
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;

    /* Mid-region playhead: localBeats = 2.0, linear interp from 0.2
     * to 0.8 over 4 beats -> 0.5. */
    eng.applyForBlock (2.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (param.setCallCount, 1);
    BOOST_CHECK_CLOSE (param.last.load(), 0.5f, 1e-4);

    /* Playhead past region end -> region inactive, no write this block. */
    eng.applyForBlock (10.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (param.setCallCount, 1);
}

BOOST_AUTO_TEST_CASE (off_mode_skips_dispatch)
{
    AutomationEngine eng;
    FakePluginParam  param;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindPluginParam (track, &param);
    track->setMode (AutomationMode::Off);

    juce::MidiBuffer midi;
    eng.applyForBlock (2.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (param.setCallCount, 0);
}

BOOST_AUTO_TEST_CASE (read_mode_publishes_mute_flag_off_mode_clears)
{
    AutomationEngine eng;
    FakePluginParam  param;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindPluginParam (track, &param);
    const int slot = track->muteSlotIndex.load();
    BOOST_REQUIRE (slot >= 0);

    /* Read mode -> mute flag should be true after applyForBlock. */
    track->setMode (AutomationMode::Read);
    juce::MidiBuffer midi;
    eng.applyForBlock (2.0, 256, 48000.0, &midi);
    BOOST_CHECK (eng.isMappingMuted (slot));

    /* Flip to Off -> next block clears the flag. */
    track->setMode (AutomationMode::Off);
    eng.applyForBlock (2.0, 256, 48000.0, &midi);
    BOOST_CHECK (! eng.isMappingMuted (slot));
}

BOOST_AUTO_TEST_CASE (remove_track_clears_mute_flag)
{
    AutomationEngine eng;
    FakePluginParam  param;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    const auto trackId = track->id;
    eng.bindPluginParam (track, &param);
    const int slot = track->muteSlotIndex.load();

    track->setMode (AutomationMode::Read);
    juce::MidiBuffer midi;
    eng.applyForBlock (2.0, 256, 48000.0, &midi);
    BOOST_REQUIRE (eng.isMappingMuted (slot));

    /* Removing the track must clear the mute slot so MappingEngine
     * stops deferring writes for the (now-unbound) target. */
    eng.removeTrack (trackId);
    BOOST_CHECK (! eng.isMappingMuted (slot));
    BOOST_CHECK_EQUAL (eng.numTracks(), 0);
}

BOOST_AUTO_TEST_CASE (node_param_read_mode_dispatches)
{
    AutomationEngine eng;
    juce::ReferenceCountedObjectPtr<FakeNodeParam> p { new FakeNodeParam() };

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 8.0, 0.0, 1.0));
    eng.bindNodeParam (track, p.get());
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;
    eng.applyForBlock (4.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (p->setCallCount, 1);
    BOOST_CHECK_CLOSE (p->last.load(), 0.5f, 1e-4);
}

BOOST_AUTO_TEST_CASE (midi_cc_read_mode_emits_event_at_offset_zero)
{
    AutomationEngine eng;
    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindMidiCc (track, 1, 11);   /* expression CC */
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;
    eng.applyForBlock (2.0, 256, 48000.0, &midi);

    BOOST_REQUIRE_EQUAL (midi.getNumEvents(), 1);
    juce::MidiBuffer::Iterator it (midi);
    juce::MidiMessage msg;
    int samplePos = -1;
    BOOST_REQUIRE (it.getNextEvent (msg, samplePos));
    BOOST_CHECK_EQUAL (samplePos, 0);
    BOOST_CHECK (msg.isController());
    BOOST_CHECK_EQUAL (msg.getControllerNumber(), 11);
    BOOST_CHECK_EQUAL (msg.getControllerValue(), juce::roundToInt (0.5f * 127.f));
}

BOOST_AUTO_TEST_CASE (rebind_target_retires_old_via_epoch_gated_trash)
{
    AutomationEngine eng;
    FakePluginParam  paramA;
    FakePluginParam  paramB;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindPluginParam (track, &paramA);
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;
    eng.applyForBlock (1.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (paramA.setCallCount, 1);

    /* Rebind to a new param -- old target moves to epoch-gated trash. */
    eng.bindPluginParam (track, &paramB);
    eng.applyForBlock (2.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (paramB.setCallCount, 1);
    BOOST_CHECK_CLOSE (paramB.last.load(), 0.5f, 1e-4);

    /* Sweep should reclaim the retired target now that the engine
     * epoch has advanced past the rebind stamp. */
    eng.sweepTrash();
    /* No direct way to assert "trash is empty"; the test passes if
     * we don't crash + paramB continues to dispatch correctly. */
    eng.applyForBlock (3.0, 256, 48000.0, &midi);
    BOOST_CHECK_EQUAL (paramB.setCallCount, 2);
}

BOOST_AUTO_TEST_CASE (find_track_by_id_round_trip)
{
    AutomationEngine eng;
    auto* t1 = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    auto* t2 = eng.addTrack (makeTrackWithLinearRegion (8.0, 4.0, 0.0, 1.0));

    BOOST_CHECK (eng.findTrackById (t1->id) == t1);
    BOOST_CHECK (eng.findTrackById (t2->id) == t2);
    BOOST_CHECK (eng.findTrackById (juce::Uuid()) == nullptr);
}

BOOST_AUTO_TEST_CASE (mute_slot_out_of_range_returns_false)
{
    AutomationEngine eng;
    /* No slots allocated yet -- any query returns false. */
    BOOST_CHECK (! eng.isMappingMuted (0));
    BOOST_CHECK (! eng.isMappingMuted (-1));
    BOOST_CHECK (! eng.isMappingMuted (1000));
}

BOOST_AUTO_TEST_CASE (mapping_lookup_empty_engine_short_circuits_false)
{
    AutomationEngine eng;
    FakePluginParam  param;
    /* activeTrackCount is 0 -- lookup must short-circuit without
     * taking the lock + return false. */
    BOOST_CHECK (! eng.isMappingMutedForPluginParam (&param));
    BOOST_CHECK (! eng.isMappingMutedForPluginParam (nullptr));
    BOOST_CHECK_EQUAL (eng.activeTrackCount(), 0);
}

BOOST_AUTO_TEST_CASE (mapping_lookup_finds_read_mode_track_for_plugin_param)
{
    AutomationEngine eng;
    FakePluginParam  param;
    FakePluginParam  otherParam;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindPluginParam (track, &param);

    /* Off mode: bound but not in Read -> mute flag is false ->
     * lookup returns false. */
    track->setMode (AutomationMode::Off);
    juce::MidiBuffer midi;
    eng.applyForBlock (1.0, 256, 48000.0, &midi);
    BOOST_CHECK (! eng.isMappingMutedForPluginParam (&param));

    /* Read mode + applyForBlock publishes mute=true -> lookup
     * returns true. */
    track->setMode (AutomationMode::Read);
    eng.applyForBlock (1.0, 256, 48000.0, &midi);
    BOOST_CHECK (eng.isMappingMutedForPluginParam (&param));

    /* Unrelated parameter must NOT be muted just because some other
     * track is in Read mode. */
    BOOST_CHECK (! eng.isMappingMutedForPluginParam (&otherParam));
}

BOOST_AUTO_TEST_CASE (mapping_lookup_node_param_variant)
{
    AutomationEngine eng;
    juce::ReferenceCountedObjectPtr<FakeNodeParam> p { new FakeNodeParam() };

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindNodeParam (track, p.get());
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;
    eng.applyForBlock (1.0, 256, 48000.0, &midi);

    BOOST_CHECK (eng.isMappingMutedForNodeParam (p.get()));

    /* Querying the wrong target kind for the same pointer must
     * return false -- discriminator + pointer both have to match. */
    BOOST_CHECK (! eng.isMappingMutedForPluginParam (
        reinterpret_cast<juce::AudioProcessorParameter*> (p.get())));
}

BOOST_AUTO_TEST_CASE (unbind_clears_lookup_state)
{
    AutomationEngine eng;
    FakePluginParam  param;

    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindPluginParam (track, &param);
    track->setMode (AutomationMode::Read);
    juce::MidiBuffer midi;
    eng.applyForBlock (1.0, 256, 48000.0, &midi);
    BOOST_REQUIRE (eng.isMappingMutedForPluginParam (&param));

    /* Unbind: liveTarget cleared atomically; mute slot cleared too.
     * Subsequent lookup must return false even though the track is
     * still in Read mode (it's now bound to nothing). */
    eng.unbindTarget (track);
    BOOST_CHECK (! eng.isMappingMutedForPluginParam (&param));
}

BOOST_AUTO_TEST_CASE (midi_cc_sub_block_emission_at_stride_offsets)
{
    /* A linear ramp 0 -> 1 over 4 beats; play head at 0 beats, block
     * spans 1024 samples at 96 bpm @ 48kHz -> beatsPerBlock =
     * (1024/48000) * (96/60) = ~0.0341 beats per block.  Inside the
     * region, ramp slope is 1.0/4.0 = 0.25/beat -> per-block delta
     * is ~0.00853 normalized; per-stride (64 samples) delta is
     * 0.00853 / 16 = ~0.000533 -- BELOW the 1/256 threshold.  Make
     * the curve steep enough by setting region length to 0.5 beats
     * (delta 16x larger) so the delta gate emits every stride. */
    AutomationEngine eng;
    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 0.5, 0.0, 1.0));
    eng.bindMidiCc (track, 1, 11);
    track->setMode (AutomationMode::Read);

    constexpr int   blockSize     = 1024;
    constexpr double sampleRate   = 48000.0;
    constexpr double bpm          = 96.0;
    const     double beatsPerBlock = ((double) blockSize / sampleRate) * (bpm / 60.0);

    juce::MidiBuffer midi;
    eng.applyForBlock (0.0, beatsPerBlock, blockSize, sampleRate, &midi);

    /* Expected events: 1024 / 64 = 16 sub-block strides; the first
     * always emits + subsequent emit when delta > 1/256.  With this
     * curve and block size, every stride crosses the threshold. */
    BOOST_CHECK_GE (midi.getNumEvents(), 8);   /* well over half of 16 */

    /* Verify the events are spaced at stride boundaries -- i.e. the
     * sample positions are multiples of kAutomationSubBlockStride. */
    juce::MidiBuffer::Iterator it (midi);
    juce::MidiMessage msg;
    int samplePos = -1;
    int eventCount = 0;
    while (it.getNextEvent (msg, samplePos))
    {
        BOOST_CHECK_EQUAL (samplePos % AutomationEngine::kAutomationSubBlockStride, 0);
        BOOST_CHECK (msg.isController());
        BOOST_CHECK_EQUAL (msg.getControllerNumber(), 11);
        ++eventCount;
    }
    BOOST_CHECK_GT (eventCount, 0);
}

BOOST_AUTO_TEST_CASE (midi_cc_flat_curve_emits_just_one_event_per_block)
{
    /* A constant-value region -- the delta gate must suppress all
     * but the first emission.  Validates the 1/256 threshold. */
    AutomationEngine eng;
    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.5, 0.5));
    eng.bindMidiCc (track, 1, 1);
    track->setMode (AutomationMode::Read);

    constexpr int    blockSize     = 1024;
    constexpr double sampleRate    = 48000.0;
    constexpr double beatsPerBlock = 0.05;

    juce::MidiBuffer midi;
    eng.applyForBlock (1.0, beatsPerBlock, blockSize, sampleRate, &midi);

    /* k == 0 always emits; all subsequent strides see |delta| == 0
     * which is NOT > 1/256, so they're suppressed. */
    BOOST_CHECK_EQUAL (midi.getNumEvents(), 1);
}

BOOST_AUTO_TEST_CASE (zero_beats_per_block_falls_back_to_coarse)
{
    AutomationEngine eng;
    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindMidiCc (track, 1, 11);
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;
    /* beatsPerBlock == 0 -> SA path disabled -> single coarse
     * emission at frameOffset 0. */
    eng.applyForBlock (2.0, /*beatsPerBlock*/ 0.0, 1024, 48000.0, &midi);
    BOOST_CHECK_EQUAL (midi.getNumEvents(), 1);

    juce::MidiBuffer::Iterator it (midi);
    juce::MidiMessage msg;
    int samplePos = -1;
    BOOST_REQUIRE (it.getNextEvent (msg, samplePos));
    BOOST_CHECK_EQUAL (samplePos, 0);
}

BOOST_AUTO_TEST_CASE (plugin_param_stays_coarse_under_sa_path)
{
    /* Verifies the Phase 1 scope decision: plugin + node params get
     * exactly ONE coarse write per block even when SA emission is
     * enabled.  Sub-block plugin emission is Phase 1.5. */
    AutomationEngine eng;
    FakePluginParam  param;
    auto* track = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.bindPluginParam (track, &param);
    track->setMode (AutomationMode::Read);

    juce::MidiBuffer midi;
    eng.applyForBlock (1.0, 0.05, 1024, 48000.0, &midi);
    BOOST_CHECK_EQUAL (param.setCallCount, 1);   /* coarse, not 16x */
    BOOST_CHECK (midi.isEmpty());                /* not a MIDI target */
}

BOOST_AUTO_TEST_CASE (save_to_value_tree_empty_engine_writes_nothing)
{
    AutomationEngine eng;
    juce::ValueTree root ("session");
    eng.saveToValueTree (root);
    /* No tracks bound -> no tags::automationTracks child added. */
    BOOST_CHECK_EQUAL (root.getNumChildren(), 0);
}

BOOST_AUTO_TEST_CASE (save_load_round_trip_preserves_tracks_and_regions)
{
    juce::ValueTree root ("session");
    {
        AutomationEngine eng;
        auto* t1 = eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.2, 0.8));
        auto* t2 = eng.addTrack (makeTrackWithLinearRegion (8.0, 2.0, 0.1, 0.9));
        t1->setMode (AutomationMode::Read);
        t2->setMode (AutomationMode::Off);
        t1->targetKey.nodeId  = juce::Uuid();
        t1->targetKey.paramId = "volume";
        t2->targetKey.midiCcChannel = 5;
        t2->targetKey.midiCcNumber  = 74;

        eng.saveToValueTree (root);
    }

    BOOST_REQUIRE (root.getChildWithName (juce::Identifier ("automationTracks")).isValid());

    /* Load into a fresh engine. */
    AutomationEngine restored;
    restored.loadFromValueTree (root);

    BOOST_CHECK_EQUAL (restored.numTracks(), 2);

    /* Sampling parity at known beats -- regions must round-trip
     * with their point lists intact. */
    int seenMidi = 0, seenInternal = 0;
    restored.forEachTrack ([&] (AutomationTrack* t)
    {
        if (t->targetKey.isMidi())
        {
            ++seenMidi;
            BOOST_CHECK_EQUAL (t->targetKey.midiCcChannel, 5);
            BOOST_CHECK_EQUAL (t->targetKey.midiCcNumber,  74);
        }
        else
        {
            ++seenInternal;
            BOOST_CHECK_EQUAL (t->targetKey.paramId, "volume");
            /* Sampling parity at midpoint of the (0..4) region. */
            auto* r = t->findActiveRegion (2.0);
            BOOST_REQUIRE (r != nullptr);
            BOOST_CHECK_CLOSE (r->sampleAtBeats (2.0), 0.5, 1e-4);
        }
    });
    BOOST_CHECK_EQUAL (seenMidi,     1);
    BOOST_CHECK_EQUAL (seenInternal, 1);
}

BOOST_AUTO_TEST_CASE (load_replaces_existing_tracks)
{
    AutomationEngine eng;
    /* Seed with two tracks. */
    eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    eng.addTrack (makeTrackWithLinearRegion (8.0, 4.0, 0.0, 1.0));
    BOOST_REQUIRE_EQUAL (eng.numTracks(), 2);

    /* Now load from a tree with just ONE track -- existing two must
     * be cleared first. */
    juce::ValueTree root ("session");
    {
        AutomationEngine src;
        src.addTrack (makeTrackWithLinearRegion (0.0, 2.0, 0.5, 0.5));
        src.saveToValueTree (root);
    }
    eng.loadFromValueTree (root);
    BOOST_CHECK_EQUAL (eng.numTracks(), 1);
}

BOOST_AUTO_TEST_CASE (load_missing_tag_clears_engine_no_crash)
{
    AutomationEngine eng;
    eng.addTrack (makeTrackWithLinearRegion (0.0, 4.0, 0.0, 1.0));
    BOOST_REQUIRE_EQUAL (eng.numTracks(), 1);

    /* Empty root -> load clears everything (forwards-compat: legacy
     * session with no automation tag). */
    juce::ValueTree root ("session");
    eng.loadFromValueTree (root);
    BOOST_CHECK_EQUAL (eng.numTracks(), 0);
}

BOOST_AUTO_TEST_CASE (drain_pending_lookups_is_safe_no_crash)
{
    /* drainPendingLookups acquires + releases the lookup lock --
     * smoke-test that it doesn't deadlock or crash when called with
     * no in-flight readers. */
    AutomationEngine eng;
    eng.drainPendingLookups();
    eng.drainPendingLookups();   /* idempotent */
}

BOOST_AUTO_TEST_SUITE_END()
