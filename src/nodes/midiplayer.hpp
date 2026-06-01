// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/midipipe.hpp>
#include <element/node.h>
#include "nodes/midifilter.hpp"
#include "services/automation/automation_target.hpp"
#include "services/timeline/midi_note_region.hpp"

#include <array>
#include <atomic>
#include <bitset>
#include <memory>

namespace element {

/** Arrangement-timeline MIDI player.  Counterpart of AudioClipNode for
 *  the MIDI lane kind -- emits NoteOn / NoteOff (+ velocity, channel)
 *  on the audio thread by walking the bound MidiNoteRegion's COW
 *  snapshot at each block's beat range.
 *
 *  ## Why MidiPlayerNode is a real graph node
 *
 *  The user's mental model from the tracker pattern: add a node ->
 *  wire its MIDI output to a Sampler / synth -> notes come out at
 *  transport time.  ArrangementView's MIDI lane spawns one of these
 *  internally so the user doesn't have to (just like
 *  createEmptyAudioLane spawns an AudioClipNode), but the node is
 *  still a first-class graph citizen -- you can wire its output
 *  anywhere, swap the bound synth, route through MidiChannelMap, etc.
 *
 *  ## Topology
 *
 *  MIDI-only output node.  No MIDI input (Session 2 paint-only --
 *  later phases may add live-record by promoting this to MidiFilter
 *  with input + output; for now keep the surface minimal).  Two
 *  ports: control "active" + MIDI output.
 *
 *  Actually we publish ONE MIDI output port and no other ports.
 *  TrackerNode adds a MIDI input + output; MidiPlayerNode keeps just
 *  the output since live-record into a piano-roll is a Session 3
 *  feature.
 *
 *  ## Region binding
 *
 *  Per-lane MidiPlayerNode tracks an array of bound MidiNoteRegions
 *  via a lock-free pointer table (RegionEntry).  Message thread
 *  publishes a new entry array on every playlist mutation (region
 *  added / removed / moved / resized); audio thread loads the
 *  current entry table once per block.
 *
 *  The entries themselves point at MidiNoteRegions that live in the
 *  Lane's Playlist (on the message thread).  Their lifetime is at
 *  least the message thread's PianoRoll edit-mutate path: the same
 *  COW snapshot discipline that MidiNoteRegion enforces (epoch trash
 *  + UI sweep) keeps the live snapshot pointer valid for the
 *  duration of one audio block.
 *
 *  Region deletion + lane teardown:
 *   - Message thread must `setBoundRegions ({})` BEFORE destroying
 *     the Playlist / Lane so the audio thread observes the empty
 *     table before the regions go away.  ArrangementView's
 *     willBeRemoved + rescanLaneTargets handle this.
 *
 *  ## Audio-thread render loop
 *
 *  Per block:
 *    1. Read transport: blockStartBeat, blockEndBeat, isPlaying.
 *    2. If not playing or no monitor info, emit all-notes-off for
 *       any currently-held notes, return.
 *    3. Load the bound region entry table (atomic snapshot).
 *    4. For each entry whose [position, position+length) overlaps
 *       the block range:
 *         - Map block beat -> local region beat (handle looped).
 *         - Walk the region's note snapshot in the local-beat
 *           window; emit NoteOn at sample offset = (onBeat-blockStart)
 *           * samplesPerBeat.
 *         - Emit NoteOff for notes whose end falls in the block range.
 *    5. Track which notes are currently held (per channel + pitch)
 *       so transport-stop / region-exit can send the matching OFFs.
 *
 *  Lock-free.  No allocations on the audio thread. */
class MidiPlayerNode : public MidiFilterNode
{
public:
    MidiPlayerNode();
    ~MidiPlayerNode() override;

    void getPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.fileOrIdentifier   = EL_NODE_ID_MIDI_PLAYER;
        desc.name               = "MIDI Player";
        desc.descriptiveName    = "Arrangement MIDI playback (piano-roll lane)";
        desc.numInputChannels   = 0;
        desc.numOutputChannels  = 0;
        desc.hasSharedContainer = false;
        desc.isInstrument       = false;
        desc.category           = "Source";
        desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
        desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
        desc.version            = "0.1.0";
        desc.uniqueId           = EL_NODE_UID_MIDI_PLAYER;
    }

    void prepareToRender (double sampleRate, int maxBufferSize) override;
    void releaseResources() override;
    void render (RenderContext& rc) override;

    void setState (const void* data, int size) override;
    void getState (juce::MemoryBlock& block) override;

    //==========================================================================
    // Region binding API.
    //
    // Message thread builds a fresh entries vector reflecting the lane's
    // current playlist + calls setBoundRegions; audio thread observes
    // the swap atomically at the start of the next render block.
    //
    // The pointer table is at most ~32 entries (lanes don't carry
    // hundreds of regions in practice).  When a region's onBeat /
    // lengthBeats / loop flag changes the message thread re-publishes
    // a fresh entries array; the underlying MidiNoteRegion pointer can
    // stay the same, only the cached position metadata is reread.

    struct RegionEntry
    {
        MidiNoteRegion* region        { nullptr };
        double          positionBeats { 0.0 };
        double          lengthBeats   { 0.0 };
        /** Source-offset into the region's note list -- mirrors
         *  MidiNoteRegion::startBeats.  Cached on the entry so the
         *  audio thread reads a consistent (position, length, start,
         *  looped) tuple without touching the region's mutable fields
         *  (which the UI thread may be racing through during a drag). */
        double          startBeats    { 0.0 };
        bool            looped        { false };
        /** Loop period in beats.  0 means "use lengthBeats" -- preserves
         *  pre-fix sessions where the loop wrapped on the region's drawn
         *  duration.  When non-zero, the audio thread's modulo uses this
         *  value so a right-edge drag extends the number of repeats
         *  rather than stretching the loop pattern. */
        double          loopLengthBeats { 0.0 };
    };

    /** Replace the active region table.  Message thread only.  The
     *  prior table is discarded after a one-block grace period --
     *  enforced via the same epoch-trash discipline as
     *  MidiNoteRegion: every render block bumps audioEpoch_; sweep
     *  drops trashed tables whose stamp is strictly less than the
     *  current epoch. */
    void setBoundRegions (std::vector<RegionEntry> entries);

    /** Drain the trash deque.  Called on the message thread by a
     *  Timer / AsyncUpdater after region edits.  Safe to call from
     *  any non-audio thread.  Idempotent. */
    void sweepBindingsTrash() noexcept;

    //==========================================================================
    // Clip-local PARAM automation binding (#11 Phase 2b).
    //
    // A MidiCcLane can target a node/plugin parameter instead of a MIDI CC
    // (MidiCcLane::isParam()).  CC lanes ride the MIDI output; param lanes
    // are APPLIED to a live parameter while the region plays.  The audio
    // thread can't reach into the graph, so the message thread (view layer,
    // which has graph access) resolves each param lane's (nodeId, paramId)
    // to a live AutomationTarget and publishes a binding table here -- same
    // lock-free swap + epoch-trash discipline as setBoundRegions.
    //
    // Application is COARSE: one setValue per block at the block-start local
    // beat, mirroring AutomationEngine's param path (a 7-bit-ish smooth
    // controller doesn't need sub-block param writes, and setValue-per-
    // stride on a hosted plugin is needless audio-thread work).

    struct ParamBinding
    {
        /** Identity that matches a region's MidiCcLane param target. */
        juce::Uuid                   nodeId;
        juce::String                 paramId;
        /** Resolved live destination (kind NodeParam / PluginParam).  Holds
         *  the Parameter alive against transient graph rebuilds (ParameterPtr
         *  by value), exactly like AutomationTrack::liveTarget. */
        automation::AutomationTarget target;
    };

    /** Replace the active param-binding table.  Message thread only.  Prior
     *  table discarded after a one-block grace period (audioEpoch_ trash). */
    void setBoundParams (std::vector<ParamBinding> bindings);

    //==========================================================================
    // Session-view API.  Parallel to TrackerNode's session-view API.
    // Each SessionClipSlot carries one MidiNoteRegion + per-clip
    // launch state machine + per-clip held-notes bitset.  Audio thread
    // walks active slots on every render block in addition to the
    // arrangement entry table -- both surfaces coexist on a single
    // MidiPlayerNode instance, though in practice ArrangementView and
    // SessionView each spawn their own.
    //
    // RT-safety contract mirrors TrackerNode B.1-B.4:
    //  - sessionClips_ grows monotonically; the audio thread reads
    //    via std::atomic<int> sessionClipCount_ (release-stored after
    //    a slot's contents are populated).
    //  - Per-slot `alive` flag is the visible discriminant; the audio
    //    thread acquire-loads it before reading any of the slot's
    //    other fields.
    //  - schedulePlayingClip pushes a LaunchReq through an SPSC FIFO;
    //    the audio thread drains under no lock at block start.
    //  - Slot state transitions on the audio thread are sequenced:
    //    drain FIFO -> apply pending for block beats -> emit notes.

    /** Allocate (or reuse a tombstoned) slot for a fresh MIDI clip.
     *  Returns the new clip's stable Uuid; subsequent session-view
     *  API calls key on this id.  Default region: empty notes, 4-beat
     *  length, looped=true (session clip semantic).
     *
     *  Message thread only.  Adding a new clip while the audio thread
     *  is running is safe: the slot's `alive` flag is published
     *  release-after the slot's other fields are populated, so the
     *  audio thread either sees the fully-initialised slot or skips it. */
    juce::Uuid createSessionClip();

    /** Same but with a caller-supplied stable Uuid + default region
     *  shape.  Used by setState's restore path.  Returns false if a
     *  slot for this id already exists. */
    bool createSessionClipWithId (const juce::Uuid& id);

    /** Tombstone the slot for the given clip id + flush any held notes
     *  to the output (via the SPSC FIFO so the audio thread observes
     *  the kill at a block boundary).  Returns false if no slot has
     *  this id.  After this call, isSessionClipPlaying / getClip... return
     *  defaults; getClipRegion returns nullptr. */
    bool removeSessionClip (const juce::Uuid& id);

    /** Schedule a future state flip for the named clip slot.
     *  beatTarget < 0 means "immediate" (fires at the start of the
     *  next render block).  beatTarget >= 0 is a transport-beat
     *  position; the flip fires inside the render block whose beat
     *  range contains it -- sample-accurate to +/-1 audio block.
     *
     *  When wantPlaying=true, the slot's local playhead rewinds to 0
     *  on launch (Bitwig/Ableton "launch restarts from start").
     *  Latest request per slot wins -- repeated bangs cancel prior
     *  queued actions.  No-op (drops silently) if the id is unknown
     *  or the FIFO is full. */
    void schedulePlayingClip (const juce::Uuid& clipId,
                              double            beatTarget,
                              bool              wantPlaying) noexcept;

    /** True if the slot is in Playing state (audio thread has fired
     *  the launch).  WaitingToStart slots return false.  Used by the
     *  session-view UI tick to reconcile the SessionClip's
     *  message-thread state. */
    bool   isSessionClipPlaying       (const juce::Uuid& clipId) const noexcept;

    /** Slot's current local playhead in beats.  Wraps modulo the
     *  clip's region length each loop.  Returns 0.0 for unknown ids. */
    double getSessionClipPositionBeats (const juce::Uuid& clipId) const noexcept;

    /** Clip's region length in beats (the loop period).  Returns 0.0
     *  for unknown ids.  Used by the UI for cosmetic progress bars. */
    double getSessionClipLengthBeats  (const juce::Uuid& clipId) const noexcept;

    /** Edge-trigger: returns true once per local-playhead wrap (used
     *  for followAction at clip end).  Consumes the wrap on read.
     *  Returns false for unknown ids. */
    bool   sessionClipWrappedSinceLastQuery (const juce::Uuid& clipId) noexcept;

    /** Live MidiNoteRegion pointer for the named clip.  Message-thread
     *  only -- the audio thread reads the region via the slot pointer
     *  it caches in render().  Returns nullptr for unknown ids. */
    MidiNoteRegion* getClipRegion (const juce::Uuid& clipId) noexcept;

    /** Reconcile slot states against a list of clip ids that
     *  SessionView still considers Playing or WaitingToStart.  Any
     *  slot not in `boundIds` and currently in a non-Stopped state
     *  is scheduled immediately stopped.  Called from
     *  SessionView::reconcileClipPlaying as the C.1 belt+suspenders
     *  silencer.  Message-thread only. */
    void reconcileBoundClips (const juce::Array<juce::Uuid>& boundIds) noexcept;

    //==========================================================================
    // Session-view mute / solo (parity with TrackerNode).

    bool getUserMuted() const noexcept { return userMuted_; }
    void setUserMuted (bool b) noexcept { userMuted_ = b; }
    bool getSoloed()    const noexcept { return soloed_; }
    void setSoloed    (bool b) noexcept { soloed_ = b; }

private:
    bool                createdPorts_   { false };

    double              currentSampleRate_ { 48000.0 };
    int                 currentBlockSize_  { 1024 };
    bool                lastPlayingState_  { false };
    bool                lastMutedState_    { false };

    /** Active entry table (atomic pointer).  Always non-null after
     *  ctor publishes an empty table. */
    using EntryList = std::vector<RegionEntry>;
    std::atomic<const EntryList*> activeEntries_ { nullptr };

    /** Trash entry + epoch counter -- mirrors MidiNoteRegion's discipline. */
    struct EntryTrash
    {
        std::unique_ptr<const EntryList> entries;
        std::uint64_t                    stampEpoch;
    };
    std::deque<EntryTrash>          entriesTrash_;
    std::atomic<std::uint64_t>      audioEpoch_ { 0 };

    /** Active param-binding table (atomic pointer).  Non-null after ctor
     *  publishes an empty table.  Same epoch-trash discipline as
     *  activeEntries_ -- swept via sweepBindingsTrash(). */
    using ParamList = std::vector<ParamBinding>;
    std::atomic<const ParamList*> activeParams_ { nullptr };
    struct ParamTrash
    {
        std::unique_ptr<const ParamList> bindings;
        std::uint64_t                    stampEpoch;
    };
    std::deque<ParamTrash>          paramsTrash_;

    /** Bitset of currently-held (channel, pitch) pairs -- one bit per
     *  pair.  16 channels x 128 pitches = 2048 bits = 256 bytes.
     *  Audio-thread-owned; never touched by message thread. */
    static constexpr int kHeldBits = 16 * 128;
    std::bitset<kHeldBits> held_ {};

    static int heldIndex (int channel /*1..16*/, int pitch /*0..127*/) noexcept
    {
        const int ch = juce::jlimit (1, 16, channel) - 1;
        const int p  = juce::jlimit (0, 127, pitch);
        return ch * 128 + p;
    }

    void emitAllNotesOff (juce::MidiBuffer& out, int sampleOffset) noexcept;

    /** Beat range covered by the current render block, in transport
     *  beats.  Returned in `blockStartBeat` / `blockEndBeat`.  Returns
     *  false if the host hasn't provided enough info (no playhead,
     *  zero bpm, etc.) -- caller emits no events in that case. */
    bool computeBlockBeats (int numSamples,
                              double& blockStartBeat,
                              double& blockEndBeat,
                              double& samplesPerBeat) noexcept;

    /** For one region entry + computed beat window, emit any
     *  NoteOn / NoteOff events whose sample-offset falls in
     *  [0, numSamples).  Handles loop wrap. */
    void emitRegionInBlock (const RegionEntry& entry,
                              double             blockStartBeat,
                              double             blockEndBeat,
                              double             samplesPerBeat,
                              int                numSamples,
                              juce::MidiBuffer&  out) noexcept;

    /** Emit the region's clip-local MIDI CC lanes for this block.  Walks
     *  the block at a fixed sub-block stride, maps each stride point to
     *  the region's local beat (loop modulo + startBeats), samples each
     *  CC lane, and emits a controllerEvent when the value changed since
     *  the last emit (per-(channel,cc) cache lastCcValue_).  Sample-
     *  accurate to the stride; lock-free + alloc-free. */
    void emitRegionCcInBlock (const RegionEntry& entry,
                              double             blockStartBeat,
                              double             samplesPerBeat,
                              int                numSamples,
                              juce::MidiBuffer&  out) noexcept;

    /** Apply the region's clip-local PARAM lanes for this block (coarse:
     *  one setValue at the block-start local beat).  For each isParam()
     *  lane in the region's snapshot, find the matching published
     *  ParamBinding and writeCoarseValue the sampled curve value.  No CC
     *  is emitted for these lanes.  Lock-free + alloc-free.  (ParamList /
     *  SessionClipSlot are defined further down -- the session-clip
     *  counterpart applyClipParamsInBlock is declared after them.) */
    void applyRegionParamsInBlock (const RegionEntry& entry,
                                   double             blockStartBeat,
                                   const ParamList*   params) noexcept;

    /** Find the published binding for a lane's (nodeId, paramId), or
     *  nullptr.  Linear scan -- binding counts are small (a handful per
     *  clip).  Audio-thread safe (reads immutable published strings). */
    static const ParamBinding* findParamBinding (const ParamList* params,
                                                 const juce::Uuid& nodeId,
                                                 const juce::String& paramId) noexcept;

    /** Sub-block CC sampling stride.  64 samples == ~1.3 ms at 48 kHz,
     *  matching AutomationEngine::kAutomationSubBlockStride.  CC is an
     *  inherently coarse 7-bit stream so this resolution is ample. */
    static constexpr int kCcSubBlockStride = 64;

    /** Per-(channel, cc) last-emitted value cache (audio-thread-owned,
     *  like held_).  -1 == nothing emitted yet.  Dedups CC across blocks
     *  AND across regions/clips sharing the same destination -- the MIDI
     *  CC output stream is per (channel, cc), so this is the correct key. */
    static constexpr int kCcCacheSize = 16 * 128;
    std::array<std::int8_t, (size_t) kCcCacheSize> lastCcValue_ {};

    static int ccCacheIndex (int channel /*1..16*/, int cc /*0..127*/) noexcept
    {
        const int ch = juce::jlimit (1, 16, channel) - 1;
        const int c  = juce::jlimit (0, 127, cc);
        return ch * 128 + c;
    }

    //==========================================================================
    // Session-view per-clip state.

    /** State machine values mirror SessionView::LiveState int order.
     *  Stored as int8 atomic so the audio thread can RMW without a
     *  C++ atomic<enum> reinterpret. */
    enum SlotState : std::int8_t
    {
        kStopped         = 0,
        kWaitingToStart  = 1,
        kPlaying         = 2,
        kWaitingToStop   = 3,
    };

    struct ClipPending
    {
        double beatTarget   = 0.0;
        bool   wantPlaying  = false;
        bool   valid        = false;
    };

    /** Per-slot session-clip state.  Allocated by the message thread on
     *  createSessionClip; never destroyed until the node is destroyed
     *  (tombstoned via `alive=false` instead so the audio thread can
     *  see a stable pointer).  Slot reuse: createSessionClip looks for
     *  a tombstoned slot before appending a new one, reinitialising
     *  fields in-place. */
    struct SessionClipSlot
    {
        juce::Uuid                       clipId;
        std::unique_ptr<MidiNoteRegion>  region;
        std::atomic<std::int8_t>         state         { kStopped };
        /** alive: audio thread should process this slot (drain pending,
         *  emit notes, flush held bits).  Stays true after
         *  removeSessionClip so the stop / held-notes-flush completes;
         *  the slot just sits in Stopped state forever after. */
        std::atomic<bool>                alive         { false };
        /** removed: message-thread-visible "deleted" marker.  Lookups
         *  (findSessionSlotIndex, getClipRegion) + persistence
         *  (getState) skip removed slots.  Audio thread ignores this
         *  flag -- it only consults alive. */
        std::atomic<bool>                removed       { false };
        /** Local playhead in source-beats, audio-thread-owned.  Stored
         *  atomic so the UI tick can read without UB; relaxed loads
         *  are fine for cosmetic progress display. */
        std::atomic<double>              localBeatPos  { 0.0 };
        /** Edge-trigger wrap flag.  Audio thread sets true on each
         *  modulo wrap; UI consumes on read. */
        std::atomic<bool>                wrappedFlag   { false };
        /** Per-clip held-notes bitset (1 bit per (channel,pitch)).
         *  Audio-thread-owned; mirrors MidiPlayerNode::held_'s shape.
         *  When the slot stops or is removed, we walk the set bits and
         *  emit matching NoteOff events. */
        static constexpr int kHeldBits = 16 * 128;
        std::bitset<kHeldBits>           held;
        /** Pending state-flip request, applied at block beat boundary
         *  by applyPendingClipForBlock.  drainClipLaunchFifo writes
         *  this; applyPendingClipForBlock clears `valid` once fired. */
        ClipPending                      pending;
    };

    /** OwnedArray of slot pointers.  Stable backing -- the audio
     *  thread loads `sessionClipCount_` (release-acquire) and walks
     *  slots [0, count).  Slot pointers themselves never move (the
     *  unique_ptr stays inside the OwnedArray entry for the lifetime
     *  of the node), so the audio thread can cache them across the
     *  block without a swap-out hazard. */
    juce::OwnedArray<SessionClipSlot> sessionClips_;
    std::atomic<int>                  sessionClipCount_ { 0 };

    /** SPSC launch FIFO: message thread pushes (slotIdx, beatTarget,
     *  wantPlaying); audio thread drains under no lock at block start.
     *  Latest request per slot wins -- drainClipLaunchFifo overwrites
     *  slot.pending on every drained entry. */
    struct ClipLaunchReq
    {
        int    slotIdx;
        double beatTarget;
        std::int8_t wantPlayingI8;
    };

    static constexpr int kClipLaunchFifoSize = 128;
    std::array<ClipLaunchReq, (size_t) kClipLaunchFifoSize> clipLaunchFifoStorage_ {};
    juce::AbstractFifo clipLaunchFifo_ { kClipLaunchFifoSize };

    /** Session-view user mute + solo.  Read + written by SessionView;
     *  not used by render() directly.  applyMuteAndSoloState reconciles
     *  effective Processor::setMuted from these. */
    bool userMuted_ = false;
    bool soloed_    = false;

    /** Audio-thread helper: drain the SPSC FIFO into per-slot pending
     *  actions.  Called once at the start of render() before the
     *  per-clip emit walk. */
    void drainClipLaunchFifo() noexcept;

    /** Audio-thread helper: walk each slot, apply any pending action
     *  whose beatTarget falls in the block's beat range, mutate
     *  state + rewind playhead on launch, emit NoteOff for held
     *  bits on stop.  No-op for slots without a valid pending
     *  action.  Sub-block sample offset for stop NoteOffs derived
     *  from (beatTarget - blockStartBeat) when beatTarget>=0; 0 for
     *  immediate (-1) targets. */
    void applyPendingClipForBlock (double            blockStartBeat,
                                   double            blockEndBeat,
                                   double            samplesPerBeat,
                                   int               numSamples,
                                   juce::MidiBuffer& out) noexcept;

    /** Audio-thread helper: per-Playing-slot emission for the block.
     *  Walks the slot's MidiNoteRegion snapshot in the local-beat
     *  window [localBeatPos, localBeatPos+blockBeats) MOD clipLen and
     *  emits NoteOn / NoteOff at sample-accurate offsets.  Advances
     *  the slot's localBeatPos.  Sets wrappedFlag on the wrap edge. */
    void emitSessionClipInBlock (SessionClipSlot& slot,
                                  double             blockBeats,
                                  double             samplesPerBeat,
                                  int                numSamples,
                                  juce::MidiBuffer&  out) noexcept;

    /** Session-clip counterpart of emitRegionCcInBlock.  Samples the
     *  clip region's CC lanes across the block at the sub-block stride,
     *  mapping each point into clip-local beats (modulo clipLen) from the
     *  slot's current localBeatPos.  Must run BEFORE emitSessionClipInBlock
     *  advances localBeatPos. */
    void emitSessionClipCcInBlock (SessionClipSlot& slot,
                                    double             blockBeats,
                                    double             samplesPerBeat,
                                    int                numSamples,
                                    juce::MidiBuffer&  out) noexcept;

    /** Session-clip counterpart of applyRegionParamsInBlock: samples the
     *  clip's param lanes at the slot's current localBeatPos (clip-local,
     *  srcOffset 0).  Coarse per block.  Must run BEFORE
     *  emitSessionClipInBlock advances localBeatPos. */
    void applyClipParamsInBlock (SessionClipSlot& slot,
                                 const ParamList* params) noexcept;

    /** Audio-thread helper: emit NoteOff for every held bit on `slot`
     *  at the supplied sample offset.  Clears the slot's held bitset. */
    void flushSlotHeldNotes (SessionClipSlot& slot,
                              int              sampleOffset,
                              juce::MidiBuffer& out) noexcept;

    /** Message-thread helper: find the index of a slot by clip id,
     *  or -1 if not found.  Linear scan over the count -- slot counts
     *  are small (~tens). */
    int findSessionSlotIndex (const juce::Uuid& clipId) const noexcept;

protected:
    void refreshPorts() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiPlayerNode)
};

} // namespace element
