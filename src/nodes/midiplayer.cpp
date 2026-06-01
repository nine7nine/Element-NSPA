// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/midiplayer.hpp"

#include <algorithm>
#include <cmath>

namespace element {

MidiPlayerNode::MidiPlayerNode()
    : MidiFilterNode (0)
{
    setName ("MIDI Player");

    /* Publish empty tables up front so the audio thread never observes
     * a null pointer. */
    activeEntries_.store (new EntryList(), std::memory_order_release);
    activeParams_.store (new ParamList(),  std::memory_order_release);
}

MidiPlayerNode::~MidiPlayerNode()
{
    if (auto* live = activeEntries_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
    if (auto* live = activeParams_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
}

void MidiPlayerNode::prepareToRender (double sampleRate, int maxBufferSize)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_  = maxBufferSize;
    lastPlayingState_  = false;
    lastMutedState_    = false;
    held_.reset();
    lastCcValue_.fill ((std::int8_t) -1);   // CC dedup cache: nothing emitted yet
}

void MidiPlayerNode::releaseResources()
{
    /* Nothing to free on the audio side; entries table is released
     * in the dtor. */
}

void MidiPlayerNode::refreshPorts()
{
    if (createdPorts_) return;

    PortList newPorts;
    newPorts.add (PortType::Midi, 0, 0, "midi_out", "MIDI Out", false);
    createdPorts_ = true;
    setPorts (newPorts);
}

void MidiPlayerNode::setState (const void* data, int size)
{
    /* Arrangement-lane usage publishes its region table from the
     * message thread on load (ArrangementView::flushLanesToSession),
     * so the wire state here only carries session-view per-clip slot
     * data.  Empty / absent state => no session-view clips, which is
     * the arrangement-only case.  Wire format: XML-serialised
     * juce::ValueTree shaped:
     *   <midiPlayer sessionClips="...">
     *     <sessionClip id="...">
     *       <region .../>     <!-- MidiNoteRegion::toValueTree shape -->
     *     </sessionClip>
     *     ...
     *   </midiPlayer> */
    /* Drop existing slots -- setState is a full-restore path. */
    for (int i = 0; i < sessionClips_.size(); ++i)
    {
        auto* slot = sessionClips_.getUnchecked (i);
        slot->alive.store (false, std::memory_order_release);
        slot->state.store (kStopped, std::memory_order_relaxed);
    }
    sessionClipCount_.store (0, std::memory_order_release);
    sessionClips_.clear (true);

    if (data == nullptr || size <= 0) return;

    auto xml = juce::parseXML (juce::String::createStringFromData (data, size));
    if (xml == nullptr) return;
    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.isValid()) return;

    auto clipsTree = root.getChildWithName ("sessionClips");
    if (! clipsTree.isValid()) return;

    for (int i = 0; i < clipsTree.getNumChildren(); ++i)
    {
        const auto sc = clipsTree.getChild (i);
        if (sc.getType().toString() != "sessionClip") continue;
        const juce::String idStr = sc.getProperty ("id").toString();
        if (idStr.isEmpty()) continue;
        const juce::Uuid clipId (idStr);

        auto* slot = new SessionClipSlot();
        slot->clipId = clipId;
        /* Restore region from child VT.  The region tree is named
         * after MidiNoteRegion::toValueTree's tag ("midiNoteRegion"),
         * not "region".  If absent or malformed, build a 4-beat
         * empty default so the slot stays playable. */
        const auto regTree = sc.getChildWithName ("midiNoteRegion");
        if (regTree.isValid())
            slot->region = MidiNoteRegion::fromValueTree (regTree);
        if (slot->region == nullptr)
        {
            slot->region = std::make_unique<MidiNoteRegion>();
            slot->region->id            = juce::Uuid();
            slot->region->positionBeats = 0.0;
            slot->region->lengthBeats   = 4.0;
            slot->region->looped        = true;
        }
        slot->state.store (kStopped, std::memory_order_relaxed);
        slot->localBeatPos.store (0.0, std::memory_order_relaxed);
        slot->wrappedFlag.store (false, std::memory_order_relaxed);
        slot->held.reset();
        slot->pending = ClipPending();
        sessionClips_.add (slot);
        slot->alive.store (true, std::memory_order_release);
    }
    sessionClipCount_.store (sessionClips_.size(), std::memory_order_release);
}

void MidiPlayerNode::getState (juce::MemoryBlock& block)
{
    juce::ValueTree root ("midiPlayer");
    juce::ValueTree clipsTree ("sessionClips");
    for (int i = 0; i < sessionClips_.size(); ++i)
    {
        auto* slot = sessionClips_.getUnchecked (i);
        if (! slot->alive.load (std::memory_order_acquire)) continue;
        if (slot->removed.load (std::memory_order_acquire)) continue;
        juce::ValueTree sc ("sessionClip");
        sc.setProperty ("id", slot->clipId.toString(), nullptr);
        if (slot->region != nullptr)
        {
            auto rt = slot->region->toValueTree();
            sc.appendChild (rt, nullptr);
        }
        clipsTree.appendChild (sc, nullptr);
    }
    root.appendChild (clipsTree, nullptr);
    /* No alive slots => no state to persist; matches the prior
     * arrangement-only contract where MidiPlayerNode emitted nothing. */
    if (clipsTree.getNumChildren() == 0)
    {
        block.setSize (0);
        return;
    }
    auto xml = root.toXmlString();
    block.setSize (0);
    block.append (xml.toRawUTF8(), (size_t) xml.getNumBytesAsUTF8());
}

//==============================================================================

void MidiPlayerNode::setBoundRegions (std::vector<RegionEntry> entries)
{
    /* Build a fresh immutable EntryList, atomic-swap it in, push the
     * displaced one onto the trash deque.  Audio thread observes the
     * swap on its next snapshot load. */
    auto next = std::make_unique<EntryList> (std::move (entries));
    const EntryList* raw   = next.release();
    const auto       stamp = audioEpoch_.load (std::memory_order_acquire);
    const EntryList* old   = activeEntries_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        entriesTrash_.push_back (EntryTrash {
            std::unique_ptr<const EntryList> (old), stamp });
}

void MidiPlayerNode::setBoundParams (std::vector<ParamBinding> bindings)
{
    auto next = std::make_unique<ParamList> (std::move (bindings));
    const ParamList* raw   = next.release();
    const auto       stamp = audioEpoch_.load (std::memory_order_acquire);
    const ParamList* old   = activeParams_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        paramsTrash_.push_back (ParamTrash {
            std::unique_ptr<const ParamList> (old), stamp });
}

void MidiPlayerNode::sweepBindingsTrash() noexcept
{
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);
    while (! entriesTrash_.empty()
           && entriesTrash_.front().stampEpoch < safeEpoch)
        entriesTrash_.pop_front();
    while (! paramsTrash_.empty()
           && paramsTrash_.front().stampEpoch < safeEpoch)
        paramsTrash_.pop_front();
}

//==============================================================================
// Session-view per-clip API.

int MidiPlayerNode::findSessionSlotIndex (const juce::Uuid& clipId) const noexcept
{
    const int n = sessionClips_.size();
    for (int i = 0; i < n; ++i)
    {
        auto* s = sessionClips_.getUnchecked (i);
        if (! s->alive.load (std::memory_order_acquire)) continue;
        if (s->removed.load (std::memory_order_acquire)) continue;
        if (s->clipId == clipId) return i;
    }
    return -1;
}

juce::Uuid MidiPlayerNode::createSessionClip()
{
    juce::Uuid newId;
    if (createSessionClipWithId (newId))
        return newId;
    return juce::Uuid::null();
}

bool MidiPlayerNode::createSessionClipWithId (const juce::Uuid& id)
{
    if (id.isNull()) return false;
    if (findSessionSlotIndex (id) >= 0) return false;

    /* Always append a fresh slot rather than reuse tombstoned ones.
     * Tombstone reuse would race the audio thread's held-notes flush
     * on the prior occupant -- between the message thread issuing the
     * stop FIFO entry and the audio thread draining it, the slot
     * would already have been re-initialised under the new clip,
     * leaving the old clip's held bits as ghost notes.  Bounded
     * growth: each deleted clip leaves ~1 KB behind for the session
     * lifetime.  Acceptable for the typical use case. */
    auto* slot = new SessionClipSlot();
    sessionClips_.add (slot);
    slot->clipId = id;
    slot->region = std::make_unique<MidiNoteRegion>();
    slot->region->id            = juce::Uuid();
    slot->region->positionBeats = 0.0;
    slot->region->lengthBeats   = 4.0;   /* one bar at 4/4 default */
    slot->region->looped        = true;
    slot->region->name          = "MIDI clip";
    slot->state.store (kStopped, std::memory_order_relaxed);
    slot->localBeatPos.store (0.0, std::memory_order_relaxed);
    slot->wrappedFlag.store (false, std::memory_order_relaxed);
    /* Release after fields are populated so the audio thread sees a
     * fully-initialised slot. */
    slot->alive.store (true, std::memory_order_release);

    /* sessionClipCount_ is monotonic in count, never shrinks.  It
     * mirrors sessionClips_.size() but is the value the audio thread
     * reads to bound its walk.  Bump after the slot fields land so a
     * concurrent render finds a stable slot. */
    sessionClipCount_.store (sessionClips_.size(), std::memory_order_release);
    return true;
}

bool MidiPlayerNode::removeSessionClip (const juce::Uuid& id)
{
    const int idx = findSessionSlotIndex (id);
    if (idx < 0) return false;

    /* Two-phase removal:
     *  1. Schedule an immediate stop through the FIFO so the audio
     *     thread fires the held-notes flush + sets state=Stopped
     *     on its next render.  Slot stays alive=true here so the
     *     FIFO drain + applyPendingClipForBlock can find + process it.
     *  2. Set removed=true (release).  Message-thread-visible
     *     lookups (findSessionSlotIndex, getClipRegion) skip the
     *     slot immediately.  Persistence (getState) also skips.
     *     Audio thread does NOT consult `removed` -- it keeps
     *     processing alive slots so the pending stop fires.
     *
     * The slot's region memory persists for the rest of the session.
     * Bounded growth -- ~1 KB per deleted clip; acceptable. */
    schedulePlayingClip (id, -1.0, false);
    auto* slot = sessionClips_.getUnchecked (idx);
    slot->removed.store (true, std::memory_order_release);
    return true;
}

void MidiPlayerNode::schedulePlayingClip (const juce::Uuid& clipId,
                                            double            beatTarget,
                                            bool              wantPlaying) noexcept
{
    const int slotIdx = findSessionSlotIndex (clipId);
    if (slotIdx < 0) return;

    int start1, size1, start2, size2;
    clipLaunchFifo_.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 <= 0) return;   // FIFO full -- drop silently (B.4 parity)
    clipLaunchFifoStorage_[(size_t) start1] = {
        slotIdx, beatTarget, wantPlaying ? (std::int8_t) 1 : (std::int8_t) 0
    };
    clipLaunchFifo_.finishedWrite (1);
}

bool MidiPlayerNode::isSessionClipPlaying (const juce::Uuid& clipId) const noexcept
{
    const int idx = findSessionSlotIndex (clipId);
    if (idx < 0) return false;
    return sessionClips_.getUnchecked (idx)->state.load (std::memory_order_acquire) == kPlaying;
}

double MidiPlayerNode::getSessionClipPositionBeats (const juce::Uuid& clipId) const noexcept
{
    const int idx = findSessionSlotIndex (clipId);
    if (idx < 0) return 0.0;
    return sessionClips_.getUnchecked (idx)->localBeatPos.load (std::memory_order_relaxed);
}

double MidiPlayerNode::getSessionClipLengthBeats (const juce::Uuid& clipId) const noexcept
{
    const int idx = findSessionSlotIndex (clipId);
    if (idx < 0) return 0.0;
    auto* slot = sessionClips_.getUnchecked (idx);
    return slot->region != nullptr ? slot->region->lengthBeats : 0.0;
}

bool MidiPlayerNode::sessionClipWrappedSinceLastQuery (const juce::Uuid& clipId) noexcept
{
    const int idx = findSessionSlotIndex (clipId);
    if (idx < 0) return false;
    auto* slot = sessionClips_.getUnchecked (idx);
    return slot->wrappedFlag.exchange (false, std::memory_order_acq_rel);
}

MidiNoteRegion* MidiPlayerNode::getClipRegion (const juce::Uuid& clipId) noexcept
{
    const int idx = findSessionSlotIndex (clipId);
    if (idx < 0) return nullptr;
    return sessionClips_.getUnchecked (idx)->region.get();
}

void MidiPlayerNode::reconcileBoundClips (const juce::Array<juce::Uuid>& boundIds) noexcept
{
    /* Walk slots; anything alive + non-Stopped that's NOT in boundIds
     * gets an immediate-stop schedule.  Mirrors C.1's belt+suspenders
     * shape for SessionView. */
    for (int i = 0; i < sessionClips_.size(); ++i)
    {
        auto* slot = sessionClips_.getUnchecked (i);
        if (! slot->alive.load (std::memory_order_acquire)) continue;
        const auto s = slot->state.load (std::memory_order_acquire);
        if (s == kStopped) continue;
        if (boundIds.contains (slot->clipId)) continue;
        schedulePlayingClip (slot->clipId, -1.0, false);
    }
}

//==============================================================================

bool MidiPlayerNode::computeBlockBeats (int    numSamples,
                                          double& blockStartBeat,
                                          double& blockEndBeat,
                                          double& samplesPerBeat) noexcept
{
    blockStartBeat = -1.0;
    blockEndBeat   = -1.0;
    samplesPerBeat = 0.0;

    auto* ph = getPlayHead();
    if (ph == nullptr) return false;
    auto pos = ph->getPosition();
    if (! pos.hasValue()) return false;

    /* BPM is required to convert sample-offset for sub-block events.
     * If absent default to 120 -- consistent with ArrangementView /
     * AudioClipNode fallback. */
    double bpm = 120.0;
    if (auto b = pos->getBpm())
        bpm = (double) *b;
    if (bpm <= 0.0) return false;

    if (currentSampleRate_ <= 0.0) return false;
    samplesPerBeat = currentSampleRate_ * 60.0 / bpm;

    /* Prefer getPpqPosition (already in beats) over time-in-samples
     * which would need a beat-of-origin reference. */
    if (auto ppq = pos->getPpqPosition())
    {
        blockStartBeat = (double) *ppq;
        blockEndBeat   = blockStartBeat + (double) numSamples / samplesPerBeat;
        return true;
    }

    if (auto inSamples = pos->getTimeInSamples())
    {
        blockStartBeat = (double) *inSamples / samplesPerBeat;
        blockEndBeat   = blockStartBeat + (double) numSamples / samplesPerBeat;
        return true;
    }

    return false;
}

void MidiPlayerNode::emitAllNotesOff (juce::MidiBuffer& out,
                                        int               sampleOffset) noexcept
{
    if (held_.none()) return;
    for (int i = 0; i < kHeldBits; ++i)
    {
        if (! held_.test ((std::size_t) i)) continue;
        const int channel = (i / 128) + 1;   /* back to 1-based */
        const int pitch   = i % 128;
        out.addEvent (juce::MidiMessage::noteOff (channel, pitch),
                      sampleOffset);
        held_.reset ((std::size_t) i);
    }
}

void MidiPlayerNode::drainClipLaunchFifo() noexcept
{
    /* Drain into per-slot pending action.  Latest entry per slot
     * wins -- repeated bangs cancel prior queued actions. */
    int start1, size1, start2, size2;
    clipLaunchFifo_.prepareToRead (kClipLaunchFifoSize, start1, size1, start2, size2);
    const int total = size1 + size2;
    if (total <= 0) return;

    for (int blk = 0; blk < 2; ++blk)
    {
        const int s = (blk == 0) ? start1 : start2;
        const int n = (blk == 0) ? size1  : size2;
        for (int k = 0; k < n; ++k)
        {
            const auto& req = clipLaunchFifoStorage_[(size_t) (s + k)];
            if (req.slotIdx < 0 || req.slotIdx >= sessionClips_.size()) continue;
            auto* slot = sessionClips_.getUnchecked (req.slotIdx);
            slot->pending.beatTarget  = req.beatTarget;
            slot->pending.wantPlaying = (req.wantPlayingI8 != 0);
            slot->pending.valid       = true;
        }
    }
    clipLaunchFifo_.finishedRead (total);
}

void MidiPlayerNode::flushSlotHeldNotes (SessionClipSlot& slot,
                                          int               sampleOffset,
                                          juce::MidiBuffer& out) noexcept
{
    if (! slot.held.any()) return;
    const int n = (int) SessionClipSlot::kHeldBits;
    for (int i = 0; i < n; ++i)
    {
        if (! slot.held.test ((std::size_t) i)) continue;
        const int channel = (i / 128) + 1;
        const int pitch   = i % 128;
        out.addEvent (juce::MidiMessage::noteOff (channel, pitch), sampleOffset);
        slot.held.reset ((std::size_t) i);
    }
}

void MidiPlayerNode::applyPendingClipForBlock (double            blockStartBeat,
                                                 double            blockEndBeat,
                                                 double            samplesPerBeat,
                                                 int               numSamples,
                                                 juce::MidiBuffer& out) noexcept
{
    const int n = sessionClipCount_.load (std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
    {
        auto* slot = sessionClips_.getUnchecked (i);
        if (! slot->alive.load (std::memory_order_acquire)) continue;
        if (! slot->pending.valid) continue;

        const double tgt = slot->pending.beatTarget;
        /* Immediate (-1) OR target falls within / before the block:
         * fire.  Otherwise leave queued for a later block.  Symmetric
         * to TrackerNode::applyPendingForBlock. */
        const bool fire = (tgt < 0.0) || (tgt < blockEndBeat);
        if (! fire) continue;

        /* Sub-block offset for the transition's emit edge.  Clamped
         * to the buffer range; <0 (block start) for immediates. */
        int sampleOff = 0;
        if (tgt >= 0.0)
        {
            const double rel = juce::jmax (0.0, tgt - blockStartBeat);
            sampleOff = juce::jlimit (0, numSamples - 1,
                                      (int) std::round (rel * samplesPerBeat));
        }

        if (slot->pending.wantPlaying)
        {
            /* Launch.  Rewind playhead; clear wrap edge; flush any
             * residual held bits at the launch edge so a freshly
             * banged clip doesn't re-emit a NoteOn against an
             * already-set bit (which would skip the held bookkeeping
             * + leave a stuck note when the clip later stops). */
            flushSlotHeldNotes (*slot, sampleOff, out);
            slot->localBeatPos.store (0.0, std::memory_order_relaxed);
            slot->wrappedFlag.store (false, std::memory_order_relaxed);
            slot->state.store (kPlaying, std::memory_order_release);
        }
        else
        {
            /* Stop.  Emit NoteOff for held bits at the transition
             * offset + clear the bitset.  State flips after the
             * flush so any concurrent UI tick observing kStopped
             * doesn't race the held emission. */
            flushSlotHeldNotes (*slot, sampleOff, out);
            slot->state.store (kStopped, std::memory_order_release);
        }
        slot->pending.valid = false;
    }
}

void MidiPlayerNode::emitSessionClipInBlock (SessionClipSlot& slot,
                                              double             blockBeats,
                                              double             samplesPerBeat,
                                              int                numSamples,
                                              juce::MidiBuffer&  out) noexcept
{
    if (slot.region == nullptr) return;
    const double clipLen = slot.region->lengthBeats;
    if (clipLen <= 0.0) return;

    const auto* snap = slot.region->loadSnapshot();
    if (snap == nullptr) return;
    slot.region->advanceAudioEpoch();

    const double localStart = slot.localBeatPos.load (std::memory_order_relaxed);
    const double localEnd   = localStart + blockBeats;

    /* Emit a single source-beat window [lo, hi); window mapped
     * to sample offsets relative to (localStart base).
     * srcOffset is supported in MidiNoteRegion (left-edge trim
     * for arrangement regions); for session clips the source
     * authoring window IS the playable window so srcOffset=0. */
    auto emitWindow = [&] (double lo, double hi, int baseSampleOffset) noexcept
    {
        for (const auto& n : *snap)
        {
            const double noteEnd = n.onBeat + n.lengthBeats;
            if (n.onBeat >= hi) break;          // snap is sorted
            if (noteEnd  <= lo) continue;

            if (n.onBeat >= lo && n.onBeat < hi)
            {
                const double beatDelta = n.onBeat - lo;
                int off = baseSampleOffset
                        + (int) std::round (beatDelta * samplesPerBeat);
                off = juce::jlimit (0, numSamples - 1, off);
                out.addEvent (juce::MidiMessage::noteOn (
                                  juce::jlimit (1, 16, n.channel),
                                  juce::jlimit (0, 127, n.pitch),
                                  (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                              off);
                slot.held.set ((std::size_t) heldIndex (n.channel, n.pitch));
            }

            /* Clamp NoteOff to clip end for the last iteration's
             * trailing notes -- the wrap path below will re-fire
             * the NoteOn on the next iteration if the clip restarts. */
            const double offBeat = juce::jmin (noteEnd, clipLen);
            if (offBeat > lo && offBeat <= hi)
            {
                const double beatDelta = offBeat - lo;
                int off = baseSampleOffset
                        + (int) std::round (beatDelta * samplesPerBeat);
                off = juce::jlimit (0, numSamples - 1, off);
                out.addEvent (juce::MidiMessage::noteOff (
                                  juce::jlimit (1, 16, n.channel),
                                  juce::jlimit (0, 127, n.pitch)),
                              off);
                slot.held.reset ((std::size_t) heldIndex (n.channel, n.pitch));
            }
        }
    };

    if (localEnd <= clipLen)
    {
        emitWindow (localStart, localEnd, 0);
        slot.localBeatPos.store (localEnd, std::memory_order_relaxed);
    }
    else
    {
        /* Loop wrap: emit [localStart, clipLen) then [0, localEnd-clipLen). */
        const double tailLen = clipLen - localStart;
        emitWindow (localStart, clipLen, 0);
        const int wrapBaseOffset = (int) std::round (tailLen * samplesPerBeat);
        const double wrapEnd = localEnd - clipLen;
        /* Flush held notes from the prior loop iteration so the new
         * iteration starts clean.  Without this, notes whose lengths
         * extend past clipLen carry over into the next iteration's
         * NoteOn paint without an OFF in between. */
        flushSlotHeldNotes (slot, wrapBaseOffset, out);
        emitWindow (0.0, wrapEnd, wrapBaseOffset);
        slot.localBeatPos.store (wrapEnd, std::memory_order_relaxed);
        slot.wrappedFlag.store (true, std::memory_order_release);
    }
}

void MidiPlayerNode::render (RenderContext& rc)
{
    if (rc.midi.getNumBuffers() <= 0) return;
    juce::MidiBuffer* const out = rc.midi.getWriteBuffer (0);
    if (out == nullptr) return;

    /* Audio thread always clears its output buffer at block start --
     * we don't forward anything from upstream (no MIDI input on this
     * node).  refreshPorts() reserves output port 0 only.  Reserve
     * a generous initial capacity so the addEvent hot path doesn't
     * realloc under the busiest plausible block (32 regions x ~16
     * sub-block events each).  juce::MidiBuffer::ensureSize is a
     * no-op once the underlying buffer is at least the requested
     * size, so this is one branch on the steady-state path. */
    out->ensureSize (2048);
    out->clear();

    const int nsamples = rc.audio.getNumSamples();

    /* Bump epoch FIRST so the message-thread sweep can prove every
     * stamp <= prevEpoch is safe to free.  Subsequent loads in this
     * block see the bumped value but the snapshot pointer they got
     * was published BEFORE the bump, so it stays valid. */
    audioEpoch_.fetch_add (1, std::memory_order_acq_rel);

    /* Helper: flush held notes across all sources -- arrangement
     * entries (held_) AND every session-clip slot.  Used on mute /
     * transport-stop edges so downstream synth voices don't hang. */
    auto flushAllSources = [this] (juce::MidiBuffer& dst) noexcept
    {
        if (held_.any())
            emitAllNotesOff (dst, 0);
        const int sc = sessionClipCount_.load (std::memory_order_acquire);
        for (int i = 0; i < sc; ++i)
        {
            auto* slot = sessionClips_.getUnchecked (i);
            if (! slot->alive.load (std::memory_order_acquire)) continue;
            flushSlotHeldNotes (*slot, 0, dst);
        }
    };

    /* Mute gate.  The graph-builder mute path applies gain ramps to
     * AUDIO buffers only -- MIDI output is unaffected unless this
     * node consults isMuted() itself.  Same shape as transport stop:
     * on the mute transition, flush every held NoteOn so downstream
     * synth voices don't hang.  Session-clip slot state is preserved
     * across mute (Bitwig-style "playing but silenced"); unmuting
     * resumes from the prior localBeatPos. */
    const bool muted = isMuted();
    if (muted)
    {
        if (! lastMutedState_)
            flushAllSources (*out);
        lastMutedState_  = true;
        lastPlayingState_ = false;
        return;
    }
    lastMutedState_ = false;

    /* Read transport state once at the top.  No playhead / no
     * play position means "stopped" -- emit all-notes-off for any
     * lingering held pairs and return.  Session-clip slot state is
     * preserved across transport stop; pressing play resumes Playing
     * slots from their saved localBeatPos. */
    bool wantPlaying = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
            wantPlaying = pos->getIsPlaying();
    }

    if (! wantPlaying)
    {
        if (lastPlayingState_)
            flushAllSources (*out);
        lastPlayingState_ = false;
        return;
    }
    lastPlayingState_ = true;

    double blockStartBeat = 0.0;
    double blockEndBeat   = 0.0;
    double samplesPerBeat = 0.0;
    if (! computeBlockBeats (nsamples, blockStartBeat, blockEndBeat, samplesPerBeat))
    {
        /* Transport says playing but no PPQ/sample info -- conservative
         * behaviour: hold the previous state (do nothing), don't
         * leak NoteOns. */
        return;
    }

    /* -------- Arrangement-side region emission -------- */
    const EntryList* entries = activeEntries_.load (std::memory_order_acquire);
    const ParamList* params  = activeParams_.load (std::memory_order_acquire);
    if (entries != nullptr)
    {
        for (const auto& entry : *entries)
        {
            if (entry.region == nullptr || entry.lengthBeats <= 0.0)
                continue;

            const double regionStart = entry.positionBeats;
            const double regionEnd   = entry.positionBeats + entry.lengthBeats;

            if (entry.looped)
            {
                emitRegionInBlock (entry, blockStartBeat, blockEndBeat,
                                   samplesPerBeat, nsamples, *out);
                emitRegionCcInBlock (entry, blockStartBeat, samplesPerBeat,
                                     nsamples, *out);
                applyRegionParamsInBlock (entry, blockStartBeat, params);
                continue;
            }

            const bool blockTouchesRegion =
                blockEndBeat > regionStart && blockStartBeat < regionEnd;
            if (! blockTouchesRegion) continue;

            emitRegionInBlock (entry, blockStartBeat, blockEndBeat,
                               samplesPerBeat, nsamples, *out);
            emitRegionCcInBlock (entry, blockStartBeat, samplesPerBeat,
                                 nsamples, *out);
            applyRegionParamsInBlock (entry, blockStartBeat, params);
        }
    }

    /* -------- Session-view per-clip emission -------- */
    drainClipLaunchFifo();
    applyPendingClipForBlock (blockStartBeat, blockEndBeat,
                              samplesPerBeat, nsamples, *out);

    const double blockBeats = blockEndBeat - blockStartBeat;
    const int sclips = sessionClipCount_.load (std::memory_order_acquire);
    for (int i = 0; i < sclips; ++i)
    {
        auto* slot = sessionClips_.getUnchecked (i);
        if (! slot->alive.load (std::memory_order_acquire)) continue;
        if (slot->state.load (std::memory_order_acquire) != kPlaying) continue;
        /* CC + param first -- both read localBeatPos as the block-start
         * position, which emitSessionClipInBlock then advances. */
        emitSessionClipCcInBlock (*slot, blockBeats, samplesPerBeat,
                                  nsamples, *out);
        applyClipParamsInBlock (*slot, params);
        emitSessionClipInBlock (*slot, blockBeats, samplesPerBeat,
                                nsamples, *out);
    }
}

void MidiPlayerNode::emitRegionInBlock (const RegionEntry& entry,
                                          double             blockStartBeat,
                                          double             blockEndBeat,
                                          double             samplesPerBeat,
                                          int                numSamples,
                                          juce::MidiBuffer&  out) noexcept
{
    /* Advance the region's audio epoch FIRST so its trash sweep can
     * reclaim displaced note + CC snapshots -- before the empty-notes
     * early return, so a CC-only region (no notes) still sweeps. */
    entry.region->advanceAudioEpoch();

    const auto* snap = entry.region->loadSnapshot();
    if (snap == nullptr || snap->empty()) return;

    const double regionStart = entry.positionBeats;
    const double regionLen   = entry.lengthBeats;
    const double regionEnd   = regionStart + regionLen;
    const double srcOffset   = entry.startBeats;
    /* Loop period: explicit when non-zero, else fall back to the
     * region's drawn length (pre-fix sessions + non-looped regions
     * never read this).  Lets the user drag the right edge to extend
     * the number of repeats without stretching the loop pattern. */
    const double loopPeriod  = (entry.looped && entry.loopLengthBeats > 0.0)
                                  ? entry.loopLengthBeats
                                  : regionLen;

    /* Map the block's transport beat range into the region's local
     * beat coordinates.  For looped regions, modulo into [0, loopPeriod).
     * srcOffset is applied AFTER the modulo so the loop wraps the
     * audible slice [srcOffset, srcOffset+loopPeriod) of the source. */
    auto toLocal = [regionStart, loopPeriod, looped = entry.looped]
                   (double beat) noexcept -> double {
        const double lb = beat - regionStart;
        if (! looped) return lb;
        if (loopPeriod <= 0.0) return lb;
        double m = std::fmod (lb, loopPeriod);
        if (m < 0.0) m += loopPeriod;
        return m;
    };

    /* localStart / localEnd are in TIMELINE-LOCAL beats (i.e. distance
     * from regionStart on the timeline).  The note list is in SOURCE
     * coordinates, so when comparing a note's onBeat we add srcOffset
     * to localStart / localEnd before the compare.  Sample-offset for
     * an emitted event then uses (note.onBeat - srcLocalStart) where
     * srcLocalStart = localStart + srcOffset. */
    double localStart, localEnd;
    if (entry.looped)
    {
        localStart = toLocal (blockStartBeat);
        localEnd   = localStart + (blockEndBeat - blockStartBeat);
        /* If localEnd wraps past loopPeriod, we have to split into
         * two windows: [localStart, loopPeriod) + [0, localEnd-loopPeriod).
         * Implementation: just call the inner loop twice with adjusted
         * sample offsets. */
        if (localEnd > loopPeriod)
        {
            const double tailLen = loopPeriod - localStart;

            /* First chunk: timeline-local [localStart, loopPeriod).
             * Source-local: [srcOffset + localStart, srcOffset + loopPeriod). */
            const double srcLo1 = srcOffset + localStart;
            const double srcHi1 = srcOffset + loopPeriod;
            for (const auto& n : *snap)
            {
                const double noteEnd = n.onBeat + n.lengthBeats;
                if (n.onBeat >= srcHi1 || noteEnd <= srcOffset) continue;

                if (n.onBeat >= srcLo1 && n.onBeat < srcHi1)
                {
                    const double beatDelta = n.onBeat - srcLo1;
                    int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOn (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch),
                                       (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                                  sampleOffset);
                    held_.set ((std::size_t) heldIndex (n.channel, n.pitch));
                }

                if (noteEnd > srcLo1 && noteEnd <= srcHi1)
                {
                    const double beatDelta = noteEnd - srcLo1;
                    int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOff (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch)),
                                  sampleOffset);
                    held_.reset ((std::size_t) heldIndex (n.channel, n.pitch));
                }
            }

            /* Second chunk: timeline-local [0, localEnd-loopPeriod).
             * Source-local: [srcOffset, srcOffset + (localEnd-loopPeriod)).
             * Sample offset begins at tailLen-into-the-block. */
            const double wrapEnd = localEnd - loopPeriod;
            const double srcLo2  = srcOffset;
            const double srcHi2  = srcOffset + wrapEnd;
            const int wrapBaseOffset =
                (int) std::round (tailLen * samplesPerBeat);
            for (const auto& n : *snap)
            {
                const double noteEnd = n.onBeat + n.lengthBeats;
                if (noteEnd <= srcLo2) continue;

                if (n.onBeat >= srcLo2 && n.onBeat < srcHi2)
                {
                    const double beatDelta = n.onBeat - srcLo2;
                    int sampleOffset = wrapBaseOffset
                                     + (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOn (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch),
                                       (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                                  sampleOffset);
                    held_.set ((std::size_t) heldIndex (n.channel, n.pitch));
                }

                if (noteEnd > srcLo2 && noteEnd < srcHi2)
                {
                    const double beatDelta = noteEnd - srcLo2;
                    int sampleOffset = wrapBaseOffset
                                     + (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOff (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch)),
                                  sampleOffset);
                    held_.reset ((std::size_t) heldIndex (n.channel, n.pitch));
                }
            }
            return;
        }
        /* fall through to common emission with looped-mapped window */
    }
    else
    {
        localStart = blockStartBeat - regionStart;
        localEnd   = blockEndBeat   - regionStart;
        if (localEnd <= 0.0) return;
    }

    /* Common emission for non-looped + looped-no-wrap.  Translate the
     * timeline-local window to source-local for comparison against
     * note onBeats.  Notes are sorted by (onBeat, pitch) so the early
     * break on onBeat >= srcLocalEnd remains correct.
     *
     * srcRegionEnd bounds the audible source slice -- for non-looped
     * regions it's the right edge of the region; for looped regions
     * it's the end of one loop iteration (loopPeriod, not lengthBeats)
     * so notes past the loop boundary don't double-fire from the
     * within-loop block. */
    const double srcLocalStart = srcOffset + localStart;
    const double srcLocalEnd   = srcOffset + localEnd;
    const double srcRegionEnd  = srcOffset + (entry.looped ? loopPeriod : regionLen);

    for (const auto& n : *snap)
    {
        const double noteEnd = n.onBeat + n.lengthBeats;
        if (n.onBeat >= srcLocalEnd) break;
        if (noteEnd  <= srcLocalStart) continue;

        if (n.onBeat >= srcLocalStart
            && n.onBeat <  srcLocalEnd
            && n.onBeat >= srcOffset
            && n.onBeat <  srcRegionEnd)
        {
            const double beatDelta = n.onBeat - srcLocalStart;
            int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
            sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
            out.addEvent (juce::MidiMessage::noteOn (
                               juce::jlimit (1, 16, n.channel),
                               juce::jlimit (0, 127, n.pitch),
                               (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                          sampleOffset);
            held_.set ((std::size_t) heldIndex (n.channel, n.pitch));
        }

        /* NoteOff clamped to the region's source-end so notes extending
         * past the trimmed right edge fire off at the edge (non-looped). */
        const double offBeat = entry.looped
            ? noteEnd
            : juce::jmin (noteEnd, srcRegionEnd);
        if (offBeat > srcLocalStart && offBeat <= srcLocalEnd)
        {
            const double beatDelta = offBeat - srcLocalStart;
            int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
            sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
            out.addEvent (juce::MidiMessage::noteOff (
                               juce::jlimit (1, 16, n.channel),
                               juce::jlimit (0, 127, n.pitch)),
                          sampleOffset);
            held_.reset ((std::size_t) heldIndex (n.channel, n.pitch));
        }
    }
}

void MidiPlayerNode::emitRegionCcInBlock (const RegionEntry& entry,
                                          double             blockStartBeat,
                                          double             samplesPerBeat,
                                          int                numSamples,
                                          juce::MidiBuffer&  out) noexcept
{
    const auto* cc = entry.region->loadCcSnapshot();
    if (cc == nullptr || cc->empty() || samplesPerBeat <= 0.0) return;

    const double regionStart = entry.positionBeats;
    const double regionLen   = entry.lengthBeats;
    const double srcOffset   = entry.startBeats;
    const double loopPeriod  = (entry.looped && entry.loopLengthBeats > 0.0)
                                  ? entry.loopLengthBeats
                                  : regionLen;

    /* Walk the block at the sub-block stride; map each stride point's
     * transport beat to the region's source-local beat (loop modulo +
     * left-trim offset), sample each lane, emit on change. */
    for (int off = 0; off < numSamples; off += kCcSubBlockStride)
    {
        const double beat = blockStartBeat + (double) off / samplesPerBeat;
        double lb = beat - regionStart;
        if (entry.looped)
        {
            if (loopPeriod <= 0.0) continue;
            lb = std::fmod (lb, loopPeriod);
            if (lb < 0.0) lb += loopPeriod;
        }
        else if (lb < 0.0 || lb >= regionLen)
        {
            continue;   // stride point outside the (non-looped) region
        }

        const double srcLocal = srcOffset + lb;
        for (const auto& lane : *cc)
        {
            /* Param-target lanes are applied to a node parameter, not
             * emitted as MIDI CC -- skip them here (they would otherwise
             * spam CC on their default cc/channel).  #11 Phase 2b adds the
             * param-apply path. */
            if (lane.isParam()) continue;
            const int ch  = juce::jlimit (1, 16, lane.channel);
            const int v   = lane.ccValueAtBeats (srcLocal);
            const int idx = ccCacheIndex (ch, lane.ccNumber);
            if ((int) lastCcValue_[(size_t) idx] != v)
            {
                out.addEvent (juce::MidiMessage::controllerEvent (
                                  ch, juce::jlimit (0, 127, lane.ccNumber), v),
                              off);
                lastCcValue_[(size_t) idx] = (std::int8_t) v;
            }
        }
    }
}

const MidiPlayerNode::ParamBinding* MidiPlayerNode::findParamBinding (
    const ParamList* params, const juce::Uuid& nodeId,
    const juce::String& paramId) noexcept
{
    if (params == nullptr) return nullptr;
    for (const auto& b : *params)
        if (b.nodeId == nodeId && b.paramId == paramId)
            return &b;
    return nullptr;
}

void MidiPlayerNode::applyRegionParamsInBlock (const RegionEntry& entry,
                                               double             blockStartBeat,
                                               const ParamList*   params) noexcept
{
    if (params == nullptr || params->empty()) return;
    const auto* cc = entry.region->loadCcSnapshot();
    if (cc == nullptr || cc->empty()) return;

    /* Map the block-start transport beat to the region's source-local
     * beat (loop modulo + left-trim offset) -- identical to the first
     * stride of emitRegionCcInBlock.  Coarse: one sample per block. */
    const double regionLen  = entry.lengthBeats;
    const double loopPeriod = (entry.looped && entry.loopLengthBeats > 0.0)
                                  ? entry.loopLengthBeats
                                  : regionLen;
    double lb = blockStartBeat - entry.positionBeats;
    if (entry.looped)
    {
        if (loopPeriod <= 0.0) return;
        lb = std::fmod (lb, loopPeriod);
        if (lb < 0.0) lb += loopPeriod;
    }
    else if (lb < 0.0 || lb >= regionLen)
    {
        return;   // block start outside the (non-looped) region
    }
    const double srcLocal = entry.startBeats + lb;

    for (const auto& lane : *cc)
    {
        if (! lane.isParam()) continue;
        if (const auto* b = findParamBinding (params, lane.paramNodeId, lane.paramId))
            b->target.writeCoarseValue ((float) lane.valueAtBeats (srcLocal));
    }
}

void MidiPlayerNode::applyClipParamsInBlock (SessionClipSlot& slot,
                                             const ParamList* params) noexcept
{
    if (params == nullptr || params->empty() || slot.region == nullptr) return;
    const auto* cc = slot.region->loadCcSnapshot();
    if (cc == nullptr || cc->empty()) return;
    const double clipLen = slot.region->lengthBeats;
    if (clipLen <= 0.0) return;

    double lb = std::fmod (slot.localBeatPos.load (std::memory_order_relaxed), clipLen);
    if (lb < 0.0) lb += clipLen;

    for (const auto& lane : *cc)
    {
        if (! lane.isParam()) continue;
        if (const auto* b = findParamBinding (params, lane.paramNodeId, lane.paramId))
            b->target.writeCoarseValue ((float) lane.valueAtBeats (lb));
    }
}

void MidiPlayerNode::emitSessionClipCcInBlock (SessionClipSlot& slot,
                                               double             /*blockBeats*/,
                                               double             samplesPerBeat,
                                               int                numSamples,
                                               juce::MidiBuffer&  out) noexcept
{
    if (slot.region == nullptr || samplesPerBeat <= 0.0) return;
    const auto* cc = slot.region->loadCcSnapshot();
    if (cc == nullptr || cc->empty()) return;
    const double clipLen = slot.region->lengthBeats;
    if (clipLen <= 0.0) return;

    /* Block-start position == current localBeatPos (note emission advances
     * it AFTER this call).  Session clips are clip-local (srcOffset == 0). */
    const double localStart = slot.localBeatPos.load (std::memory_order_relaxed);

    for (int off = 0; off < numSamples; off += kCcSubBlockStride)
    {
        double lb = localStart + (double) off / samplesPerBeat;
        lb = std::fmod (lb, clipLen);
        if (lb < 0.0) lb += clipLen;

        for (const auto& lane : *cc)
        {
            /* Param-target lanes are not MIDI CC -- skip (see emitRegionCcInBlock). */
            if (lane.isParam()) continue;
            const int ch  = juce::jlimit (1, 16, lane.channel);
            const int v   = lane.ccValueAtBeats (lb);
            const int idx = ccCacheIndex (ch, lane.ccNumber);
            if ((int) lastCcValue_[(size_t) idx] != v)
            {
                out.addEvent (juce::MidiMessage::controllerEvent (
                                  ch, juce::jlimit (0, 127, lane.ccNumber), v),
                              off);
                lastCcValue_[(size_t) idx] = (std::int8_t) v;
            }
        }
    }
}

} // namespace element
