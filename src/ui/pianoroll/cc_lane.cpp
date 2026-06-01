// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/cc_lane.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/fontcache.hpp"

#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/midi_cc_lane.hpp"
#include "services/automation/automation_target_resolver.hpp"
#include "dsp/automation/curve.hpp"

#include <element/context.hpp>
#include <element/node.hpp>
#include <element/services.hpp>
#include <element/session.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
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

/* Distinct per-lane colour, mirroring the arranger's automationBindingColour
 * so the piano-roll CC tabs/curves read in the same palette. */
juce::Colour ccLaneColour (int idx) noexcept
{
    static const juce::uint32 kPalette[] = {
        0xff'4a'c8'a0, 0xff'e8'a1'3a, 0xff'6a'a8'e8, 0xff'd0'6a'e0, 0xff'9a'd8'4a,
        0xff'e8'5a'6a, 0xff'e0'c8'4a, 0xff'8a'6a'e0, 0xff'4a'c8'e0, 0xff'e0'7a'3a,
    };
    constexpr int n = (int) (sizeof (kPalette) / sizeof (kPalette[0]));
    return juce::Colour (kPalette[((idx % n) + n) % n]);
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

void CcLane::setActiveCc (int cc)
{
    cc = juce::jlimit (0, 127, cc);
    if (! activeIsParam_ && cc == ccNumber_) return;
    activeIsParam_ = false;
    activeParamId_ = {};
    ccNumber_      = cc;
    channel_       = 1;
    repaint();
}

bool CcLane::laneIsActive (const MidiCcLane& lane) const noexcept
{
    if (activeIsParam_)
        return lane.isParam()
            && lane.paramNodeId == activeParamNode_
            && lane.paramId     == activeParamId_;
    return ! lane.isParam()
        && lane.ccNumber == ccNumber_
        && lane.channel  == channel_;
}

MidiCcLane::PointList CcLane::activeLanePoints (const MidiNoteRegion& region) const
{
    if (const auto* snap = region.loadCcSnapshot())
        for (const auto& lane : *snap)
            if (laneIsActive (lane))
                return lane.points;
    return {};
}

void CcLane::writeActiveLane (MidiNoteRegion& region, MidiCcLane::PointList points)
{
    if (activeIsParam_)
        region.setParamLane (activeParamNode_, activeParamId_, std::move (points));
    else
        region.setCcLane (ccNumber_, channel_, std::move (points));
}

void CcLane::resized() {}

juce::Rectangle<int> CcLane::curveArea() const noexcept
{
    return { 0, kHeaderH + kCurvePadY,
             getWidth(),
             juce::jmax (1, getHeight() - kHeaderH - kCurvePadY * 2) };
}

CcLane::CcChipLayout CcLane::ccChipLayout() const
{
    CcChipLayout out;
    const int y = 1, h = kHeaderH - 2;
    int x = 2;
    if (auto* region = const_cast<CcLane*> (this)->resolveBoundRegion())
        if (const auto* snap = region->loadCcSnapshot())
        {
            int i = 0;
            for (const auto& lane : *snap)
            {
                CcChip chip;
                chip.laneIndex = i;
                if (lane.isParam())
                {
                    chip.isParam = true;
                    chip.nodeId  = lane.paramNodeId;
                    chip.paramId = lane.paramId;
                    chip.label   = laneIsActive (lane) && activeParamLabel_.isNotEmpty()
                                       ? activeParamLabel_
                                       : ("P" + lane.paramId);
                }
                else
                {
                    chip.cc      = lane.ccNumber;
                    chip.channel = lane.channel;
                    chip.label   = "CC " + juce::String (lane.ccNumber);
                }
                /* Param chips need more room for a name; CC chips stay compact. */
                const int w = chip.isParam ? 64 : 44;
                chip.rect = juce::Rectangle<int> (x, y, w, h);
                out.chips.push_back (std::move (chip));
                x += w + 3;
                ++i;
            }
        }
    out.addRect = juce::Rectangle<int> (x, y, 16, h);
    return out;
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
    const auto pts = activeLanePoints (region);
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
    const auto pts = activeLanePoints (region);
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

    auto* region = resolveBoundRegion();
    auto* grid   = parent_.getGrid();
    const int pxPerBeat = grid != nullptr ? grid->getPxPerBeat() : 0;

    /* --- Tabbed chip rail: one colour-coded chip per CC lane + "+". --- */
    const auto layout = ccChipLayout();
    g.setFont (monoFont (9.0f, juce::Font::bold));
    for (const auto& chip : layout.chips)
    {
        const bool active = chip.isParam
            ? (activeIsParam_ && chip.nodeId == activeParamNode_ && chip.paramId == activeParamId_)
            : (! activeIsParam_ && chip.cc == ccNumber_ && chip.channel == channel_);
        const juce::Colour col = ccLaneColour (chip.laneIndex);
        g.setColour (active ? col
                            : col.withMultipliedBrightness (0.32f).withAlpha (0.85f));
        g.fillRect (chip.rect);
        g.setColour (active ? col.brighter (0.25f) : col.withAlpha (0.45f));
        g.drawRect (chip.rect, 1);                 // no white active outline
        g.setColour (active ? col.contrasting (0.92f)
                            : col.brighter (0.3f).withAlpha (0.95f));
        g.drawText (chip.label,
                    chip.rect.reduced (3, 0), juce::Justification::centredLeft, true);
    }
    g.setColour (juce::Colour (0xff'2c'2c'32));
    g.fillRect (layout.addRect);
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.setFont (monoFont (11.0f, juce::Font::bold));
    g.drawText ("+", layout.addRect, juce::Justification::centred);

    if (region == nullptr || pxPerBeat <= 0) return;

    const auto area = curveArea();

    /* Playhead -- mirror the grid's so live playback tracks the automation
     * position here too (region-local beat from the grid, mapped through
     * this lane's mirrored horizontal scroll). */
    auto drawPlayhead = [&]
    {
        const double localBeat = grid->playheadLocalBeat();
        if (localBeat < 0.0) return;
        const int px = xForBeat (localBeat, pxPerBeat);
        if (px < 0 || px >= getWidth()) return;
        g.setColour (juce::Colour (0xff'40'ff'80).withAlpha (0.85f));
        g.drawVerticalLine (px, (float) kHeaderH, (float) getHeight());
    };

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

    const float leftX  = (float) xForBeat (0.0, pxPerBeat);
    const float rightX = (float) xForBeat (region->lengthBeats, pxPerBeat);

    /* Draw ONE lane's curve.  Active = full colour + 2D bend handles +
     * breakpoint squares; ghost = dim, no handles.  Lead-in/out hold the
     * end values flat across the whole region (real-automation feel). */
    auto drawCurve = [&] (const MidiCcLane::PointList& pts, juce::Colour col, bool active)
    {
        if (pts.empty()) return;
        g.setColour (active ? col : col.withAlpha (0.35f));
        const float strokeW = active ? 1.6f : 1.1f;

        if (pts.size() == 1)
        {
            const float y = yForValue (pts[0].valueNormalized);
            g.drawLine (leftX, y, rightX, y, strokeW);
        }
        else
        {
            juce::Path path;
            path.startNewSubPath (leftX, yForValue (pts.front().valueNormalized));
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
                    path.lineTo (x0 + (float) f * (x1 - x0),
                                 yForValue (segValueAt (from, to, f)));
                }
            }
            path.lineTo (rightX, yForValue (pts.back().valueNormalized));
            g.strokePath (path, juce::PathStrokeType (strokeW));
        }

        if (! active) return;

        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const double cot = juce::jlimit (0.25, 0.75, pts[i].curve.offsetT);
            const double pinBeat = pts[i].tBeats + cot * (pts[i + 1].tBeats - pts[i].tBeats);
            const double chordMid = 0.5 * (pts[i].valueNormalized + pts[i + 1].valueNormalized);
            const double pinV = juce::jlimit (0.0, 1.0, chordMid + pts[i].curve.offsetV);
            g.setColour (col.withAlpha (0.5f));
            g.drawEllipse ((float) xForBeat (pinBeat, pxPerBeat) - 3.0f,
                           yForValue (pinV) - 3.0f, 6.0f, 6.0f, 1.2f);
        }
        g.setColour (col.brighter (0.5f));
        for (const auto& p : pts)
            g.fillRect (juce::Rectangle<float> (
                (float) xForBeat (p.tBeats, pxPerBeat) - 2.5f,
                yForValue (p.valueNormalized) - 2.5f, 5.0f, 5.0f));
    };

    const auto* snap = region->loadCcSnapshot();
    if (snap == nullptr || snap->empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.setFont (monoFont (10.0f, juce::Font::plain));
        g.drawText ("click to add points (\"+\" picks a CC or parameter)",
                    area, juce::Justification::centred);
        drawPlayhead();
        return;
    }

    /* Ghosts first, the active lane on top so its handles are reachable. */
    int idx = 0;
    for (const auto& lane : *snap)
    {
        if (! laneIsActive (lane))
            drawCurve (lane.points, ccLaneColour (idx), false);
        ++idx;
    }
    idx = 0;
    for (const auto& lane : *snap)
    {
        if (laneIsActive (lane))
            drawCurve (lane.points, ccLaneColour (idx), true);
        ++idx;
    }

    drawPlayhead();   // on top of the curves
}

void CcLane::mouseDown (const juce::MouseEvent& e)
{
    /* Header chip rail: click a chip to make that CC the active/editable
     * lane, or "+" to pick/add a controller. */
    if (e.y < kHeaderH)
    {
        const auto layout = ccChipLayout();
        if (layout.addRect.contains (e.x, e.y))
        {
            showAddTargetMenu (e.getScreenPosition());
            return;
        }
        for (const auto& chip : layout.chips)
            if (chip.rect.contains (e.x, e.y))
            {
                if (chip.isParam)
                {
                    activeIsParam_   = true;
                    activeParamNode_ = chip.nodeId;
                    activeParamId_   = chip.paramId;
                    activeParamLabel_ = chip.label;
                }
                else
                {
                    activeIsParam_ = false;
                    ccNumber_      = chip.cc;
                    channel_       = chip.channel;
                }
                repaint();
                return;
            }
        return;   // header strip, no chip hit
    }

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;
    auto* grid = parent_.getGrid();
    if (grid == nullptr) return;
    const int pxPerBeat = grid->getPxPerBeat();
    if (pxPerBeat <= 0) return;

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
    auto pts = activeLanePoints (*region);
    const double rawBeat = grid->snapBeat (beatForX (e.x, pxPerBeat));
    const double local   = juce::jlimit (0.0, region->lengthBeats, rawBeat);
    AutomationPoint np;
    np.tBeats          = local;
    np.valueNormalized = valueForY (e.y);
    size_t insertIdx = 0;
    while (insertIdx < pts.size() && pts[insertIdx].tBeats < local)
        ++insertIdx;
    pts.insert (pts.begin() + (long) insertIdx, np);
    writeActiveLane (*region, pts);

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

    auto pts = activeLanePoints (*region);

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

    writeActiveLane (*region, pts);
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

void CcLane::showAddTargetMenu (juce::Point<int> screenPos)
{
    /* The "+" picker offers BOTH target kinds (#11 full symmetry): a "MIDI
     * CC" submenu of common controllers, plus every automatable node/plugin
     * parameter in the active graph (grouped per node), so a clip-local lane
     * can drive a synth/FX param OR a CC to outboard gear.
     *
     * Result-id scheme: 1..128 = CC (cc+1); 1000+ = a graph param, indexed
     * into paramTargets below. */
    juce::PopupMenu menu;

    juce::PopupMenu ccMenu;
    for (const auto& c : kCommonCcs)
        ccMenu.addItem (c.cc + 1, "CC " + juce::String (c.cc) + "  " + c.name,
                        true, ! activeIsParam_ && c.cc == ccNumber_);
    menu.addSubMenu ("MIDI CC", ccMenu);

    /* Enumerate every automatable param in the active graph, grouped per
     * node -- mirrors ArrangementView::collectAutomatableTargets. */
    struct ParamTarget { juce::Uuid nodeId; juce::String paramId, label; };
    std::vector<ParamTarget> paramTargets;

    if (services_.context().session() != nullptr)
    {
        const Node root = services_.context().session()->getActiveGraph();
        std::function<void (const Node&)> walk = [&] (const Node& parent)
        {
            for (int i = 0; i < parent.getNumNodes(); ++i)
            {
                Node child = parent.getNode (i);
                if (! child.isValid()) continue;
                if (auto* proc = child.getObject())
                {
                    const auto params =
                        automation::enumerateAutomatableParams (proc->getParameters (true));
                    if (! params.empty())
                    {
                        juce::PopupMenu nodeMenu;
                        const juce::String nodeName = child.getDisplayName();
                        for (const auto& ap : params)
                        {
                            const int id = 1000 + (int) paramTargets.size();
                            const juce::String pid = automation::encodeNodeParamId (ap.index);
                            const bool on = activeIsParam_
                                         && child.getUuid() == activeParamNode_
                                         && pid == activeParamId_;
                            nodeMenu.addItem (id, ap.name, true, on);
                            paramTargets.push_back ({ child.getUuid(), pid,
                                                      nodeName + ": " + ap.name });
                        }
                        menu.addSubMenu (nodeName, nodeMenu);
                    }
                }
                if (child.isGraph())
                    walk (child);
            }
        };
        walk (root);
    }

    /* Synchronous showAt at the cursor -- the proven pattern in this build
     * (SessionView's context menus).  showMenuAsync mis-rendered detached
     * at the window bottom + keyboard-only under this windowing setup. */
    const int result = menu.showAt (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1));
    if (result <= 0) return;

    if (result >= 1000)
    {
        const size_t idx = (size_t) (result - 1000);
        if (idx >= paramTargets.size()) return;
        activeIsParam_    = true;
        activeParamNode_  = paramTargets[idx].nodeId;
        activeParamId_    = paramTargets[idx].paramId;
        activeParamLabel_ = paramTargets[idx].label;
        repaint();
    }
    else
    {
        setActiveCc (result - 1);
    }
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
    auto pts = activeLanePoints (*region);
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

    writeActiveLane (*region, pts);
    parent_.notifyRegionEdited();
    repaint();
}

} // namespace element
