// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/midi_cc_lane.hpp"

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>

#include <vector>

namespace element {

class MidiNoteRegion;
class PianoRollView;

/** MIDI CC automation lane under the piano-roll grid.  Sibling of
 *  VelocityLane: sits outside the grid's viewport and mirrors its
 *  horizontal scroll + pxPerBeat so the curve aligns with the notes
 *  above.  Edits ONE (ccNumber, channel) lane of the bound
 *  MidiNoteRegion at a time -- a clickable "CC n" selector in the lane
 *  header picks which controller.
 *
 *  The curve reuses the timeline-automation breakpoint model
 *  (dsp::automation::AutomationPoint + evaluate) stored on the region's
 *  MidiCcLane, so paint + point gestures mirror the arrangement
 *  automation overlays:
 *    - left-click a breakpoint  -> drag (x snaps to the grid, y = value)
 *    - left-click empty curve   -> add a breakpoint + drag it
 *    - right-click a breakpoint -> menu (delete / segment curve type)
 *
 *  Mutation goes through MidiNoteRegion::setCcLane (COW publish; the
 *  MidiPlayerNode emits the new curve next block) and persists via
 *  PianoRollView::notifyRegionEdited (same sink the velocity lane uses).
 *  No undo step yet -- CC undo is a follow-up (would need a CC diff
 *  command paralleling MidiNoteDiffCommand). */
class CcLane : public juce::Component
{
public:
    CcLane (PianoRollView& parent, Services& services);
    ~CcLane() override;

    void paint   (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

    /** Pixel-X of the grid's horizontal scroll origin (mirrored from the
     *  viewport, identical to VelocityLane::setScrollX). */
    void setScrollX (int x);

    /** Make a MIDI CC the active/editable lane (channel fixed at 1). */
    void setActiveCc (int cc);

    /** Surgically repaint only the old + new playhead strips (driven by the
     *  grid's playhead timer while transport rolls). */
    void updatePlayhead();

    static constexpr int kDefaultHeight = 84;

    MidiNoteRegion* resolveBoundRegion() const noexcept;

private:
    PianoRollView& parent_;
    Services&      services_;

    int scrollX_  { 0 };
    int playheadPxX_ { -1 };   // last painted playhead X (component coords)

    /* Active (editable) lane identity -- a CC controller OR a clip-local
     * node-parameter target (#11 unification).  All editing/paint reads
     * resolve the active lane through laneIsActive(); mutation routes to
     * setCcLane / setParamLane accordingly. */
    bool         activeIsParam_ { false };
    int          ccNumber_ { 1 };   // mod wheel (CC mode)
    int          channel_  { 1 };
    juce::Uuid   activeParamNode_;  // param mode
    juce::String activeParamId_;
    juce::String activeParamLabel_; // display text for the param chip

    /* In-flight drag.  Two modes:
     *   MovePoint  -- drag a breakpoint (pointIndex), x clamped between
     *                 neighbours so the index stays stable.
     *   ShapeCurve -- drag a segment's midpoint handle (segmentIndex) to
     *                 bend the curve (sets the from-point's curviness). */
    enum class DragMode { None, MovePoint, ShapeCurve };
    struct Drag
    {
        DragMode mode   { DragMode::None };
        int  pointIndex { -1 };
        int  segmentIndex { -1 };
        bool moved      { false };
        bool active() const noexcept { return mode != DragMode::None; }
    };
    Drag drag_;

    static constexpr int kHeaderH      = 15;   // chip-rail strip
    static constexpr int kCurvePadY    = 6;
    static constexpr double kHandleGrabPx = 8.0;   // match arranger overlay grab

    /* Layout: the header strip claims the top kHeaderH; the curve area
     * is everything below, inset kCurvePadY top/bottom. */
    juce::Rectangle<int> curveArea() const noexcept;

    /* Tabbed chip rail (Cubase/arranger style): one colour-coded chip per
     * CC lane present on the region + a trailing "+" add chip.  Clicking a
     * chip makes that (cc, channel) the active/editable lane; all lanes
     * render superimposed in the curve area, the active one bright with
     * handles, the rest dim ghosts. */
    struct CcChip
    {
        bool         isParam { false };
        int          cc { 1 };
        int          channel { 1 };
        juce::Uuid   nodeId;
        juce::String paramId;
        juce::String label;     // chip text ("CC 11" / param name)
        int          laneIndex { 0 };
        juce::Rectangle<int> rect;
    };
    struct CcChipLayout { std::vector<CcChip> chips; juce::Rectangle<int> addRect; };
    CcChipLayout ccChipLayout() const;

    /** True when `lane` is the active/editable lane (param or CC). */
    bool laneIsActive (const MidiCcLane& lane) const noexcept;

    /** Copy the active lane's points from the region snapshot (empty if
     *  the active lane doesn't exist yet). */
    MidiCcLane::PointList activeLanePoints (const MidiNoteRegion& region) const;

    /** Upsert the active lane's points (routes to setParamLane /
     *  setCcLane by the active target kind). */
    void writeActiveLane (MidiNoteRegion& region, MidiCcLane::PointList points);

    /** Build the "+" picker: common CCs + every automatable graph param. */
    void showAddTargetMenu (juce::Point<int> screenPos);

    int    xForBeat   (double localBeat, int pxPerBeat) const noexcept;
    double beatForX   (int x, int pxPerBeat) const noexcept;
    float  yForValue  (double v) const noexcept;
    double valueForY  (int y) const noexcept;

    /** Index of the breakpoint within grab range of (x,y) in the current
     *  lane's points, or -1.  Reads the region snapshot. */
    int findPointNear (const MidiNoteRegion& region, int x, int y,
                       int pxPerBeat) const noexcept;

    /** Index of the SEGMENT (from-point index) whose midpoint handle is
     *  within grab range of (x,y), or -1.  Used to start a curve-shaping
     *  drag.  Flat segments (equal endpoint values) report no handle --
     *  there's nothing to bend. */
    int findMidpointNear (const MidiNoteRegion& region, int x, int y,
                          int pxPerBeat) const noexcept;

    void showPointMenu (int pointIndex, juce::Point<int> screenPos);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CcLane)
};

} // namespace element
