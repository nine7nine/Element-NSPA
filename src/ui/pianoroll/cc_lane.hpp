// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>

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

    /** Currently-edited controller.  Channel fixed at 1 for v1 (matches
     *  the common single-channel piano-roll clip); the selector changes
     *  the CC number. */
    int  getCcNumber() const noexcept { return ccNumber_; }
    void setCcNumber (int cc);

    static constexpr int kDefaultHeight = 84;

    MidiNoteRegion* resolveBoundRegion() const noexcept;

private:
    PianoRollView& parent_;
    Services&      services_;

    int scrollX_  { 0 };
    int ccNumber_ { 1 };   // mod wheel
    int channel_  { 1 };

    /* In-flight point drag.  Index into the lane's sorted point list;
     * x-moves clamp between neighbours so the index stays stable. */
    struct Drag
    {
        bool active     { false };
        int  pointIndex { -1 };
        bool moved      { false };
    };
    Drag drag_;

    static constexpr int kHeaderH      = 15;   // "CC n" selector strip
    static constexpr int kCurvePadY    = 6;
    static constexpr double kHandleGrabPx = 6.0;

    /* Layout: the header strip claims the top kHeaderH; the curve area
     * is everything below, inset kCurvePadY top/bottom. */
    juce::Rectangle<int> curveArea() const noexcept;
    juce::Rectangle<int> selectorRect() const noexcept;

    int    xForBeat   (double localBeat, int pxPerBeat) const noexcept;
    double beatForX   (int x, int pxPerBeat) const noexcept;
    float  yForValue  (double v) const noexcept;
    double valueForY  (int y) const noexcept;

    /** Index of the breakpoint within grab range of (x,y) in the current
     *  lane's points, or -1.  Reads the region snapshot. */
    int findPointNear (const MidiNoteRegion& region, int x, int y,
                       int pxPerBeat) const noexcept;

    void showSelectorMenu (juce::Point<int> screenPos);
    void showPointMenu (int pointIndex, juce::Point<int> screenPos);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CcLane)
};

} // namespace element
