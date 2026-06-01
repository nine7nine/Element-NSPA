// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_keyboard.hpp"
#include "ui/pianoroll/note_drag.hpp"
#include "ui/pianoroll/midi_note_diff_command.hpp"
#include "ui/fontcache.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/midi_note.hpp"
#include "dsp/quantize_options.hpp"
#include "dsp/quantize_ops.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/ui.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace element {

namespace {

constexpr int kResizeHandlePx = 5;   /* width of the right-edge resize hot zone */

/* Static per-process piano-roll clipboard.  Notes are stored
 * relative to the selection's minimum onBeat so paste is positionable
 * at any anchor.  Distinct from the arrangement clipboard (which
 * holds regions/lanes, not notes), so a user can have an
 * arrangement copy AND a note copy on the same clipboard
 * conceptually.  Lifetime = process; tabs / sessions reset it
 * naturally on Element restart. */
struct NoteClipboard
{
    std::vector<MidiNote> notes;   /* onBeats already rebased to 0 */
    double                spanBeats { 0.0 };
    bool                  empty() const noexcept { return notes.empty(); }
};

NoteClipboard& noteClipboard() noexcept
{
    static NoteClipboard singleton;
    return singleton;
}

} // namespace

//==============================================================================

PianoRollGrid::PianoRollGrid (PianoRollView& parent, Services& services)
    : parent_ (parent), services_ (services)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::NormalCursor);
    /* Opaque: the grid fully fills its bounds every paint (paintRuler fills
     * the ruler band, fillRect(bodyBounds()) fills the body), so JUCE can
     * skip painting anything behind it -- no overdraw on the frequent
     * surgical playhead repaints. */
    setOpaque (true);

    if (auto* eng = services.context().audio().get())
        monitor_ = eng->getTransportMonitor();

    startTimerHz (30);
}

PianoRollGrid::~PianoRollGrid()
{
    stopTimer();
}

//==============================================================================
// Bound-region helpers.

MidiNoteRegion* PianoRollGrid::resolveBoundRegion() const noexcept
{
    const auto& resolver = parent_.getRegionResolver();
    const auto regionId  = parent_.getBoundRegionId();
    if (! resolver || regionId.isNull()) return nullptr;
    return resolver (regionId);
}

void PianoRollGrid::boundRegionChanged()
{
    /* Clear selection -- a fresh region binding shouldn't carry over
     * note ids from the previous one (different regions have
     * disjoint id namespaces). */
    selectedNoteIds_.clear();

    /* Re-sync the cached region length + recompute auto-fit zoom. */
    regionLenBeats_ = 16.0;
    if (auto* region = resolveBoundRegion())
        regionLenBeats_ = juce::jmax (1.0, region->lengthBeats);

    /* Auto-fit on bind: pick pxPerBeat so the whole region fits in
     * the viewport.  If the user later zooms, that overrides
     * auto-fit. */
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int visibleW = vp->getMaximumVisibleWidth();
        if (visibleW > 0 && regionLenBeats_ > 0.0)
        {
            const int fit = (int) std::floor ((double) visibleW / regionLenBeats_);
            pxPerBeat_ = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax, fit);
        }
    }

    setSize ((int) std::round (regionLenBeats_ * pxPerBeat_), getHeight());
    repaint();
}

void PianoRollGrid::activeToolChanged()
{
    /* No active gesture state survives a tool change -- be defensive
     * and drop any in-flight drag.  Repaint to update cursor hints. */
    activeDrag_.reset();
    repaint();
}

void PianoRollGrid::updateSizeForViewport (int visibleW, int visibleH)
{
    visibleH_ = juce::jmax (1, visibleH);
    /* Width: max(visibleW, region span at current zoom).  visibleW
     * ensures the grid fills the viewport even if the region is
     * shorter than the visible area. */
    const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
    setSize (juce::jmax (visibleW, spanW), visibleH_);
}

void PianoRollGrid::setPxPerBeat (int newPxPerBeat)
{
    newPxPerBeat = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax, newPxPerBeat);
    if (newPxPerBeat == pxPerBeat_) return;
    pxPerBeat_ = newPxPerBeat;
    const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
    setSize (juce::jmax (spanW, 1), getHeight());
    repaint();
}

void PianoRollGrid::resized()
{
    /* No child layout -- the grid paints directly into its bounds. */
}

void PianoRollGrid::timerCallback()
{
    if (! isShowing()) return;

    /* Surgical playhead update: repaint ONLY the old + new playhead strips
     * instead of the whole grid.  A full repaint() every tick was the
     * piano-roll's biggest cost (it re-rasterised every note into the GL
     * texture 30x/sec, even while STOPPED).  The only thing that moves per
     * tick is the playhead; notes / selection / preview repaint on their
     * own edit paths.  When transport is stopped, playheadLocalBeat()
     * returns -1 -> newPxX -1 -> the last strip clears and then nothing
     * repaints (idle = zero frames). */
    const double localBeat = playheadLocalBeat();
    const int newPxX = (localBeat >= 0.0) ? (int) std::round (localBeat * pxPerBeat_) : -1;
    if (newPxX != playheadPxX_)
    {
        const int top = kRulerH, h = juce::jmax (1, getHeight() - kRulerH);
        if (playheadPxX_ >= 0) repaint (playheadPxX_ - 2, top, 5, h);
        if (newPxX      >= 0)  repaint (newPxX - 2,       top, 5, h);
        playheadPxX_ = newPxX;
    }

    /* Drive the controller lanes' (velocity + CC) surgical playhead too --
     * one final tick on the play->stop edge clears theirs. */
    const bool playing = monitor_ != nullptr && monitor_->playing.get();
    if (playing || playing != lastTimerPlaying_)
        parent_.updateControllerLanePlayheads();
    lastTimerPlaying_ = playing;
}

//==============================================================================
// Coordinate helpers.

double PianoRollGrid::beatForX (int x, double lengthBeatsClamp) const noexcept
{
    if (pxPerBeat_ <= 0) return 0.0;
    const double b = (double) x / (double) pxPerBeat_;
    if (lengthBeatsClamp <= 0.0) return juce::jmax (0.0, b);
    return juce::jlimit (0.0, lengthBeatsClamp, b);
}

int PianoRollGrid::pitchForY (int y) const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 60;
    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;
    if (rowH <= 0.0f) return lo;
    const int yInBody = y - body.getY();
    const int row = (int) ((float) yInBody / rowH);
    const int pitch = hi - row;
    return juce::jlimit (lo, hi, pitch);
}

int PianoRollGrid::xForBeat (double localBeat) const noexcept
{
    return (int) std::round (localBeat * pxPerBeat_);
}

int PianoRollGrid::yForPitch (int pitch) const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;
    const int hi = kb->getHighestVisibleNoteNumber();
    const int lo = kb->getLowestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;
    return body.getY() + (int) ((float) (hi - pitch) * rowH);
}

int PianoRollGrid::rowHeight() const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 1;
    const int hi = kb->getHighestVisibleNoteNumber();
    const int lo = kb->getLowestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    return juce::jmax (1, bodyBounds().getHeight() / span);
}

double PianoRollGrid::snapBeat (double localBeat) const noexcept
{
    if (! snapEnabled_ || snapDivision_ <= 0.0)
        return localBeat;
    return std::round (localBeat / snapDivision_) * snapDivision_;
}

double PianoRollGrid::snapBeatKeepOffset (double localBeat,
                                            double anchorBeat) const noexcept
{
    /* Keep-offset snap (B.7 fix): preserve the cursor's offset from
     * the nearest grid point at gesture start so the dragged element
     * doesn't jump to the cursor on the first snap.  Matches the
     * arranger's snap pipeline introduced in a2a18109. */
    if (! snapEnabled_ || snapDivision_ <= 0.0)
        return localBeat;
    const double delta   = localBeat - anchorBeat;
    const double snapped = std::round (delta / snapDivision_) * snapDivision_;
    return anchorBeat + snapped;
}

void PianoRollGrid::setSnapDivision (double beats) noexcept
{
    if (beats < 0.0) beats = 0.0;
    if (juce::approximatelyEqual (beats, snapDivision_)) return;
    snapDivision_ = beats;
    repaint();   /* affects sub-grid line density */
}

void PianoRollGrid::setSnapEnabled (bool b) noexcept
{
    if (b == snapEnabled_) return;
    snapEnabled_ = b;
    repaint();
}

//==============================================================================
// Zoom helpers used by the toolbar +/-/Fit buttons.

void PianoRollGrid::zoomBy (double factor)
{
    if (factor <= 0.0) return;
    /* Anchor: viewport centre in beat coords.  Symmetric with the
     * cmd+wheel anchored zoom in mouseWheelMove. */
    int anchorBeatPx = getWidth() / 2;
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        anchorBeatPx = vp->getViewPositionX() + vp->getMaximumVisibleWidth() / 2;

    const double anchorBeat = (pxPerBeat_ > 0)
        ? (double) anchorBeatPx / (double) pxPerBeat_
        : 0.0;

    const int next = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax,
                                     (int) std::round ((double) pxPerBeat_ * factor));
    if (next == pxPerBeat_) return;
    pxPerBeat_ = next;

    const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
    setSize (juce::jmax (spanW, 1), getHeight());

    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int newAnchorPx = (int) std::round (anchorBeat * pxPerBeat_);
        const int viewX = juce::jmax (0, newAnchorPx - vp->getMaximumVisibleWidth() / 2);
        vp->setViewPosition (viewX, vp->getViewPositionY());
    }
    repaint();
}

void PianoRollGrid::zoomToFit()
{
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int visibleW = vp->getMaximumVisibleWidth();
        if (visibleW > 0 && regionLenBeats_ > 0.0)
        {
            const int fit = (int) std::floor ((double) visibleW / regionLenBeats_);
            pxPerBeat_ = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax, fit);
            const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
            setSize (juce::jmax (spanW, 1), getHeight());
            vp->setViewPosition (0, vp->getViewPositionY());
        }
    }
    repaint();
}

juce::Rectangle<int> PianoRollGrid::visibleRect() const noexcept
{
    /* If we're inside a Viewport, use its view-area; otherwise the
     * grid's full bounds. */
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int x = vp->getViewPositionX();
        const int y = vp->getViewPositionY();
        const int w = vp->getMaximumVisibleWidth();
        const int h = vp->getMaximumVisibleHeight();
        return { x, y, w, h };
    }
    return getLocalBounds();
}

//==============================================================================
// Selection.

void PianoRollGrid::selectOnly (std::uint64_t noteId)
{
    selectedNoteIds_.clear();
    if (noteId != 0) selectedNoteIds_.insert (noteId);
    repaint();
}

void PianoRollGrid::selectToggle (std::uint64_t noteId)
{
    if (noteId == 0) return;
    auto it = selectedNoteIds_.find (noteId);
    if (it == selectedNoteIds_.end())
        selectedNoteIds_.insert (noteId);
    else
        selectedNoteIds_.erase (it);
    repaint();
}

void PianoRollGrid::selectAdd (std::uint64_t noteId)
{
    if (noteId == 0) return;
    selectedNoteIds_.insert (noteId);
    repaint();
}

void PianoRollGrid::selectClear()
{
    if (selectedNoteIds_.empty()) return;
    selectedNoteIds_.clear();
    repaint();
}

//==============================================================================
// Paint.

void PianoRollGrid::paint (juce::Graphics& g)
{
    /* Body background: mid-dark grey lifted from near-black so the
     * grid + notes have visual breathing room.  Matches Element's
     * contentBackgroundColor (0xff141414) shifted a hair to keep
     * the dock distinct from the arrangement view above. */
    g.setColour (juce::Colour (0xff'16'16'16));
    g.fillRect (bodyBounds());

    /* Paint the "beyond region" zone darker so the user can see the
     * span they're actually editing.  Grid component width may exceed
     * regionLen * pxPerBeat when the viewport is wider than the
     * region (auto-fit on bind), and we want the user to know clicks
     * past the right edge are no-ops. */
    const int regionEndX = (int) std::round (regionLenBeats_ * pxPerBeat_);
    if (getWidth() > regionEndX)
    {
        const auto body = bodyBounds();
        const juce::Rectangle<int> beyond (regionEndX,
                                            body.getY(),
                                            getWidth() - regionEndX,
                                            body.getHeight());
        g.setColour (juce::Colour (0xff'08'08'08));
        g.fillRect (beyond);
        /* 1 px brighter divider line at the region end. */
        g.setColour (juce::Colour (0xff'40'40'40));
        g.drawVerticalLine (regionEndX,
                              (float) body.getY(),
                              (float) body.getBottom());
    }

    auto* region = resolveBoundRegion();
    if (region != nullptr && juce::jmax (1.0, region->lengthBeats) != regionLenBeats_)
    {
        /* Region length changed since last paint (e.g. user resized
         * a region in the arrangement view).  Re-sync component width
         * so the horizontal scrollbar tracks correctly.  Done from
         * paint() rather than a separate observer because the
         * resolver lookup happens here anyway. */
        regionLenBeats_ = juce::jmax (1.0, region->lengthBeats);
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            updateSizeForViewport (vp->getMaximumVisibleWidth(),
                                    vp->getMaximumVisibleHeight());
    }

    const int beatsPerBar = monitor_ != nullptr
        ? juce::jmax (1, (int) monitor_->beatsPerBar.get())
        : 4;
    paintBarGrid (g, beatsPerBar);
    paintRuler   (g, beatsPerBar);

    if (region == nullptr)
    {
        paintEmptyState (g);
        return;
    }

    paintNotes (g, *region);
    paintPlayhead (g);
    paintActiveDragOverlay (g);
}

void PianoRollGrid::paintEmptyState (juce::Graphics& g)
{
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.setFont (monoFont (12.0f, juce::Font::plain));
    g.drawText ("Double-click a MIDI region to edit.",
                bodyBounds().getIntersection (visibleRect()),
                juce::Justification::centred,
                false);
}

void PianoRollGrid::paintBarGrid (juce::Graphics& g, int beatsPerBar)
{
    const auto vr   = visibleRect();
    const auto body = bodyBounds();
    if (vr.getWidth() <= 0 || body.getHeight() <= 0) return;
    if (pxPerBeat_ <= 0) return;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return;

    const int lo   = kb->getLowestVisibleNoteNumber();
    const int hi   = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) body.getHeight() / (float) span;

    /* Pitch row backdrop -- black-key rows tinted slightly darker so
     * the user reads octaves at a glance.  We paint each visible row
     * as a thin strip; one fillRect per pitch is cheap and avoids
     * blending artefacts at the row edges. */
    const float bodyL = (float) vr.getX();
    const float bodyR = (float) vr.getRight();
    const juce::Colour rowBlack { 0xff'11'12'14 };  /* faintly cooler than bg */
    const juce::Colour rowEdge  { 0xff'1d'1d'1d };  /* row separator */
    const juce::Colour rowOctaveEdge { 0xff'30'30'30 };

    for (int p = lo; p <= hi; ++p)
    {
        const float y = (float) body.getY() + (float) (hi - p) * rowH;
        /* Black-key pitches: C# D# F# G# A#  (mod 12 in {1,3,6,8,10}). */
        const int m = ((p % 12) + 12) % 12;
        const bool isBlack = (m == 1 || m == 3 || m == 6 || m == 8 || m == 10);
        if (isBlack)
        {
            g.setColour (rowBlack);
            g.fillRect (juce::Rectangle<float> (bodyL, y, bodyR - bodyL, rowH));
        }
        /* Row separator -- brighter on octave (every C). */
        g.setColour ((p % 12) == 0 ? rowOctaveEdge : rowEdge);
        g.drawHorizontalLine ((int) y, bodyL, bodyR);
    }

    /* Three-tier vertical grid: sub-snap (faintest) -> beat (medium)
     * -> bar (brightest).  Sub-snap lines drawn first so beat/bar
     * lines overpaint them at coincident X. */
    const int startBeat = juce::jmax (0, vr.getX() / pxPerBeat_);
    const int endBeat   = (vr.getRight() + pxPerBeat_ - 1) / pxPerBeat_;
    const float yT = (float) body.getY();
    const float yB = (float) body.getBottom();

    /* Sub-snap: only paint when snap is enabled, snapDivision_ subdivides
     * the beat (i.e. < 1.0), and at least 6 px between lines (avoids
     * visual noise when zoomed out). */
    if (snapEnabled_ && snapDivision_ > 0.0 && snapDivision_ < 1.0)
    {
        const double subPx = snapDivision_ * pxPerBeat_;
        if (subPx >= 6.0)
        {
            g.setColour (juce::Colour (0xff'1c'1c'1c));
            for (int beat = startBeat; beat <= endBeat; ++beat)
            {
                const double base = (double) beat;
                /* Step through subdivisions strictly between integer
                 * beats so the per-beat line doesn't get overdrawn by
                 * the sub-snap colour. */
                for (double sub = snapDivision_; sub < 1.0 - 1e-9; sub += snapDivision_)
                {
                    const int x = (int) std::round ((base + sub) * pxPerBeat_);
                    g.drawVerticalLine (x, yT, yB);
                }
            }
        }
    }

    /* Per-beat lines -- medium brightness. */
    g.setColour (juce::Colour (0xff'26'26'26));
    for (int beat = startBeat; beat <= endBeat; ++beat)
    {
        if (beat % beatsPerBar == 0) continue;
        const int x = beat * pxPerBeat_;
        g.drawVerticalLine (x, yT, yB);
    }

    /* Per-bar lines -- brightest. */
    g.setColour (juce::Colour (0xff'42'42'42));
    const int firstBar = (startBeat / beatsPerBar) * beatsPerBar;
    for (int beat = firstBar; beat <= endBeat; beat += beatsPerBar)
    {
        const int x = beat * pxPerBeat_;
        g.drawVerticalLine (x, yT, yB);
    }
}

void PianoRollGrid::paintRuler (juce::Graphics& g, int beatsPerBar)
{
    const auto vr = visibleRect();
    if (vr.getWidth() <= 0 || pxPerBeat_ <= 0) return;

    /* Palette mirrors ArrangementView::paintRuler -- bezel + cool-grey
     * vertical gradient inside + LCD-blue ticks at three brightness
     * tiers.  Single source of truth for the look so the piano-roll
     * + arrangement rulers read as siblings. */
    const juce::Colour kBezel       { 0xff'08'08'08 };
    const juce::Colour kBezelEdge   { 0xff'3a'3a'3a };
    const juce::Colour kLcdTop      { 0xff'14'19'1e };
    const juce::Colour kLcdBot      { 0xff'0c'0f'12 };
    const juce::Colour kLcdBlue     { 0xff'9e'dc'ff };
    const juce::Colour kLcdBlueMid  { 0xff'6f'b0'e0 };
    const juce::Colour kLcdBlueDim  { 0xff'4a'7c'a0 };

    const juce::Rectangle<int> ruler { 0, 0, getWidth(), kRulerH };

    /* Bezel + inset gradient + hairline divider against the body. */
    g.setColour (kBezel);
    g.fillRect (ruler);
    const auto inner = ruler.reduced (0, 2).toFloat();
    juce::ColourGradient lcdGrad (kLcdTop, inner.getX(), inner.getY(),
                                    kLcdBot, inner.getX(), inner.getBottom(),
                                    false);
    g.setGradientFill (lcdGrad);
    g.fillRect (inner);
    g.setColour (kBezelEdge);
    g.drawHorizontalLine (kRulerH - 1,
                            (float) vr.getX(),
                            (float) vr.getRight());

    /* Tick subdivision tracks the active snap setting -- the user
     * should always SEE the grid they're snapping to.  Snap off (or
     * snap >= 1 beat) falls back to a zoom-driven 4/2/1 ladder so the
     * ruler still has some sub-resolution at high zoom.  Triplet
     * snaps emit 3, 6 or 12 sub-ticks/beat (rounding to the nearest
     * supported subdivision count).  Final subdiv is capped so no
     * two adjacent sub-ticks land closer than ~5 px -- below that
     * they blur into a solid bar. */
    int subdiv = 1;
    if (snapEnabled_ && snapDivision_ > 0.0 && snapDivision_ <= 1.0)
    {
        const double subsPerBeat = 1.0 / snapDivision_;
        /* Round to nearest integer; covers duple (2/4/8/16/32) +
         * triplet (3/6/12) families since 1/0.125 = 8, 1/(1/6) = 6,
         * etc. */
        subdiv = juce::jlimit (1, 32, (int) std::round (subsPerBeat));
    }
    else
    {
        /* No snap -- use the prior zoom ladder so the ruler still
         * communicates beat resolution at high zoom. */
        subdiv = (pxPerBeat_ >= 32) ? 4
              : (pxPerBeat_ >= 16) ? 2 : 1;
    }
    while (subdiv > 1 && (double) pxPerBeat_ / (double) subdiv < 5.0)
        subdiv = juce::jmax (1, subdiv / 2);
    const int subStepPx = juce::jmax (1, pxPerBeat_ / subdiv);

    /* Loop bounds via clip rect intersected with visible -- caps the
     * work to what's actually drawn even on long regions. */
    const int x0 = juce::jmax (vr.getX(), g.getClipBounds().getX());
    const int x1 = juce::jmin (vr.getRight(), g.getClipBounds().getRight());
    const int subStart = juce::jmax (0, x0 / subStepPx - 1);
    const int subEnd   =                 x1 / subStepPx + 1;

    g.setFont (monoFont (10.0f, juce::Font::bold));

    for (int sub = subStart; sub <= subEnd; ++sub)
    {
        const int x = sub * subStepPx;
        if (x > vr.getRight()) break;
        const int beat   = sub / subdiv;
        const int phase  = sub % subdiv;
        const bool atBeat = (phase == 0);
        const bool atBar  = atBeat && (beat % beatsPerBar) == 0;

        int tickTop;
        juce::Colour tickCol;
        if      (atBar)  { tickTop = 3;            tickCol = kLcdBlue;    }
        else if (atBeat) { tickTop = kRulerH - 11; tickCol = kLcdBlueMid; }
        else             { tickTop = kRulerH - 6;  tickCol = kLcdBlueDim; }

        g.setColour (tickCol);
        g.drawVerticalLine (x, (float) tickTop, (float) (kRulerH - 2));

        if (atBar)
        {
            const int barNum = (beat / beatsPerBar) + 1;
            g.setColour (kLcdBlue);
            g.drawText (juce::String (barNum),
                        x + 3, 1, 32, kRulerH - 4,
                        juce::Justification::topLeft);
        }
    }
}

double PianoRollGrid::playheadLocalBeat() const noexcept
{
    if (monitor_ == nullptr || ! monitor_->playing.get()) return -1.0;
    auto* region = const_cast<PianoRollGrid*> (this)->resolveBoundRegion();
    if (region == nullptr) return -1.0;
    const double localBeat = (double) monitor_->getPositionBeats() - region->positionBeats;
    if (localBeat < 0.0 || localBeat > regionLenBeats_) return -1.0;
    return localBeat;
}

void PianoRollGrid::paintPlayhead (juce::Graphics& g)
{
    /* Draw at the x the timer last COMMITTED (playheadPxX_), NOT a fresh
     * transport read.  The repaint strip is sized around playheadPxX_; if
     * paint() re-read the (still-advancing) transport, the GL render thread
     * running a few ms behind the timer would draw the line at a slightly
     * different x than its strip covers -> the line lands outside its own
     * repainted region on some frames -> flicker.  Tying the line to the
     * committed px keeps strip + line in lockstep. */
    if (playheadPxX_ < 0) return;
    g.setColour (juce::Colour (0xff'40'ff'80).withAlpha (0.85f));
    g.drawVerticalLine (playheadPxX_, (float) kRulerH, (float) getHeight());
}

void PianoRollGrid::paintNotes (juce::Graphics& g, const MidiNoteRegion& region)
{
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;

    const auto vr = visibleRect();
    /* Cull rect: only paint notes whose pixel rect intersects what
     * the user can actually see, AND intersects the body (notes that
     * scrolled up under the ruler are clipped).  NB: culls to the whole
     * visible viewport (not the live clip) on purpose -- JUCE clips the
     * actual draws to the repainted strip, so a surgical playhead repaint
     * still only rasterises the strip, while every crossing note is
     * guaranteed to redraw (a clip-based cull risked dropping notes under
     * the GL partial-repaint path -> blank gaps trailing the playhead). */
    const auto cullRect = vr.getIntersection (body);

    /* Note style mirrors ArrangementView region paint: a darker
     * shaded interior tinted from the region's colour, with the full
     * saturated colour applied as the outer outline.  White note-name
     * label reads against the dim body across the velocity range
     * without the black-on-bright fallback the gradient style needed.
     * Velocity scales body brightness so harder-hit notes still stand
     * out -- but never bright enough to fight the label or the
     * saturated outline. */
    const juce::Colour baseBody = region.colour;
    const juce::Colour selWash  { 0xff'40'ff'80 };   /* kAccentGreen */
    const juce::Colour selEdge  { 0xff'cf'ff'd6 };

    /* Loop iterations for the paint walk.  Iteration 0 is the source
     * content (the editable notes); iterations >= 1 are read-only
     * ghosts the audio thread will emit when looped.  Matches the
     * arrangement-view repeat semantic so the two surfaces show the
     * same content at the same x-positions.  Non-looped regions and
     * regions whose loopLengthBeats >= lengthBeats paint a single
     * iteration as before. */
    const double loopPeriod = (region.looped
                                  && region.loopLengthBeats > 0.0)
                                  ? region.loopLengthBeats
                                  : regionLenBeats_;
    const int iterations = (region.looped
                               && loopPeriod > 0.0
                               && loopPeriod < regionLenBeats_)
        ? (int) std::ceil (regionLenBeats_ / loopPeriod)
        : 1;

    for (int iter = 0; iter < iterations; ++iter)
    {
        const double iterBeatOffset = (double) iter * loopPeriod;
        const bool   ghost          = (iter > 0);

        for (const auto& n : *snap)
        {
            if (n.pitch < lo || n.pitch > hi) continue;

            /* Ghost iterations only paint notes inside [0, loopPeriod)
             * of the source -- a note authored past the loop period
             * would never reach the next iteration in the audio
             * thread's emit window, so painting it as a ghost would
             * mis-signal what will play. */
            if (ghost && n.onBeat >= loopPeriod) continue;

            const int xRaw = (int) std::round ((n.onBeat + iterBeatOffset) * pxPerBeat_);
            const int wRaw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
            const int yRaw = body.getY() + (int) ((float) (hi - n.pitch) * rowH);
            const int hRaw = juce::jmax (2, (int) rowH - 1);

            const juce::Rectangle<int> rect (xRaw, yRaw, wRaw, hRaw);
            if (! cullRect.intersects (rect)) continue;

            const bool selected = ! ghost && isSelected (n.id);
            const float velNorm = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);

            /* Body fill (shaded/tinted) and outline (saturated tint) --
             * timeline-region pattern.  ArrangementView's MIDI region uses
             * sat x 0.55 / brightness x 0.45; piano-roll notes match that
             * cap exactly but scale brightness down with velocity so soft
             * notes still read as darker.  Selection uses the accent-green
             * wash + bright accent edge for unambiguous read.  Ghost
             * iterations dim further so iteration 0 reads as the source
             * of truth. */
            juce::Colour fill = selected
                ? selWash.withMultipliedSaturation (0.85f)
                         .withMultipliedBrightness (0.45f)
                : baseBody.withMultipliedSaturation (0.55f)
                           .withMultipliedBrightness (0.30f + 0.15f * velNorm);
            juce::Colour edge = selected ? selEdge : baseBody;
            if (ghost)
            {
                fill = fill.withMultipliedAlpha (0.55f);
                edge = edge.withMultipliedAlpha (0.55f);
            }

            const auto rf = rect.toFloat();
            const float corner = juce::jmin (3.0f, rf.getHeight() * 0.35f);

            g.setColour (fill);
            g.fillRoundedRectangle (rf, corner);

            g.setColour (edge);
            g.drawRoundedRectangle (rf, corner, selected ? 1.6f : 1.0f);

            /* Preview-affected highlight ring.  Painted on top of the
             * normal outline so the user can see which notes the dialog's
             * pending op will touch.  Soft amber so it reads as "pending"
             * rather than "selected" (which uses the accent green).
             * Ghost iterations skip the ring -- the source notes are the
             * editable ones; the rings stay anchored there. */
            if (! ghost
                && ! previewAffectedIds_.empty()
                && previewAffectedIds_.find (n.id) != previewAffectedIds_.end())
            {
                const juce::Colour previewRing { 0xff'ff'b3'4a };
                g.setColour (previewRing);
                g.drawRoundedRectangle (rf.expanded (1.5f), corner + 1.0f, 1.4f);
            }

            /* Pitch label inside wide-enough notes.  Skip for tiny rows
             * (would clip vertically) and short notes.  Always-white text
             * pairs cleanly with the dim shaded body across the velocity
             * range -- no per-note brightness branch required.  Ghost
             * iterations skip the label to keep the visual hierarchy
             * (iteration 0 reads as primary, ghosts as background). */
            if (! ghost && rf.getWidth() >= 40.0f && rf.getHeight() >= 12.0f)
            {
                g.setColour (juce::Colours::white.withAlpha (0.90f));
                g.setFont (monoFont (juce::jmin (10.0f, rf.getHeight() * 0.55f),
                                      juce::Font::plain));
                static const char* const pcs[12] = {
                    "C", "C#", "D", "D#", "E", "F",
                    "F#", "G", "G#", "A", "A#", "B" };
                const int oct = (n.pitch / 12) - 1;
                juce::String label = juce::String (pcs[n.pitch % 12]) + juce::String (oct);
                g.drawText (label,
                            rect.reduced (4, 1),
                            juce::Justification::centredLeft, false);
            }
        }
    }

    /* Dashed loop boundary lines at each iteration boundary.  Mirrors
     * the arrangement-view cue so the user reads "this is where the
     * loop wraps" the same way on both surfaces. */
    if (iterations > 1)
    {
        const float dashLengths[] = { 4.0f, 4.0f };
        g.setColour (baseBody.withAlpha (0.55f));
        for (int iter = 1; iter < iterations; ++iter)
        {
            const int bx = (int) std::round ((double) iter * loopPeriod * pxPerBeat_);
            if (bx < body.getX() + 1) continue;
            if (bx > body.getRight() - 1) break;
            const juce::Line<float> seg ((float) bx, (float) body.getY(),
                                           (float) bx, (float) body.getBottom());
            g.drawDashedLine (seg, dashLengths, 2, 1.4f);
        }
    }
}

void PianoRollGrid::paintActiveDragOverlay (juce::Graphics& g)
{
    if (activeDrag_ == nullptr) return;
    activeDrag_->paintOverlay (g, *this);
}

//==============================================================================
// Hit test.

std::uint64_t PianoRollGrid::hitTestNote (int x, int y,
                                            const MidiNoteRegion& region) const noexcept
{
    if (y < kRulerH) return 0;   /* ruler clicks aren't note hits */
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;

    /* Iterate in reverse so notes painted on top (later in the
     * sorted snapshot if any overlap) win the hit. */
    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        if (n.pitch < lo || n.pitch > hi) continue;
        const int nx = (int) std::round (n.onBeat * pxPerBeat_);
        const int nw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int ny = body.getY() + (int) ((float) (hi - n.pitch) * rowH);
        const int nh = juce::jmax (2, (int) rowH - 1);
        if (x >= nx && x < nx + nw && y >= ny && y < ny + nh)
            return n.id;
    }
    return 0;
}

std::uint64_t PianoRollGrid::hitTestResizeHandle (int x, int y,
                                                    const MidiNoteRegion& region) const noexcept
{
    bool dummy = false;
    return hitTestResizeHandleEx (x, y, region, dummy);
}

std::uint64_t PianoRollGrid::hitTestResizeHandleEx (int x, int y,
                                                      const MidiNoteRegion& region,
                                                      bool& outLeftEdge) const noexcept
{
    outLeftEdge = false;
    if (y < kRulerH) return 0;
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;

    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        if (n.pitch < lo || n.pitch > hi) continue;
        const int nx = (int) std::round (n.onBeat * pxPerBeat_);
        const int nw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int ny = body.getY() + (int) ((float) (hi - n.pitch) * rowH);
        const int nh = juce::jmax (2, (int) rowH - 1);
        if (y < ny || y >= ny + nh) continue;

        /* Right-edge handle: within kResizeHandlePx of nx+nw. */
        if (x >= nx + nw - kResizeHandlePx && x < nx + nw + 1)
        {
            outLeftEdge = false;
            return n.id;
        }
        /* Left-edge handle: within kResizeHandlePx of nx.  Tiny notes
         * (nw <= 2 * kResizeHandlePx) would have the two handles
         * overlap -- preferring the right handle keeps the
         * extend-rightward gesture intuitive on short notes. */
        if (nw > 2 * kResizeHandlePx
            && x >= nx && x < nx + kResizeHandlePx)
        {
            outLeftEdge = true;
            return n.id;
        }
    }
    return 0;
}

//==============================================================================
// Mouse handlers.

void PianoRollGrid::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    /* Ruler band absorbs clicks -- no marquee/pencil-create above the
     * note body.  Transport-seek via ruler click is a Session 4 item. */
    if (e.y < kRulerH) return;

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;

    /* "Beyond region" zone -- if the grid component is wider than the
     * bound region's beat span (auto-fit on bind, or user shrank the
     * region after a zoom), clicks past the region end are no-ops.
     * Prevents pencil-create from placing notes outside the region. */
    const int regionEndX = (int) std::round (regionLenBeats_ * pxPerBeat_);
    if (e.x >= regionEndX)
    {
        /* Click in the void = clear selection, same as empty-body
         * click inside the region. */
        if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown())
            selectClear();
        return;
    }

    /* Brush tool short-circuits hit-test + selection so it always
     * paints fresh notes regardless of what's underneath the cursor.
     * No selection update either -- the brush is purely additive. */
    if (parent_.getActiveTool() == PianoRollView::Tool::Brush)
    {
        activeDrag_ = NoteDrag::makeBrush (*this, *region, e);
        return;
    }

    /* First try the resize handle hit (5 px at the right or left
     * edge of any note) regardless of tool -- consistent affordance
     * like all DAWs.  Right-edge resize extends the note's tail;
     * left-edge resize trims into the head, mirroring the arranger's
     * Slice-4 left-trim shape.  Then fall back to body hit + branch
     * on active tool. */
    if (parent_.getActiveTool() == PianoRollView::Tool::Select)
    {
        bool leftEdge = false;
        if (auto hitResize = hitTestResizeHandleEx (e.x, e.y, *region, leftEdge))
        {
            if (! isSelected (hitResize))
                selectOnly (hitResize);
            activeDrag_ = leftEdge
                ? NoteDrag::makeResizeLeft (*this, *region, hitResize, e)
                : NoteDrag::makeResize     (*this, *region, hitResize, e);
            return;
        }
    }

    const auto hitId = hitTestNote (e.x, e.y, *region);

    if (hitId != 0)
    {
        /* Selection update before drag setup so the drag captures the
         * full set in motion. */
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            selectToggle (hitId);
        else if (! isSelected (hitId))
            selectOnly (hitId);

        if (parent_.getActiveTool() == PianoRollView::Tool::Erase)
        {
            /* Erase tool on a hit -> remove + push undo. */
            if (auto* gui = services_.find<GuiService>())
            {
                auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                   parent_.getRegionResolver());
                cmd->recordRemove (*region, hitId);
                juce::Component::SafePointer<PianoRollView> safeView (&parent_);
                cmd->onApplied = [safeView]() {
                    if (auto* v = safeView.getComponent())
                        v->notifyRegionEdited();
                };
                /* Start a fresh transaction so this diff doesn't
                 * coalesce with whatever the ArrangementView /
                 * SessionView left open -- without this, Ctrl+Z would
                 * undo the cross-surface group rather than just the
                 * piano-roll edit. */
                gui->getUndoManager().beginNewTransaction ("Erase MIDI note");
                gui->getUndoManager().perform (cmd.release(), "Erase MIDI note");
            }
            else
            {
                region->removeNoteById (hitId);
                parent_.notifyRegionEdited();
            }
            selectedNoteIds_.erase (hitId);
            repaint();
            return;
        }

        /* Select / Pencil on a body hit -> move drag. */
        activeDrag_ = NoteDrag::makeMove (*this, *region, hitId, e);
        return;
    }

    /* Empty area click. */
    if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown())
        selectClear();

    if (parent_.getActiveTool() == PianoRollView::Tool::Pencil)
    {
        activeDrag_ = NoteDrag::makeCreate (*this, *region, e);
        return;
    }

    /* Select + Erase on empty area -> marquee rubber-band. */
    activeDrag_ = NoteDrag::makeMarquee (*this, e,
                                           parent_.getActiveTool() == PianoRollView::Tool::Erase);
}

void PianoRollGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (activeDrag_ != nullptr)
        activeDrag_->mouseDrag (e, *this);
}

void PianoRollGrid::mouseUp (const juce::MouseEvent& e)
{
    if (activeDrag_ == nullptr) return;
    auto drag = std::move (activeDrag_);
    drag->mouseUp (e, *this);
    repaint();
}

void PianoRollGrid::mouseMove (const juce::MouseEvent& e)
{
    /* Update cursor: resize handle = horizontal-resize cursor; over a
     * note body = grab cursor (Select/Pencil) or x cursor (Erase);
     * empty area = pencil cursor (Pencil) or normal (Select/Erase). */
    if (auto* region = resolveBoundRegion())
    {
        if (parent_.getActiveTool() == PianoRollView::Tool::Select
            && hitTestResizeHandle (e.x, e.y, *region) != 0)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }
        if (hitTestNote (e.x, e.y, *region) != 0)
        {
            setMouseCursor (parent_.getActiveTool() == PianoRollView::Tool::Erase
                              ? juce::MouseCursor::PointingHandCursor
                              : juce::MouseCursor::DraggingHandCursor);
            return;
        }
    }
    const auto tool = parent_.getActiveTool();
    setMouseCursor ((tool == PianoRollView::Tool::Pencil
                     || tool == PianoRollView::Tool::Brush)
                      ? juce::MouseCursor::CrosshairCursor
                      : juce::MouseCursor::NormalCursor);
}

void PianoRollGrid::mouseWheelMove (const juce::MouseEvent& e,
                                      const juce::MouseWheelDetails& wheel)
{
    /* Wheel conventions (piano-roll standard):
     *   no mod      -> vertical PAN through pitches
     *   shift+wheel -> horizontal scroll (defer to viewport)
     *   alt+wheel   -> vertical zoom around centre
     *   cmd/ctrl+wheel -> horizontal zoom anchored at cursor X */
    if (e.mods.isAltDown())
    {
        if (auto* kb = parent_.getKeyboard())
        {
            const double factor = (wheel.deltaY > 0.0f) ? (1.0 / 1.15) : 1.15;
            kb->zoomVertically (factor);
            /* Keyboard repaint already triggered by setVisibleNoteRange;
             * the grid's own paint reads kb->getLowestVisibleNoteNumber
             * + getHighestVisibleNoteNumber live, so a repaint here
             * picks up the new pitch span. */
            repaint();
        }
        return;
    }

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        /* Zoom: cmd / ctrl + scroll.  Anchor zoom around the mouse X
         * so the beat under the cursor stays put.  We resize in the
         * grid's own coords; the surrounding Viewport will adjust
         * its scrollPos to keep the visible area sensible. */
        const double mouseBeat = (pxPerBeat_ > 0)
            ? (double) e.x / (double) pxPerBeat_
            : 0.0;

        const int delta = (wheel.deltaY > 0.0f) ? 4 : -4;
        const int next  = juce::jlimit (kPxPerBeatMin,
                                          kPxPerBeatMax,
                                          pxPerBeat_ + delta);
        if (next == pxPerBeat_) return;
        pxPerBeat_ = next;

        /* Resize self so the viewport scrollbar tracks. */
        const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
        setSize (juce::jmax (spanW, 1), getHeight());

        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const int newMouseX = (int) std::round (mouseBeat * pxPerBeat_);
            const int dx        = newMouseX - e.x;
            const int viewX     = juce::jmax (0, vp->getViewPositionX() + dx);
            vp->setViewPosition (viewX, vp->getViewPositionY());
        }

        repaint();
        return;
    }

    /* Plain wheel: vertical pan through pitches.  Scroll AMOUNT is
     * derived from wheel.deltaY so a high-resolution trackpad gets
     * sub-row precision while a notched mouse wheel still moves a
     * meaningful chunk per click.  Negative sign because wheel.deltaY
     * positive == finger moving up (= scroll upward in content =
     * raise visible-pitch lower-bound).
     *
     * Shift+wheel forwards to the parent component, letting the host
     * viewport handle horizontal scroll the same way a non-modified
     * wheel did before this change. */
    if (e.mods.isShiftDown())
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    if (auto* kb = parent_.getKeyboard())
    {
        /* Convert wheel.deltaY (typically -1.0..+1.0 per notch) into a
         * semitone delta.  ~3 semitones per full wheel tick feels
         * close to JUCE's default scroll speed in other vertical
         * surfaces without being so coarse that an octave click skips
         * the target row entirely. */
        const float k = wheel.isReversed ? -3.0f : 3.0f;
        int delta = (int) std::lround (-wheel.deltaY * k);
        if (delta == 0)
            delta = (wheel.deltaY > 0.0f) ? -1 : (wheel.deltaY < 0.0f ? 1 : 0);
        if (delta != 0)
        {
            kb->shiftVisibleRange (delta);
            repaint();
        }
        return;
    }

    juce::Component::mouseWheelMove (e, wheel);
}

bool PianoRollGrid::keyPressed (const juce::KeyPress& key)
{
    auto* region = resolveBoundRegion();

    /* Delete / Backspace -- remove selected notes via undoable diff. */
    if (key == juce::KeyPress::deleteKey
        || key == juce::KeyPress::backspaceKey)
    {
        if (selectedNoteIds_.empty()) return false;
        if (region == nullptr) return false;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto id : selectedNoteIds_)
                cmd->recordRemove (*region, id);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().beginNewTransaction ("Delete MIDI notes");
            gui->getUndoManager().perform (cmd.release(), "Delete MIDI notes");
        }
        else
        {
            for (auto id : selectedNoteIds_)
                region->removeNoteById (id);
            parent_.notifyRegionEdited();
        }
        selectedNoteIds_.clear();
        repaint();
        return true;
    }

    /* Ctrl+C -- copy current selection to the piano-roll clipboard,
     * rebasing onBeats so the selection's earliest note lands at 0.
     * Paste re-anchors against the playhead or the bound region's
     * start.  Per-process static clipboard. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'C')
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return false;
        std::vector<MidiNote> sel;
        double minOn = std::numeric_limits<double>::max();
        for (const auto& n : *snap)
            if (isSelected (n.id))
            {
                sel.push_back (n);
                minOn = juce::jmin (minOn, n.onBeat);
            }
        if (sel.empty()) return false;
        double maxEnd = 0.0;
        for (auto& n : sel)
        {
            n.onBeat -= minOn;
            maxEnd    = juce::jmax (maxEnd, n.onBeat + n.lengthBeats);
        }
        auto& clip = noteClipboard();
        clip.notes      = std::move (sel);
        clip.spanBeats  = maxEnd;
        return true;
    }

    /* Ctrl+X -- cut: copy then delete in one undo step. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'X')
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return false;

        std::vector<MidiNote> sel;
        double minOn = std::numeric_limits<double>::max();
        for (const auto& n : *snap)
            if (isSelected (n.id))
            {
                sel.push_back (n);
                minOn = juce::jmin (minOn, n.onBeat);
            }
        if (sel.empty()) return false;
        double maxEnd = 0.0;
        auto copy = sel;
        for (auto& n : copy)
        {
            n.onBeat -= minOn;
            maxEnd    = juce::jmax (maxEnd, n.onBeat + n.lengthBeats);
        }
        auto& clip = noteClipboard();
        clip.notes      = std::move (copy);
        clip.spanBeats  = maxEnd;

        /* Delete the originals.  Same undo path as Delete key. */
        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (const auto& n : sel) cmd->recordRemove (*region, n.id);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().beginNewTransaction ("Cut MIDI notes");
            gui->getUndoManager().perform (cmd.release(), "Cut MIDI notes");
        }
        selectedNoteIds_.clear();
        repaint();
        return true;
    }

    /* Ctrl+V -- paste clipboard at the playhead (region-local).  Falls
     * back to beat 0 if the transport isn't reachable.  Notes get
     * fresh ids stamped on insert; the paste set replaces the current
     * selection so the user can keep editing the freshly-pasted
     * material. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'V')
    {
        const auto& clip = noteClipboard();
        if (region == nullptr || clip.empty()) return false;

        double anchor = 0.0;
        if (monitor_ != nullptr && monitor_->playing.get())
        {
            const double transportBeat = (double) monitor_->getPositionBeats();
            anchor = juce::jmax (0.0, transportBeat - region->positionBeats);
        }
        anchor = juce::jlimit (0.0,
                                juce::jmax (0.0, region->lengthBeats - clip.spanBeats),
                                anchor);

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            std::vector<MidiNote> stagedForSelect;
            stagedForSelect.reserve (clip.notes.size());
            for (const auto& src : clip.notes)
            {
                MidiNote n = src;
                n.id     = 0;            /* fresh id stamped on add */
                n.onBeat = src.onBeat + anchor;
                if (n.onBeat + n.lengthBeats > region->lengthBeats)
                    n.lengthBeats = juce::jmax (1.0 / 64.0,
                                                  region->lengthBeats - n.onBeat);
                cmd->recordAdd (*region, n);
                stagedForSelect.push_back (n);
            }
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().beginNewTransaction ("Paste MIDI notes");
            gui->getUndoManager().perform (cmd.release(), "Paste MIDI notes");

            /* Reselect the pasted notes -- their ids landed in the
             * snapshot after perform.  Match by (pitch, onBeat ±eps)
             * which is unique enough for the typical paste set. */
            selectedNoteIds_.clear();
            if (const auto* snap2 = region->loadSnapshot())
                for (const auto& n : *snap2)
                    for (const auto& s : stagedForSelect)
                        if (n.pitch == s.pitch
                            && std::abs (n.onBeat - s.onBeat) < 1e-9
                            && n.channel == s.channel
                            && selectedNoteIds_.find (n.id) == selectedNoteIds_.end())
                        {
                            selectedNoteIds_.insert (n.id);
                            break;
                        }
        }
        parent_.notifyRegionEdited();
        repaint();
        return true;
    }

    /* Ctrl+A -- select every note in the bound region. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'A')
    {
        if (region == nullptr) return false;
        if (const auto* snap = region->loadSnapshot())
        {
            selectedNoteIds_.clear();
            for (const auto& n : *snap)
                selectedNoteIds_.insert (n.id);
            repaint();
        }
        return true;
    }

    /* Shift +/- zoom in / out around the viewport centre.  Mirrors
     * the arranger's Shift +/- shape so the editing surfaces feel
     * uniform.  Anchor zoom (cmd / ctrl + wheel) handles its own
     * pivot; these versions zoom around the visible centre. */
    if (key.getModifiers().isShiftDown())
    {
        if (key.isKeyCode ('=') || key.isKeyCode ('+'))
        {
            zoomBy (1.20);
            return true;
        }
        if (key.isKeyCode ('-') || key.isKeyCode ('_'))
        {
            zoomBy (1.0 / 1.20);
            return true;
        }
    }

    /* Ctrl+Q -- snap onsets of selected notes to the current grid
     * division.  Primary trigger surface is the toolbar's Q button on
     * the piano-roll header; this hotkey is the power-user alias.
     * Routed through quantizeSelection() so the toolbar + hotkey share
     * one implementation and stay in lockstep with future ops engine
     * changes. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Q')
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        quantizeSelection();
        return true;
    }

    /* Ctrl+D -- duplicate selection one snap-division to the right.
     * Each duplicate gets a fresh id; final selection contains the
     * duplicates so subsequent edits operate on the new notes. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'D')
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return false;

        const double step = isSnapEnabled() && getSnapDivision() > 0.0
                             ? getSnapDivision()
                             : 1.0;
        std::vector<MidiNote> dupes;
        dupes.reserve (selectedNoteIds_.size());
        for (const auto& n : *snap)
            if (isSelected (n.id))
            {
                MidiNote copy = n;
                copy.id     = 0;   /* fresh id stamped on add */
                copy.onBeat = n.onBeat + step;
                if (copy.onBeat < region->lengthBeats)
                    dupes.push_back (copy);
            }
        if (dupes.empty()) return true;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto& d : dupes)
                cmd->recordAdd (*region, d);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().beginNewTransaction ("Duplicate MIDI notes");
            gui->getUndoManager().perform (cmd.release(), "Duplicate MIDI notes");
        }
        else
        {
            for (auto& d : dupes)
                region->addNote (d);
        }
        /* Re-select the duplicates -- ids assigned by recordAdd are
         * stable post-perform; the snapshot has new ids in the same
         * order as `dupes`, so iterating again finds them. */
        selectedNoteIds_.clear();
        if (const auto* snap2 = region->loadSnapshot())
        {
            for (const auto& n : *snap2)
                for (const auto& d : dupes)
                    if (n.pitch == d.pitch
                        && std::abs (n.onBeat - d.onBeat) < 1e-9
                        && n.channel == d.channel
                        && ! isSelected (n.id))
                    {
                        selectedNoteIds_.insert (n.id);
                        break;
                    }
        }
        parent_.notifyRegionEdited();
        repaint();
        return true;
    }

    /* Arrow keys -- nudge selection.  Left/Right by snap division (or
     * 1 beat if snap off); Up/Down by 1 semitone; Shift+Up/Down by 12
     * (octave).  All routed through the undo path so each press is a
     * step the user can rewind. */
    auto nudge = [&] (double beatDelta, int pitchDelta, const juce::String& label) -> bool
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return false;

        std::vector<std::pair<MidiNote, MidiNote>> moves;
        moves.reserve (selectedNoteIds_.size());
        for (const auto& n : *snap)
            if (isSelected (n.id))
            {
                MidiNote after = n;
                after.onBeat = juce::jmax (0.0, n.onBeat + beatDelta);
                if (after.onBeat + after.lengthBeats > region->lengthBeats)
                    after.onBeat = juce::jmax (0.0,
                        region->lengthBeats - after.lengthBeats);
                after.pitch  = juce::jlimit (0, 127, n.pitch + pitchDelta);
                if (after.pitch == n.pitch
                    && std::abs (after.onBeat - n.onBeat) < 1e-9) continue;
                moves.emplace_back (n, after);
            }
        if (moves.empty()) return true;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto& mv : moves)
                cmd->recordUpdate (mv.first.id, mv.first, mv.second);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().beginNewTransaction (label);
            gui->getUndoManager().perform (cmd.release(), label);
        }
        else
        {
            for (auto& mv : moves)
                region->updateNoteById (mv.first.id, mv.second);
        }
        parent_.notifyRegionEdited();
        repaint();
        return true;
    };

    if (key == juce::KeyPress::leftKey
        || key == juce::KeyPress::rightKey
        || key == juce::KeyPress::upKey
        || key == juce::KeyPress::downKey)
    {
        const double step = isSnapEnabled() && getSnapDivision() > 0.0
                             ? getSnapDivision()
                             : 1.0;
        if (key == juce::KeyPress::leftKey)  return nudge (-step, 0,  "Nudge left");
        if (key == juce::KeyPress::rightKey) return nudge ( step, 0,  "Nudge right");
        const int p = key.getModifiers().isShiftDown() ? 12 : 1;
        if (key == juce::KeyPress::upKey)    return nudge (0,  p, "Transpose up");
        if (key == juce::KeyPress::downKey)  return nudge (0, -p, "Transpose down");
    }

    return false;
}

namespace {

/* Convert the grid's snapDivision (a beats double set by
 * PianoRollView::applySnapFromComboBox) back to the (NoteLength,
 * NoteType) pair the quantize ops engine consumes.  The mapping is the
 * inverse of the snapBox id table -- each unique snapDivision value
 * lines up with exactly one (NoteLength, NoteType) pair.  Falls back
 * to (Sixteenth, Normal) when the division doesn't match a known
 * preset (e.g. a future custom division). */
std::pair<dsp::quantize::NoteLength, dsp::quantize::NoteType>
divisionToNoteShape (double divisionBeats) noexcept
{
    using L = dsp::quantize::NoteLength;
    using T = dsp::quantize::NoteType;
    constexpr double eps = 1.0e-6;
    auto near = [eps] (double a, double b) { return std::abs (a - b) < eps; };

    if (near (divisionBeats, 4.0))         return { L::Whole,        T::Normal  };
    if (near (divisionBeats, 2.0))         return { L::Half,         T::Normal  };
    if (near (divisionBeats, 1.0))         return { L::Quarter,      T::Normal  };
    if (near (divisionBeats, 0.5))         return { L::Eighth,       T::Normal  };
    if (near (divisionBeats, 0.25))        return { L::Sixteenth,    T::Normal  };
    if (near (divisionBeats, 0.125))       return { L::ThirtySecond, T::Normal  };
    if (near (divisionBeats, 2.0 / 3.0))   return { L::Quarter,      T::Triplet };
    if (near (divisionBeats, 1.0 / 3.0))   return { L::Eighth,       T::Triplet };
    if (near (divisionBeats, 1.0 / 6.0))   return { L::Sixteenth,    T::Triplet };
    return { L::Sixteenth, T::Normal };
}

} // namespace

namespace {

/* Shared apply-and-push template for all three ops.  `populate` runs
 * the dsp::quantize op against the bound region + selection, filling
 * `cmd` with updates.  If anything was touched, the command is pushed
 * onto the GuiService UndoManager with `label`; the standard
 * onApplied -> notifyRegionEdited hook is wired so the session XML
 * write fires.  Falls back to direct cmd.perform() when no
 * GuiService is available -- mirrors the delete-key path. */
template <typename Populate>
std::size_t applyOpAndPush (PianoRollGrid&           grid,
                              MidiNoteRegion&          region,
                              const std::unordered_set<std::uint64_t>& selection,
                              PianoRollView&           parent,
                              Services&                services,
                              const juce::String&      label,
                              Populate&&               populate)
{
    if (selection.empty()) return 0;

    if (auto* gui = services.find<GuiService>())
    {
        auto cmd = std::make_unique<MidiNoteDiffCommand> (parent.getBoundRegionId(),
                                                            parent.getRegionResolver());
        const std::size_t touched = populate (region, selection, *cmd);
        if (touched == 0 || cmd->isEmpty()) return 0;

        juce::Component::SafePointer<PianoRollView> safeView (&parent);
        cmd->onApplied = [safeView]() {
            if (auto* v = safeView.getComponent())
                v->notifyRegionEdited();
        };
        /* Fresh transaction so this diff doesn't coalesce with any
         * still-open ArrangementView / SessionView transaction.  Same
         * pattern as the other piano-roll perform sites. */
        gui->getUndoManager().beginNewTransaction (label);
        gui->getUndoManager().perform (cmd.release(), label);
        grid.repaint();
        return touched;
    }

    MidiNoteDiffCommand cmd (parent.getBoundRegionId(),
                              parent.getRegionResolver());
    const std::size_t touched = populate (region, selection, cmd);
    if (touched == 0 || cmd.isEmpty()) return 0;
    cmd.perform();
    parent.notifyRegionEdited();
    grid.repaint();
    return touched;
}

/* Single-pass preview-id helper.  Builds a throwaway diff command
 * with a no-op resolver, runs the SAME dsp::quantize op the apply
 * path will run, then reads out the touched-id list directly from
 * the command.  No duplicated compare logic -- the preview ring is
 * guaranteed to match what Apply will commit (closes the A.3 gap
 * from the audit doc). */
template <typename PopulateOp>
std::vector<std::uint64_t> idsTouchedBy (const MidiNoteRegion& region,
                                           const std::unordered_set<std::uint64_t>& selection,
                                           PopulateOp&& populate)
{
    MidiNoteDiffCommand cmd (juce::Uuid::null(),
                              [] (const juce::Uuid&) -> MidiNoteRegion* { return nullptr; });
    populate (region, selection, cmd);
    return cmd.touchedIds();
}

} // namespace

std::size_t PianoRollGrid::applyQuantize (const dsp::quantize::QuantizeOptions& opts)
{
    auto* region = resolveBoundRegion();
    if (region == nullptr) return 0;
    return applyOpAndPush (*this, *region, selectedNoteIds_, parent_, services_,
                            "Quantize MIDI notes",
                            [&opts] (MidiNoteRegion& r,
                                     const std::unordered_set<std::uint64_t>& sel,
                                     MidiNoteDiffCommand& cmd)
                            {
                                return dsp::quantize::quantizeNotes (r, sel, opts, cmd);
                            });
}

std::size_t PianoRollGrid::applyHumanize (const dsp::quantize::HumanizeOptions& opts)
{
    auto* region = resolveBoundRegion();
    if (region == nullptr) return 0;
    return applyOpAndPush (*this, *region, selectedNoteIds_, parent_, services_,
                            "Humanize MIDI velocities",
                            [&opts] (MidiNoteRegion& r,
                                     const std::unordered_set<std::uint64_t>& sel,
                                     MidiNoteDiffCommand& cmd)
                            {
                                return dsp::quantize::humanizeVelocity (r, sel, opts, cmd);
                            });
}

std::size_t PianoRollGrid::applyScale (dsp::quantize::Scale scale, int rootSemitone)
{
    auto* region = resolveBoundRegion();
    if (region == nullptr) return 0;
    return applyOpAndPush (*this, *region, selectedNoteIds_, parent_, services_,
                            "Scale-snap MIDI notes",
                            [scale, rootSemitone] (MidiNoteRegion& r,
                                                   const std::unordered_set<std::uint64_t>& sel,
                                                   MidiNoteDiffCommand& cmd)
                            {
                                return dsp::quantize::scaleQuantize (r, sel, scale, rootSemitone, cmd);
                            });
}

std::vector<std::uint64_t>
PianoRollGrid::previewQuantizeIds (const dsp::quantize::QuantizeOptions& opts) const
{
    const auto* region = const_cast<PianoRollGrid*> (this)->resolveBoundRegion();
    if (region == nullptr || selectedNoteIds_.empty()) return {};
    return idsTouchedBy (*region, selectedNoteIds_,
                          [&opts] (const MidiNoteRegion& r,
                                   const std::unordered_set<std::uint64_t>& sel,
                                   MidiNoteDiffCommand& cmd)
                          { dsp::quantize::quantizeNotes (r, sel, opts, cmd); });
}

std::vector<std::uint64_t>
PianoRollGrid::previewHumanizeIds (const dsp::quantize::HumanizeOptions& opts) const
{
    const auto* region = const_cast<PianoRollGrid*> (this)->resolveBoundRegion();
    if (region == nullptr || selectedNoteIds_.empty()) return {};
    return idsTouchedBy (*region, selectedNoteIds_,
                          [&opts] (const MidiNoteRegion& r,
                                   const std::unordered_set<std::uint64_t>& sel,
                                   MidiNoteDiffCommand& cmd)
                          { dsp::quantize::humanizeVelocity (r, sel, opts, cmd); });
}

std::vector<std::uint64_t>
PianoRollGrid::previewScaleIds (dsp::quantize::Scale scale, int rootSemitone) const
{
    const auto* region = const_cast<PianoRollGrid*> (this)->resolveBoundRegion();
    if (region == nullptr || selectedNoteIds_.empty()) return {};
    return idsTouchedBy (*region, selectedNoteIds_,
                          [scale, rootSemitone]
                          (const MidiNoteRegion& r,
                           const std::unordered_set<std::uint64_t>& sel,
                           MidiNoteDiffCommand& cmd)
                          { dsp::quantize::scaleQuantize (r, sel, scale, rootSemitone, cmd); });
}

void PianoRollGrid::setPreviewAffectedNotes (std::vector<std::uint64_t> ids)
{
    previewAffectedIds_.clear();
    for (auto id : ids) previewAffectedIds_.insert (id);
    repaint();
}

void PianoRollGrid::clearPreviewAffectedNotes() noexcept
{
    if (previewAffectedIds_.empty()) return;
    previewAffectedIds_.clear();
    repaint();
}

void PianoRollGrid::quantizeSelection()
{
    if (selectedNoteIds_.empty()) return;
    /* Hotkey-driven default: use the view's last-used QuantizeOptions
     * (set by the dialog).  If the user has never opened the dialog,
     * fall back to options derived from the current snap division at
     * full amount, start-only.  Lives here -- not on PianoRollView --
     * because the snap-division -> NoteLength mapping is grid-private. */
    dsp::quantize::QuantizeOptions opts = parent_.getLastQuantizeOptions();
    if (! parent_.isLastQuantizeUserAdjusted())
    {
        const auto shape = divisionToNoteShape (snapDivision_);
        opts.noteLength  = shape.first;
        opts.noteType    = shape.second;
        opts.amount      = 1.0;
        opts.adjustStart = true;
        opts.adjustEnd   = false;
    }
    applyQuantize (opts);
}

void PianoRollGrid::humanizeSelection()
{
    if (selectedNoteIds_.empty()) return;
    const auto opts = parent_.getLastHumanizeOptions();
    applyHumanize (opts);
}

} // namespace element
