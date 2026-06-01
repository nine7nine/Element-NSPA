// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/cc_lane.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/fontcache.hpp"

#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/midi_cc_lane.hpp"
#include "dsp/automation/curve.hpp"

#include <element/services.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace element {

using dsp::automation::AutomationPoint;

namespace {

/* Common controllers offered by the selector menu.  (number, label). */
struct CcChoice { int cc; const char* name; };
const CcChoice kCommonCcs[] = {
    {  1, "Mod Wheel" },
    {  2, "Breath" },
    {  7, "Volume" },
    { 10, "Pan" },
    { 11, "Expression" },
    { 64, "Sustain" },
    { 71, "Resonance" },
    { 74, "Cutoff" },
    { 91, "Reverb" },
    { 93, "Chorus" },
};

/* Find the (cc, channel) lane's points in a region snapshot; empty if
 * absent. */
MidiCcLane::PointList copyLanePoints (const MidiNoteRegion& region, int cc, int ch)
{
    if (const auto* snap = region.loadCcSnapshot())
        for (const auto& lane : *snap)
            if (lane.ccNumber == cc && lane.channel == ch)
                return lane.points;
    return {};
}

} // namespace

CcLane::CcLane (PianoRollView& parent, Services& services)
    : parent_ (parent), services_ (services)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

CcLane::~CcLane() = default;

MidiNoteRegion* CcLane::resolveBoundRegion() const noexcept
{
    const auto& resolver = parent_.getRegionResolver();
    const auto  regionId = parent_.getBoundRegionId();
    if (! resolver || regionId.isNull()) return nullptr;
    return resolver (regionId);
}

void CcLane::setScrollX (int x)
{
    if (x == scrollX_) return;
    scrollX_ = x;
    repaint();
}

void CcLane::setCcNumber (int cc)
{
    cc = juce::jlimit (0, 127, cc);
    if (cc == ccNumber_) return;
    ccNumber_ = cc;
    repaint();
}

void CcLane::resized() {}

juce::Rectangle<int> CcLane::curveArea() const noexcept
{
    return { 0, kHeaderH + kCurvePadY,
             getWidth(),
             juce::jmax (1, getHeight() - kHeaderH - kCurvePadY * 2) };
}

juce::Rectangle<int> CcLane::selectorRect() const noexcept
{
    return { 2, 1, 96, kHeaderH - 2 };
}

int CcLane::xForBeat (double localBeat, int pxPerBeat) const noexcept
{
    return (int) std::round (localBeat * (double) pxPerBeat) - scrollX_;
}

double CcLane::beatForX (int x, int pxPerBeat) const noexcept
{
    if (pxPerBeat <= 0) return 0.0;
    return juce::jmax (0.0, (double) (x + scrollX_) / (double) pxPerBeat);
}

float CcLane::yForValue (double v) const noexcept
{
    const auto a = curveArea();
    const double cl = juce::jlimit (0.0, 1.0, v);
    return (float) (a.getBottom() - cl * (double) a.getHeight());
}

double CcLane::valueForY (int y) const noexcept
{
    const auto a = curveArea();
    if (a.getHeight() <= 0) return 0.5;
    return juce::jlimit (0.0, 1.0,
                         (double) (a.getBottom() - y) / (double) a.getHeight());
}

int CcLane::findPointNear (const MidiNoteRegion& region, int x, int y,
                           int pxPerBeat) const noexcept
{
    const auto pts = copyLanePoints (region, ccNumber_, channel_);
    int    best  = -1;
    double bestD = 1.0e9;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const double px = xForBeat (pts[i].tBeats, pxPerBeat);
        const double py = yForValue (pts[i].valueNormalized);
        const double d  = juce::jmax (std::abs (px - x), std::abs (py - y));
        if (d < bestD) { bestD = d; best = (int) i; }
    }
    return (best >= 0 && bestD <= kHandleGrabPx) ? best : -1;
}

namespace {
/* Value of a segment at fraction f in [0,1] -- the unified value-space
 * 2D-Bezier shared with the audio thread (MidiCcLane::valueAtBeats). */
double segValueAt (const AutomationPoint& from, const AutomationPoint& to, double f)
{
    return dsp::automation::evaluateSegment (
        f, from.valueNormalized, to.valueNormalized, from.curve);
}
} // namespace

int CcLane::findMidpointNear (const MidiNoteRegion& region, int x, int y,
                              int pxPerBeat) const noexcept
{
    const auto pts = copyLanePoints (region, ccNumber_, channel_);
    int    best  = -1;
    double bestD = 1.0e9;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        /* 2D bend handle at the pin (offsetT, chordMid+offsetV) -- sits
         * ON the curve, grabbable on flat segments too. */
        const double cot = juce::jlimit (0.25, 0.75, pts[i].curve.offsetT);
        const double pinBeat = pts[i].tBeats + cot * (pts[i + 1].tBeats - pts[i].tBeats);
        const double chordMid = 0.5 * (pts[i].valueNormalized + pts[i + 1].valueNormalized);
        const double pinV = juce::jlimit (0.0, 1.0, chordMid + pts[i].curve.offsetV);
        const double px = xForBeat (pinBeat, pxPerBeat);
        const double py = yForValue (pinV);
        const double d  = juce::jmax (std::abs (px - x), std::abs (py - y));
        if (d < bestD) { bestD = d; best = (int) i; }
    }
    return (best >= 0 && bestD <= kHandleGrabPx) ? best : -1;
}

void CcLane::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff'0e'0e'10));
    g.setColour (juce::Colour (0xff'30'30'30));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());

    /* Header: "CC n  <name>" selector chip. */
    const auto sel = selectorRect();
    g.setColour (juce::Colour (0xff'20'20'26));
    g.fillRect (sel);
    g.setColour (juce::Colour (0xff'40'40'48));
    g.drawRect (sel, 1);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (monoFont (9.5f, juce::Font::bold));
    juce::String name;
    for (const auto& c : kCommonCcs)
        if (c.cc == ccNumber_) { name = c.name; break; }
    g.drawText ("CC " + juce::String (ccNumber_)
                    + (name.isNotEmpty() ? "  " + name : juce::String()) + "  v",
                sel.reduced (4, 0), juce::Justification::centredLeft);

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;
    auto* grid = parent_.getGrid();
    if (grid == nullptr) return;
    const int pxPerBeat = grid->getPxPerBeat();
    if (pxPerBeat <= 0) return;

    const auto area = curveArea();

    /* Centre (0.5) reference + region-end fence (mirrors VelocityLane). */
    g.setColour (juce::Colour (0xff'1c'1c'1c));
    g.drawHorizontalLine (area.getCentreY(), 0.0f, (float) getWidth());

    const int regionEndX = (int) std::round (region->lengthBeats * (double) pxPerBeat) - scrollX_;
    if (regionEndX > 0 && regionEndX < getWidth())
    {
        g.setColour (juce::Colour (0xff'08'08'08));
        g.fillRect (regionEndX, 0, getWidth() - regionEndX, getHeight());
        g.setColour (juce::Colour (0xff'40'40'40));
        g.drawVerticalLine (regionEndX, 0.0f, (float) getHeight());
    }

    const auto pts = copyLanePoints (*region, ccNumber_, channel_);
    if (pts.empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.setFont (monoFont (10.0f, juce::Font::plain));
        g.drawText ("click to add CC points",
                    area, juce::Justification::centred);
        return;
    }

    const juce::Colour curveCol { 0xff'd0'9a'48 };   // amber, distinct from velocity blue

    /* Segments through the curve shape.  The curve is held flat at the
     * first point's value from the region start, and at the last point's
     * value to the region end (lead-in / lead-out), so it spans the whole
     * region like a real automation lane rather than floating between the
     * outermost points. */
    g.setColour (curveCol);
    const float leftX  = (float) xForBeat (0.0, pxPerBeat);
    const float rightX = (float) xForBeat (region->lengthBeats, pxPerBeat);
    if (pts.size() == 1)
    {
        const float y = yForValue (pts[0].valueNormalized);
        g.drawLine (leftX, y, rightX, y, 1.4f);
    }
    else
    {
        juce::Path path;
        bool started = false;
        /* Lead-in: flat at the first point's value from the region start. */
        path.startNewSubPath (leftX, yForValue (pts.front().valueNormalized));
        started = true;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const auto& from = pts[i];
            const auto& to   = pts[i + 1];
            const float x0 = (float) xForBeat (from.tBeats, pxPerBeat);
            const float x1 = (float) xForBeat (to.tBeats, pxPerBeat);
            const int steps = juce::jlimit (1, 96, (int) std::abs (x1 - x0) / 3);
            for (int s = 0; s <= steps; ++s)
            {
                const double f  = (double) s / (double) steps;
                const float px = x0 + (float) f * (x1 - x0);
                const float py = yForValue (segValueAt (from, to, f));
                if (! started) { path.startNewSubPath (px, py); started = true; }
                else            path.lineTo (px, py);
            }
        }
        /* Lead-out: flat at the last point's value to the region end. */
        path.lineTo (rightX, yForValue (pts.back().valueNormalized));
        g.strokePath (path, juce::PathStrokeType (1.4f));
    }

    /* Segment 2D bend handles -- hollow rings sitting ON the curve at the
     * bend pin (offsetT, chordMid+offsetV); drag anywhere on the line to
     * shape, like the volume envelope. */
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const double cot = juce::jlimit (0.25, 0.75, pts[i].curve.offsetT);
        const double pinBeat = pts[i].tBeats + cot * (pts[i + 1].tBeats - pts[i].tBeats);
        const double chordMid = 0.5 * (pts[i].valueNormalized + pts[i + 1].valueNormalized);
        const double pinV = juce::jlimit (0.0, 1.0, chordMid + pts[i].curve.offsetV);
        const float mx = (float) xForBeat (pinBeat, pxPerBeat);
        const float my = yForValue (pinV);
        g.setColour (curveCol.withAlpha (0.5f));
        g.drawEllipse (mx - 3.0f, my - 3.0f, 6.0f, 6.0f, 1.2f);
    }

    /* Breakpoint handles (filled squares, on top of midpoint rings). */
    g.setColour (curveCol.brighter (0.5f));
    for (const auto& p : pts)
    {
        const float hx = (float) xForBeat (p.tBeats, pxPerBeat);
        const float hy = yForValue (p.valueNormalized);
        g.fillRect (juce::Rectangle<float> (hx - 2.5f, hy - 2.5f, 5.0f, 5.0f));
    }
}

void CcLane::mouseDown (const juce::MouseEvent& e)
{
    /* Header selector chip. */
    if (selectorRect().contains (e.x, e.y)) { showSelectorMenu (e.getScreenPosition()); return; }

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;
    auto* grid = parent_.getGrid();
    if (grid == nullptr) return;
    const int pxPerBeat = grid->getPxPerBeat();
    if (pxPerBeat <= 0) return;

    if (e.y < kHeaderH) return;

    const int existing = findPointNear (*region, e.x, e.y, pxPerBeat);

    if (e.mods.isPopupMenu())
    {
        if (existing >= 0)
            showPointMenu (existing, e.getScreenPosition());
        return;
    }
    if (! e.mods.isLeftButtonDown()) return;

    /* Priority: existing breakpoint (move) > segment midpoint (shape
     * curve) > empty area (add a breakpoint + move). */
    if (existing >= 0)
    {
        drag_.mode       = DragMode::MovePoint;
        drag_.pointIndex = existing;
        drag_.moved      = false;
        repaint();
        return;
    }

    const int seg = findMidpointNear (*region, e.x, e.y, pxPerBeat);
    if (seg >= 0)
    {
        drag_.mode         = DragMode::ShapeCurve;
        drag_.segmentIndex = seg;
        drag_.moved        = false;
        repaint();
        return;
    }

    /* Empty area -> add a breakpoint at the snapped beat + clicked value,
     * then drag it. */
    auto pts = copyLanePoints (*region, ccNumber_, channel_);
    const double rawBeat = grid->snapBeat (beatForX (e.x, pxPerBeat));
    const double local   = juce::jlimit (0.0, region->lengthBeats, rawBeat);
    AutomationPoint np;
    np.tBeats          = local;
    np.valueNormalized = valueForY (e.y);
    size_t insertIdx = 0;
    while (insertIdx < pts.size() && pts[insertIdx].tBeats < local)
        ++insertIdx;
    pts.insert (pts.begin() + (long) insertIdx, np);
    region->setCcLane (ccNumber_, channel_, pts);

    drag_.mode       = DragMode::MovePoint;
    drag_.pointIndex = (int) insertIdx;
    drag_.moved      = true;   // an add always commits
    repaint();
}

void CcLane::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag_.active()) return;
    auto* region = resolveBoundRegion();
    if (region == nullptr) return;
    auto* grid = parent_.getGrid();
    if (grid == nullptr) return;
    const int pxPerBeat = grid->getPxPerBeat();
    if (pxPerBeat <= 0) return;

    auto pts = copyLanePoints (*region, ccNumber_, channel_);

    if (drag_.mode == DragMode::MovePoint)
    {
        const int i = drag_.pointIndex;
        if (i < 0 || i >= (int) pts.size()) return;

        const double rawBeat = grid->snapBeat (beatForX (e.x, pxPerBeat));
        double local = juce::jlimit (0.0, region->lengthBeats, rawBeat);

        constexpr double eps = 1.0e-4;
        if (i > 0)                    local = juce::jmax (local, pts[(size_t) i - 1].tBeats + eps);
        if (i < (int) pts.size() - 1) local = juce::jmin (local, pts[(size_t) i + 1].tBeats - eps);

        pts[(size_t) i].tBeats          = local;
        pts[(size_t) i].valueNormalized = valueForY (e.y);
    }
    else if (drag_.mode == DragMode::ShapeCurve)
    {
        const int i = drag_.segmentIndex;
        if (i < 0 || i + 1 >= (int) pts.size()) return;

        /* 2D bend handle, identical feel to the volume envelope: offsetT =
         * cursor X as a fraction within the segment (no snap -- shaping is
         * continuous); offsetV = cursor value minus the chord midpoint, so
         * dragging up bulges the curve up regardless of segment direction. */
        const auto& a = pts[(size_t) i];
        const auto& b = pts[(size_t) i + 1];
        const double localBeat = beatForX (e.x, pxPerBeat);
        const double span = juce::jmax (1e-9, b.tBeats - a.tBeats);
        const double cot  = juce::jlimit (0.25, 0.75, (localBeat - a.tBeats) / span);
        const double chordMid = 0.5 * (a.valueNormalized + b.valueNormalized);
        pts[(size_t) i].curve.offsetT = cot;
        pts[(size_t) i].curve.offsetV = valueForY (e.y) - chordMid;
    }

    region->setCcLane (ccNumber_, channel_, pts);
    drag_.moved = true;
    repaint();
}

void CcLane::mouseUp (const juce::MouseEvent&)
{
    const bool committed = drag_.active() && drag_.moved;
    drag_ = Drag {};
    if (committed)
        parent_.notifyRegionEdited();   // persist (flushLanesToSession) + repaint
    repaint();
}

void CcLane::showSelectorMenu (juce::Point<int> screenPos)
{
    juce::PopupMenu menu;
    for (const auto& c : kCommonCcs)
        menu.addItem (c.cc + 1, "CC " + juce::String (c.cc) + "  " + c.name,
                      true, c.cc == ccNumber_);

    /* Synchronous showAt at the cursor -- the proven pattern in this build
     * (SessionView's context menus).  showMenuAsync mis-rendered detached
     * at the window bottom + keyboard-only under this windowing setup. */
    const int result = menu.showAt (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1));
    if (result > 0)
        setCcNumber (result - 1);
}

void CcLane::showPointMenu (int pointIndex, juce::Point<int> screenPos)
{
    /* Curve shape is the 2D bend handle now (drag the segment line), so the
     * discrete-algorithm submenu is gone -- only delete + straighten. */
    juce::PopupMenu menu;
    menu.addItem (1, "Delete point");
    menu.addItem (2, "Straighten segment after this point");

    const int result = menu.showAt (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1));
    if (result <= 0) return;

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;
    auto pts = copyLanePoints (*region, ccNumber_, channel_);
    if (pointIndex < 0 || pointIndex >= (int) pts.size()) return;

    if (result == 1)
    {
        pts.erase (pts.begin() + (long) pointIndex);
    }
    else if (result == 2)
    {
        pts[(size_t) pointIndex].curve.offsetT = 0.5;
        pts[(size_t) pointIndex].curve.offsetV = 0.0;
    }

    region->setCcLane (ccNumber_, channel_, pts);
    parent_.notifyRegionEdited();
    repaint();
}

} // namespace element
