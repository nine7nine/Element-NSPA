// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace element {

class MidiNoteRegion;
class PianoRollView;
class PianoRollGrid;
struct MidiNote;
class MidiNoteDiffCommand;

/** Velocity lollipops strip under the piano-roll grid.  Standard
 *  Ableton + Zrythm convention: one stem per note in the bound
 *  region, vertical extent encoded as `velocity / 127`, head
 *  draggable to change velocity.  Selection mirrored from the grid
 *  via PianoRollGrid::selection() so a selected note's stem reads
 *  in the accent colour.
 *
 *  Horizontal scroll: the lane sits OUTSIDE the grid's viewport
 *  (sibling component in PianoRollView's layout) and manually mirrors
 *  the viewport's view-position-x.  Cheap: one paint loop reads the
 *  cached scroll offset.
 *
 *  Drag semantics:
 *   - mouseDown on a stem head    -> begin velocity drag for that note
 *     + every selected note (Ableton convention).
 *   - mouseDrag                   -> live preview via updateNoteById
 *     (each tick publishes a new snapshot; cheap on small note counts).
 *   - mouseUp                     -> commit via MidiNoteDiffCommand
 *     (single undo step covers the whole drag).
 *
 *  The drag uses RELATIVE deltas so multi-note drags scale velocities
 *  proportionally to each note's starting velocity (matches Bitwig). */
class VelocityLane : public juce::Component
{
public:
    VelocityLane (PianoRollView& parent, Services& services);
    ~VelocityLane() override;

    void paint   (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

    /** Pixel-X of the grid's horizontal scroll origin.  Called by
     *  PianoRollView whenever the grid viewport scrolls so the lane
     *  redraws lollipops at the right X. */
    void setScrollX (int x);

    /** Surgically repaint only the old + new playhead strips (driven by the
     *  grid's playhead timer while transport rolls). */
    void updatePlayhead();

    /** Default lane height.  PianoRollView's resized clamps the dock
     *  body to (gridH + this) so the lane has a stable footprint. */
    static constexpr int kDefaultHeight = 72;

    /** Pull the bound region from the parent view's resolver.  Same
     *  pattern as PianoRollGrid::resolveBoundRegion. */
    MidiNoteRegion* resolveBoundRegion() const noexcept;

private:
    PianoRollView& parent_;
    Services&      services_;

    int  scrollX_ { 0 };
    int  playheadPxX_ { -1 };   // last painted playhead X (component coords)

    /* Drag state.  Mirrors NoteDrag's lifecycle but scoped to the
     * lane -- drag begins on stem-head hit + ends on mouseUp. */
    struct Drag
    {
        bool   active        { false };
        int    anchorY       { 0 };
        std::vector<MidiNote> originals;   /* before-snapshot for undo */
        /* Drag mode: head-grab on a hit note locks the gesture's
         * reference velocity to that note's velocity at mouseDown.
         * Other selected notes scale by the same delta. */
        int    referenceVel  { 100 };
    };
    Drag drag_;

    /** Hit-test: find the stem nearest to (x, y) within a 6 px slop.
     *  Returns the matching note id or 0.  Iterated in reverse-paint
     *  order so the top-most stem wins on overlap. */
    std::uint64_t hitTestStem (int x, int y, const MidiNoteRegion& region) const noexcept;

    /** Convert a velocity value [1, 127] to a Y coordinate inside the
     *  lane bounds.  Top of stem (the head) lives at this Y. */
    int    yForVelocity (int vel) const noexcept;
    int    velocityForY (int y) const noexcept;

    /** Stem x in lane-local coords (lane shares the grid's horizontal
     *  scroll origin via scrollX_). */
    int    xForBeat (double localBeat, int pxPerBeat) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelocityLane)
};

} // namespace element
