// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/arrangementview.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/engine.hpp>
#include <element/node.hpp>
#include <element/session.hpp>
#include <element/services.hpp>
#include <element/tags.hpp>
#include <element/ui.hpp>
#include <element/ui/style.hpp>

#include "nodes/audioclip.hpp"
#include "nodes/midiplayer.hpp"
#include "nodes/tracker.hpp"
#include "engine/graphnode.hpp"
#include "dsp/automation/automation_region.hpp"
#include "dsp/automation/automation_track.hpp"
#include "services/automation/automation_engine.hpp"
#include "services/automation/automation_session.hpp"
#include "services/automation/automation_target_resolver.hpp"
#include "services/arrangementtracksservice.hpp"
#include "services/sources/sourceregistry.hpp"
#include "services/timeline/audiolaneadapter.hpp"
#include "ui/fontcache.hpp"
#include "ui/lanepalette.hpp"
#include "ui/viewhelpers.hpp"
#include <element/ui/standard.hpp>

#include <climits>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

namespace element {

using juce::Colour;
using juce::Graphics;
using juce::MouseEvent;
using juce::Rectangle;
using juce::String;
namespace Colours = juce::Colours;

namespace {

/** Acceptable audio extensions for external file drop.  Lowercase
 *  comparison via String::endsWithIgnoreCase. */
const char* const kAudioDropExtensions[] = {
    ".wav", ".aiff", ".aif", ".flac", ".ogg", ".mp3", ".w64", ".au"
};

/** Acceptable MIDI extensions for external file drop.  Only .mid/.midi
 *  for now; karaoke (.kar) is structurally SMF but unconventional. */
const char* const kMidiDropExtensions[] = {
    ".mid", ".midi"
};

/* Lane tint palette lives in ui/lanepalette.hpp (shared with the
 * tracker editor + future session-view clip cells).  Local helper
 * kept so call sites read naturally. */
inline juce::Colour laneTintForIndex (int idx) noexcept
{
    return element::ui::laneTint (idx);
}

/** Distinct, readable-on-dark colour per automation binding, so
 *  superimposed overlay curves + their chip-rail tabs are easy to tell
 *  apart.  Cycles a fixed palette by binding index. */
inline juce::Colour automationBindingColour (int idx) noexcept
{
    static const juce::uint32 kPalette[] = {
        0xff'4a'c8'a0,  // teal
        0xff'e8'a1'3a,  // orange
        0xff'6a'a8'e8,  // blue
        0xff'd0'6a'e0,  // magenta
        0xff'9a'd8'4a,  // lime
        0xff'e8'5a'6a,  // red
        0xff'e0'c8'4a,  // yellow
        0xff'8a'6a'e0,  // violet
        0xff'4a'c8'e0,  // cyan
        0xff'e0'7a'3a,  // amber
    };
    constexpr int n = (int) (sizeof (kPalette) / sizeof (kPalette[0]));
    return juce::Colour (kPalette[((idx % n) + n) % n]);
}

bool isAcceptableAudioFile (const juce::String& path) noexcept
{
    for (const char* ext : kAudioDropExtensions)
        if (path.endsWithIgnoreCase (ext))
            return true;
    return false;
}

bool isAcceptableMidiFile (const juce::String& path) noexcept
{
    for (const char* ext : kMidiDropExtensions)
        if (path.endsWithIgnoreCase (ext))
            return true;
    return false;
}

bool anyAudioFileIn (const juce::StringArray& files) noexcept
{
    for (const auto& f : files)
        if (isAcceptableAudioFile (f))
            return true;
    return false;
}

bool anyMidiFileIn (const juce::StringArray& files) noexcept
{
    for (const auto& f : files)
        if (isAcceptableMidiFile (f))
            return true;
    return false;
}

bool anyDroppableFileIn (const juce::StringArray& files) noexcept
{
    return anyAudioFileIn (files) || anyMidiFileIn (files);
}

} // namespace

/* ===================================================================== */
/* Body: inline-paint component drawing every lane in one paint() pass.  */
/* Also the FileDragAndDropTarget so external file drops hit the lanes.  */
/* ===================================================================== */

class ArrangementView::Body : public juce::Component,
                              public juce::FileDragAndDropTarget,
                              private juce::ChangeListener
{
public:
    explicit Body (ArrangementView& o)
        : owner (o),
          thumbnailCache_ (kThumbnailCacheEntries)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);   // Delete key handling

        /* Register stock audio formats once.  Same shape Sampler /
         * AudioFilePlayerNode use; libsndfile is the underlying
         * reader for WAV/AIF/FLAC under JUCE_LINUX (no wineserver). */
        formatManager_.registerBasicFormats();
    }

    ~Body() override
    {
        /* Detach as a listener from any thumbnails we built so their
         * background-thread completion doesn't fire callbacks into a
         * dying Body. */
        for (auto& kv : thumbnails_)
            if (kv.second)
                kv.second->removeChangeListener (this);
    }

    /** Active editing tool.  Toolbar above the ruler swaps between
     *  these; mouseDown / mouseDrag branch on the value.
     *   - Select   : drag = move, edge-drag = resize, click = launch,
     *                right-click = context menu (existing behaviour)
     *   - Range    : drag in strip = set time-range overlay
     *                (loops between rangeStart/rangeEnd when loop
     *                button is on; transport wraps in timerCallback)
     *   - Split    : click on region = split at click beat
     *   - Trim     : drag on region = resize only (no move)
     *   - Audition : click on region = launch immediately, no drag
     *   - Env      : automation/envelope editing.  Clicks edit volume-
     *                envelope + automation-curve points (add / move / shape
     *                / delete); clips never move or resize.  A dedicated
     *                tool so envelope editing isn't tangled with clip
     *                move under Select (Ardour/Cubase-style). */
    enum class Tool { Select, Range, Split, Trim, Audition, Env };

    /** The cursor that represents a tool -- the single source of truth so
     *  setActiveTool + every hover path agree.  Shown over clips too, so
     *  the cursor always says what a click in this tool will do. */
    juce::MouseCursor toolCursor() const noexcept
    {
        switch (activeTool_)
        {
            /* Use the compositor's NATIVE arrow for the precise editors.
             * Custom-shaped cursors (crosshair / hand) get a wrong hotspot
             * in the wayland/winelib backend -- the rendered glyph sits
             * offset from where the click actually registers, which makes
             * grabbing envelope dots feel "off".  The native arrow's tip
             * hotspot is exact, so the cursor tells the truth.  (Backend
             * hotspot fix is the real root cure -- tracked separately.) */
            case Tool::Select:   return juce::MouseCursor::NormalCursor;
            case Tool::Range:    return juce::MouseCursor::NormalCursor;
            case Tool::Split:    return juce::MouseCursor::IBeamCursor;
            case Tool::Trim:     return juce::MouseCursor::LeftRightResizeCursor;
            case Tool::Audition: return juce::MouseCursor::PointingHandCursor;
            case Tool::Env:      return juce::MouseCursor::NormalCursor;
        }
        return juce::MouseCursor::NormalCursor;
    }

    void setActiveTool (Tool t)
    {
        if (activeTool_ == t) return;
        activeTool_ = t;
        /* Cancel any in-flight gesture if it doesn't suit the new
         * tool; harmless to wipe even when it does. */
        gesture_ = Gesture {};
        setMouseCursor (toolCursor());
        repaint();
    }
    Tool getActiveTool() const noexcept { return activeTool_; }

    /** Loop-range state.  rangeActive_ is "user has drawn a range";
     *  loopActive_ is "and the loop button is on".  Loop only takes
     *  effect when both are true.  Range bounds are in beats and
     *  always normalised so rangeStart_ <= rangeEnd_. */
    bool   hasRange()       const noexcept { return rangeActive_; }
    double rangeStart()     const noexcept { return rangeStart_; }
    double rangeEnd()       const noexcept { return rangeEnd_;   }
    bool   isLooping()      const noexcept { return loopActive_ && rangeActive_
                                                     && rangeEnd_ > rangeStart_; }
    void   setLoopActive (bool on) { loopActive_ = on; repaint(); }
    void   clearRange()
    {
        rangeActive_ = false;
        rangeDragging_ = false;
        repaint();
    }

private:
    /** juce::AudioThumbnail emits a change notification each time the
     *  background reader updates its peak-data window.  Repaint the
     *  whole body when any thumbnail progresses -- cheap, JUCE
     *  invalidates only what changed at the windowing layer. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        repaint();
    }

public:

    void paint (Graphics& g) override
    {
        g.fillAll (Colors::contentBackgroundColor);

        const int laneCount = owner.lanes_.size();
        /* Viewport-clip skip: at high lane counts + zoomed-in vertical
         * extent, the dirty rect from a scroll / playhead / zoom only
         * intersects a handful of lanes.  Walking paintLane for the
         * rest is wasted work (each lane builds Paths + reads thumbs +
         * paints region fills).  JUCE clips the actual draw calls, but
         * the per-region preamble runs anyway -- skipping at the lane
         * level kills the whole branch. */
        const auto laneClip = g.getClipBounds();
        int laneY = kRulerH;   // accumulate full heights -- variable per lane
        for (int i = 0; i < laneCount; ++i)
        {
            const int fullH = laneFullHeight (i);
            if (laneClip.getBottom() <= laneY || laneClip.getY() >= laneY + fullH)
            {
                laneY += fullH;
                continue;
            }
            paintLane (g, i);
            laneY += fullH;
        }

        if (laneCount == 0)
        {
            g.setColour (Colors::textColor.withAlpha (0.5f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText ("Drop an audio or MIDI file here, or click + Audio / + MIDI to add a track.",
                        getLocalBounds().withTrimmedTop (kRulerH),
                        juce::Justification::centred);
        }

        /* Range overlay -- drawn over all lanes so it reads as a
         * timeline-wide selection.  Loop-armed range gets a brighter
         * outline + tinted fill; plain range gets a softer wash.  The
         * ruler-row band + LOOP badge are painted by paintRuler so
         * they follow the sticky ruler's vertical position. */
        if (rangeActive_ && rangeEnd_ > rangeStart_)
        {
            const int xs = kLabelW + (int) (rangeStart_ * kPxPerBeat);
            const int xe = kLabelW + (int) (rangeEnd_   * kPxPerBeat);
            const int w  = juce::jmax (1, xe - xs);
            const int yLanes = kRulerH + juce::jmax (kLaneH, totalLanesHeight());

            const juce::Colour fillCol = loopActive_
                ? juce::Colour::fromRGBA (90, 180, 110, 50)
                : juce::Colour::fromRGBA (140, 170, 210, 38);
            const juce::Colour edgeCol = loopActive_
                ? juce::Colour::fromRGB (110, 220, 130)
                : juce::Colour::fromRGB (170, 200, 240);

            /* Lane-area wash. */
            g.setColour (fillCol);
            g.fillRect (xs, kRulerH, w, juce::jmax (0, yLanes - kRulerH));

            /* Edges -- 1 px vertical lines at start + end across the
             * lane area; the ruler paints its own band edges so they
             * track the sticky scroll offset. */
            g.setColour (edgeCol);
            g.drawVerticalLine (xs,        (float) kRulerH, (float) yLanes);
            g.drawVerticalLine (xe - 1,    (float) kRulerH, (float) yLanes);
        }

        /* Marker hairlines down the lane area.  Drawn AFTER the range
         * overlay so a range outline doesn't obscure a coincident
         * marker, but BEFORE ghosts / marquee / sticky ruler so drag
         * overlays read on top. */
        paintMarkerHairlines (g);

        if (dropHover_)
        {
            /* Both branches now route through the cumulative-offset helpers,
             * which also restores the kRulerH origin the old hand-rolled
             * `idx * kLaneH` form dropped -- the hover band sat one ruler-
             * height too high before (transient drag feedback, so it never
             * affected the static lane layout). */
            const Rectangle<int> hover = dropHoverLaneIdx_ >= 0
                ? Rectangle<int> (kLabelW, laneClipTopY (dropHoverLaneIdx_),
                                   getWidth() - kLabelW, laneClipStripH (dropHoverLaneIdx_))
                : Rectangle<int> (kLabelW, kRulerH + totalLanesHeight(),
                                   getWidth() - kLabelW, laneClipStripH (-1));
            g.setColour (Colours::limegreen.withAlpha (0.18f));
            g.fillRect (hover);
            g.setColour (Colours::limegreen);
            g.drawRect (hover, 2);
        }

        /* Lane-header drag feedback.  Source lane gets a translucent
         * overlay; hover lane gets a horizontal amber drop-indicator
         * at its top or bottom edge depending on direction. */
        if (laneDragActive_)
        {
            if (laneDragSource_ >= 0 && laneDragSource_ < laneCount)
            {
                const int y = laneClipTopY (laneDragSource_);
                g.setColour (juce::Colours::black.withAlpha (0.30f));
                g.fillRect (0, y, kLabelW, laneFullHeight (laneDragSource_));
            }
            if (laneDragHoverIdx_ >= 0 && laneDragHoverIdx_ < laneCount
                && laneDragHoverIdx_ != laneDragSource_)
            {
                const int y = laneClipTopY (laneDragHoverIdx_);
                const bool dropOnTop = (laneDragHoverIdx_ < laneDragSource_);
                const int yLine = dropOnTop ? y
                                            : y + laneFullHeight (laneDragHoverIdx_) - 2;
                g.setColour (juce::Colour { 0xff'ff'a0'40 });
                g.fillRect (0, yLine, getWidth(), 2);
            }
        }

        /* Ghost preview overlay.  Fires for:
         *   - any cross-lane Move drag (laneOffset != 0) -- sources
         *     stay at originals, ghosts mark would-be destinations.
         *   - any copy-drag Move (copyOnCommit, in-lane or cross-lane)
         *     -- sources stay put, ghosts mark where copies will land.
         *   - same-lane Move WITHOUT copy live-mutates the originals
         *     so their painted positions ARE the preview -- no ghost.
         * Colours: green = valid drop; red = refused (wrong-kind dest
         * lane, out of bounds, or MIDI overlap). */
        const bool showGhost = gesture_.laneIdx >= 0
                             && gesture_.dragActive
                             && gesture_.mode == Gesture::Move
                             && ! gesture_.members.empty()
                             && (gesture_.laneOffset != 0
                                 || gesture_.copyOnCommit);
        if (showGhost)
        {
            const juce::Colour fillCol = gesture_.crossLaneInvalid
                ? juce::Colour::fromRGBA (220, 80, 80, 70)
                : juce::Colour::fromRGBA (110, 200, 130, 70);
            const juce::Colour edgeCol = gesture_.crossLaneInvalid
                ? juce::Colour::fromRGB (220, 80, 80)
                : juce::Colour::fromRGB (110, 220, 130);

            for (const auto& mb : gesture_.members)
            {
                const int destLaneIdx = mb.laneIdx + gesture_.laneOffset;
                if (destLaneIdx < 0 || destLaneIdx >= laneCount) continue;

                const double newPos = juce::jmax (0.0, mb.originalPos + gesture_.appliedDelta);
                const int xs = kLabelW + (int) (newPos * kPxPerBeat);
                const int xe = kLabelW + (int) ((newPos + mb.originalLen) * kPxPerBeat);
                const int yT = laneClipTopY (destLaneIdx) + 4;
                const Rectangle<int> ghost (xs, yT, juce::jmax (2, xe - xs), laneClipStripH (destLaneIdx) - 8);

                g.setColour (fillCol);
                g.fillRect (ghost);
                g.setColour (edgeCol);
                g.drawRect (ghost, 2);
            }
        }

        /* Marquee overlay.  Translucent white wash + 1-px outline,
         * painted over lanes + ghost so the user can see WHICH ones
         * the rect will touch.  Skipped sub-threshold (dragActive
         * still false) so an aborted click doesn't flash a stray
         * 1x1 marquee.  Goes under the sticky ruler so it doesn't
         * paint over the LCD strip when the marquee crosses up. */
        if (gesture_.mode == Gesture::Marquee && gesture_.dragActive)
        {
            const auto rect = currentMarqueeRect();
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (rect);
            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.drawRect (rect, 1);
        }

        /* Sticky ruler.  Painted LAST so it overlays whatever lane
         * content sits under it once the viewport scrolls down.
         * paintRuler offsets the LCD strip by the viewport's vertical
         * scroll so it always reads at the visible top of the body. */
        paintRuler (g, owner.viewport_.getViewPositionY());
    }

    //==========================================================================
    // FileDragAndDropTarget

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        const bool interested = anyDroppableFileIn (files);
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::Body] isInterestedInFileDrag count=")
            + juce::String (files.size())
            + (interested ? " ACCEPT" : " REJECT"));
        return interested;
    }

    void fileDragEnter (const juce::StringArray& files, int x, int y) override
    {
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::Body] fileDragEnter x=")
            + juce::String (x) + " y=" + juce::String (y)
            + " count=" + juce::String (files.size()));
        if (! anyDroppableFileIn (files)) return;
        dropHover_         = true;
        dropHoverLaneIdx_  = owner.laneIdxFromY (y);
        juce::ignoreUnused (x);
        repaint();
    }

    void fileDragMove (const juce::StringArray&, int /*x*/, int y) override
    {
        const int li = owner.laneIdxFromY (y);
        if (li == dropHoverLaneIdx_) return;
        dropHoverLaneIdx_ = li;
        repaint();
    }

    void fileDragExit (const juce::StringArray&) override
    {
        dropHover_        = false;
        dropHoverLaneIdx_ = -1;
        repaint();
    }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::Body] filesDropped x=")
            + juce::String (x) + " y=" + juce::String (y)
            + " count=" + juce::String (files.size()));

        dropHover_        = false;
        dropHoverLaneIdx_ = -1;
        repaint();

        const int targetLane = owner.laneIdxFromY (y);
        const double dropBeats =
            juce::jmax (0.0, (double) (x - kLabelW) / (double) kPxPerBeat);

        /* Partition by kind.  Audio files go through the existing
         * importAudioFileToLane path; MIDI files create / target a
         * MIDI lane through importMidiFileToLane.  Mixed drops produce
         * two distinct lanes in sequence rather than trying to coerce
         * kinds together. */
        juce::StringArray audioPaths;
        juce::StringArray midiPaths;
        for (const auto& path : files)
        {
            if      (isAcceptableAudioFile (path)) audioPaths.add (path);
            else if (isAcceptableMidiFile  (path)) midiPaths .add (path);
        }

        if (! audioPaths.isEmpty())
        {
            int laneIdx = targetLane;
            if (laneIdx >= 0 && laneIdx < owner.lanes_.size())
            {
                const auto& runtime = owner.laneRuntime_.getReference (laneIdx);
                if (! runtime.isAudioLane())
                    laneIdx = -1;
            }
            else
            {
                laneIdx = -1;
            }

            double cursor = dropBeats;
            for (const auto& path : audioPaths)
            {
                const juce::File f (path);
                if (! f.existsAsFile()) continue;

                const bool ok = owner.importAudioFileToLane (f, laneIdx, cursor);
                if (! ok) continue;

                if (laneIdx < 0)
                    laneIdx = owner.lanes_.size() - 1;

                if (laneIdx >= 0 && laneIdx < owner.lanes_.size())
                {
                    const auto& regs = owner.lanes_.getReference (laneIdx).playlist.regions();
                    if (! regs.empty())
                        cursor = regs.back().endBeats();
                }
            }
        }

        if (! midiPaths.isEmpty())
        {
            /* MIDI drop must land on a MIDI lane.  Same rule as audio:
             * if the target lane is the wrong kind, create a fresh
             * lane. */
            int midiLaneIdx = targetLane;
            if (midiLaneIdx >= 0 && midiLaneIdx < owner.lanes_.size())
            {
                if (owner.lanes_.getReference (midiLaneIdx).kind != Lane::Kind::Midi)
                    midiLaneIdx = -1;
            }
            else
            {
                midiLaneIdx = -1;
            }

            double cursor = dropBeats;
            for (const auto& path : midiPaths)
            {
                const juce::File f (path);
                if (! f.existsAsFile()) continue;

                const bool ok = owner.importMidiFileToLane (f, midiLaneIdx, cursor);
                if (! ok) continue;

                if (midiLaneIdx < 0)
                    midiLaneIdx = owner.lanes_.size() - 1;

                if (midiLaneIdx >= 0 && midiLaneIdx < owner.lanes_.size())
                {
                    const auto& midis = owner.lanes_.getReference (midiLaneIdx)
                                              .playlist.midiRegions();
                    if (! midis.empty() && midis.back() != nullptr)
                        cursor = midis.back()->positionBeats
                               + midis.back()->lengthBeats;
                }
            }
        }
    }

    //==========================================================================
    // Mouse interaction
    //
    // Gesture model:
    //   - mouseDown on label arm-toggle  -> flip arm (audio lanes)
    //   - mouseDown on region right-edge -> begin resize
    //   - mouseDown on region body       -> select + arm potential drag
    //   - mouseDrag (after threshold)    -> commit drag/resize and live-update
    //   - mouseUp without drag           -> click-launch the region
    //   - mouseUp after drag/resize      -> writeLanesToSession + repaint
    //
    // Keyboard:
    //   - Delete / Backspace             -> remove selected region
    //
    // Hit-test sub-region for the resize handle is the last
    // kEdgeHandlePx pixels of the region's strip rectangle.  Inside
    // that band, mouseDown enters Resize mode; outside it's Move.

    void mouseDown (const MouseEvent& e) override
    {
        /* Ruler clicks (the bars:beats strip at the top).  Right-click
         * routes to the marker context menu (per-marker if the click
         * hits a triangle; "Add marker here" otherwise).  Left-click
         * seeks transport to the clicked beat; if a drag follows,
         * redefines the range from the click point.  Mirrors Ardour /
         * Bitwig convention: ruler = locator + range + marker surface. */
        if (e.y < kRulerH && e.x >= kLabelW)
        {
            if (e.mods.isPopupMenu())
            {
                showRulerContextMenu (e.x, e.y);
                return;
            }

            const double beat = juce::jmax (0.0,
                (double) (e.x - kLabelW) / (double) kPxPerBeat);
            owner.seekToBeat (beat);

            rangeAnchor_   = beat;
            rangeStart_    = beat;
            rangeEnd_      = beat;
            rangeActive_   = true;
            rangeDragging_ = true;
            grabKeyboardFocus();
            repaint();
            return;
        }
        if (e.y < kRulerH) return;   /* ruler click in label gutter */

        /* Automation overlay vertical resize: grab the overlay's bottom edge.
         * Checked first + works in any tool (it's below the envelope area). */
        if (! e.mods.isPopupMenu())
        {
            const int orh = overlayResizeHandleAt (e.y);
            if (orh >= 0)
            {
                overlayResize_ = OverlayResize { true, orh, e.y, laneOverlayCurveH (orh) };
                return;
            }
        }

        /* Per-lane vertical resize: grab a lane's bottom divider + drag.
         * Disabled in the Env tool, where the bottom edge is for envelope
         * point editing instead (so resize never steals curve clicks). */
        if (! e.mods.isPopupMenu() && activeTool_ != Tool::Env)
        {
            const int rh = laneResizeHandleAt (e.x, e.y);
            if (rh >= 0)
            {
                laneResize_ = LaneResize { true, rh, e.y, laneClipStripH (rh) };
                return;
            }
        }

        const int laneIdx = rowAtY (e.y);
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane    = owner.lanes_.getReference (laneIdx);
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);

        /* Clicks below the clip strip land in this lane's automation
         * overlay stack -- route there (remove [x] now; point gestures
         * land in step 3) so the clip-strip region logic stays clip-only. */
        if (e.y >= laneClipTopY (laneIdx) + laneClipStripH (laneIdx))
        {
            handleAutomationOverlayMouseDown (e, laneIdx);
            return;
        }

        /* Label area button hits (M/S/R) are tool-independent. */
        if (e.x < kLabelW)
        {
            /* Right-click anywhere in the label strip opens the
             * track-level context menu.  Tested first so it preempts
             * the M / S / R button click handlers. */
            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu m;
                m.addItem (1, "Move up",   laneIdx > 0);
                m.addItem (2, "Move down", laneIdx < owner.lanes_.size() - 1);
                m.addSeparator();
                m.addItem (3, "Delete track");
                const int r = m.showAt (
                    juce::Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                switch (r)
                {
                    case 1: owner.moveLane   (laneIdx, -1); break;
                    case 2: owner.moveLane   (laneIdx, +1); break;
                    case 3: owner.removeLane (laneIdx);     break;
                    default: break;
                }
                return;
            }
            /* Automation reveal toggle.  Empty lane -> open the picker to
             * add the first param; otherwise show/hide the overlay area
             * (adding more + per-param selection live in the chip rail).
             * Inert on orphan lanes (no bindable target node). */
            if (! runtime.isOrphan() && automationToggleRect (laneIdx).contains (e.x, e.y))
            {
                if (owner.automationBindingCountForLane (lane.id) == 0)
                    owner.showAddAutomationMenuForLane (
                        laneIdx, localAreaToGlobal (automationToggleRect (laneIdx)));
                else
                    owner.setLaneAutomationCollapsed (
                        lane.id, ! owner.isLaneAutomationCollapsed (lane.id));
                return;
            }
            if (muteToggleRect (laneIdx).contains (e.x, e.y))
            {
                lane.muted = ! lane.muted;
                owner.writeLanesToSession();
                owner.propagateMuteSolo();
                repaintLane (laneIdx);
                return;
            }
            if (soloToggleRect (laneIdx).contains (e.x, e.y))
            {
                lane.soloed = ! lane.soloed;
                owner.writeLanesToSession();
                owner.propagateMuteSolo();
                repaint();
                return;
            }
            if (runtime.isAudioLane() && armToggleRect (laneIdx).contains (e.x, e.y))
            {
                lane.armed = ! lane.armed;
                runtime.audioClipCache->setArmed (lane.armed);
                owner.writeLanesToSession();
                repaintLane (laneIdx);
                return;
            }

            /* Stage a potential lane reorder.  No-op on stationary
             * click; promoted past the drag threshold in mouseDrag. */
            laneDragSource_   = laneIdx;
            laneDragStart_    = juce::Point<int> (e.x, e.y);
            laneDragActive_   = false;
            laneDragHoverIdx_ = laneIdx;
            return;
        }

        if (runtime.isOrphan()) return;

        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;

        /* Range tool: any strip-area click begins a range drag.
         * Doesn't matter whether a region was hit -- range overlays
         * sit above all lanes. */
        if (activeTool_ == Tool::Range)
        {
            rangeAnchor_   = beat;
            rangeStart_    = beat;
            rangeEnd_      = beat;
            rangeActive_   = true;
            rangeDragging_ = true;
            clearSelection();
            grabKeyboardFocus();
            repaint();
            return;
        }

        const auto& regions = lane.playlist.regions();
        for (const auto& r : regions)
        {
            /* Env tool grabs envelope points by PIXEL proximity, so a
             * region qualifies when the click is within the grab band of
             * its body -- not only strictly inside its beat span.  Without
             * this the START/END anchors (painted right on the body edge)
             * have a clipped, asymmetric grab zone: their outer ~5px falls
             * just outside containsBeat (or into the label gutter for a
             * region at beat 0), so a centred click misses.  Interior
             * points are unaffected -- that's why it "works on all others". */
            const bool envEligible = activeTool_ == Tool::Env
                                     && runtime.isAudioLane()
                                     && r.sequenceIdx < 0;
            const bool envHit = envEligible
                && regionBodyRect (laneIdx, r)
                       .expanded ((int) kEnvPointGrabPx, (int) kEnvPointGrabPx)
                       .contains (e.x, e.y);

            if (! r.containsBeat (beat) && ! envHit) continue;

            /* Env tool: volume-envelope editing on audio clips.  Owns the
             * whole gesture -- grab a breakpoint, bend a segment midpoint,
             * or click empty curve to add a point + drag it.  Never moves
             * or selects the clip (that's the Select tool's job), so the
             * two never fight. */
            if (activeTool_ == Tool::Env
                && runtime.isAudioLane()
                && r.sequenceIdx < 0)
            {
                lastInteractedLane_ = laneIdx;
                setPrimarySelection (laneIdx, r.id);   // so handles draw
                grabKeyboardFocus();

                /* Breakpoint grab (right-click = curve/delete menu). */
                const int ptIdx = envPointHitAt (laneIdx, r, e.x, e.y);
                if (ptIdx >= 0)
                {
                    const auto pointId = r.volumeEnvelope[(size_t) ptIdx].id;
                    if (e.mods.isPopupMenu())
                        showEnvPointContextMenu (laneIdx, r.id, pointId);
                    else
                    {
                        const auto p = envPointPx (r, (size_t) ptIdx,
                                                   regionBodyRect (laneIdx, r));
                        envGesture_.laneIdx    = laneIdx;
                        envGesture_.regionId   = r.id;
                        envGesture_.pointId    = pointId;
                        envGesture_.dragActive = false;
                        envGesture_.grabDx     = p.x - (float) e.x;
                        envGesture_.grabDy     = p.y - (float) e.y;
                    }
                    repaintLane (laneIdx);
                    return;
                }

                /* Shape grab: clicking anywhere along the drawn curve grabs
                 * that segment and drags to bend it -- exactly like the
                 * automation overlay.  envSegmentLineHitAt samples the real
                 * painted bezier (not the straight chord), so the visible
                 * line is grabbable even where a bent/curved segment bulges
                 * away from its chord (the steep fade-in/out segments at the
                 * clip ends bend most -- testing only the chord left their
                 * mid-slope handle ungrabbable).  Right-click = reset to
                 * straight.  To ADD a point, double-click -- a single click
                 * never spawns one, so a near-miss can't litter the curve. */
                const int segIdx = envSegmentLineHitAt (laneIdx, r, e.x, e.y);
                if (segIdx >= 0)
                {
                    if (e.mods.isPopupMenu())
                    {
                        auto& laneMut = owner.lanes_.getReference (laneIdx);
                        if (auto* rmut = laneMut.playlist.findRegion (r.id))
                            if (segIdx < (int) rmut->volumeEnvelope.size())
                            {
                                auto& seg = rmut->volumeEnvelope[(size_t) segIdx];
                                seg.curveOffsetT  = 0.5f;
                                seg.curveOffsetDb = 0.0f;
                                owner.writeLanesToSession();
                                repaintLane (laneIdx);
                            }
                    }
                    else
                    {
                        envMidGesture_.laneIdx    = laneIdx;
                        envMidGesture_.regionId   = r.id;
                        envMidGesture_.segIndex   = segIdx;
                        envMidGesture_.dragActive = false;
                    }
                    return;
                }

                /* Missed every point + the curve line: consume the click
                 * (Env tool never moves the clip) but add nothing. */
                return;
            }

            lastInteractedLane_ = laneIdx;

            /* Right-click context menu works in every tool.  Preserve
             * an existing multi-selection if the right-clicked region
             * is already part of it -- otherwise collapse to a single
             * selection so the menu's act on a clear target. */
            if (e.mods.isPopupMenu())
            {
                if (! isSelected (laneIdx, r.id))
                    setPrimarySelection (laneIdx, r.id);
                repaintLane (laneIdx);
                showRegionContextMenu (laneIdx, r.id, beat);
                return;
            }

            /* Ctrl/Cmd+click toggles the clicked region in the
             * multi-selection without starting a drag.  Shift+click
             * range-extends along the same lane from the primary. */
            if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            {
                toggleSelection (laneIdx, r.id);
                grabKeyboardFocus();
                repaintLane (laneIdx);
                return;
            }
            if (e.mods.isShiftDown())
            {
                extendSelection (laneIdx, r.id);
                grabKeyboardFocus();
                repaintLane (laneIdx);
                return;
            }

            /* Split tool: cleave the region in two at the click beat. */
            if (activeTool_ == Tool::Split)
            {
                const auto newId = lane.playlist.splitRegion (r.id, beat);
                if (! newId.isNull())
                {
                    owner.writeLanesToSession();
                    repaintLane (laneIdx);
                }
                return;
            }

            /* Audition tool: launch the region immediately (preview),
             * skipping the move/resize gesture path entirely. */
            if (activeTool_ == Tool::Audition)
            {
                launchRegionAudition (laneIdx, r);
                setPrimarySelection (laneIdx, r.id);
                repaintLane (laneIdx);
                return;
            }

            /* Select + Trim tools start a gesture; the mode differs:
             * - Select: edge hit = Resize, body = Move
             * - Trim:   anywhere on the region = Resize (right edge) */
            gesture_                 = Gesture {};
            gesture_.laneIdx         = laneIdx;
            gesture_.regionId        = r.id;
            gesture_.originalPos     = r.positionBeats;
            gesture_.originalLen     = r.lengthBeats;
            gesture_.originalStart   = r.startBeats;
            gesture_.mouseDownXBeats = beat;
            gesture_.mouseDownYPx    = e.y;
            gesture_.kind            = Gesture::Audio;
            gesture_.dragActive      = false;

            const int regionStartX = kLabelW + (int) (r.positionBeats * kPxPerBeat);
            const int regionEndX   = kLabelW + (int) (r.endBeats() * kPxPerBeat);
            const bool overRightEdge =
                (e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX);
            /* Left-edge takes precedence ONLY when not also over right --
             * tiny regions (<2*kEdgeHandlePx wide) keep right-edge
             * resize so the user can always grow them rightward. */
            const bool overLeftEdge = ! overRightEdge
                && (e.x >= regionStartX && e.x <= regionStartX + kEdgeHandlePx);

            if (activeTool_ == Tool::Trim || overRightEdge)
            {
                gesture_.mode = Gesture::Resize;
                gesture_.edge = Gesture::RightEdge;
            }
            else if (overLeftEdge)
            {
                gesture_.mode = Gesture::Resize;
                gesture_.edge = Gesture::LeftEdge;
            }
            else
            {
                gesture_.mode = Gesture::Move;
            }

            /* Alt-drag = copy-drag.  Only applies to Move (Resize is
             * in-place; copying an edge-grab doesn't have a sensible
             * meaning).  Mirrors Zrythm v1 MovingCopy.  Q8a. */
            if (gesture_.mode == Gesture::Move && e.mods.isAltDown())
                gesture_.copyOnCommit = true;

            /* Click-on-already-selected region in a multi-selection
             * PRESERVES the selection and queues every selected region
             * for the drag.  Otherwise (click on a region outside the
             * current selection, or no multi-selection) collapse to
             * single-selection -- same shape as before the multi-drag
             * landed.  Resize gestures stay anchor-only per roadmap
             * S1.1 (selection-wide resize is awkward + rarely
             * wanted). */
            if (gesture_.mode == Gesture::Move
                && isSelected (laneIdx, r.id)
                && selected_.size() > 1)
            {
                buildGestureMembersForMove();
            }
            else
            {
                setPrimarySelection (laneIdx, r.id);
                seedSingleGestureMember();
            }

            grabKeyboardFocus();
            repaintLane (laneIdx);
            return;
        }

        /* MIDI region hit-test.  Parallel to the audio scan above --
         * MIDI regions live in a separate list (playlist.midiRegions())
         * and use direct field mutation rather than the audio
         * playlist::moveRegion / resizeRegion helpers.  Same gesture
         * surface: Select tool body = Move, right-edge or Trim tool =
         * Resize.  Right-click opens the region context menu.  Split
         * is not yet supported for MIDI (data layer has no
         * splitMidiRegion); a Split-tool click on a MIDI region is
         * absorbed silently rather than splitting the underlying
         * audio region at that beat. */
        for (const auto& mp : lane.playlist.midiRegions())
        {
            if (mp == nullptr) continue;
            const auto& m = *mp;
            const double endBeat = m.positionBeats + m.lengthBeats;
            if (beat < m.positionBeats || beat >= endBeat) continue;

            lastInteractedLane_ = laneIdx;

            if (e.mods.isPopupMenu())
            {
                if (! isSelected (laneIdx, m.id))
                    setPrimarySelection (laneIdx, m.id);
                repaintLane (laneIdx);
                showRegionContextMenu (laneIdx, m.id, beat);
                return;
            }

            if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            {
                toggleSelection (laneIdx, m.id);
                grabKeyboardFocus();
                repaintLane (laneIdx);
                return;
            }
            if (e.mods.isShiftDown())
            {
                extendSelection (laneIdx, m.id);
                grabKeyboardFocus();
                repaintLane (laneIdx);
                return;
            }

            /* Split tool: wire to the new splitMidiRegion data-layer
             * helper.  Returns null on out-of-range cut (the splitMidiRegion
             * impl rejects cuts within ~1e-9 of either edge), in which
             * case we leave the region intact + fall through to no-op. */
            if (activeTool_ == Tool::Split)
            {
                const auto newId = lane.playlist.splitMidiRegion (m.id, beat);
                if (! newId.isNull())
                {
                    owner.publishMidiBindingsForLane (laneIdx);
                    owner.writeLanesToSession();
                    repaintLane (laneIdx);
                }
                return;
            }

            /* Audition: no MIDI preview wired yet -- absorb. */
            if (activeTool_ == Tool::Audition)
            {
                setPrimarySelection (laneIdx, m.id);
                repaintLane (laneIdx);
                return;
            }

            gesture_                 = Gesture {};
            gesture_.laneIdx         = laneIdx;
            gesture_.regionId        = m.id;
            gesture_.originalPos     = m.positionBeats;
            gesture_.originalLen     = m.lengthBeats;
            gesture_.originalStart   = m.startBeats;
            gesture_.mouseDownXBeats = beat;
            gesture_.mouseDownYPx    = e.y;
            gesture_.kind            = Gesture::Midi;
            gesture_.dragActive      = false;

            const int regionStartX = kLabelW + (int) (m.positionBeats * kPxPerBeat);
            const int regionEndX   = kLabelW + (int) (endBeat         * kPxPerBeat);
            const bool overRightEdge =
                (e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX);
            const bool overLeftEdge = ! overRightEdge
                && (e.x >= regionStartX && e.x <= regionStartX + kEdgeHandlePx);

            if (activeTool_ == Tool::Trim || overRightEdge)
            {
                gesture_.mode = Gesture::Resize;
                gesture_.edge = Gesture::RightEdge;
            }
            else if (overLeftEdge)
            {
                gesture_.mode = Gesture::Resize;
                gesture_.edge = Gesture::LeftEdge;
            }
            else
            {
                gesture_.mode = Gesture::Move;
            }

            /* Alt-drag copy on MIDI -- see audio branch above. */
            if (gesture_.mode == Gesture::Move && e.mods.isAltDown())
                gesture_.copyOnCommit = true;

            /* See audio branch above for multi-selection drag rules. */
            if (gesture_.mode == Gesture::Move
                && isSelected (laneIdx, m.id)
                && selected_.size() > 1)
            {
                buildGestureMembersForMove();
            }
            else
            {
                setPrimarySelection (laneIdx, m.id);
                seedSingleGestureMember();
            }

            grabKeyboardFocus();
            repaintLane (laneIdx);
            return;
        }

        /* Empty strip area.  Right-click opens the paste-only context
         * menu (no region target).  Select-tool drag starts a marquee
         * (rubber-band).  Other tools fall through to the legacy
         * clearSelection on plain click. */
        lastInteractedLane_ = laneIdx;
        if (e.mods.isPopupMenu())
        {
            showEmptyStripContextMenu (laneIdx, beat);
            return;
        }
        if (activeTool_ == Tool::Select)
        {
            /* Start a Marquee gesture.  laneIdx is set so the gesture
             * passes the `gesture_.laneIdx < 0 return` guards
             * elsewhere; the marquee itself spans all lanes (laneIdx
             * isn't a constraint, just a no-op-passing value). */
            const auto mode = marqueeModeFromMods (e.mods);
            gesture_                  = Gesture {};
            gesture_.laneIdx          = laneIdx;
            gesture_.mode             = Gesture::Marquee;
            gesture_.mouseDownXBeats  = beat;
            gesture_.mouseDownYPx     = e.y;
            gesture_.marqueeStartXPx  = e.x;
            gesture_.marqueeStartYPx  = e.y;
            gesture_.marqueeEndXPx    = e.x;
            gesture_.marqueeEndYPx    = e.y;
            gesture_.marqueeMode      = mode;
            gesture_.dragActive       = false;   /* set true after threshold */
            /* Replace mode previews intent immediately by clearing
             * the existing selection; Add / Toggle keep it. */
            if (mode == Gesture::Replace)
                clearSelection();
            grabKeyboardFocus();
            repaint();
            return;
        }
        clearSelection();
        gesture_        = Gesture {};
        repaint();
    }

    /** Compute the body rect of a region (the area BELOW its title
     *  strip).  Mirrors the paintLane layout maths so hit-tests +
     *  envelope edits sit precisely on the painted curve. */
    Rectangle<int> regionBodyRect (int laneIdx, const Region& r) const noexcept
    {
        constexpr int kTitleH = 13;
        const int laneTopY = laneClipTopY (laneIdx);
        const int xs = kLabelW + (int) (r.positionBeats * kPxPerBeat);
        const int ws = juce::jmax (4, (int) (r.lengthBeats * kPxPerBeat));
        const int innerTopY = laneTopY + 4 + kTitleH;
        const int innerH    = juce::jmax (1, laneClipStripH (laneIdx) - 8 - kTitleH);
        return Rectangle<int> (xs, innerTopY, ws, innerH).reduced (3, 2);
    }

    /** Inverse of paintVolumeEnvelope's gainToY -- map a Y inside
     *  a body rect back to a gain in dB over the [+6, -24] window. */
    float envYToGainDb (int y, const Rectangle<int>& body) const noexcept
    {
        constexpr float kTopDb = 6.0f;
        constexpr float kBotDb = -24.0f;
        const int yPad = 3;
        const int H = juce::jmax (1, body.getHeight() - yPad * 2);
        const float t = juce::jlimit (0.0f, 1.0f,
            (float) (y - body.getY() - yPad) / (float) H);
        return kTopDb - t * (kTopDb - kBotDb);
    }

    /** Inverse of beatToX -- map an X to a beat offset within the
     *  region's local timeline.  Clamped to [0, lengthBeats]. */
    double envXToBeatOffset (int x, const Rectangle<int>& body,
                              const Region& r) const noexcept
    {
        if (r.lengthBeats <= 1e-6) return 0.0;
        const double t = juce::jlimit (0.0, 1.0,
            (double) (x - body.getX()) / juce::jmax (1.0, (double) body.getWidth()));
        return t * r.lengthBeats;
    }

    /** Pixel position of envelope point `i` inside `body`.  The single
     *  source of truth for both hit-testing and (matching) paint maths --
     *  kTopDb/kBotDb/yPad mirror paintVolumeEnvelope's gainToY/beatToX. */
    juce::Point<float> envPointPx (const Region& r, size_t i,
                                   const Rectangle<int>& body) const noexcept
    {
        constexpr float kTopDb = 6.0f, kBotDb = -24.0f;
        const int yPad = 3;
        const int H = juce::jmax (1, body.getHeight() - yPad * 2);
        const auto& pt = r.volumeEnvelope[i];
        const float px = body.getX()
            + (float) (pt.beatOffset / juce::jmax (1e-9, r.lengthBeats))
              * (float) body.getWidth();
        const float t  = juce::jlimit (0.0f, 1.0f,
            (kTopDb - pt.gainDb) / (kTopDb - kBotDb));
        const float py = body.getY() + yPad + t * (float) H;
        return { px, py };
    }

    /** Hit-test for envelope breakpoint dots.  8 px radius -- the painted
     *  dot is only ~3 px, so a generous grab band is what makes points
     *  reliably grabbable (a tight radius turned every near-miss into a
     *  spurious "add point" under the old gesture model).  Returns the
     *  matching point's index, or -1. */
    static constexpr float kEnvPointGrabPx = 8.0f;
    int envPointHitAt (int laneIdx, const Region& r, int x, int y) const noexcept
    {
        if (r.volumeEnvelope.empty()) return -1;
        const auto body = regionBodyRect (laneIdx, r);
        /* Square (max-metric) grab zone -- matches the rectangular
         * eligibility gate + the overlay's findPointNear, so a corner
         * approach to an edge anchor can't fall in the gap between a
         * circular hit-test and a rectangular gate (the "sometimes I
         * can't grab the first point" inconsistency). */
        for (size_t i = 0; i < r.volumeEnvelope.size(); ++i)
        {
            const auto p = envPointPx (r, i, body);
            if (juce::jmax (std::abs ((float) x - p.x),
                            std::abs ((float) y - p.y)) <= kEnvPointGrabPx)
                return (int) i;
        }
        return -1;
    }

    /** Pixel position on segment `i`'s DRAWN curve at parameter u in
     *  [0,1].  Replicates paintVolumeEnvelope's pushSegment exactly so a
     *  hit-test against the sampled curve lands on what the user sees --
     *  including the horizontal+vertical bend of a shaped segment. */
    juce::Point<float> envCurvePx (const Region& r, size_t i, double u,
                                   const Rectangle<int>& body) const noexcept
    {
        constexpr float kTopDb = 6.0f, kBotDb = -24.0f;
        const int yPad = 3;
        const int H = juce::jmax (1, body.getHeight() - yPad * 2);
        const auto& a = r.volumeEnvelope[i];
        const auto& b = r.volumeEnvelope[i + 1];

        double xLocal, yLocal;
        const bool useBezier = (a.curveOffsetT != 0.5f) || (a.curveOffsetDb != 0.0f);
        if (useBezier)
        {
            const double cot = (double) juce::jlimit (0.25f, 0.75f, a.curveOffsetT);
            const double cx  = 2.0 * cot - 0.5;
            const double chordMidDb = 0.5 * ((double) a.gainDb + (double) b.gainDb);
            const double pinDb = chordMidDb + (double) a.curveOffsetDb;
            const double cy  = 2.0 * pinDb - chordMidDb;
            const double om  = 1.0 - u;
            xLocal = 2.0 * om * u * cx + u * u;
            yLocal = om * om * (double) a.gainDb
                   + 2.0 * om * u * cy
                   + u * u * (double) b.gainDb;
        }
        else
        {
            double shaped = u;
            if (a.curve == EnvelopeCurve::Exponential)   shaped = u * u;
            else if (a.curve == EnvelopeCurve::Smooth)
                shaped = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u);
            xLocal = u;
            yLocal = (double) a.gainDb + shaped * ((double) b.gainDb - (double) a.gainDb);
        }

        const double beat = a.beatOffset + xLocal * (b.beatOffset - a.beatOffset);
        const float px = body.getX()
            + (float) (juce::jlimit (0.0, 1.0, beat / juce::jmax (1e-9, r.lengthBeats))
                       * (double) body.getWidth());
        const float t  = juce::jlimit (0.0f, 1.0f,
            (kTopDb - (float) yLocal) / (kTopDb - kBotDb));
        const float py = body.getY() + yPad + t * (float) H;
        return { px, py };
    }

    /** Hit-test for the curve LINE itself: the index of the left
     *  breakpoint of the segment whose DRAWN curve passes within the grab
     *  band of (x, y), or -1.  Samples the actual painted bezier (not the
     *  straight chord) so a bent segment is grabbable along its visible
     *  bulge -- matches the automation overlay's findSegmentLineNear, so
     *  the volume envelope and overlay feel identical.  Hold segments are
     *  excluded (a step has no slope to shape). */
    static constexpr float kEnvLineGrabPx = 8.0f;
    int envSegmentLineHitAt (int laneIdx, const Region& r, int x, int y) const noexcept
    {
        if (r.volumeEnvelope.size() < 2) return -1;
        const auto body = regionBodyRect (laneIdx, r);
        const float fx = (float) x, fy = (float) y;
        constexpr int kSamples = 16;
        int    best  = -1;
        float  bestD = 1.0e9f;
        for (size_t i = 0; i + 1 < r.volumeEnvelope.size(); ++i)
        {
            if (r.volumeEnvelope[i].curve == EnvelopeCurve::Hold) continue;
            for (int s = 0; s <= kSamples; ++s)
            {
                const auto p = envCurvePx (r, i, (double) s / kSamples, body);
                const float dx = fx - p.x, dy = fy - p.y;
                const float d  = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; best = (int) i; }
            }
        }
        return (best >= 0 && bestD <= kEnvLineGrabPx * kEnvLineGrabPx) ? best : -1;
    }

    /** Insert a breakpoint at the clicked beat + gain.  When the
     *  envelope was empty, ALSO seed start + end anchors at the
     *  region's static gainDb so the user gets a visible curve out
     *  of one double-click instead of an invisible single point. */
    juce::Uuid insertEnvelopePoint (int laneIdx, juce::Uuid regionId,
                                    double beatOffset, float gainDb)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return {};
        auto& lane = owner.lanes_.getReference (laneIdx);
        auto* r = lane.playlist.findRegion (regionId);
        if (r == nullptr) return {};

        if (r->volumeEnvelope.empty())
        {
            /* Seed at 0 dB (unity gain) so the envelope defaults to
             * "no attenuation", not whatever the static gainDb was.
             * The user drags individual points down from there. */
            EnvelopePoint anchor0; anchor0.id = juce::Uuid();
            anchor0.beatOffset = 0.0; anchor0.gainDb = 0.0f;
            EnvelopePoint anchorN; anchorN.id = juce::Uuid();
            anchorN.beatOffset = r->lengthBeats; anchorN.gainDb = 0.0f;
            r->volumeEnvelope.push_back (anchor0);
            r->volumeEnvelope.push_back (anchorN);
        }
        EnvelopePoint pt;
        pt.id         = juce::Uuid();
        pt.beatOffset = juce::jlimit (0.0, r->lengthBeats, beatOffset);
        pt.gainDb     = juce::jlimit (-60.0f, 12.0f, gainDb);
        pt.curve      = EnvelopeCurve::Linear;
        const auto newId = pt.id;
        r->volumeEnvelope.push_back (pt);
        r->sortEnvelope();
        owner.writeLanesToSession();
        repaintLane (laneIdx);
        return newId;
    }

    /** Right-click context menu on an envelope point.  Curve type
     *  + delete.  Async so the menu can outlive its trigger event
     *  without keeping mouse state alive. */
    void showEnvPointContextMenu (int laneIdx, juce::Uuid regionId,
                                    juce::Uuid pointId)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane = owner.lanes_.getReference (laneIdx);
        auto* r = lane.playlist.findRegion (regionId);
        if (r == nullptr) return;

        EnvelopePoint* match = nullptr;
        for (auto& pt : r->volumeEnvelope)
            if (pt.id == pointId) { match = &pt; break; }
        if (match == nullptr) return;

        enum { ItemLinear = 1, ItemExp, ItemSmooth, ItemHold, ItemDelete = 10 };

        juce::PopupMenu menu;
        menu.addSectionHeader ("Curve to next point");
        menu.addItem (ItemLinear, "Linear", true, match->curve == EnvelopeCurve::Linear);
        menu.addItem (ItemExp,    "Exponential", true, match->curve == EnvelopeCurve::Exponential);
        menu.addItem (ItemSmooth, "Smooth",       true, match->curve == EnvelopeCurve::Smooth);
        menu.addItem (ItemHold,   "Hold (step)",  true, match->curve == EnvelopeCurve::Hold);
        menu.addSeparator();
        menu.addItem (ItemDelete, "Delete point");

        juce::Component::SafePointer<Body> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options(),
            [safe, laneIdx, regionId, pointId] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr || result == 0) return;
                if (laneIdx < 0 || laneIdx >= self->owner.lanes_.size()) return;
                auto& l = self->owner.lanes_.getReference (laneIdx);
                auto* rr = l.playlist.findRegion (regionId);
                if (rr == nullptr) return;

                if (result == ItemDelete)
                {
                    rr->volumeEnvelope.erase (
                        std::remove_if (rr->volumeEnvelope.begin(),
                                         rr->volumeEnvelope.end(),
                                         [pointId] (const EnvelopePoint& p)
                                         { return p.id == pointId; }),
                        rr->volumeEnvelope.end());
                }
                else
                {
                    EnvelopeCurve nc = EnvelopeCurve::Linear;
                    if (result == ItemExp)    nc = EnvelopeCurve::Exponential;
                    if (result == ItemSmooth) nc = EnvelopeCurve::Smooth;
                    if (result == ItemHold)   nc = EnvelopeCurve::Hold;
                    for (auto& pt : rr->volumeEnvelope)
                        if (pt.id == pointId) { pt.curve = nc; break; }
                }
                self->owner.writeLanesToSession();
                self->repaintLane (laneIdx);
            });
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (e.y < kRulerH) return;
        const int laneIdx = rowAtY (e.y);
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        const auto& runtime = owner.laneRuntime_.getReference (laneIdx);

        /* Tracker lane: double-click anywhere on a tracker clip
         * surfaces the bottom-attach tracker strip bound to this
         * lane's TrackerNode.  Lets the user edit the pattern while
         * keeping the timeline visible, matching Bitwig / Ableton /
         * Ardour. */
        if (runtime.isTrackerLane())
        {
            const auto& lane = owner.lanes_.getReference (laneIdx);
            const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
            for (const auto& r : lane.playlist.regions())
            {
                if (! r.containsBeat (beat)) continue;
                const auto body = regionBodyRect (laneIdx, r);
                if (! body.contains (e.x, e.y)) continue;
                if (auto* sc = dynamic_cast<StandardContent*> (
                        ViewHelpers::findContentComponent (this)))
                    sc->showTrackerDockForNode (lane.targetNodeUuid,
                                                 r.sequenceIdx);
                return;
            }
            return;
        }

        /* MIDI lane: double-click on a region surfaces the bottom
         * piano-roll dock bound to that region.  Double-click on
         * empty area of the MIDI lane creates a fresh empty MIDI
         * region at the clicked beat (snapped to bar) + opens the
         * piano-roll on it.  This is the create-from-scratch path
         * that matches the real-DAW workflow -- load-a-.mid is
         * supported but atypical. */
        {
            const auto& lane = owner.lanes_.getReference (laneIdx);
            if (lane.kind == Lane::Kind::Midi)
            {
                const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
                if (beat < 0.0) return;

                /* Region hit -- open piano-roll on it. */
                for (const auto& mp : lane.playlist.midiRegions())
                {
                    if (mp == nullptr) continue;
                    const auto& m = *mp;
                    if (beat <  m.positionBeats) continue;
                    if (beat >= m.positionBeats + m.lengthBeats) continue;
                    if (auto* sc = dynamic_cast<StandardContent*> (
                            ViewHelpers::findContentComponent (this)))
                        sc->showPianoRollForRegion (m.id);
                    return;
                }

                /* Empty MIDI lane area -- create a fresh region.
                 * Snap the click beat down to the nearest bar so
                 * created regions align to the grid by default;
                 * default length is one bar (BPM-current). */
                const int beatsPerBar = owner.monitor_ != nullptr
                    ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
                    : 4;
                const double snappedStart =
                    std::floor (beat / beatsPerBar) * beatsPerBar;
                const double defaultLen   = (double) beatsPerBar;

                const juce::Uuid newId =
                    owner.createEmptyMidiRegion (laneIdx, snappedStart, defaultLen);
                if (! newId.isNull())
                {
                    if (auto* sc = dynamic_cast<StandardContent*> (
                            ViewHelpers::findContentComponent (this)))
                        sc->showPianoRollForRegion (newId);
                }
                return;
            }
        }

        if (! runtime.isAudioLane()) return;

        const auto& lane = owner.lanes_.getReference (laneIdx);
        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
        for (const auto& r : lane.playlist.regions())
        {
            if (! r.containsBeat (beat) || r.sequenceIdx >= 0) continue;
            const auto body = regionBodyRect (laneIdx, r);
            if (! body.contains (e.x, e.y)) continue;
            const double off = envXToBeatOffset (e.x, body, r);
            const float  db  = envYToGainDb     (e.y, body);
            insertEnvelopePoint (laneIdx, r.id, off, db);
            return;
        }
    }

    /** Audition helper -- routes a region launch through TrackerNode /
     *  AudioClipNode the same way the mouseUp click-launch does, but
     *  factored so the Audition tool can call it from mouseDown. */
    void launchRegionAudition (int laneIdx, const Region& r)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        if (runtime.isTrackerLane() && r.sequenceIdx >= 0)
        {
            if (runtime.lastDispatchedSeqIdx >= 0
                && runtime.lastDispatchedSeqIdx != r.sequenceIdx)
                runtime.trackerCache->schedulePlaying (
                    runtime.lastDispatchedSeqIdx, -1.0, false);
            runtime.trackerCache->schedulePlaying (r.sequenceIdx, -1.0, true);
        }
        else if (runtime.isAudioLane())
        {
            const double bpm = owner.monitor_ != nullptr
                ? (double) owner.monitor_->tempo.get() : 120.0;
            const double sessionSR = owner.monitor_ != nullptr
                ? (double) owner.monitor_->sampleRate.get() : 48000.0;
            const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
            runtime.audioClipCache->schedulePlay (
                r.id, r.sourceId, -1.0, 0, r.looped,
                r.gainDb,
                (juce::int64) (r.fadeInBeats  * secsPerBeat * sessionSR),
                (juce::int64) (r.fadeOutBeats * secsPerBeat * sessionSR),
                (juce::int64) (r.lengthBeats  * secsPerBeat * sessionSR),
                r.fadeInCurve, r.fadeOutCurve);
        }
        runtime.lastDispatchedRegion = r.id;
        runtime.lastDispatchedSeqIdx = r.sequenceIdx;
    }

    void mouseDrag (const MouseEvent& e) override
    {
        /* Automation overlay vertical resize takes precedence once begun. */
        if (overlayResize_.active)
        {
            if (overlayResize_.laneIdx >= 0 && overlayResize_.laneIdx < owner.lanes_.size())
            {
                setLaneOverlayCurveH (overlayResize_.laneIdx,
                                      overlayResize_.startH + (e.y - overlayResize_.startY));
                resizeForLanes();
                repaint();
            }
            return;
        }

        /* Per-lane vertical resize takes precedence once begun. */
        if (laneResize_.active)
        {
            if (laneResize_.laneIdx >= 0 && laneResize_.laneIdx < owner.lanes_.size())
            {
                const int newH = juce::jlimit (kLaneHMin, kLaneHMax,
                                               laneResize_.startH + (e.y - laneResize_.startY));
                owner.lanes_.getReference (laneResize_.laneIdx).heightPx = newH;
                resizeForLanes();
                repaint();
            }
            return;
        }

        /* Automation overlay point drag takes precedence -- it owns the
         * gesture once a breakpoint mousedown started it. */
        if (autoEdit_.active) { handleAutomationOverlayDrag (e); return; }

        /* Envelope point drag -- updates the point's beat + gain.
         * Sort happens at mouseUp so neighbours don't reshuffle
         * mid-drag (would break the pointer to the in-flight
         * pt for subsequent drag events). */
        if (envGesture_.laneIdx >= 0)
        {
            envGesture_.dragActive = true;
            if (envGesture_.laneIdx >= owner.lanes_.size()) return;
            auto& lane = owner.lanes_.getReference (envGesture_.laneIdx);
            auto* r = lane.playlist.findRegion (envGesture_.regionId);
            if (r == nullptr) return;
            EnvelopePoint* pt = nullptr;
            for (auto& p : r->volumeEnvelope)
                if (p.id == envGesture_.pointId) { pt = &p; break; }
            if (pt == nullptr) return;

            const auto body = regionBodyRect (envGesture_.laneIdx, *r);
            /* Snap the point to the cursor (no grab offset).  A grab offset
             * made an edge-positioned first point sit diagonally off the
             * cursor when grabbed from just outside its dot -- snapping is
             * the original, predictable behaviour. */
            pt->beatOffset = juce::jlimit (0.0, r->lengthBeats,
                                            envXToBeatOffset (e.x, body, *r));
            pt->gainDb     = juce::jlimit (-60.0f, 12.0f,
                                            envYToGainDb (e.y, body));
            repaintLane (envGesture_.laneIdx);
            return;
        }

        /* Per-segment midpoint-handle drag (2D control point).
         * Free X+Y placement: convert the current mouse pos to a
         * (cot, cod) pair via the inverse of the paint mapping.
         *   cot = (x - segLeftX) / segWidthPx           (clamp 0.25..0.75)
         *   cod = envYToGainDb(y) - chordMidDb          (no clamp; segment
         *                                                 gain range is
         *                                                 already enforced
         *                                                 by envYToGainDb). */
        if (envMidGesture_.laneIdx >= 0)
        {
            envMidGesture_.dragActive = true;
            if (envMidGesture_.laneIdx >= owner.lanes_.size()) return;
            auto& lane = owner.lanes_.getReference (envMidGesture_.laneIdx);
            auto* r = lane.playlist.findRegion (envMidGesture_.regionId);
            if (r == nullptr) return;
            const int seg = envMidGesture_.segIndex;
            if (seg < 0 || seg + 1 >= (int) r->volumeEnvelope.size()) return;

            auto& a = r->volumeEnvelope[(size_t) seg];
            const auto& b = r->volumeEnvelope[(size_t) seg + 1];

            const auto body = regionBodyRect (envMidGesture_.laneIdx, *r);
            /* Segment-local X mapping: project pixel x back to a beat
             * offset within the segment, then normalise to [0,1]. */
            const double beat = envXToBeatOffset (e.x, body, *r);
            const double span = juce::jmax (1e-9, b.beatOffset - a.beatOffset);
            const double cot  = juce::jlimit (0.25, 0.75,
                                              (beat - a.beatOffset) / span);

            const float  pinDb     = envYToGainDb (e.y, body);
            const double chordMidDb = 0.5 * ((double) a.gainDb + (double) b.gainDb);

            a.curveOffsetT  = (float) cot;
            a.curveOffsetDb = (float) ((double) pinDb - chordMidDb);

            repaintLane (envMidGesture_.laneIdx);
            return;
        }

        /* Range drag (lane strip OR ruler) is independent of the
         * region-gesture path -- it updates the range overlay and
         * skips lane mutation. */
        if (rangeDragging_)
        {
            const double mouseBeat = juce::jmax (0.0,
                (double) (e.x - kLabelW) / (double) kPxPerBeat);
            rangeStart_ = juce::jmin (rangeAnchor_, mouseBeat);
            rangeEnd_   = juce::jmax (rangeAnchor_, mouseBeat);
            repaint();
            return;
        }

        /* Lane-header drag -- vertical reorder.  Promoted past the
         * drag threshold; hover-lane resolved from yToLaneIdx. */
        if (laneDragSource_ >= 0)
        {
            if (! laneDragActive_)
            {
                if (juce::Point<int> (e.x, e.y).getDistanceFrom (laneDragStart_) < 6)
                    return;
                laneDragActive_ = true;
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            }
            const int hover = yToLaneIdx (e.y);
            if (hover != laneDragHoverIdx_)
            {
                laneDragHoverIdx_ = hover;
                repaint();
            }
            return;
        }

        if (gesture_.laneIdx < 0) return;
        if (e.getDistanceFromDragStart() < kDragThresholdPx && ! gesture_.dragActive)
            return;
        gesture_.dragActive = true;

        if (gesture_.laneIdx >= owner.lanes_.size()) return;

        /* Marquee gesture: live-update the rect bounds + repaint.
         * Hit-test + selection update happen at mouseUp.  Clamping
         * keeps the rect inside the body so an off-edge drag doesn't
         * leak past the visible strip area. */
        if (gesture_.mode == Gesture::Marquee)
        {
            gesture_.marqueeEndXPx = juce::jlimit (kLabelW, getWidth(),  e.x);
            gesture_.marqueeEndYPx = juce::jlimit (kRulerH, getHeight(), e.y);
            repaint();
            return;
        }

        const double mouseBeat = juce::jmax (0.0,
            (double) (e.x - kLabelW) / (double) kPxPerBeat);

        /* Build the trigger-snap exclusion set for this gesture --
         * dragged regions are skipped so the snap doesn't latch onto
         * the very edges being moved.  For Move we exclude every
         * member; for Resize the single anchor. */
        juce::Array<juce::Uuid> snapExclude;
        if (gesture_.mode == Gesture::Move)
        {
            for (const auto& mb : gesture_.members)
                snapExclude.add (mb.regionId);
        }
        else
        {
            snapExclude.add (gesture_.regionId);
        }

        /* Resize gestures stay anchor-only -- selection-wide resize is
         * awkward + rarely wanted (roadmap S1.1).  Mutate the anchor's
         * playlist directly; mouseUp persists.  Edge determines which
         * boundary the drag moves -- RightEdge mutates lengthBeats only;
         * LeftEdge also shifts positionBeats + advances startBeats
         * (source offset) for both audio + MIDI -- the note list /
         * audio source is preserved and only the playable window
         * shifts, so dragging the left edge out and back recovers the
         * original content.  Snap uses the EDGE's original position as
         * the keep-offset anchor (right edge for RightEdge, left for
         * LeftEdge). */
        if (gesture_.mode == Gesture::Resize)
        {
            auto& lane = owner.lanes_.getReference (gesture_.laneIdx);
            if (gesture_.edge == Gesture::RightEdge)
            {
                const double origEnd    = gesture_.originalPos + gesture_.originalLen;
                const double snappedEnd = snapBeat (mouseBeat, origEnd, snapExclude);
                const double newLength  = juce::jmax (kMinRegionBeats,
                    snappedEnd - gesture_.originalPos);
                if (gesture_.kind == Gesture::Audio)
                {
                    lane.playlist.resizeRegion (gesture_.regionId, newLength);
                }
                else
                {
                    if (auto* m = lane.playlist.findMidiRegion (gesture_.regionId))
                    {
                        m->lengthBeats = newLength;
                        owner.publishMidiBindingsForLane (gesture_.laneIdx);
                    }
                }
            }
            else /* LeftEdge */
            {
                const double origEnd  = gesture_.originalPos + gesture_.originalLen;
                const double maxStart = origEnd - kMinRegionBeats;
                /* Lower bound on newPos: the source offset must stay
                 * >= 0 for both kinds.  Audio: can't extend past the
                 * start of the underlying file.  MIDI (Q5): same shape
                 * -- can't extend past source-beat 0 in the pristine
                 * note list. */
                const double lowerBound =
                    juce::jmax (0.0, gesture_.originalPos - gesture_.originalStart);
                const double snapped = juce::jlimit (lowerBound, maxStart,
                                          snapBeat (mouseBeat,
                                                    gesture_.originalPos,
                                                    snapExclude));
                const double delta   = snapped - gesture_.originalPos;
                const double newPos  = snapped;
                const double newLen  = gesture_.originalLen - delta;
                if (gesture_.kind == Gesture::Audio)
                {
                    if (auto* r = lane.playlist.findRegion (gesture_.regionId))
                    {
                        r->positionBeats = newPos;
                        r->lengthBeats   = newLen;
                        r->startBeats    = gesture_.originalStart + delta;
                    }
                }
                else
                {
                    /* MIDI left-trim mirrors the audio source-offset
                     * pattern: advance startBeats by the trim delta
                     * INSTEAD of rewriting the note list.  Notes hidden
                     * by the trim survive in the pristine snapshot and
                     * reappear when the user drags the edge back. */
                    if (auto* m = lane.playlist.findMidiRegion (gesture_.regionId))
                    {
                        m->positionBeats = newPos;
                        m->lengthBeats   = newLen;
                        m->startBeats    = gesture_.originalStart + delta;
                        owner.publishMidiBindingsForLane (gesture_.laneIdx);
                    }
                }
            }
            if (body_resizeNeeded()) resizeForLanes();
            else                     repaintLane (gesture_.laneIdx);
            return;
        }

        /* Move: multi-member path.  Anchor's originalPos drives the
         * delta-and-snap; every other member shifts by the same snapped
         * delta so intra-selection spacing is preserved.  Cross-lane
         * drags are PREVIEW-ONLY -- ghost overlay paints the would-be
         * destinations, commit happens at mouseUp (extract + transfer
         * each member's region).  Same-lane drags live-mutate every
         * tick so the user sees the drag in place. */
        const double rawDeltaBeats = mouseBeat - gesture_.mouseDownXBeats;
        const double rawTarget     = snapBeat (gesture_.originalPos + rawDeltaBeats,
                                                gesture_.originalPos,
                                                snapExclude);
        double       deltaBeats    = rawTarget - gesture_.originalPos;

        const int rawTargetLane = yToLaneIdx (e.y);
        int newLaneOffset = (rawTargetLane >= 0)
            ? rawTargetLane - gesture_.laneIdx
            : gesture_.laneOffset;   /* keep last valid offset when cursor strays */

        /* Shift = axis lock.  First Shift-detection during a Move
         * picks the dominant axis (whichever has travelled further in
         * pixels) and locks the drag to it.  Released Shift restores
         * free movement; if the user re-presses Shift, the dominant
         * axis is recomputed from the live cursor offset.  Matches the
         * Ableton / Bitwig convention; B18. */
        if (e.mods.isShiftDown())
        {
            if (gesture_.axis == Gesture::AxisFree)
            {
                const int dxPx = std::abs ((int) ((deltaBeats) * kPxPerBeat));
                const int dyPx = std::abs (e.y - gesture_.mouseDownYPx);
                gesture_.axis = (dxPx >= dyPx) ? Gesture::AxisHorizontal
                                               : Gesture::AxisVertical;
            }
            if (gesture_.axis == Gesture::AxisHorizontal)
                newLaneOffset = 0;
            else /* AxisVertical */
                deltaBeats = 0.0;
        }
        else
        {
            gesture_.axis = Gesture::AxisFree;
        }

        gesture_.appliedDelta = deltaBeats;

        if (newLaneOffset == 0)
        {
            /* Same-lane case.
             *   Move: live-mutate every member (intuitive in-lane drag).
             *   Copy: preview-only via ghost overlay so the source
             *         regions stay visible at their original positions
             *         and the copies-to-be render where they'll land. */
            const bool wasCrossLane = gesture_.laneOffset != 0;
            gesture_.laneOffset       = 0;
            gesture_.crossLaneInvalid = false;
            if (gesture_.copyOnCommit)
            {
                /* Same-lane copy: no mutation; whole-body repaint to
                 * render the ghost overlay (extended to cover this
                 * case in paint()). */
                if (body_resizeNeeded()) resizeForLanes();
                else                     repaint();
            }
            else
            {
                const auto touched = applySameLaneMoveTick (deltaBeats);
                if (body_resizeNeeded()) resizeForLanes();
                else if (wasCrossLane)   repaint();    /* clear ghost overlay */
                else
                {
                    for (int li : touched) repaintLane (li);
                }
            }
            return;
        }

        /* Cross-lane preview.  Transitioning from same-lane requires
         * a restore tick so the source-lane strips show the regions in
         * their original positions while the ghost shows the preview.
         * Skipped for copy gestures since copy never mutated sources. */
        if (gesture_.laneOffset == 0 && ! gesture_.copyOnCommit)
            applySameLaneMoveTick (0.0);

        gesture_.laneOffset = newLaneOffset;
        validateCrossLaneMove (newLaneOffset, deltaBeats);
        if (body_resizeNeeded()) resizeForLanes();
        else                     repaint();    /* whole-body for ghost overlay */
    }

    void mouseUp (const MouseEvent& e) override
    {
        if (overlayResize_.active)
        {
            overlayResize_ = OverlayResize {};
            owner.writeLanesToSession();   // persist binding heightPx (overlay height)
            return;
        }

        if (laneResize_.active)
        {
            laneResize_ = LaneResize {};
            owner.flushLanesToSession();   // persist the new height (no undo step)
            return;
        }

        if (autoEdit_.active) { handleAutomationOverlayUp (e); return; }

        /* Finish envelope point drag -- sort the envelope (in case
         * the point passed neighbours) + persist.  Clicks without
         * drag still consumed by the env path, no fallthrough to
         * region-launch. */
        if (envGesture_.laneIdx >= 0)
        {
            if (envGesture_.dragActive
                && envGesture_.laneIdx < owner.lanes_.size())
            {
                auto& lane = owner.lanes_.getReference (envGesture_.laneIdx);
                if (auto* r = lane.playlist.findRegion (envGesture_.regionId))
                    r->sortEnvelope();
                owner.writeLanesToSession();
                repaintLane (envGesture_.laneIdx);
            }
            envGesture_ = EnvGesture {};
            return;
        }

        if (envMidGesture_.laneIdx >= 0)
        {
            if (envMidGesture_.dragActive
                && envMidGesture_.laneIdx < owner.lanes_.size())
            {
                /* Persist the new curveAmount.  No sortEnvelope needed
                 * (we didn't touch beatOffsets). */
                owner.writeLanesToSession();
                repaintLane (envMidGesture_.laneIdx);
            }
            envMidGesture_ = EnvMidGesture {};
            return;
        }

        /* Finish range drag (lane strip OR ruler).  Empty range
         * (start == end) collapses to no-range; non-empty range
         * stays armed for loop / future delete-in-range. */
        if (rangeDragging_)
        {
            rangeDragging_ = false;
            if (rangeEnd_ <= rangeStart_ + 1e-6)
            {
                rangeActive_ = false;
            }
            repaint();
            return;
        }

        /* Lane-header drag commit.  Stepwise swap from source toward
         * hover so persistence stays coherent across multi-position
         * moves.  Stationary click is a no-op. */
        if (laneDragSource_ >= 0)
        {
            if (laneDragActive_)
            {
                setMouseCursor (juce::MouseCursor::NormalCursor);
                if (laneDragHoverIdx_ >= 0
                    && laneDragHoverIdx_ < owner.lanes_.size()
                    && laneDragHoverIdx_ != laneDragSource_)
                {
                    int src = laneDragSource_;
                    const int dst = laneDragHoverIdx_;
                    while (src != dst)
                    {
                        const int step = (dst > src) ? +1 : -1;
                        owner.moveLane (src, step);
                        src += step;
                    }
                }
            }
            laneDragSource_   = -1;
            laneDragActive_   = false;
            laneDragHoverIdx_ = -1;
            repaint();
            return;
        }

        if (gesture_.laneIdx < 0) return;

        /* Marquee commit.  Computed BEFORE the gesture reset so
         * regionsInMarquee() can read the live rect state.  No-drag
         * marquee (just a click on empty area) is a no-op here -- the
         * Replace-mode clearSelection already fired in mouseDown. */
        if (gesture_.mode == Gesture::Marquee)
        {
            const bool wasMarqueeDragged = gesture_.dragActive;
            const auto mqMode            = gesture_.marqueeMode;
            if (wasMarqueeDragged)
            {
                const auto hits = regionsInMarquee();
                applyMarqueeSelection (hits, mqMode);
            }
            gesture_ = Gesture {};
            repaint();
            return;
        }

        const int    anchorLane    = gesture_.laneIdx;
        const auto   anchorRegion  = gesture_.regionId;
        const bool   wasDragged    = gesture_.dragActive;
        const auto   gestureMode   = gesture_.mode;
        const auto   gestureKind   = gesture_.kind;
        const int    finalOffset   = gesture_.laneOffset;
        const double finalDelta    = gesture_.appliedDelta;
        const bool   invalidCross  = gesture_.crossLaneInvalid;
        const bool   copyOnCommit  = gesture_.copyOnCommit;
        const auto   members       = gesture_.members;   /* copy before reset */

        gesture_ = Gesture {};

        if (anchorLane < 0 || anchorLane >= owner.lanes_.size()) return;
        auto& anchorLaneRef = owner.lanes_.getReference (anchorLane);
        auto& anchorRuntime = owner.laneRuntime_.getReference (anchorLane);

        /* Click without drag.  For MIDI the click opens the piano-roll
         * dock bound to the anchor region (mirrors double-click);
         * for AUDIO/tracker the click launches the anchor region.
         * Multi-selection visibility is preserved -- this click
         * neither extends nor reduces it. */
        if (! wasDragged)
        {
            if (gestureKind == Gesture::Midi)
            {
                if (auto* sc = findParentComponentOfClass<StandardContent>())
                    sc->showPianoRollForRegion (anchorRegion);
                return;
            }
            const auto* r = anchorLaneRef.playlist.findRegion (anchorRegion);
            if (r == nullptr) return;

            if (anchorRuntime.isTrackerLane() && r->sequenceIdx >= 0)
            {
                if (anchorRuntime.lastDispatchedSeqIdx >= 0
                    && anchorRuntime.lastDispatchedSeqIdx != r->sequenceIdx)
                    anchorRuntime.trackerCache->schedulePlaying (
                        anchorRuntime.lastDispatchedSeqIdx, -1.0, false);
                anchorRuntime.trackerCache->schedulePlaying (r->sequenceIdx, -1.0, true);
            }
            else if (anchorRuntime.isAudioLane())
            {
                const double bpm = owner.monitor_ != nullptr
                    ? (double) owner.monitor_->tempo.get() : 120.0;
                const double sessionSR = owner.monitor_ != nullptr
                    ? (double) owner.monitor_->sampleRate.get() : 48000.0;
                const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
                anchorRuntime.audioClipCache->schedulePlay (
                    r->id, r->sourceId, -1.0, 0, r->looped,
                    r->gainDb,
                    (juce::int64) (r->fadeInBeats  * secsPerBeat * sessionSR),
                    (juce::int64) (r->fadeOutBeats * secsPerBeat * sessionSR),
                    (juce::int64) (r->lengthBeats  * secsPerBeat * sessionSR),
                    r->fadeInCurve, r->fadeOutCurve);
            }
            anchorRuntime.lastDispatchedRegion = r->id;
            anchorRuntime.lastDispatchedSeqIdx = r->sequenceIdx;
            repaintLane (anchorLane);
            juce::ignoreUnused (e, gestureMode);
            return;
        }

        /* Resize finished -- single region.  Length was mutated each
         * tick by mouseDrag's Resize branch; persist + invalidate
         * dispatch cache. */
        if (gestureMode == Gesture::Resize)
        {
            if (gestureKind == Gesture::Midi)
                anchorLaneRef.playlist.rebuildMidiOrder();
            anchorRuntime.lastDispatchedRegion = juce::Uuid::null();
            owner.writeLanesToSession();
            if (body_resizeNeeded()) resizeForLanes();
            else                     repaint();
            return;
        }

        /* Move finished.  Commit shapes branch on (finalOffset != 0)
         * and (copyOnCommit):
         *   - same-lane move: positions were live-mutated each tick;
         *     just persist + rebuild MIDI sort on touched lanes.
         *   - same-lane copy: source never mutated; clone each member
         *     in-lane at the dragged position via commitCopyMove.
         *   - cross-lane refuse (invalidCross): wrong-kind dest, OOB,
         *     or MIDI dest overlap.  Move snap-back is a no-op (drag
         *     handler restored sources).  Copy snap-back is also a
         *     no-op (copy never mutated sources).
         *   - cross-lane move: extract each member + insert at dest.
         *   - cross-lane copy: clone each member + insert at dest;
         *     source stays put. */
        std::set<int> midiLanesNeedingSort;
        std::set<int> runtimesToInvalidate;
        auto gatherMidiSort = [&] (int lane) {
            if (lane < 0 || lane >= owner.lanes_.size()) return;
            if (owner.lanes_.getReference (lane).kind == Lane::Kind::Midi)
                midiLanesNeedingSort.insert (lane);
        };

        if (finalOffset == 0)
        {
            if (copyOnCommit)
            {
                /* Same-lane copy commit -- clone each member in-lane
                 * at the dragged position.  commitCopyMove handles
                 * displace-on-overlap for MIDI. */
                commitCopyMove (members, anchorLane, 0, finalDelta);
                for (const auto& mb : members)
                {
                    runtimesToInvalidate.insert (mb.laneIdx);
                    if (mb.kind == Gesture::Midi)
                        gatherMidiSort (mb.laneIdx);
                }
            }
            else
            {
                /* Same-lane move was live-applied; just persist. */
                for (const auto& mb : members)
                {
                    runtimesToInvalidate.insert (mb.laneIdx);
                    if (mb.kind == Gesture::Midi)
                        gatherMidiSort (mb.laneIdx);
                }
            }
        }
        else if (invalidCross)
        {
            /* Snap-back is a no-op for both move + copy -- move
             * restored sources on the cross-lane-transition tick;
             * copy never mutated sources to begin with.  Invalidate
             * dispatch cache on the anchor + repaint to clear the
             * ghost overlay. */
            anchorRuntime.lastDispatchedRegion = juce::Uuid::null();
            if (body_resizeNeeded()) resizeForLanes();
            else                     repaint();
            return;
        }
        else
        {
            if (copyOnCommit)
                commitCopyMove      (members, anchorLane, finalOffset, finalDelta);
            else
                commitCrossLaneMove (members, anchorLane, finalOffset, finalDelta);
            for (const auto& mb : members)
            {
                runtimesToInvalidate.insert (mb.laneIdx);
                runtimesToInvalidate.insert (mb.laneIdx + finalOffset);
                if (mb.kind == Gesture::Midi)
                {
                    gatherMidiSort (mb.laneIdx);
                    gatherMidiSort (mb.laneIdx + finalOffset);
                }
            }
        }

        for (int li : midiLanesNeedingSort)
            owner.lanes_.getReference (li).playlist.rebuildMidiOrder();

        for (int li : runtimesToInvalidate)
            if (li >= 0 && li < owner.laneRuntime_.size())
                owner.laneRuntime_.getReference (li).lastDispatchedRegion = juce::Uuid::null();

        owner.writeLanesToSession();
        if (body_resizeNeeded()) resizeForLanes();
        else                     repaint();
    }

    void mouseMove (const MouseEvent& e) override
    {
        /* Cursor feedback: change to resize cursor when hovering a
         * region's right-edge handle. */
        if (e.y < kRulerH)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        /* Automation overlay vertical-resize handle (overlay bottom edge). */
        if (overlayResizeHandleAt (e.y) >= 0)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }
        /* Per-lane vertical-resize handle (lane bottom divider) -- not in
         * the Env tool (bottom edge edits envelope points there). */
        if (activeTool_ != Tool::Env && laneResizeHandleAt (e.x, e.y) >= 0)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }
        const int laneIdx = rowAtY (e.y);
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size())
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        if (e.x < kLabelW)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        /* Over the clip strip: show the ACTIVE TOOL's own cursor, never a
         * move-hand.  Each tool's cursor (arrow / crosshair / I-beam /
         * resize / pointing-hand) tells the user what a click will do --
         * the dragging-hand told them "move", which both Select and Env do
         * not primarily do, so it was misleading + unwanted. */
        const auto& lane = owner.lanes_.getReference (laneIdx);
        const int laneTopY = laneClipTopY (laneIdx);
        for (const auto& r : lane.playlist.regions())
        {
            const int regionStartX = kLabelW + (int) (r.positionBeats * kPxPerBeat);
            const int regionEndX   = kLabelW + (int) (r.endBeats()    * kPxPerBeat);
            /* Right-edge resize affordance only for the tools that resize a
             * clip from its edge (Select drag-resize, Trim).  Other tools
             * keep their own cursor right up to the edge. */
            if ((activeTool_ == Tool::Select || activeTool_ == Tool::Trim)
                && e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX
                && e.y >= laneTopY && e.y < laneTopY + laneClipStripH (laneIdx))
            {
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
            if (e.x >= regionStartX && e.x < regionEndX
                && e.y >= laneTopY && e.y < laneTopY + laneClipStripH (laneIdx))
            {
                setMouseCursor (toolCursor());
                return;
            }
        }
        setMouseCursor (toolCursor());
    }

    /* Wheel-zoom removed per UX feedback 2026-05-24 -- the mouse wheel
     * + Ctrl/Cmd modifier overloaded scroll with zoom, which made
     * accidental zooms common.  Zoom now lives exclusively on the
     * toolbar +/- buttons + Shift +/- keys (Body::zoomBy entry).
     * The default Component::mouseWheelMove forwards to the parent
     * Viewport which gives standard vertical-scroll behaviour. */

    /** Empty-strip right-click menu.  Exposes "Paste here" so a user
     *  who has just copied/cut a selection can drop it at the click
     *  position without first picking a target via keyboard.  The
     *  menu is dimmed-out when the clipboard is empty so the user
     *  knows the affordance exists but isn't usable yet. */
    void showEmptyStripContextMenu (int laneIdx, double atBeat)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;

        enum { ItemPaste = 1 };
        juce::PopupMenu menu;
        menu.addItem (ItemPaste, "Paste here (Ctrl+V)", ! clipboard_.empty());

        juce::Component::SafePointer<Body> safe (this);
        const double clickBeat = juce::jmax (0.0, atBeat);
        menu.showMenuAsync (juce::PopupMenu::Options(),
            [safe, laneIdx, clickBeat] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr || result == 0) return;
                if (result == ItemPaste)
                    self->pasteClipboardAt (laneIdx, clickBeat);
            });
    }

    /** Build + show the region right-click popup.  Stop-gap UI until
     *  the Ardour-style tool-mode toolbar lands -- once the toolbar
     *  exists, tool selection determines mouseDown behaviour and
     *  this menu goes away.  Until then, it's the only way to expose
     *  Region.looped toggling + Split. */
    void showRegionContextMenu (int laneIdx, juce::Uuid regionId, double atBeat)
    {
        auto& lane = owner.lanes_.getReference (laneIdx);

        /* Kind-discriminated menu: an audio/tracker region uses the
         * Region API; a MIDI region uses MidiNoteRegion + the parallel
         * Playlist mutators.  Both menus expose Loop / Split / Cut /
         * Copy / Duplicate / Delete with the same shape so the user
         * sees a uniform surface.  Cut / Copy / Duplicate operate on
         * the CURRENT MULTI-SELECTION (which always contains the
         * right-clicked region -- mouseDown promotes it if necessary
         * before the menu opens). */
        if (const auto* r = lane.playlist.findRegion (regionId))
        {
            enum { ItemLoop = 1, ItemSplit, ItemCut, ItemCopy, ItemDuplicate, ItemDelete };
            juce::PopupMenu menu;
            menu.addItem (ItemLoop,  "Loop (Ctrl+L)", true, r->looped);
            menu.addItem (ItemSplit, "Split at click",
                           atBeat > r->positionBeats + 0.0625
                           && atBeat < r->endBeats() - 0.0625);
            menu.addSeparator();
            menu.addItem (ItemCut,       "Cut (Ctrl+X)");
            menu.addItem (ItemCopy,      "Copy (Ctrl+C)");
            menu.addItem (ItemDuplicate, "Duplicate (Ctrl+D)");
            menu.addSeparator();
            menu.addItem (ItemDelete, "Delete");

            juce::Component::SafePointer<Body> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options(),
                [safe, laneIdx, regionId, atBeat] (int result)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr || result == 0) return;
                    if (laneIdx < 0 || laneIdx >= self->owner.lanes_.size()) return;
                    auto& l = self->owner.lanes_.getReference (laneIdx);

                    bool changed = false;
                    switch (result)
                    {
                        case ItemLoop:
                            self->toggleLoopOnSelection();
                            return;     /* helper handles persist + repaint */
                        case ItemSplit:
                            if (! l.playlist.splitRegion (regionId, atBeat).isNull())
                                changed = true;
                            break;
                        case ItemCut:
                            self->cutSelectionToClipboard();
                            return;
                        case ItemCopy:
                            self->copySelectionToClipboard();
                            return;
                        case ItemDuplicate:
                            self->duplicateSelection();
                            return;
                        case ItemDelete:
                            if (l.playlist.removeRegion (regionId))
                            {
                                if (self->selectedRegion_ == regionId)
                                    self->clearSelection();
                                changed = true;
                            }
                            break;
                        default: break;
                    }

                    if (changed)
                    {
                        self->owner.writeLanesToSession();
                        self->resizeForLanes();
                        self->repaintLane (laneIdx);
                    }
                });
            return;
        }

        /* MIDI region branch. */
        if (const auto* m = lane.playlist.findMidiRegion (regionId))
        {
            const double midiEnd = m->positionBeats + m->lengthBeats;
            const bool splittable = atBeat > m->positionBeats + 0.0625
                                  && atBeat < midiEnd            - 0.0625;

            enum { ItemOpenEditor = 1, ItemLoop, ItemSplit,
                    ItemCut, ItemCopy, ItemDuplicate, ItemDelete };
            juce::PopupMenu menu;
            menu.addItem (ItemOpenEditor, "Open in piano-roll");
            menu.addSeparator();
            menu.addItem (ItemLoop,   "Loop (Ctrl+L)", true, m->looped);
            menu.addItem (ItemSplit,  "Split at click", splittable);
            menu.addSeparator();
            menu.addItem (ItemCut,       "Cut (Ctrl+X)");
            menu.addItem (ItemCopy,      "Copy (Ctrl+C)");
            menu.addItem (ItemDuplicate, "Duplicate (Ctrl+D)");
            menu.addSeparator();
            menu.addItem (ItemDelete, "Delete");

            juce::Component::SafePointer<Body> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options(),
                [safe, laneIdx, regionId, atBeat] (int result)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr || result == 0) return;
                    if (laneIdx < 0 || laneIdx >= self->owner.lanes_.size()) return;
                    auto& l = self->owner.lanes_.getReference (laneIdx);

                    bool changed = false;
                    switch (result)
                    {
                        case ItemOpenEditor:
                            if (auto* sc = self->findParentComponentOfClass<StandardContent>())
                                sc->showPianoRollForRegion (regionId);
                            return;   /* no session write needed */
                        case ItemLoop:
                            self->toggleLoopOnSelection();
                            return;     /* helper handles MIDI republish + persist */
                        case ItemSplit:
                            if (! l.playlist.splitMidiRegion (regionId, atBeat).isNull())
                            {
                                self->owner.publishMidiBindingsForLane (laneIdx);
                                changed = true;
                            }
                            break;
                        case ItemCut:
                            self->cutSelectionToClipboard();
                            return;
                        case ItemCopy:
                            self->copySelectionToClipboard();
                            return;
                        case ItemDuplicate:
                            self->duplicateSelection();
                            return;
                        case ItemDelete:
                            if (l.playlist.removeMidiRegion (regionId))
                            {
                                if (self->selectedRegion_ == regionId)
                                    self->clearSelection();
                                self->owner.publishMidiBindingsForLane (laneIdx);
                                changed = true;
                            }
                            break;
                        default: break;
                    }

                    if (changed)
                    {
                        self->owner.writeLanesToSession();
                        self->resizeForLanes();
                        self->repaintLane (laneIdx);
                    }
                });
            return;
        }
    }

    //==========================================================================
    // Marker-track helpers -- ruler right-click + per-marker context
    // menu (rename / recolour / delete).  Marker mutations persist via
    // owner.writeMarkersOnly() so they skip the lanes_ undo snapshot.

    static const juce::Colour* markerPalette() noexcept
    {
        static const juce::Colour palette[] = {
            juce::Colour { 0xff'e0'c0'70 }, // amber (default)
            juce::Colour { 0xff'c5'5a'5a }, // red
            juce::Colour { 0xff'c5'8a'4a }, // orange
            juce::Colour { 0xff'6a'b5'5a }, // green
            juce::Colour { 0xff'4a'a5'b5 }, // cyan
            juce::Colour { 0xff'5a'7a'c5 }, // blue
            juce::Colour { 0xff'9a'5a'c5 }, // purple
            juce::Colour { 0xff'c5'5a'9a }  // pink
        };
        return palette;
    }
    static constexpr int kMarkerPaletteSize = 8;

    void addMarkerAtBeat (double atBeat)
    {
        owner.markerTrack_.addMarker (juce::jmax (0.0, atBeat), juce::String());
        owner.writeMarkersOnly();
        repaint();
    }

    void renameMarker (const juce::Uuid& markerId)
    {
        const auto* m = owner.markerTrack_.findMarker (markerId);
        if (m == nullptr) return;
        auto* aw = new juce::AlertWindow ("Rename marker",
                                           "New name for marker:",
                                           juce::AlertWindow::NoIcon);
        aw->addTextEditor ("name", m->name);
        aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<Body> safe (this);
        const juce::Uuid id = markerId;
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create (
                [safe, id, aw] (int result)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    if (result == 1)
                    {
                        const auto newName = aw->getTextEditorContents ("name").trim();
                        if (self->owner.markerTrack_.renameMarker (id, newName))
                        {
                            self->owner.writeMarkersOnly();
                            self->repaint();
                        }
                    }
                }),
            true);
    }

    void cycleMarkerColour (const juce::Uuid& markerId)
    {
        const auto* m = owner.markerTrack_.findMarker (markerId);
        if (m == nullptr) return;
        const auto* palette = markerPalette();
        int curIdx = -1;
        for (int i = 0; i < kMarkerPaletteSize; ++i)
            if (palette[i].getARGB() == m->colour.getARGB()) { curIdx = i; break; }
        const int nextIdx = (curIdx + 1) % kMarkerPaletteSize;
        owner.markerTrack_.setMarkerColour (markerId, palette[nextIdx]);
        owner.writeMarkersOnly();
        repaint();
    }

    void deleteMarker (const juce::Uuid& markerId)
    {
        if (owner.markerTrack_.removeMarker (markerId))
        {
            owner.writeMarkersOnly();
            repaint();
        }
    }

    /** Ruler right-click router.  Hits a marker triangle -> per-marker
     *  menu; empty ruler area -> "Add marker here". */
    void showRulerContextMenu (int x, int y)
    {
        const double atBeat = juce::jmax (0.0,
            (double) (x - kLabelW) / (double) kPxPerBeat);

        const juce::Uuid hit = markerAtRulerPx (x, y);
        juce::Component::SafePointer<Body> safe (this);

        if (! hit.isNull())
        {
            enum { ItemRename = 1, ItemColour, ItemDelete };
            juce::PopupMenu menu;
            menu.addItem (ItemRename, "Rename marker...");
            menu.addItem (ItemColour, "Cycle colour");
            menu.addSeparator();
            menu.addItem (ItemDelete, "Delete marker");
            menu.showMenuAsync (juce::PopupMenu::Options(),
                [safe, hit] (int result)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr || result == 0) return;
                    switch (result)
                    {
                        case ItemRename: self->renameMarker      (hit); break;
                        case ItemColour: self->cycleMarkerColour (hit); break;
                        case ItemDelete: self->deleteMarker      (hit); break;
                        default: break;
                    }
                });
            return;
        }

        enum { ItemAdd = 1 };
        juce::PopupMenu menu;
        menu.addItem (ItemAdd, "Add marker here");
        menu.showMenuAsync (juce::PopupMenu::Options(),
            [safe, atBeat] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr || result == 0) return;
                if (result == ItemAdd) self->addMarkerAtBeat (atBeat);
            });
    }

    //==========================================================================
    // Active gesture (move / resize) data.  Declared HERE -- above the
    // Multi-region gesture helpers -- because those helpers reference
    // Gesture::Member + Gesture::Kind in their function signatures;
    // ordinary lookup demands the type be visible at signature time
    // (only method BODIES get deferred class-scope lookup).  Keep the
    // rest of the class data section at the bottom.
    //==========================================================================

    /* Anchor fields describe the region that was clicked; members
     * describes the full set of regions being moved together (single-
     * region drag = 1 entry = anchor only; multi-region drag = anchor
     * + every other selected region).  Resize gestures stay anchor-only
     * (roadmap S1.1 decision: selection-wide resize is awkward + rarely
     * wanted).  laneIdx < 0 means no active gesture. */
    struct Gesture {
        /* Marquee: rubber-band select.  Started by a drag on empty
         * lane area; commit hit-tests every region against the live
         * rect on mouseUp. */
        enum Mode { Move, Resize, Marquee };
        enum Kind { Audio, Midi };
        /* Marquee selection-set semantic decided at mouseDown from
         * modifiers (no-mod = Replace, Shift = Add, Ctrl = Toggle).
         * Mirrors Zrythm v1's shift_held/ctrl_held branches. */
        enum MarqueeMode { Replace, Add, Toggle };
        /* Resize gestures track which edge was grabbed.  Right (default)
         * mutates lengthBeats; Left also shifts positionBeats + advances
         * startBeats (source offset) for both audio + MIDI -- notes /
         * audio source survive the trim and reappear on drag-back. */
        enum Edge { RightEdge, LeftEdge };
        /* Shift-axis constrain on Move.  Locks the drag to either
         * horizontal (time) or vertical (lane) once Shift is first
         * detected; cleared when Shift is released.  Decision is sticky
         * for the duration of the Shift hold so a small mouse jitter
         * doesn't flip the lock.  B18. */
        enum Axis { AxisFree, AxisHorizontal, AxisVertical };

        struct Member {
            int        laneIdx       = -1;
            juce::Uuid regionId;
            double     originalPos   = 0.0;
            double     originalLen   = 0.0;
            double     originalStart = 0.0;   /* source offset (Region.startBeats / MidiNoteRegion.startBeats) */
            Kind       kind          = Audio;
        };

        int        laneIdx         = -1;      /* anchor */
        juce::Uuid regionId;                   /* anchor */
        double     originalPos     = 0.0;     /* anchor */
        double     originalLen     = 0.0;     /* anchor */
        double     originalStart   = 0.0;     /* anchor source offset (audio + MIDI) */
        double     mouseDownXBeats = 0.0;
        int        mouseDownYPx    = 0;       /* anchor pixel y for axis lock */
        Mode       mode            = Move;
        Edge       edge            = RightEdge;
        Kind       kind            = Audio;   /* anchor */
        bool       dragActive      = false;

        /* Multi-region drag.  Populated on mouseDown.  Empty == not
         * a Move gesture (Resize) or anchor wasn't part of an active
         * multi-selection.  When populated, the Move path iterates
         * this list instead of the single-region anchor fields. */
        std::vector<Member> members;

        /* Live cross-lane state, recomputed every mouseDrag tick.
         * laneOffset = current target lane delta (anchor's destLane -
         * anchor's source lane).  crossLaneInvalid = at least one
         * member would land out-of-bounds, on a wrong-kind lane, OR
         * (MIDI only) on an overlapping span; ghost paint tints red
         * and mouseUp refuses + snaps back. */
        int    laneOffset       = 0;
        bool   crossLaneInvalid = false;
        double appliedDelta     = 0.0;        /* snapped beat delta */
        Axis   axis             = AxisFree;

        /* Alt-drag copy.  Set on mouseDown when Alt held + click hits
         * a region in the current selection.  When true, the Move
         * gesture NEVER live-mutates the originals (ghost overlay
         * shows the preview just like cross-lane) and mouseUp clones
         * each member at its target instead of moving the source.
         * Roadmap Q8a / Zrythm UiOverlayAction::MovingCopy. */
        bool copyOnCommit = false;

        /* Marquee bounds in body-coord pixels.  start{X,Y}Px set on
         * mouseDown, end{X,Y}Px updated every mouseDrag. */
        int  marqueeStartXPx = 0;
        int  marqueeStartYPx = 0;
        int  marqueeEndXPx   = 0;
        int  marqueeEndYPx   = 0;
        MarqueeMode marqueeMode = Replace;
    };
    Gesture gesture_;

    //==========================================================================
    // Multi-region gesture helpers.  Members are populated on mouseDown;
    // mouseDrag (Move) iterates them, mouseUp commits.  Single-region
    // drag is the 1-member case so the Move path doesn't fork.
    //==========================================================================

    /** Lane index at vertical pixel `y`, or -1 if outside the lane
     *  strips (above the ruler or past the last lane). */
    int yToLaneIdx (int y) const noexcept
    {
        const int idx = rowAtY (y);
        if (idx < 0 || idx >= owner.lanes_.size()) return -1;
        return idx;
    }

    /** Populate gesture_.members with just the anchor.  Used when the
     *  click did NOT land on an already-selected region in a
     *  multi-selection (i.e. the single-region drag case). */
    void seedSingleGestureMember()
    {
        gesture_.members.clear();
        Gesture::Member m;
        m.laneIdx       = gesture_.laneIdx;
        m.regionId      = gesture_.regionId;
        m.originalPos   = gesture_.originalPos;
        m.originalLen   = gesture_.originalLen;
        m.originalStart = gesture_.originalStart;
        m.kind          = gesture_.kind;
        gesture_.members.push_back (m);
    }

    /** Populate gesture_.members from the current multi-selection.
     *  Skips entries whose lane/region is no longer resolvable.  The
     *  anchor is included (it must already be in selected_ for this
     *  helper to be called).  Each member captures its ORIGINAL
     *  position + length so mouseDrag can apply a uniform delta no
     *  matter how the live values drift during the drag. */
    void buildGestureMembersForMove()
    {
        gesture_.members.clear();
        for (const auto& s : selected_)
        {
            if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
            auto& l = owner.lanes_.getReference (s.first);
            if (const auto* r = l.playlist.findRegion (s.second))
            {
                Gesture::Member m;
                m.laneIdx       = s.first;
                m.regionId      = s.second;
                m.originalPos   = r->positionBeats;
                m.originalLen   = r->lengthBeats;
                m.originalStart = r->startBeats;
                m.kind          = Gesture::Audio;
                gesture_.members.push_back (m);
            }
            else if (const auto* mr = l.playlist.findMidiRegion (s.second))
            {
                Gesture::Member m;
                m.laneIdx       = s.first;
                m.regionId      = s.second;
                m.originalPos   = mr->positionBeats;
                m.originalLen   = mr->lengthBeats;
                m.originalStart = mr->startBeats;
                m.kind          = Gesture::Midi;
                gesture_.members.push_back (m);
            }
        }
    }

    /** Returns the Lane::Kind that a member's destination lane must be
     *  for the drag to land.  Audio regions need Audio lanes; MIDI
     *  regions need MIDI lanes. */
    Lane::Kind requiredLaneKindFor (Gesture::Kind k) const noexcept
    {
        return k == Gesture::Audio ? Lane::Kind::Audio : Lane::Kind::Midi;
    }

    /** Apply a same-lane multi-region Move tick.  Every member's
     *  positionBeats is shifted by the same snapped delta computed
     *  against the anchor.  For MIDI members we mutate the field
     *  directly (matches the single-region path); republish bindings
     *  for every touched MIDI lane so the audio thread sees the new
     *  start positions on the next render block.  Returns the set of
     *  touched lanes for the caller to repaint. */
    std::set<int> applySameLaneMoveTick (double deltaBeats)
    {
        std::set<int> touched;
        std::set<int> midiTouched;
        for (const auto& m : gesture_.members)
        {
            if (m.laneIdx < 0 || m.laneIdx >= owner.lanes_.size()) continue;
            auto& lane = owner.lanes_.getReference (m.laneIdx);
            const double newPos = juce::jmax (0.0, m.originalPos + deltaBeats);
            if (m.kind == Gesture::Audio)
            {
                if (lane.playlist.moveRegion (m.regionId, newPos))
                    touched.insert (m.laneIdx);
            }
            else
            {
                if (auto* mr = lane.playlist.findMidiRegion (m.regionId))
                {
                    mr->positionBeats = newPos;
                    midiTouched.insert (m.laneIdx);
                    touched.insert (m.laneIdx);
                }
            }
        }
        for (int li : midiTouched)
            owner.publishMidiBindingsForLane (li);
        return touched;
    }

    /** Validate a cross-lane move against bounds + kind + (MIDI only)
     *  destination overlap.  Sets gesture_.crossLaneInvalid as a side
     *  effect so paint can tint the ghost.  Returns true only when
     *  every member would land cleanly. */
    bool validateCrossLaneMove (int laneOffset, double deltaBeats)
    {
        gesture_.crossLaneInvalid = false;
        for (const auto& m : gesture_.members)
        {
            const int destLaneIdx = m.laneIdx + laneOffset;
            if (destLaneIdx < 0 || destLaneIdx >= owner.lanes_.size())
            {
                gesture_.crossLaneInvalid = true;
                return false;
            }
            const auto& destLane = owner.lanes_.getReference (destLaneIdx);
            if (destLane.kind != requiredLaneKindFor (m.kind))
            {
                gesture_.crossLaneInvalid = true;
                return false;
            }
            if (m.kind == Gesture::Midi)
            {
                /* MIDI dest must have no overlap with the would-be
                 * span.  Slice 2's displace policy may relax this for
                 * drop later. */
                const double newPos = juce::jmax (0.0, m.originalPos + deltaBeats);
                const double newEnd = newPos + m.originalLen;
                for (const auto& other : destLane.playlist.midiRegions())
                {
                    if (other == nullptr) continue;
                    if (other->id == m.regionId) continue;   /* skip self if same lane somehow */
                    const double otherEnd = other->positionBeats + other->lengthBeats;
                    if (newPos < otherEnd && newEnd > other->positionBeats)
                    {
                        gesture_.crossLaneInvalid = true;
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /** Commit a cross-lane move at mouseUp.  Caller has already
     *  validated, but we re-check MIDI overlap before extracting from
     *  source -- addMidiRegion takes its argument by value, so a
     *  late-failing add would LOSE the moved-from unique_ptr.  Touched
     *  MIDI lanes get their bindings republished after the bulk move.
     *  Members are passed explicitly so the caller can have already
     *  cleared gesture_ (mouseUp resets gesture state before commit
     *  so any re-entrancy via repaint sees a clean state). */
    void commitCrossLaneMove (const std::vector<Gesture::Member>& members,
                              int origAnchorLane,
                              int laneOffset,
                              double deltaBeats)
    {
        std::set<int> midiTouched;
        for (const auto& m : members)
        {
            const int srcIdx  = m.laneIdx;
            const int destIdx = m.laneIdx + laneOffset;
            if (srcIdx  < 0 || srcIdx  >= owner.lanes_.size()) continue;
            if (destIdx < 0 || destIdx >= owner.lanes_.size()) continue;
            auto& srcLane  = owner.lanes_.getReference (srcIdx);
            auto& destLane = owner.lanes_.getReference (destIdx);
            const double newPos = juce::jmax (0.0, m.originalPos + deltaBeats);

            if (m.kind == Gesture::Audio)
            {
                const auto* r = srcLane.playlist.findRegion (m.regionId);
                if (r == nullptr) continue;
                Region copy        = *r;
                copy.positionBeats = newPos;
                copy.colour        = destLane.colour;
                srcLane.playlist.removeRegion (m.regionId);
                destLane.playlist.addRegion (std::move (copy));
            }
            else
            {
                /* Pre-check overlap on dest before extracting -- a late
                 * addMidiRegion rejection would destroy the moved-from
                 * unique_ptr.  validateCrossLaneMove already screens
                 * this, but doing it twice keeps the destructive
                 * extract atomic with success. */
                const double newEnd = newPos + m.originalLen;
                bool overlapHit = false;
                for (const auto& other : destLane.playlist.midiRegions())
                {
                    if (other == nullptr) continue;
                    if (other->id == m.regionId) continue;
                    const double oEnd = other->positionBeats + other->lengthBeats;
                    if (newPos < oEnd && newEnd > other->positionBeats)
                    {
                        overlapHit = true;
                        break;
                    }
                }
                if (overlapHit) continue;     /* leave in source */

                auto detached = srcLane.playlist.extractMidiRegion (m.regionId);
                if (detached == nullptr) continue;
                detached->positionBeats = newPos;
                detached->colour        = destLane.colour;
                midiTouched.insert (srcIdx);
                midiTouched.insert (destIdx);
                destLane.playlist.addMidiRegion (std::move (detached));
            }
        }
        /* Update selected_ to track the migrated regions on their new
         * lanes.  Region ids are stable across transfer so we just
         * rebind the laneIdx column. */
        for (auto& s : selected_)
        {
            for (const auto& m : members)
            {
                if (s.second == m.regionId)
                {
                    s.first = m.laneIdx + laneOffset;
                    break;
                }
            }
        }
        if (selectedLane_ == origAnchorLane)
            selectedLane_ = origAnchorLane + laneOffset;

        for (int li : midiTouched)
            owner.publishMidiBindingsForLane (li);
    }

    /** Commit Alt-drag copy at mouseUp.  For each member, CLONE the
     *  source region (audio: copy-by-value; MIDI: clone()) and insert
     *  the clone at the target lane + position.  Originals stay put.
     *  Handles same-lane (laneOffset==0) and cross-lane (laneOffset!=0)
     *  uniformly.  MIDI clones that would overlap on the destination
     *  use displaceMidiRegionsForSpan to make room -- consistent with
     *  paste/duplicate.  Touched MIDI lanes get bindings republished.
     *  Selection is rebuilt to the freshly-created clones so the user
     *  can immediately drag/copy them again (matches paste). */
    void commitCopyMove (const std::vector<Gesture::Member>& members,
                         int origAnchorLane,
                         int laneOffset,
                         double deltaBeats)
    {
        std::set<int> midiTouched;
        juce::Array<std::pair<int, juce::Uuid>> newSelection;
        for (const auto& m : members)
        {
            const int srcIdx  = m.laneIdx;
            const int destIdx = m.laneIdx + laneOffset;
            if (srcIdx  < 0 || srcIdx  >= owner.lanes_.size()) continue;
            if (destIdx < 0 || destIdx >= owner.lanes_.size()) continue;
            auto& srcLane  = owner.lanes_.getReference (srcIdx);
            auto& destLane = owner.lanes_.getReference (destIdx);
            /* Per-kind sanity: dest must be same kind as source.
             * Cross-lane wrong-kind drops are screened by
             * validateCrossLaneMove during drag; this check guards
             * the race-free message-thread commit. */
            if (destLane.kind != requiredLaneKindFor (m.kind)) continue;
            const double newPos = juce::jmax (0.0, m.originalPos + deltaBeats);

            if (m.kind == Gesture::Audio)
            {
                const auto* r = srcLane.playlist.findRegion (m.regionId);
                if (r == nullptr) continue;
                Region copy        = *r;
                copy.id            = juce::Uuid();            /* fresh id */
                copy.positionBeats = newPos;
                copy.colour        = destLane.colour;
                const juce::Uuid newId = copy.id;
                if (destLane.playlist.addRegion (std::move (copy)))
                    newSelection.add ({ destIdx, newId });
            }
            else
            {
                const auto* mr = srcLane.playlist.findMidiRegion (m.regionId);
                if (mr == nullptr) continue;
                auto clone           = mr->clone();
                clone->id            = juce::Uuid();          /* fresh id */
                clone->positionBeats = newPos;
                clone->colour        = destLane.colour;
                const juce::Uuid newId  = clone->id;
                const double     newEnd = newPos + clone->lengthBeats;
                /* Displace MIDI dest overlaps so the copy lands cleanly
                 * (matches paste / duplicate semantic from Slice 2). */
                const auto displaced = destLane.playlist
                    .displaceMidiRegionsForSpan (newPos, newEnd);
                if (! displaced.empty())
                    midiTouched.insert (destIdx);
                if (destLane.playlist.addMidiRegion (std::move (clone)))
                {
                    newSelection.add ({ destIdx, newId });
                    midiTouched.insert (destIdx);
                }
            }
        }
        if (! newSelection.isEmpty())
        {
            selected_       = newSelection;
            const auto& back = selected_.getLast();
            selectedLane_   = back.first;
            selectedRegion_ = back.second;
        }
        juce::ignoreUnused (origAnchorLane);
        for (int li : midiTouched)
            owner.publishMidiBindingsForLane (li);
    }

    //==========================================================================
    // Marquee (rubber-band) selection helpers.  Started on mouseDown
    // when the Select tool is active + click hits empty lane area.
    // mouseDrag updates the rect; mouseUp hit-tests every region on
    // every lane against the rect + applies the marqueeMode
    // (Replace / Add / Toggle).  Roadmap Q2 / Zrythm v1
    // UiOverlayAction::SELECTING.
    //==========================================================================

    /** Choose marquee mode from modifier state at mouseDown.  Shift =
     *  Add (union with current selection), Ctrl/Cmd = Toggle (xor),
     *  no modifier = Replace.  Shift wins over Ctrl when both are
     *  held (matches GTK + JUCE convention). */
    Gesture::MarqueeMode marqueeModeFromMods (const juce::ModifierKeys& mods) const noexcept
    {
        if (mods.isShiftDown())                          return Gesture::Add;
        if (mods.isCommandDown() || mods.isCtrlDown())   return Gesture::Toggle;
        return Gesture::Replace;
    }

    /** Normalised marquee rect from start/end pixel pairs.  Always
     *  returns positive width/height regardless of drag direction. */
    Rectangle<int> currentMarqueeRect() const noexcept
    {
        return Rectangle<int>::leftTopRightBottom (
            juce::jmin (gesture_.marqueeStartXPx, gesture_.marqueeEndXPx),
            juce::jmin (gesture_.marqueeStartYPx, gesture_.marqueeEndYPx),
            juce::jmax (gesture_.marqueeStartXPx, gesture_.marqueeEndXPx),
            juce::jmax (gesture_.marqueeStartYPx, gesture_.marqueeEndYPx));
    }

    /** Return every (laneIdx, regionId) pair whose painted body rect
     *  intersects the marquee.  Walks audio + MIDI regions across all
     *  lanes.  Bounded by the rect's beat range so unrelated lanes
     *  are skipped cheaply (Y-range filter). */
    juce::Array<std::pair<int, juce::Uuid>> regionsInMarquee() const
    {
        juce::Array<std::pair<int, juce::Uuid>> hits;
        const auto rect = currentMarqueeRect();
        if (rect.isEmpty()) return hits;

        const double beatLo = juce::jmax (0.0,
            (double) (rect.getX()     - kLabelW) / (double) kPxPerBeat);
        const double beatHi = juce::jmax (0.0,
            (double) (rect.getRight() - kLabelW) / (double) kPxPerBeat);
        const int laneLo = juce::jmax (0, rowAtY (rect.getY()));
        const int laneHi = juce::jmin (owner.lanes_.size() - 1,
                                       rowAtY (rect.getBottom()));

        for (int li = laneLo; li <= laneHi; ++li)
        {
            const auto& l = owner.lanes_.getReference (li);
            for (const auto& r : l.playlist.regions())
            {
                if (r.endBeats()      <= beatLo) continue;
                if (r.positionBeats   >= beatHi) continue;
                hits.add ({ li, r.id });
            }
            for (const auto& mp : l.playlist.midiRegions())
            {
                if (mp == nullptr) continue;
                const double end = mp->positionBeats + mp->lengthBeats;
                if (end                <= beatLo) continue;
                if (mp->positionBeats  >= beatHi) continue;
                hits.add ({ li, mp->id });
            }
        }
        return hits;
    }

    /** Apply a marquee selection to selected_ based on marqueeMode.
     *  - Replace: selected_ = hits.
     *  - Add:     selected_ unions hits (no duplicates).
     *  - Toggle:  hits already-selected ones are removed; missing ones
     *             are added (xor).
     *  Primary anchor updates to the last hit; if no hits + Replace,
     *  selection clears (mirrors empty-click). */
    void applyMarqueeSelection (const juce::Array<std::pair<int, juce::Uuid>>& hits,
                                Gesture::MarqueeMode mode)
    {
        auto containsId = [] (const juce::Array<std::pair<int, juce::Uuid>>& arr,
                              const std::pair<int, juce::Uuid>& key)
        {
            for (const auto& s : arr)
                if (s.first == key.first && s.second == key.second) return true;
            return false;
        };

        if (mode == Gesture::Replace)
        {
            selected_.clearQuick();
            for (const auto& h : hits) selected_.add (h);
        }
        else if (mode == Gesture::Add)
        {
            for (const auto& h : hits)
                if (! containsId (selected_, h)) selected_.add (h);
        }
        else /* Toggle */
        {
            for (const auto& h : hits)
            {
                bool removed = false;
                for (int i = 0; i < selected_.size(); ++i)
                {
                    const auto& s = selected_.getReference (i);
                    if (s.first == h.first && s.second == h.second)
                    {
                        selected_.remove (i);
                        removed = true;
                        break;
                    }
                }
                if (! removed) selected_.add (h);
            }
        }

        if (selected_.isEmpty())
        {
            selectedLane_   = -1;
            selectedRegion_ = juce::Uuid::null();
        }
        else
        {
            const auto& back = selected_.getLast();
            selectedLane_   = back.first;
            selectedRegion_ = back.second;
        }
    }

    //==========================================================================
    // Multi-selection helpers.
    //==========================================================================

    bool isSelected (int lane, juce::Uuid id) const noexcept
    {
        for (const auto& s : selected_)
            if (s.first == lane && s.second == id) return true;
        return false;
    }

    void clearSelection()
    {
        selected_.clearQuick();
        selectedLane_   = -1;
        selectedRegion_ = juce::Uuid::null();
    }

    void setPrimarySelection (int lane, juce::Uuid id)
    {
        selected_.clearQuick();
        if (lane >= 0 && ! id.isNull())
            selected_.add ({ lane, id });
        selectedLane_   = lane;
        selectedRegion_ = id;
    }

    void toggleSelection (int lane, juce::Uuid id)
    {
        if (lane < 0 || id.isNull()) return;
        for (int i = 0; i < selected_.size(); ++i)
        {
            if (selected_.getReference (i).first == lane
                && selected_.getReference (i).second == id)
            {
                selected_.remove (i);
                if (selectedLane_ == lane && selectedRegion_ == id)
                {
                    if (selected_.isEmpty())
                    {
                        selectedLane_   = -1;
                        selectedRegion_ = juce::Uuid::null();
                    }
                    else
                    {
                        const auto& back = selected_.getLast();
                        selectedLane_   = back.first;
                        selectedRegion_ = back.second;
                    }
                }
                return;
            }
        }
        selected_.add ({ lane, id });
        selectedLane_   = lane;
        selectedRegion_ = id;
    }

    /** Shift+click range-extend.  Anchor = current primary; clicked
     *  region defines the other endpoint.  Selects every region (both
     *  audio AND MIDI) on the SAME lane whose positionBeats falls
     *  inside [min, max] of the two anchor + click positions.  If the
     *  primary is on a different lane, falls back to a plain primary
     *  select on the clicked region. */
    void extendSelection (int lane, juce::Uuid id)
    {
        if (lane < 0 || id.isNull()) return;
        if (selectedLane_ < 0 || selectedLane_ != lane || selectedRegion_.isNull())
        {
            setPrimarySelection (lane, id);
            return;
        }
        if (lane >= owner.lanes_.size()) return;
        auto& l = owner.lanes_.getReference (lane);

        auto rangeOf = [&] (juce::Uuid rid) -> std::pair<double, double> {
            if (auto* r = l.playlist.findRegion (rid))
                return { r->positionBeats, r->endBeats() };
            if (auto* m = l.playlist.findMidiRegion (rid))
                return { m->positionBeats, m->positionBeats + m->lengthBeats };
            return { 0.0, 0.0 };
        };
        const auto a = rangeOf (selectedRegion_);
        const auto b = rangeOf (id);
        const double lo = juce::jmin (a.first,  b.first);
        const double hi = juce::jmax (a.second, b.second);

        for (const auto& r : l.playlist.regions())
        {
            if (r.positionBeats >= lo && r.positionBeats <= hi
                && ! isSelected (lane, r.id))
                selected_.add ({ lane, r.id });
        }
        for (const auto& mp : l.playlist.midiRegions())
        {
            if (mp == nullptr) continue;
            if (mp->positionBeats >= lo && mp->positionBeats <= hi
                && ! isSelected (lane, mp->id))
                selected_.add ({ lane, mp->id });
        }
        selectedLane_   = lane;
        selectedRegion_ = id;
    }

    //==========================================================================
    // Clipboard ops.
    //==========================================================================

    /** Return every region (audio + MIDI, across every lane) whose
     *  [positionBeats, endBeats) span overlaps [rangeStart_, rangeEnd_).
     *  Pro Tools / Reaper convention: a region only partially in the
     *  range is included WHOLE (no trim).  Empty when no range is
     *  active or it's degenerate. */
    juce::Array<std::pair<int, juce::Uuid>> regionsIntersectingRange() const
    {
        juce::Array<std::pair<int, juce::Uuid>> hits;
        if (! rangeActive_ || rangeEnd_ <= rangeStart_) return hits;

        const double lo = rangeStart_;
        const double hi = rangeEnd_;
        for (int laneIdx = 0; laneIdx < owner.lanes_.size(); ++laneIdx)
        {
            const auto& l = owner.lanes_.getReference (laneIdx);
            for (const auto& r : l.playlist.regions())
            {
                if (r.endBeats() <= lo || r.positionBeats >= hi) continue;
                hits.add ({ laneIdx, r.id });
            }
            for (const auto& mp : l.playlist.midiRegions())
            {
                if (mp == nullptr) continue;
                const double end = mp->positionBeats + mp->lengthBeats;
                if (end <= lo || mp->positionBeats >= hi) continue;
                hits.add ({ laneIdx, mp->id });
            }
        }
        return hits;
    }

    /** Snapshot every region in `source` into clipboard_.  anchorBeat
     *  is the clipboard's time origin (paste rebuilds positions
     *  relative to it).  anchorLane is the topmost lane index in the
     *  source.  Empty source -> no-op. */
    void copyRegionsToClipboard (const juce::Array<std::pair<int, juce::Uuid>>& source,
                                 int anchorLane,
                                 double anchorBeat)
    {
        if (source.isEmpty()) return;
        clipboard_.clear();
        for (const auto& s : source)
        {
            if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
            auto& l = owner.lanes_.getReference (s.first);
            if (const auto* r = l.playlist.findRegion (s.second))
            {
                ClipboardEntry e;
                e.originLaneKind = l.kind;
                e.laneOffset     = s.first - anchorLane;
                e.beatOffset     = r->positionBeats - anchorBeat;
                e.isAudio        = true;
                e.audioRegion    = *r;
                clipboard_.push_back (std::move (e));
            }
            else if (const auto* m = l.playlist.findMidiRegion (s.second))
            {
                ClipboardEntry e;
                e.originLaneKind = l.kind;
                e.laneOffset     = s.first - anchorLane;
                e.beatOffset     = m->positionBeats - anchorBeat;
                e.isAudio        = false;
                e.midiRegion     = m->clone();
                clipboard_.push_back (std::move (e));
            }
        }
    }

    /** Range-priority copy.  When rangeActive_ + non-degenerate, the
     *  clipboard sources from regions intersecting the range with
     *  rangeStart_ as the beat anchor.  Otherwise falls back to the
     *  selected_ set with min-beat anchor (legacy behaviour). */
    void copySelectionToClipboard()
    {
        if (rangeActive_ && rangeEnd_ > rangeStart_)
        {
            const auto source = regionsIntersectingRange();
            if (source.isEmpty()) return;
            int anchorLane = INT_MAX;
            for (const auto& s : source) anchorLane = juce::jmin (anchorLane, s.first);
            if (anchorLane == INT_MAX) return;
            copyRegionsToClipboard (source, anchorLane, rangeStart_);
            return;
        }

        if (selected_.isEmpty()) return;
        int anchorLane = INT_MAX;
        double anchorBeat = std::numeric_limits<double>::infinity();
        for (const auto& s : selected_)
        {
            if (s.first >= owner.lanes_.size()) continue;
            auto& l = owner.lanes_.getReference (s.first);
            if (const auto* r = l.playlist.findRegion (s.second))
            {
                anchorLane = juce::jmin (anchorLane, s.first);
                anchorBeat = juce::jmin (anchorBeat, r->positionBeats);
            }
            else if (const auto* m = l.playlist.findMidiRegion (s.second))
            {
                anchorLane = juce::jmin (anchorLane, s.first);
                anchorBeat = juce::jmin (anchorBeat, m->positionBeats);
            }
        }
        if (anchorLane == INT_MAX) return;
        copyRegionsToClipboard (selected_, anchorLane, anchorBeat);
    }

    /** Cut = copy + delete every region in the source set.  Range mode
     *  uses the range-intersecting set; selection mode uses selected_.
     *  Touched MIDI lanes get their bindings republished after the
     *  bulk delete so the audio thread drops stale entries before any
     *  held NoteOff. */
    void cutSelectionToClipboard()
    {
        juce::Array<std::pair<int, juce::Uuid>> victims;
        if (rangeActive_ && rangeEnd_ > rangeStart_)
            victims = regionsIntersectingRange();
        else
            victims = selected_;
        if (victims.isEmpty()) return;

        copySelectionToClipboard();

        std::set<int> midiLanesTouched;
        for (const auto& s : victims)
        {
            if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
            auto& l = owner.lanes_.getReference (s.first);
            if (l.playlist.removeRegion (s.second)) continue;
            if (l.playlist.removeMidiRegion (s.second))
                midiLanesTouched.insert (s.first);
        }
        clearSelection();
        for (int lane : midiLanesTouched)
            owner.publishMidiBindingsForLane (lane);
        owner.writeLanesToSession();
        resizeForLanes();
        repaint();
    }

    /** Paste every clipboard entry relative to (targetLane, targetBeat).
     *  Entries whose target lane is out of range OR whose origin kind
     *  doesn't match the destination's kind are skipped.  Overlap on
     *  MIDI lanes is rejected by Playlist; we silently drop those (the
     *  audio path allows overlap by design).  Pasted regions become
     *  the new selection so the user can immediately drag or copy
     *  them again. */
    void pasteClipboardAt (int targetLane, double targetBeat)
    {
        if (clipboard_.empty()) return;
        if (targetLane < 0 || targetLane >= owner.lanes_.size()) return;

        std::set<int> midiLanesTouched;
        juce::Array<std::pair<int, juce::Uuid>> newSelection;

        for (const auto& e : clipboard_)
        {
            const int destLaneIdx = targetLane + e.laneOffset;
            if (destLaneIdx < 0 || destLaneIdx >= owner.lanes_.size()) continue;
            auto& destLane = owner.lanes_.getReference (destLaneIdx);
            if (destLane.kind != e.originLaneKind) continue;

            const double destBeat = juce::jmax (0.0, targetBeat + e.beatOffset);

            if (e.isAudio)
            {
                Region copy        = e.audioRegion;
                copy.id            = juce::Uuid();
                copy.positionBeats = destBeat;
                /* Inherit destination lane tint -- the cached colour
                 * on the clipboard entry tracked the SOURCE lane. */
                copy.colour        = destLane.colour;
                if (destLane.playlist.addRegion (Region (copy)))
                    newSelection.add ({ destLaneIdx, copy.id });
            }
            else
            {
                if (e.midiRegion == nullptr) continue;
                auto clone           = e.midiRegion->clone();
                clone->id            = juce::Uuid();
                clone->positionBeats = destBeat;
                clone->colour        = destLane.colour;
                const juce::Uuid newId   = clone->id;
                const double     newEnd  = destBeat + clone->lengthBeats;
                /* Ableton/Bitwig parity: paste DISPLACES overlapping
                 * regions instead of silently dropping the paste.
                 * displace runs in the same writeLanesToSession scope
                 * so undo treats it as a single gesture. */
                const auto displaced = destLane.playlist
                    .displaceMidiRegionsForSpan (destBeat, newEnd);
                if (! displaced.empty())
                    midiLanesTouched.insert (destLaneIdx);
                if (destLane.playlist.addMidiRegion (std::move (clone)))
                {
                    newSelection.add ({ destLaneIdx, newId });
                    midiLanesTouched.insert (destLaneIdx);
                    /* Selected_ may carry now-stale ids that displace
                     * removed.  Strip them so the new selection only
                     * contains the freshly-pasted clones. */
                    for (auto did : displaced)
                        for (int i = 0; i < selected_.size(); )
                            if (selected_.getReference (i).second == did)
                                selected_.remove (i);
                            else
                                ++i;
                }
            }
        }

        if (newSelection.isEmpty()) return;

        selected_ = newSelection;
        const auto& back = selected_.getLast();
        selectedLane_   = back.first;
        selectedRegion_ = back.second;

        for (int lane : midiLanesTouched)
            owner.publishMidiBindingsForLane (lane);
        owner.writeLanesToSession();
        resizeForLanes();
        repaint();
    }

    /** Duplicate every selected region in place: each copy lands at
     *  the original's endBeats on the same lane.  Audio lanes allow
     *  overlap by Playlist design; MIDI overlap is rejected by
     *  addMidiRegion so the dup silently drops -- callers can split
     *  the original to make room first. */
    void duplicateSelection()
    {
        if (selected_.isEmpty()) return;

        std::set<int> midiLanesTouched;
        juce::Array<std::pair<int, juce::Uuid>> newSelection;

        for (const auto& s : selected_)
        {
            if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
            auto& lane = owner.lanes_.getReference (s.first);
            if (const auto* r = lane.playlist.findRegion (s.second))
            {
                Region copy        = *r;
                copy.id            = juce::Uuid();
                copy.positionBeats = r->endBeats();
                if (lane.playlist.addRegion (Region (copy)))
                    newSelection.add ({ s.first, copy.id });
            }
            else if (const auto* m = lane.playlist.findMidiRegion (s.second))
            {
                auto clone           = m->clone();
                clone->id            = juce::Uuid();
                clone->positionBeats = m->positionBeats + m->lengthBeats;
                const juce::Uuid newId  = clone->id;
                const double     newPos = clone->positionBeats;
                const double     newEnd = newPos + clone->lengthBeats;
                /* Same displacement policy as paste -- no silent drop
                 * when the duplicated span overlaps existing MIDI. */
                const auto displaced = lane.playlist
                    .displaceMidiRegionsForSpan (newPos, newEnd);
                if (! displaced.empty())
                    midiLanesTouched.insert (s.first);
                if (lane.playlist.addMidiRegion (std::move (clone)))
                {
                    newSelection.add ({ s.first, newId });
                    midiLanesTouched.insert (s.first);
                }
            }
        }

        if (newSelection.isEmpty()) return;

        selected_ = newSelection;
        const auto& back = selected_.getLast();
        selectedLane_   = back.first;
        selectedRegion_ = back.second;

        for (int lane : midiLanesTouched)
            owner.publishMidiBindingsForLane (lane);
        owner.writeLanesToSession();
        resizeForLanes();
        repaint();
    }

    /** Flip the .looped flag on every selected region.  All-on if any
     *  was off; all-off only when every one was already looped.  MIDI
     *  lanes get their bindings republished so the audio thread's
     *  EntryList picks up the new boundary-loop behaviour. */
    void toggleLoopOnSelection()
    {
        if (selected_.isEmpty()) return;

        bool anyOff = false;
        for (const auto& s : selected_)
        {
            if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
            auto& l = owner.lanes_.getReference (s.first);
            if (auto* r = l.playlist.findRegion (s.second))      { if (! r->looped) { anyOff = true; break; } }
            else if (auto* m = l.playlist.findMidiRegion (s.second)) { if (! m->looped) { anyOff = true; break; } }
        }
        const bool target = anyOff;  /* if any is off -> turn all on; else turn all off */

        std::set<int> midiLanesTouched;
        bool changed = false;
        for (const auto& s : selected_)
        {
            if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
            auto& l = owner.lanes_.getReference (s.first);
            if (auto* r = l.playlist.findRegion (s.second))
            {
                if (r->looped != target) { r->looped = target; changed = true; }
            }
            else if (auto* m = l.playlist.findMidiRegion (s.second))
            {
                if (m->looped != target)
                {
                    m->looped = target;
                    /* Snapshot loopLengthBeats at the moment Loop is
                     * toggled on so subsequent right-edge drags extend
                     * the number of repeats (not the loop pattern).
                     * Skip when a non-zero value is already present --
                     * preserves whatever the user previously dialed in
                     * across Loop on -> off -> on cycles. */
                    if (target && m->loopLengthBeats == 0.0 && m->lengthBeats > 0.0)
                        m->loopLengthBeats = m->lengthBeats;
                    midiLanesTouched.insert (s.first);
                    changed = true;
                }
            }
        }

        if (! changed) return;
        for (int lane : midiLanesTouched)
            owner.publishMidiBindingsForLane (lane);
        owner.writeLanesToSession();
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        /* Numpad 1-9 -- jump to marker N (1-indexed against the
         * position-sorted marker list).  Handled before the modifier
         * branches so an unmodified numpad press works the way DAW
         * users expect.  Numpad 0 left free for a future "next/prev"
         * pair or zoom-to-fit shortcut. */
        for (int i = 1; i <= 9; ++i)
        {
            const int numPad = juce::KeyPress::numberPad0 + i;
            if (key.getKeyCode() == numPad)
            {
                if (const auto* m = owner.markerTrack_.markerAt ((std::size_t) (i - 1)))
                {
                    owner.seekToBeat (m->positionBeats);
                    repaint();
                    return true;
                }
                return false;
            }
        }

        /* Ctrl/Cmd shortcuts -- handled before the legacy Delete /
         * zoom branches so the modifier-bearing keys don't fall
         * through to a parent's command dispatcher. */
        const auto mods = key.getModifiers();
        if (mods.isCommandDown() || mods.isCtrlDown())
        {
            if (key.getKeyCode() == 'C')
            {
                copySelectionToClipboard();
                return true;
            }
            if (key.getKeyCode() == 'X')
            {
                cutSelectionToClipboard();
                return true;
            }
            if (key.getKeyCode() == 'V')
            {
                /* Paste anchor: primary selection's lane (still
                 * meaningful even after Cut since Ctrl+X clears
                 * selection -> use lastInteractedLane_).  Beat = the
                 * transport's current playhead, which the Body
                 * paints in the ruler regardless of playback state. */
                int   targetLane = selectedLane_;
                if (targetLane < 0) targetLane = lastInteractedLane_;
                if (targetLane < 0) return true;
                pasteClipboardAt (targetLane, juce::jmax (0.0, owner.lastBeat_));
                return true;
            }
            if (key.getKeyCode() == 'D')
            {
                duplicateSelection();
                return true;
            }
            if (key.getKeyCode() == 'L')
            {
                toggleLoopOnSelection();
                return true;
            }
        }

        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            if (selected_.isEmpty()) return false;

            std::set<int> midiLanesTouched;
            std::set<int> lanesTouched;
            bool removed = false;
            const auto victims = selected_;
            for (const auto& s : victims)
            {
                if (s.first < 0 || s.first >= owner.lanes_.size()) continue;
                auto& lane = owner.lanes_.getReference (s.first);
                /* Try audio first; fall through to MIDI.  Same
                 * tear-down semantics as the single-region delete:
                 * MIDI deletion republishes bindings so the audio
                 * thread drops stale entries before any held NoteOff. */
                if (lane.playlist.removeRegion (s.second))
                {
                    lanesTouched.insert (s.first);
                    removed = true;
                }
                else if (lane.playlist.removeMidiRegion (s.second))
                {
                    midiLanesTouched.insert (s.first);
                    lanesTouched.insert (s.first);
                    removed = true;
                }
            }
            if (! removed) return false;
            clearSelection();
            for (int lane : midiLanesTouched)
                owner.publishMidiBindingsForLane (lane);
            owner.writeLanesToSession();
            resizeForLanes();
            if (lanesTouched.size() <= 2)
                for (int lane : lanesTouched) repaintLane (lane);
            else
                repaint();
            return true;
        }

        /* Shift + = / + : zoom in.  Shift + - : zoom out.  Mirrors the
         * mouse-wheel pinch step; anchor stays at the viewport centre
         * since there's no cursor X for keyboard zoom. */
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
        return false;
    }

    /** Step the horizontal zoom by `factor` (>1 zooms in).  When
     *  `anchorBodyX < 0` the anchor is the centre of the visible
     *  viewport area so the user keeps roughly the same beats on
     *  screen across the step.  Shared between toolbar +/- buttons
     *  and the keyboard Shift +/- shortcut. */
    void zoomBy (double factor, int anchorBodyX = -1)
    {
        const auto viewArea = owner.viewport_.getViewArea();
        if (anchorBodyX < 0)
            anchorBodyX = viewArea.getCentreX();

        const int stripPxX = anchorBodyX - kLabelW;
        const double anchorBeat = (stripPxX <= 0)
            ? 0.0
            : (double) stripPxX / (double) kPxPerBeat;

        const int newPxPerBeat = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax,
                                                (int) std::lround (kPxPerBeat * factor));
        if (newPxPerBeat == kPxPerBeat) return;
        kPxPerBeat = newPxPerBeat;
        resizeForLanes();

        const int newAnchorBodyX = kLabelW + (int) (anchorBeat * kPxPerBeat);
        const int anchorScreenX  = anchorBodyX - viewArea.getX();
        const int newScrollX     = juce::jmax (0, newAnchorBodyX - anchorScreenX);
        owner.viewport_.setViewPosition (newScrollX, viewArea.getY());

        /* Persist zoom + scroll on the actual change.  Eliminates the
         * willBeRemoved write-on-detach pattern that was racy with
         * session reload and unreliable for view-switch persistence. */
        owner.writeViewStateToSession();
    }

    /** Zoom out until the longest region's end fits inside the visible
     *  viewport width.  Scrolls back to x=0 (overview).  No-op if
     *  there are no regions or the visible width is non-positive.
     *
     *  Width source = viewport.getMaximumVisibleWidth() (the
     *  viewport's inner width minus the vertical scrollbar when
     *  shown).  Earlier versions used viewport.getViewArea().getWidth()
     *  which JUCE clamps to `min(contentWidth - scrollX, viewportInnerWidth)`
     *  -- when content is narrower than the viewport that returned
     *  the CONTENT width, making fit a no-op or, with the +4 beat
     *  padding feedback loop, an incremental crawl over many clicks.
     *
     *  Padding compensation: resizeForLanes pads the body to
     *  (maxBeats + kFitPaddingBeats) * pxPerBeat.  zoomToFit divides
     *  by the SAME padded count so a single click lands an exact
     *  fit; without compensation the body is ~kFitPaddingBeats *
     *  pxPerBeat short of the viewport every time. */
    void zoomToFit()
    {
        double maxEndBeats = 0.0;
        for (const auto& l : owner.lanes_)
        {
            for (const auto& r : l.playlist.regions())
                maxEndBeats = juce::jmax (maxEndBeats, r.endBeats());
            for (const auto& m : l.playlist.midiRegions())
                if (m != nullptr)
                    maxEndBeats = juce::jmax (maxEndBeats,
                                                m->positionBeats + m->lengthBeats);
        }
        if (maxEndBeats <= 0.0) return;

        const int viewportInnerW = owner.viewport_.getMaximumVisibleWidth();
        const int availPx = viewportInnerW - kLabelW - 8;
        if (availPx <= 0) return;

        const double fitBeats = maxEndBeats + (double) kFitPaddingBeats;
        const int newPxPerBeat = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax,
            (int) std::floor ((double) availPx / fitBeats));

        if (newPxPerBeat == kPxPerBeat)
        {
            owner.viewport_.setViewPosition (0, owner.viewport_.getViewPositionY());
            owner.writeViewStateToSession();
            return;
        }
        kPxPerBeat = newPxPerBeat;
        resizeForLanes();
        owner.viewport_.setViewPosition (0, owner.viewport_.getViewPositionY());
        owner.writeViewStateToSession();
    }

    //==========================================================================
    // Layout helpers

    /** Beat at which the live recording placeholder currently ends.
     *  Used by resizeForLanes + body_resizeNeeded so the body width
     *  grows in real time as the playhead extends past the previous
     *  edge -- otherwise the ruler / placeholder freeze at the old
     *  size until the AudioClipNode commit hands back a finalised
     *  Region.  Returns 0.0 unless transport is recording AND at
     *  least one audio lane is armed. */
    double liveRecordingEnd() const noexcept
    {
        if (owner.monitor_ == nullptr) return 0.0;
        if (! owner.monitor_->recording.get()) return 0.0;
        bool anyArmed = false;
        for (int i = 0; i < owner.lanes_.size(); ++i)
        {
            if (i >= owner.laneRuntime_.size()) break;
            const auto& l  = owner.lanes_.getReference (i);
            const auto& rs = owner.laneRuntime_.getReference (i);
            if (rs.isAudioLane() && l.armed) { anyArmed = true; break; }
        }
        if (! anyArmed) return 0.0;
        return juce::jmax (0.0, owner.lastBeat_);
    }

    /** Snap a target beat to the configured grid, optionally with
     *  trigger snap (region edges) + keep-offset (preserve the
     *  anchor's fractional offset within the grid division).
     *
     *  @param target    Raw target beat (post-delta for Move, raw
     *                   cursor beat for Resize).
     *  @param anchorBeat Reference for keep-offset semantics -- the
     *                   ORIGINAL beat position of the edge being
     *                   moved (Move = originalPos; RightEdge resize
     *                   = originalPos + originalLen; LeftEdge resize
     *                   = originalPos).  Ignored when keep-offset
     *                   is off.
     *  @param excludeIds Region ids to skip during trigger-snap so a
     *                   drag doesn't snap to its own edges.
     *  @returns Snapped beat (non-negative); raw target when snap is
     *           disabled OR division <= 0.
     *
     *  Roadmap Q3 / mirrors Zrythm SnapGrid.snap. */
    double snapBeat (double target,
                     double anchorBeat = 0.0,
                     const juce::Array<juce::Uuid>& excludeIds = {}) const
    {
        if (! owner.snapEnabled_ || owner.snapDivision_ <= 0.0)
            return juce::jmax (0.0, target);

        const double div = owner.snapDivision_;

        /* Grid candidate.  KeepOffset preserves the anchor's
         * fractional remainder within the grid division so an off-grid
         * region stays off-grid after snap (nudges in step). */
        double gridCand = 0.0;
        if (owner.snapKeepOffset_)
        {
            const double offset = anchorBeat - std::floor (anchorBeat / div) * div;
            gridCand = std::round ((target - offset) / div) * div + offset;
        }
        else
        {
            gridCand = std::round (target / div) * div;
        }
        gridCand = juce::jmax (0.0, gridCand);

        if (! owner.snapToEvents_)
            return gridCand;

        /* Trigger-snap candidate: nearest region edge within +/- div
         * of the raw target.  Exclude any dragged-region ids so a
         * drag doesn't snap to its own current edge. */
        const double radius = div;
        double bestEvent = 0.0;
        double bestEventDist = radius + 1.0;

        auto consider = [&] (double cand)
        {
            const double d = std::abs (cand - target);
            if (d < bestEventDist)
            {
                bestEventDist = d;
                bestEvent     = juce::jmax (0.0, cand);
            }
        };

        auto excluded = [&] (juce::Uuid id) -> bool
        {
            for (const auto& x : excludeIds)
                if (x == id) return true;
            return false;
        };

        for (const auto& l : owner.lanes_)
        {
            for (const auto& r : l.playlist.regions())
            {
                if (excluded (r.id)) continue;
                consider (r.positionBeats);
                consider (r.endBeats());
            }
            for (const auto& mp : l.playlist.midiRegions())
            {
                if (mp == nullptr) continue;
                if (excluded (mp->id)) continue;
                consider (mp->positionBeats);
                consider (mp->positionBeats + mp->lengthBeats);
            }
        }

        /* Pick the closer of grid vs event. */
        const double gridDist = std::abs (gridCand - target);
        return (bestEventDist < gridDist) ? bestEvent : gridCand;
    }

    void resizeForLanes()
    {
        /* Floor + per-lane padding tuned to keep the strip flush with
         * content.  Floor of 16 beats = 4 bars at 4/4 -- enough empty
         * timeline to be a drop target on a fresh session, no further.
         * kFitPaddingBeats past the last region = ~1 bar buffer to
         * make trailing drag-resize feel snappy without leaving a
         * sea of empty area past the content.  Was 32 + 8 which left
         * 8 bars of wasted scroll area on session open. */
        int maxBeats = 16;
        for (const auto& l : owner.lanes_)
        {
            double end = 0.0;
            for (const auto& r : l.playlist.regions())
                end = juce::jmax (end, r.endBeats());
            /* MIDI regions extend the body width too -- Phase 2 omitted
             * this loop so MIDI-only lanes capped the body at the 16-beat
             * floor regardless of their actual span, visually clipping
             * regions past 4 bars.  body_resizeNeeded already iterates
             * both kinds; keeping the two methods symmetric. */
            for (const auto& m : l.playlist.midiRegions())
                if (m != nullptr)
                    end = juce::jmax (end, m->positionBeats + m->lengthBeats);
            const int needed = (int) end + kFitPaddingBeats;
            if (needed > maxBeats) maxBeats = needed;
        }
        /* Live recording end overrides the static region scan -- the
         * placeholder grows past the last committed region. */
        const double recEnd = liveRecordingEnd();
        if (recEnd > 0.0)
        {
            const int recNeeded = (int) recEnd + kFitPaddingBeats;
            if (recNeeded > maxBeats) maxBeats = recNeeded;
        }
        const int w = kLabelW + maxBeats * kPxPerBeat;
        /* Sticky ruler reserves an extra kRulerH at the bottom so the
         * user can scroll the last lane fully into view without the
         * sticky overlay clipping its top.  Without this padding, the
         * lane at the very bottom never reaches a position where its
         * top edge is below the sticky ruler band. */
        const int h = kRulerH
                       + juce::jmax (kLaneH, totalLanesHeight())
                       + kRulerH;
        if (w != getWidth() || h != getHeight())
            setSize (w, h);
        else
            repaint();
    }

    void repaintLane (int idx)
    {
        if (idx < 0) { repaint(); return; }
        repaint (0, laneClipTopY (idx), getWidth(), laneFullHeight (idx));
    }

    void repaintPlayhead (int oldPxX, int newPxX)
    {
        /* 5-px clear strip per side (was 3) -- gives the regions /
         * grid AA an extra pixel of overlap so partial repaint of
         * the lane row catches stale pixels from a 1-2 px AA fringe.
         * The playhead jump artifact reported 2026-05-24 had leftover
         * green at the OLD position because a 3-px strip plus a
         * strict-intersect region clip-skip missed pixels at the
         * boundary. */
        const int W = getWidth();
        const int H = getHeight();
        if (oldPxX >= 0)
            repaint (juce::jlimit (0, W, oldPxX - 2), 0, 5, H);
        if (newPxX >= 0)
            repaint (juce::jlimit (0, W, newPxX - 2), 0, 5, H);
    }

    static constexpr int kLabelW         = 130;
    /* Vertical zoom: per-instance lane height.  Ctrl+wheel scales
     * within [kLaneHMin, kLaneHMax].  Default bumped from 64 -> 88
     * so the title strip + waveform + R/S/M cluster breathe. */
    int kLaneH = 88;
    static constexpr int kLaneHMin = 40;
    static constexpr int kLaneHMax = 240;
    /* Horizontal zoom: per-instance px-per-beat.  Sampler-style
     * mouse-wheel zoom (mouseWheelMove) scales this in [kPxPerBeatMin,
     * kPxPerBeatMax].  All paint + hit-test math uses the instance
     * value; the outer ArrangementView dereferences it via
     * body_->kPxPerBeat for playhead scrolling. */
    int kPxPerBeat = 24;
    static constexpr int kPxPerBeatMin = 4;
    static constexpr int kPxPerBeatMax = 256;
    /* Trailing empty-bar padding past the last region.  resizeForLanes
     * sizes the body to (maxBeats + kFitPaddingBeats) * kPxPerBeat so
     * the user has a snappy drag-resize target.  zoomToFit divides by
     * the same padded count so a single click lands an exact fit. */
    static constexpr int kFitPaddingBeats = 4;
    static constexpr int kRulerH         = 24;   /* bars:beats ruler row */
    static constexpr int kEdgeHandlePx   = 6;    /* width of right-edge resize handle */
    static constexpr int kDragThresholdPx = 4;   /* pixels before mouseDown -> drag */
    static constexpr double kMinRegionBeats = 0.25;

    /* AudioThumbnail config: 1024 source samples per thumbnail
     * sample -- standard for DAW-style waveform overviews at the
     * zoom levels arrangement view uses.  Cache holds up to 256
     * recently-rendered sources; enough headroom that thumbnails
     * survive across multiple lane-add cycles without reload. */
    static constexpr int kThumbnailSamplesPerPixel = 1024;
    static constexpr int kThumbnailCacheEntries    = 256;

    //==========================================================================
    //  Variable-row vertical layout
    //
    //  Lanes were laid on a UNIFORM grid: laneY = kRulerH + i*kLaneH.  That
    //  breaks once a lane can expand to host automation overlay sub-rows of
    //  its own (Layer 3), because lane heights then differ.  These helpers
    //  replace the uniform i*kLaneH arithmetic with a cumulative-offset model:
    //  every "top of lane i" / "lane index at y" / "total height" computation
    //  walks per-lane heights instead of multiplying by a constant.
    //
    //  Until overlay rendering lands (step 2), overlayStackHeight() returns 0,
    //  so laneFullHeight()==kLaneH for every lane and the geometry is
    //  bit-identical to the old uniform grid -- this step is behaviour-
    //  preserving by construction.
    //
    //  Vocabulary:
    //    clip strip  -- the kLaneH-tall top band of a lane that holds its
    //                   regions/waveform + label.  Always kLaneH; overlays
    //                   never push into it.
    //    full height -- clip strip + the lane's expanded overlay sub-rows.
    //==========================================================================

    /** Pixel height of the region/waveform clip strip for lane `laneIdx`.
     *  Per-lane: a lane with Lane.heightPx > 0 uses that custom height
     *  (clamped to the zoom range); heightPx == 0 (the default) follows the
     *  global kLaneH zoom.  Pass laneIdx < 0 (or out of range) to get the
     *  global default -- used for the "drop a new lane below" preview.
     *  Overlays stack below this strip within the lane's full height. */
    int laneClipStripH (int laneIdx) const noexcept
    {
        if (laneIdx >= 0 && laneIdx < owner.lanes_.size())
        {
            const int h = owner.lanes_.getReference (laneIdx).heightPx;
            if (h > 0) return juce::jlimit (kLaneHMin, kLaneHMax, h);
        }
        return kLaneH;
    }

    /* Per-lane vertical resize: grab a lane's clip-strip BOTTOM edge in the
     * label column and drag.  Global Ctrl/Cmd+wheel zoom still scales lanes
     * that follow it (heightPx == 0); a drag sets an explicit per-lane
     * override (Lane.heightPx). */
    static constexpr int kLaneResizeGrabPx = 5;
    struct LaneResize { bool active = false; int laneIdx = -1; int startY = 0; int startH = 0; };
    LaneResize laneResize_;

    /** Lane whose clip-strip bottom border is within grab range of
     *  body-coord (x, y), or -1.  Spans the full track width so you can
     *  grab the visible divider line anywhere along it.  The mouseDown
     *  caller gates this OFF in the Env tool, where the bottom edge is for
     *  envelope-point editing instead -- so resize never steals curve
     *  clicks.
     *
     *  The grab band sits on the CLIP-STRIP side of the divider (above it,
     *  with only 1 px of slop below) so it never bleeds into the automation
     *  chip rail that begins exactly at this border -- otherwise hovering
     *  the tabs showed the resize cursor "below where it should be". */
    int laneResizeHandleAt (int /*x*/, int y) const noexcept
    {
        for (int i = 0; i < owner.lanes_.size(); ++i)
        {
            const int bottom = laneClipTopY (i) + laneClipStripH (i);
            if (y <= bottom + 1 && y >= bottom - 2 * kLaneResizeGrabPx) return i;
        }
        return -1;
    }

    /* Vertical resize of a lane's automation OVERLAY: grab the overlay's
     * bottom divider (= the lane's full bottom) and drag.  Only lanes with
     * a visible (expanded, non-empty) overlay qualify; the band sits above
     * the divider so it never bleeds into the next lane.  Works in any tool
     * (it's a layout op, not envelope editing). */
    struct OverlayResize { bool active = false; int laneIdx = -1; int startY = 0; int startH = 0; };
    OverlayResize overlayResize_;

    int overlayResizeHandleAt (int y) const noexcept
    {
        for (int i = 0; i < owner.lanes_.size(); ++i)
        {
            if (overlayStackHeight (i) <= 0) continue;
            const int bottom = laneClipTopY (i) + laneFullHeight (i);
            if (y <= bottom + 1 && y >= bottom - 2 * kLaneResizeGrabPx) return i;
        }
        return -1;
    }

    /** Write a new overlay curve height to every binding of lane `laneIdx`
     *  (they share one superimposed area) + relayout. */
    void setLaneOverlayCurveH (int laneIdx, int h)
    {
        const int clamped = juce::jlimit (kOverlayCurveHMin, kOverlayCurveHMax, h);
        for (int bi : bindingIndicesForLane (laneIdx))
            owner.automationBindings_.getReference (bi).heightPx = clamped;
    }

    /* Automation overlay metrics.  A lane gets ONE bounded overlay area
     * (NOT a stacked row per parameter): a thin chip-rail header + a single
     * curve area in which ALL the lane's automation curves render
     * SUPERIMPOSED + colour-coded.  One binding is "active" (editable, full
     * opacity, handles + draggable segment midpoints); the rest are dimmed
     * ghosts.  The chip rail scales with param count, the lane height does
     * not -- the "no 50 stacked lanes" model. */
    static constexpr int kOverlayHeaderH   = 16;   // chip-rail strip
    static constexpr int kOverlayCurveH    = 90;   // default superimposed curve area
    static constexpr int kOverlayCurvePadY = 5;    // inset within the curve area
    static constexpr int kOverlayCurveHMin = 40;   // resize clamp
    static constexpr int kOverlayCurveHMax = 360;

    /** Per-lane height of the superimposed curve area.  Stored on the lane's
     *  bindings' heightPx (all share one area); resizing writes each.  The
     *  vertical-resize handle on the overlay's bottom edge sets it. */
    int laneOverlayCurveH (int laneIdx) const noexcept
    {
        const int bi = activeBindingIndexForLane (laneIdx);
        if (bi < 0) return kOverlayCurveH;
        return juce::jlimit (kOverlayCurveHMin, kOverlayCurveHMax,
                             owner.automationBindings_.getReference (bi).heightPx);
    }

    /** Height of lane `i`'s overlay area: 0 when collapsed or no bindings,
     *  else the chip-rail header + the (per-lane resizable) curve height.
     *  The single seam where variable-row height enters the layout model. */
    int overlayStackHeight (int laneIdx) const noexcept
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return 0;
        const auto laneId = owner.lanes_.getReference (laneIdx).id;
        if (owner.collapsedAutomationLanes_.count (laneId) > 0) return 0;
        if (owner.automationBindingCountForLane (laneId) == 0) return 0;
        return kOverlayHeaderH + laneOverlayCurveH (laneIdx);
    }

    /** Body-coord Y of the top of lane `i`'s overlay area (just below the
     *  clip strip). */
    int overlayAreaTopY (int laneIdx) const noexcept
    {
        return laneClipTopY (laneIdx) + laneClipStripH (laneIdx);
    }

    /** The shared superimposed-curve edit area (below the chip-rail header),
     *  spanning the timeline strip width. */
    Rectangle<int> overlayCurveArea (int laneIdx) const noexcept
    {
        const int top = overlayAreaTopY (laneIdx) + kOverlayHeaderH + kOverlayCurvePadY;
        return Rectangle<int> (kLabelW, top,
                               juce::jmax (1, getWidth() - kLabelW),
                               juce::jmax (1, laneOverlayCurveH (laneIdx) - kOverlayCurvePadY * 2));
    }

    /** Array indices of the bindings owned by lane `laneIdx`, in order. */
    std::vector<int> bindingIndicesForLane (int laneIdx) const
    {
        std::vector<int> out;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return out;
        const auto laneId = owner.lanes_.getReference (laneIdx).id;
        for (int i = 0; i < owner.automationBindings_.size(); ++i)
            if (owner.automationBindings_.getReference (i).ownerLaneId == laneId)
                out.push_back (i);
        return out;
    }

    /** Array index of the lane's ACTIVE (editable) binding, or -1 if none.
     *  Falls back to the first binding when the stored active id is absent
     *  or stale. */
    int activeBindingIndexForLane (int laneIdx) const
    {
        const auto idxs = bindingIndicesForLane (laneIdx);
        if (idxs.empty()) return -1;
        const auto laneId = owner.lanes_.getReference (laneIdx).id;
        auto it = owner.activeAutomationBinding_.find (laneId);
        if (it != owner.activeAutomationBinding_.end())
            for (int i : idxs)
                if (owner.automationBindings_.getReference (i).id == it->second)
                    return i;
        return idxs.front();
    }

    /* Chip-rail layout: one colour-coded chip per binding + a trailing "+"
     * add chip, laid out left-to-right in the overlay header strip. */
    struct OverlayChip { int bindingIdx = -1; Rectangle<int> rect; };
    struct OverlayChips { std::vector<OverlayChip> chips; Rectangle<int> addRect; };

    static juce::String chipLabel (const juce::String& name)
    {
        /* Show the param part after "Node: " when present, else the name;
         * truncate so chips stay compact. */
        juce::String s = name.contains (": ") ? name.fromFirstOccurrenceOf (": ", false, false)
                                              : name;
        if (s.length() > 14) s = s.substring (0, 13) + juce::String::charToString ((juce_wchar) 0x2026);
        return s.isNotEmpty() ? s : juce::String ("Param");
    }

    OverlayChips overlayChipLayout (int laneIdx) const
    {
        OverlayChips out;
        const int y = overlayAreaTopY (laneIdx) + 2;
        const int h = kOverlayHeaderH - 4;
        int x = kLabelInsetX;
        for (int bi : bindingIndicesForLane (laneIdx))
        {
            const auto& b = owner.automationBindings_.getReference (bi);
            const int w = juce::jlimit (40, 150,
                                        16 + (int) chipLabel (b.name).length() * 6);
            out.chips.push_back ({ bi, Rectangle<int> (x, y, w, h) });
            x += w + 4;
        }
        out.addRect = Rectangle<int> (x, y, 16, h);
        return out;
    }

    /** Total vertical extent of lane `i` = clip strip + expanded overlays. */
    int laneFullHeight (int laneIdx) const noexcept
    {
        return laneClipStripH (laneIdx) + overlayStackHeight (laneIdx);
    }

    /** Body-coord Y of the top of lane `i`'s clip strip = ruler height plus
     *  the cumulative full height of all preceding lanes.  Equals the old
     *  kRulerH + i*kLaneH while no lane is expanded. */
    int laneClipTopY (int laneIdx) const noexcept
    {
        int y = kRulerH;
        for (int j = 0; j < laneIdx; ++j) y += laneFullHeight (j);
        return y;
    }

    /** Summed full height of all lanes (excludes the ruler row). */
    int totalLanesHeight() const noexcept
    {
        int h = 0;
        for (int i = 0; i < owner.lanes_.size(); ++i) h += laneFullHeight (i);
        return h;
    }

    /** Lane index whose full extent contains body-coord `y`.  Returns -1 for
     *  y above the lane area (in/above the ruler) and lanes_.size() for y
     *  below the last lane, so existing `< 0 || >= size` range checks keep
     *  working unchanged.  Replaces the old (y - kRulerH) / kLaneH. */
    int rowAtY (int y) const noexcept
    {
        if (y < kRulerH) return -1;
        int top = kRulerH;
        const int n = owner.lanes_.size();
        for (int i = 0; i < n; ++i)
        {
            const int h = laneFullHeight (i);
            if (y < top + h) return i;
            top += h;
        }
        return n;   // past the last lane -- callers range-check
    }

private:
    /** Paint the bars:beats ruler row.  rulerY is the body-coord Y
     *  origin -- the caller passes viewport_.getViewPositionY() so the
     *  ruler always lands at the visible top of the body, regardless
     *  of vertical scroll.  Lane content under [rulerY, rulerY+kRulerH]
     *  is occluded by the LCD strip; the body height reserves an
     *  extra kRulerH bottom margin so the last lane can still be
     *  scrolled fully into view.
     *
     *  LCD-style faceplate matching TransportBar + MainDisplayPanel
     *  (matte-black bezel + cool-grey vertical gradient inside), bar
     *  numbers + ticks in LCD digit blue so the ruler reads as a
     *  hardware-style display flush with the transport cluster.  Bar
     *  count derives from the session's beatsPerBar (default 4 in
     *  4/4). */
    void paintRuler (Graphics& g, int rulerY)
    {
        /* LCD palette: matches the digit blue used in MainDisplayPanel
         * (content.cpp BPM / POS displays) so the ruler + transport
         * read as one continuous LCD strip.  Brightened from the
         * initial pass per visual feedback -- the body text + bar
         * markers now read clearly against the LCD gradient. */
        const juce::Colour kBezel       { 0xff'08'08'08 };
        const juce::Colour kBezelEdge   { 0xff'3a'3a'3a };
        const juce::Colour kLcdTop      { 0xff'14'19'1e };
        const juce::Colour kLcdBot      { 0xff'0c'0f'12 };
        const juce::Colour kLcdBlue     { 0xff'9e'dc'ff };  // bar marker / text -- bright
        const juce::Colour kLcdBlueMid  { 0xff'6f'b0'e0 };  // beat tick -- mid
        const juce::Colour kLcdBlueDim  { 0xff'4a'7c'a0 };  // sub-beat tick -- dim

        const Rectangle<int> rulerArea (0, rulerY, getWidth(), kRulerH);

        /* Outer matte-black bezel, then cool-grey vertical gradient
         * inset by 2 px so the bezel band is visible top + bottom.
         * Mirrors the TransportBar paint structure without rounded
         * corners -- the ruler spans the full width and clips to the
         * viewport edges, so rounded ends never show. */
        g.setColour (kBezel);
        g.fillRect (rulerArea);

        const auto inner = rulerArea.reduced (0, 2).toFloat();
        juce::ColourGradient lcdGrad (kLcdTop,
                                       inner.getX(), inner.getY(),
                                       kLcdBot,
                                       inner.getX(), inner.getBottom(),
                                       false);
        g.setGradientFill (lcdGrad);
        g.fillRect (inner);

        /* Hairline below the ruler against the lane bodies. */
        g.setColour (kBezelEdge);
        g.drawHorizontalLine (rulerY + kRulerH - 1, 0.0f, (float) getWidth());

        /* Label column header: "Bars:Beats" in LCD blue. */
        g.setColour (kLcdBlue);
        g.setFont (monoFont (10.0f, juce::Font::bold));
        g.drawText ("Bars:Beats",
                    Rectangle<int> (6, rulerY, kLabelW - 12, kRulerH),
                    juce::Justification::centredLeft, true);

        /* Determine beats per bar from the session monitor; fall back
         * to 4 (4/4) if unavailable. */
        const int beatsPerBar = owner.monitor_ != nullptr
            ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
            : 4;

        const int stripX = kLabelW;
        const int stripW = getWidth() - kLabelW;
        const int totalBeats = stripW / kPxPerBeat + 1;

        g.setFont (monoFont (10.0f, juce::Font::bold));

        /* Tick subdivision scales with zoom so more ruler resolution
         * appears as the user zooms in.  Threshold = ~8 px between
         * adjacent ticks so they don't blur into a solid line:
         *   kPxPerBeat >= 32  -> 4 ticks/beat (1/16 in 4/4 if beat=quarter)
         *   kPxPerBeat >= 16  -> 2 ticks/beat (1/2 beat)
         *   else              -> 1 tick/beat (beats only). */
        const int subdiv = (kPxPerBeat >= 32) ? 4
                        : (kPxPerBeat >= 16) ? 2
                        : 1;
        /* subStepPx is FLOATING-POINT.  Integer kPxPerBeat / subdiv
         * floors and drops the remainder, so each sub-step is short
         * by `kPxPerBeat % subdiv` pixels -- the drift accumulates
         * across bars and the ruler falls behind the regions (which
         * use full-precision r.positionBeats * kPxPerBeat).  At
         * kPxPerBeat = 33, subdiv = 4, bar 5 (beat 16) lands 16 px
         * left of the regions.  Compute in double + round on use. */
        const double subStepPx = (double) kPxPerBeat / (double) subdiv;
        const int totalSubticks = totalBeats * subdiv;

        /* Viewport-clip the tick loop.  At low kPxPerBeat (zoomed out)
         * + a wide strip this loop fires drawVerticalLine + drawText
         * thousands of times per repaint; only ticks whose X lies
         * inside the clip rect are observable.  +/-1 sub padding on
         * each end protects bar-number labels that overhang their
         * tick by a few px. */
        const auto rulerClip = g.getClipBounds();
        const int subStart = juce::jmax (0,
            (int) std::floor ((double) (rulerClip.getX()     - stripX) / subStepPx) - 1);
        const int subEnd   = juce::jmin (totalSubticks,
            (int) std::ceil  ((double) (rulerClip.getRight() - stripX) / subStepPx) + 1);

        for (int sub = subStart; sub <= subEnd; ++sub)
        {
            const int x      = stripX + (int) std::round (sub * subStepPx);
            const int beat   = sub / subdiv;
            const int phase  = sub % subdiv;
            const bool atBeat = (phase == 0);
            const bool atBar  = atBeat && (beat % beatsPerBar) == 0;

            /* Tick height + colour by class:
             *   bar  -> full height, bright LCD blue (and bar number above)
             *   beat -> half height, mid LCD blue
             *   sub  -> short stub at bottom, dim LCD blue */
            int tickTop;
            juce::Colour tickCol;
            if (atBar)        { tickTop = rulerY + 3;            tickCol = kLcdBlue;    }
            else if (atBeat)  { tickTop = rulerY + kRulerH - 12; tickCol = kLcdBlueMid; }
            else              { tickTop = rulerY + kRulerH - 6;  tickCol = kLcdBlueDim; }

            g.setColour (tickCol);
            g.drawVerticalLine (x, (float) tickTop, (float) (rulerY + kRulerH - 2));

            if (atBar)
            {
                const int barNum = beat / beatsPerBar + 1;
                g.setColour (kLcdBlue);
                g.drawText (juce::String (barNum),
                            x + 3, rulerY + 1, 28, kRulerH - 4,
                            juce::Justification::topLeft);
            }
        }

        /* Range overlay band + LOOP badge -- painted inside the ruler
         * so it tracks the sticky vertical scroll instead of staying
         * fixed at body.y=0 (which would scroll off with the lanes). */
        if (rangeActive_ && rangeEnd_ > rangeStart_)
        {
            const int rxs = kLabelW + (int) (rangeStart_ * kPxPerBeat);
            const int rxe = kLabelW + (int) (rangeEnd_   * kPxPerBeat);
            const int rw  = juce::jmax (1, rxe - rxs);
            g.setColour (loopActive_
                            ? juce::Colour::fromRGBA (90, 200, 110, 130)
                            : juce::Colour::fromRGBA (140, 170, 210, 100));
            g.fillRect (rxs, rulerY, rw, kRulerH);
            if (loopActive_)
            {
                g.setColour (juce::Colours::black);
                g.setFont (monoFont (9.0f, juce::Font::bold));
                g.drawText ("LOOP", rxs + 4, rulerY + 2,
                            juce::jmin (40, rw - 8), kRulerH - 4,
                            juce::Justification::centredLeft, false);
            }
        }

        /* Marker triangles + names on the ruler.  Painted between the
         * range band and the playhead so the playhead's vertical line
         * stays on top.  Triangle sits in the LOWER half of the ruler
         * (where the beat ticks are), 6 px wide pointing down; the
         * name (if any) prints to the right of the triangle in the
         * remaining ruler space, clipped to ~6 chars to avoid clutter. */
        paintMarkersOnRuler (g, rulerY);

        /* Playhead overlay on the ruler. */
        const int phx = stripX + (int) (owner.lastBeat_ * kPxPerBeat);
        if (phx >= stripX && phx < getWidth())
        {
            g.setColour (Colours::limegreen);
            g.drawVerticalLine (phx, (float) rulerY, (float) (rulerY + kRulerH));
        }
    }

    /** Marker triangle on the ruler row.  Called from paintRuler so the
     *  triangles stick to the visible top during vertical scroll. */
    void paintMarkersOnRuler (juce::Graphics& g, int rulerY)
    {
        const auto& markers = owner.markerTrack_.markers();
        if (markers.empty()) return;

        const auto clip = g.getClipBounds();
        const int rulerLeft  = clip.getX();
        const int rulerRight = clip.getRight();

        g.setFont (monoFont (9.0f, juce::Font::bold));
        for (const auto& m : markers)
        {
            const int mx = kLabelW + (int) (m.positionBeats * kPxPerBeat);
            if (mx < rulerLeft - 64 || mx > rulerRight + 8) continue;
            if (mx < kLabelW) continue;

            /* Triangle 6 px wide, 7 px tall, base at rulerY+2, apex
             * pointing DOWN to rulerY+9 -- sits over the bar-tick zone
             * without obscuring the LCD bar-number text. */
            juce::Path tri;
            tri.startNewSubPath ((float) (mx - 3), (float) (rulerY + 2));
            tri.lineTo          ((float) (mx + 3), (float) (rulerY + 2));
            tri.lineTo          ((float) mx,       (float) (rulerY + 9));
            tri.closeSubPath();
            g.setColour (m.colour);
            g.fillPath (tri);
            g.setColour (m.colour.darker (0.4f));
            g.strokePath (tri, juce::PathStrokeType (1.0f));

            if (m.name.isNotEmpty())
            {
                g.setColour (m.colour.brighter (0.2f));
                g.drawText (m.name,
                            mx + 5, rulerY + 1, 60, kRulerH - 4,
                            juce::Justification::topLeft, true);
            }
        }
    }

    /** Vertical hairline through the lane area at each marker's X.
     *  Called from Body::paint after the lanes + range overlay but
     *  before the ghost / marquee / sticky ruler so drag overlays read
     *  on top of decorations. */
    void paintMarkerHairlines (juce::Graphics& g)
    {
        const auto& markers = owner.markerTrack_.markers();
        if (markers.empty()) return;

        const int yTop      = kRulerH;
        const int yBot      = kRulerH + juce::jmax (kLaneH, totalLanesHeight());

        const auto clip = g.getClipBounds();
        if (clip.getBottom() <= yTop) return;

        for (const auto& m : markers)
        {
            const int mx = kLabelW + (int) (m.positionBeats * kPxPerBeat);
            if (mx < clip.getX() - 1 || mx > clip.getRight() + 1) continue;
            if (mx < kLabelW) continue;
            g.setColour (m.colour.withAlpha (0.35f));
            g.drawVerticalLine (mx, (float) yTop, (float) yBot);
        }
    }

    /** Hit-test the ruler band against the marker triangles.  Returns
     *  the marker id under (x, y) in BODY coordinates, or null if none.
     *  Used by ruler right-click to open the per-marker menu (rename /
     *  colour / delete). */
    juce::Uuid markerAtRulerPx (int x, int y) const noexcept
    {
        const int rulerY = owner.viewport_.getViewPositionY();
        if (y < rulerY || y >= rulerY + kRulerH) return {};
        const auto& markers = owner.markerTrack_.markers();
        /* Triangle hit zone: +/- 5 px around the X; full ruler height
         * so the entire triangle column is clickable. */
        for (const auto& m : markers)
        {
            const int mx = kLabelW + (int) (m.positionBeats * kPxPerBeat);
            if (mx < kLabelW) continue;
            if (std::abs (x - mx) <= 5) return m.id;
        }
        return {};
    }

    /** ARM / MUTE / SOLO button stack lives on the right side of
     *  the lane label area, three buttons tall.  Visual style mirrors
     *  SessionView column-header buttons: flat fillRect + drawRect
     *  with full-word labels and tint-derived idle colour.  Tracker
     *  lanes paint only MUTE + SOLO (ARM slot is left blank so M+S
     *  stay at the same Y positions across lane kinds).
     *
     *  Cluster X is right-anchored to the label area so shrinking
     *  kLabelW just shrinks the lane-name column to its left.  Vertical
     *  stacking trades horizontal label width for height -- buttons
     *  read big because they span the full width of the right column. */
    static constexpr int kBtnW       = 46;
    static constexpr int kBtnH       = 14;
    static constexpr int kBtnGap     = 2;
    static constexpr int kBtnRightPad = 4;
    static constexpr int kBtnTopPad  = 4;

    int laneButtonX() const noexcept
    {
        return kLabelW - kBtnW - kBtnRightPad;
    }

    Rectangle<int> armToggleRect (int laneIdx) const noexcept
    {
        const int y = laneClipTopY (laneIdx) + kBtnTopPad;
        return Rectangle<int> (laneButtonX(), y, kBtnW, kBtnH);
    }

    Rectangle<int> muteToggleRect (int laneIdx) const noexcept
    {
        const int y = laneClipTopY (laneIdx) + kBtnTopPad
                    + (kBtnH + kBtnGap);
        return Rectangle<int> (laneButtonX(), y, kBtnW, kBtnH);
    }

    Rectangle<int> soloToggleRect (int laneIdx) const noexcept
    {
        const int y = laneClipTopY (laneIdx) + kBtnTopPad
                    + (kBtnH + kBtnGap) * 2;
        return Rectangle<int> (laneButtonX(), y, kBtnW, kBtnH);
    }

    //==========================================================================
    //  Automation overlay chrome (Layer 3 step 2)
    //==========================================================================

    /* Left inset of the lane label column's text/controls -- matches the
     * `labelInset = kTintStripW + 4` paintLane uses (kTintStripW = 6). */
    static constexpr int kLabelInsetX = 10;

    /** "AUTO ▾/▸" reveal toggle, bottom-left of a lane's clip-strip label
     *  column.  Bottom-anchored so it always sits inside the clip strip
     *  regardless of vertical zoom.  Click: opens the picker when the lane
     *  has no automation yet, else shows/hides the overlay area.  Adding
     *  more params + per-param selection happen in the overlay's chip rail. */
    Rectangle<int> automationToggleRect (int laneIdx) const noexcept
    {
        const int h = 13;
        const int y = laneClipTopY (laneIdx) + laneClipStripH (laneIdx) - h - 3;
        return Rectangle<int> (kLabelInsetX, y, 56, h);
    }

    //==========================================================================
    //  Automation overlay point editing (Layer 3 step 3)
    //
    //  Curve point gestures inside an overlay's curve area:
    //    left-click on a breakpoint  -> drag it (x snapped to grid -> beat,
    //                                   y -> value, clamped between neighbours)
    //    left-click on empty curve   -> add a breakpoint there + drag it
    //    right-click on a breakpoint -> menu (delete / segment curve type)
    //
    //  All mutation goes through AutomationRegion's COW setPoints (wait-free
    //  publish; the audio thread sees the new curve next block).  Live drag
    //  republishes per move for instant feedback but only PERSISTS
    //  (saveAutomationToSession) on mouse-up -- one tree rewrite per gesture,
    //  not per delta.
    //==========================================================================

    /* In-flight automation point drag.  The point is identified by region
     * id + index; x-moves clamp between neighbours so the index stays valid
     * across the COW re-publish for the whole drag. */
    enum class AutoEditMode { MovePoint, ShapeCurve };
    struct AutoPointEdit
    {
        bool         active       = false;
        AutoEditMode mode         = AutoEditMode::MovePoint;
        int          laneIdx      = -1;
        int          bindingIdx   = -1;
        juce::Uuid   trackId;
        juce::Uuid   regionId;
        int          pointIndex   = -1;   // MovePoint
        int          segmentIndex = -1;   // ShapeCurve (from-point index)
        bool         moved        = false;
        /* Pixel grab offset (point centre - click), applied each drag
         * frame so the point tracks from where it was, not snapping under
         * the cursor.  Matches the clip-envelope EnvGesture semantics. */
        float        grabDx       = 0.0f;
        float        grabDy       = 0.0f;
    };
    AutoPointEdit autoEdit_;

    /* Pixel radius for grabbing an existing breakpoint. */
    static constexpr double kAutoHandleGrabPx = 8.0;   // match clip-envelope grab band

    double overlayXToBeat (int x) const noexcept
    {
        return juce::jmax (0.0, (double) (x - kLabelW) / (double) kPxPerBeat);
    }

    double overlayYToValue (int y, const Rectangle<int>& curveArea) const noexcept
    {
        const double t = (double) curveArea.getY();
        const double b = (double) curveArea.getBottom();
        if (b <= t) return 0.5;
        return juce::jlimit (0.0, 1.0, (b - (double) y) / (b - t));
    }

    dsp::automation::AutomationRegion*
    regionById (dsp::automation::AutomationTrack* track, const juce::Uuid& id) const
    {
        if (track == nullptr) return nullptr;
        const auto* regions = track->loadRegionSnapshot();
        if (regions == nullptr) return nullptr;
        for (auto* r : *regions)
            if (r != nullptr && r->id == id) return r;
        return nullptr;
    }

    /** Message-thread-safe "region covering this beat" lookup.  Walks the
     *  region snapshot read-only; unlike AutomationTrack::findActiveRegion
     *  it does NOT touch the audio thread's single-writer cache. */
    dsp::automation::AutomationRegion*
    regionAtBeat (dsp::automation::AutomationTrack* track, double beat) const
    {
        if (track == nullptr) return nullptr;
        const auto* regions = track->loadRegionSnapshot();
        if (regions == nullptr) return nullptr;
        for (auto* r : *regions)
            if (r != nullptr
                && beat >= r->positionBeats
                && beat <  r->positionBeats + r->lengthBeats)
                return r;
        return nullptr;
    }

    /** Index of the breakpoint within grab range of (x, y) in the given
     *  region, or -1.  Pixel mapping mirrors paintAutomationCurve. */
    int findPointNear (dsp::automation::AutomationRegion* region,
                       const Rectangle<int>& curveArea, int x, int y) const
    {
        const auto* pts = region->loadSnapshot();
        if (pts == nullptr) return -1;
        const double t = (double) curveArea.getY();
        const double b = (double) curveArea.getBottom();
        int    best  = -1;
        double bestD = 1.0e9;
        for (size_t i = 0; i < pts->size(); ++i)
        {
            const double px = (double) kLabelW
                            + (region->positionBeats + (*pts)[i].tBeats) * (double) kPxPerBeat;
            const double py = b - juce::jlimit (0.0, 1.0, (*pts)[i].valueNormalized) * (b - t);
            const double d  = juce::jmax (std::abs (px - x), std::abs (py - y));
            if (d < bestD) { bestD = d; best = (int) i; }
        }
        return (best >= 0 && bestD <= kAutoHandleGrabPx) ? best : -1;
    }

    /** Value-normalised position of segment (from,to) at fraction f --
     *  the unified value-space 2D-Bezier shared with the audio thread. */
    static double autoSegValueAt (const dsp::automation::AutomationPoint& from,
                                  const dsp::automation::AutomationPoint& to,
                                  double f)
    {
        return dsp::automation::evaluateSegment (
            f, from.valueNormalized, to.valueNormalized, from.curve);
    }

    /** Pixel position of segment i's 2D bend handle (the pin the curve
     *  passes through), in curveArea space.  Mirrors the volume
     *  envelope's pin maths (paintVolumeEnvelope) so the handle sits ON
     *  the curve and drags it in 2D. */
    juce::Point<float> autoHandlePx (const dsp::automation::AutomationRegion* region,
                                     size_t i, const Rectangle<int>& curveArea) const
    {
        const auto* pts = region->loadSnapshot();
        const auto& a = (*pts)[i];
        const auto& b = (*pts)[i + 1];
        const double cot = juce::jlimit (0.25, 0.75, a.curve.offsetT);
        const double pinBeat = a.tBeats + cot * (b.tBeats - a.tBeats);
        const double chordMid = 0.5 * (a.valueNormalized + b.valueNormalized);
        const double pinV = juce::jlimit (0.0, 1.0, chordMid + a.curve.offsetV);
        const float px = (float) (kLabelW
                       + (region->positionBeats + pinBeat) * (double) kPxPerBeat);
        const float py = (float) (curveArea.getBottom() - pinV * curveArea.getHeight());
        return { px, py };
    }

    /** Index of the SEGMENT (from-point index) whose 2D bend handle is
     *  within grab range of (x, y), or -1.  Flat segments report none. */
    int findMidpointNear (dsp::automation::AutomationRegion* region,
                          const Rectangle<int>& curveArea, int x, int y) const
    {
        const auto* pts = region->loadSnapshot();
        if (pts == nullptr) return -1;
        int    best  = -1;
        double bestD = 1.0e9;
        for (size_t i = 0; i + 1 < pts->size(); ++i)
        {
            const auto h = autoHandlePx (region, i, curveArea);
            const double d = juce::jmax (std::abs (h.x - (float) x),
                                         std::abs (h.y - (float) y));
            if (d < bestD) { bestD = d; best = (int) i; }
        }
        return (best >= 0 && bestD <= kAutoHandleGrabPx) ? best : -1;
    }

    /** Index of the SEGMENT whose drawn curve passes within grab range of
     *  (x, y) -- lets the user grab a slope anywhere along the line, like
     *  the volume envelope.  Samples the segment in pixel space. */
    int findSegmentLineNear (dsp::automation::AutomationRegion* region,
                             const Rectangle<int>& curveArea, int x, int y) const
    {
        const auto* pts = region->loadSnapshot();
        if (pts == nullptr || pts->size() < 2) return -1;
        const double b = (double) curveArea.getBottom();
        const double h = (double) curveArea.getHeight();
        int    best  = -1;
        double bestD = 1.0e9;
        for (size_t i = 0; i + 1 < pts->size(); ++i)
        {
            const auto& a = (*pts)[i];
            const auto& c = (*pts)[i + 1];
            const double x0 = (double) kLabelW + (region->positionBeats + a.tBeats) * (double) kPxPerBeat;
            const double x1 = (double) kLabelW + (region->positionBeats + c.tBeats) * (double) kPxPerBeat;
            if ((double) x < juce::jmin (x0, x1) - kAutoHandleGrabPx
                || (double) x > juce::jmax (x0, x1) + kAutoHandleGrabPx) continue;
            constexpr int kSamples = 10;
            for (int s = 0; s <= kSamples; ++s)
            {
                const double f  = (double) s / kSamples;
                const double px = x0 + f * (x1 - x0);
                const double py = b - autoSegValueAt (a, c, f) * h;
                const double d  = std::hypot (px - x, py - y);
                if (d < bestD) { bestD = d; best = (int) i; }
            }
        }
        return (best >= 0 && bestD <= kAutoHandleGrabPx) ? best : -1;
    }

    /** Persist the engine curve edit + bindings.  Called once per gesture
     *  (mouse-up / menu action), NOT per drag delta. */
    void persistAutomationEdit()
    {
        if (auto* engine = owner.activeAutomationEngine())
            if (owner.services_ != nullptr)
                if (auto sess = owner.services_->context().session())
                    automation::saveAutomationToSession (*engine, sess->data());
        owner.flushLanesToSession();
    }

    /** Dispatch a mouseDown that landed in lane `laneIdx`'s automation
     *  overlay area: chip-rail (select active / add / remove) in the header
     *  strip, or curve gestures (move point / shape segment / add point) on
     *  the ACTIVE binding in the curve area. */
    void handleAutomationOverlayMouseDown (const MouseEvent& e, int laneIdx)
    {
        using namespace element::dsp::automation;
        const auto laneId  = owner.lanes_.getReference (laneIdx).id;
        const int  areaTop = overlayAreaTopY (laneIdx);

        /* --- Chip-rail header --- */
        if (e.y < areaTop + kOverlayHeaderH)
        {
            const auto layout = overlayChipLayout (laneIdx);
            if (layout.addRect.contains (e.x, e.y))
            {
                owner.showAddAutomationMenuForLane (laneIdx, localAreaToGlobal (layout.addRect));
                return;
            }
            for (const auto& c : layout.chips)
                if (c.rect.contains (e.x, e.y))
                {
                    const auto id = owner.automationBindings_.getReference (c.bindingIdx).id;
                    if (e.mods.isPopupMenu())
                        showOverlayChipMenu (id, e.getScreenPosition());
                    else
                    {
                        owner.activeAutomationBinding_[laneId] = id;   // make active
                        repaint();
                    }
                    return;
                }
            return;
        }

        /* --- Curve area: edit the ACTIVE binding --- */
        /* Curve point editing is the Env tool's job (the chip rail above
         * works in any tool); other tools leave the curve area inert so
         * automation editing stays consistent across surfaces. */
        if (activeTool_ != Tool::Env) return;

        const int activeIdx = activeBindingIndexForLane (laneIdx);
        if (activeIdx < 0) return;
        const auto& binding = owner.automationBindings_.getReference (activeIdx);

        auto* engine = owner.activeAutomationEngine();
        if (engine == nullptr) return;
        auto* track = engine->findTrackById (binding.trackId);
        if (track == nullptr) return;

        const auto curveArea = overlayCurveArea (laneIdx);
        const double timelineBeat = overlayXToBeat (e.x);
        auto* region = regionAtBeat (track, timelineBeat);
        if (region == nullptr) return;   // click outside any region span

        const int existing = findPointNear (region, curveArea, e.x, e.y);

        /* Right-click a breakpoint: delete / curve-type menu. */
        if (e.mods.isPopupMenu())
        {
            if (existing >= 0)
                showOverlayPointMenu (binding.trackId, region->id, existing,
                                      localAreaToGlobal (Rectangle<int> (e.x, e.y, 1, 1)));
            return;
        }
        if (! e.mods.isLeftButtonDown()) return;

        auto beginEdit = [&] (AutoEditMode mode, int pointIdx, int segIdx)
        {
            autoEdit_              = AutoPointEdit {};
            autoEdit_.active       = true;
            autoEdit_.mode         = mode;
            autoEdit_.laneIdx      = laneIdx;
            autoEdit_.bindingIdx   = activeIdx;
            autoEdit_.trackId      = binding.trackId;
            autoEdit_.regionId     = region->id;
            autoEdit_.pointIndex   = pointIdx;
            autoEdit_.segmentIndex = segIdx;
        };

        /* Priority: breakpoint (move) > segment midpoint (shape) > empty (add). */
        if (existing >= 0)
        {
            beginEdit (AutoEditMode::MovePoint, existing, -1);
            if (const auto* pts = region->loadSnapshot())
                if (existing < (int) pts->size())
                {
                    const double tt = (double) curveArea.getY();
                    const double bb = (double) curveArea.getBottom();
                    const float px = (float) (kLabelW
                        + (region->positionBeats + (*pts)[(size_t) existing].tBeats)
                          * (double) kPxPerBeat);
                    const float py = (float) (bb - juce::jlimit (0.0, 1.0,
                        (*pts)[(size_t) existing].valueNormalized) * (bb - tt));
                    autoEdit_.grabDx = px - (float) e.x;
                    autoEdit_.grabDy = py - (float) e.y;
                }
            repaint();
            return;
        }
        const int seg = findSegmentLineNear (region, curveArea, e.x, e.y);
        if (seg >= 0)
        {
            beginEdit (AutoEditMode::ShapeCurve, -1, seg);
            repaint();
            return;
        }

        /* Empty curve -> add a breakpoint + drag it. */
        const double value   = overlayYToValue (e.y, curveArea);
        const double rawBeat  = snapBeat (timelineBeat);
        const double local    = juce::jlimit (0.0, region->lengthBeats,
                                              rawBeat - region->positionBeats);
        auto pts = region->loadSnapshot() ? *region->loadSnapshot()
                                          : AutomationRegion::PointList {};
        AutomationPoint np;
        np.tBeats          = local;
        np.valueNormalized = value;
        size_t insertIdx = 0;
        while (insertIdx < pts.size() && pts[insertIdx].tBeats < local)
            ++insertIdx;
        pts.insert (pts.begin() + (long) insertIdx, np);
        region->setPoints (pts);
        beginEdit (AutoEditMode::MovePoint, (int) insertIdx, -1);
        autoEdit_.moved = true;   // an add always persists on up
        repaint();
    }

    void handleAutomationOverlayDrag (const MouseEvent& e)
    {
        using namespace element::dsp::automation;
        auto* engine = owner.activeAutomationEngine();
        if (engine == nullptr) return;
        auto* track = engine->findTrackById (autoEdit_.trackId);
        auto* region = regionById (track, autoEdit_.regionId);
        if (region == nullptr) return;

        const auto curveArea = overlayCurveArea (autoEdit_.laneIdx);
        if (curveArea.isEmpty()) return;

        auto pts = region->loadSnapshot() ? *region->loadSnapshot()
                                          : AutomationRegion::PointList {};

        if (autoEdit_.mode == AutoEditMode::MovePoint)
        {
            const int i = autoEdit_.pointIndex;
            if (i < 0 || i >= (int) pts.size()) return;

            /* Snap to cursor (no grab offset) -- matches the volume-envelope
             * point move; a grab offset made edge points sit diagonally off
             * the cursor. */
            const double rawBeat = snapBeat (overlayXToBeat (e.x));
            double local = juce::jlimit (0.0, region->lengthBeats,
                                         rawBeat - region->positionBeats);
            constexpr double eps = 1.0e-4;
            if (i > 0)                    local = juce::jmax (local, pts[(size_t) i - 1].tBeats + eps);
            if (i < (int) pts.size() - 1) local = juce::jmin (local, pts[(size_t) i + 1].tBeats - eps);

            pts[(size_t) i].tBeats          = local;
            pts[(size_t) i].valueNormalized = overlayYToValue (e.y, curveArea);
        }
        else // ShapeCurve -- 2D bend handle, identical feel to the volume env
        {
            const int i = autoEdit_.segmentIndex;
            if (i < 0 || i + 1 >= (int) pts.size()) return;
            const auto& a = pts[(size_t) i];
            const auto& b = pts[(size_t) i + 1];

            /* offsetT: cursor X as a fraction within the segment (no snap --
             * curve shaping is continuous).  offsetV: cursor value minus the
             * chord midpoint, so dragging up bulges the curve up regardless
             * of segment direction. */
            const double localBeat = overlayXToBeat (e.x) - region->positionBeats;
            const double span = juce::jmax (1e-9, b.tBeats - a.tBeats);
            const double cot  = juce::jlimit (0.25, 0.75, (localBeat - a.tBeats) / span);
            const double chordMid = 0.5 * (a.valueNormalized + b.valueNormalized);
            const double pinV = overlayYToValue (e.y, curveArea);

            pts[(size_t) i].curve.offsetT = cot;
            pts[(size_t) i].curve.offsetV = pinV - chordMid;
        }

        region->setPoints (pts);
        autoEdit_.moved = true;
        repaint();
    }

    void handleAutomationOverlayUp (const MouseEvent&)
    {
        if (autoEdit_.moved)
            persistAutomationEdit();
        autoEdit_ = AutoPointEdit {};
        repaint();
    }

    /** Async context menu for a breakpoint: delete + per-segment curve
     *  algorithm.  Re-resolves track/region by id in the callback since
     *  the menu outlives the click. */
    void showOverlayPointMenu (juce::Uuid trackId, juce::Uuid regionId,
                               int pointIndex, Rectangle<int> screenAnchor)
    {
        using namespace element::dsp::automation;

        /* Curve shape is now the 2D bend handle (drag the segment line),
         * so the discrete-algorithm submenu is gone -- the only segment
         * action is "straighten" (reset both offsets). */
        juce::PopupMenu menu;
        menu.addItem (1, "Delete point");
        menu.addItem (2, "Straighten segment after this point");

        /* Synchronous showAt -- mouse-usable + positions at the cursor
         * (the showMenuAsync + withTargetScreenArea variant mis-rendered
         * detached at the window bottom under this windowing setup). */
        const int result = menu.showAt (screenAnchor);
        if (result <= 0) return;
        auto* engine = owner.activeAutomationEngine();
        if (engine == nullptr) return;
        auto* track  = engine->findTrackById (trackId);
        auto* region = regionById (track, regionId);
        if (region == nullptr) return;

        auto pts = region->loadSnapshot() ? *region->loadSnapshot()
                                          : AutomationRegion::PointList {};
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

        region->setPoints (pts);
        persistAutomationEdit();
        repaint();
    }

    /** Right-click menu for a chip: remove that automation overlay. */
    void showOverlayChipMenu (juce::Uuid bindingId, juce::Point<int> screenPos)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Remove automation");
        const int r = menu.showAt (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1));
        if (r == 1)
            owner.removeAutomationBinding (bindingId);
    }

    /** Paint a volume envelope curve over the audio region body
     *  plus an attenuation-shading polygon above the curve.
     *
     *  Visual: the area BETWEEN the top of the body and the envelope
     *  curve is dimmed by a translucent black overlay, so the
     *  shaded portion of the clip reads as "this much gain is being
     *  cut here".  Below the curve stays at full waveform intensity.
     *
     *  Empty envelope -> single horizontal line at 0 dB (the curve
     *  defaults to unity gain on first paint); the area above that
     *  line is also shaded so the surface always has something to
     *  read against. */
    void paintVolumeEnvelope (Graphics& g,
                                const Rectangle<int>& body,
                                const Region& r,
                                juce::Colour tint,
                                bool selected) const
    {
        if (body.getWidth() < 4 || body.getHeight() < 6) return;
        if (r.lengthBeats <= 1e-6) return;

        /* Map gainDb to Y over a [+6, -24] dB window.  0 dB sits at
         * 80 % up the body so the user can pull above unity gain too. */
        constexpr float kTopDb = 6.0f;
        constexpr float kBotDb = -24.0f;
        const int yPad = 3;
        auto gainToY = [&] (float dB) noexcept
        {
            const float t = juce::jlimit (0.0f, 1.0f,
                (kTopDb - dB) / (kTopDb - kBotDb));
            return (float) body.getY() + yPad
                 + t * (float) juce::jmax (1, body.getHeight() - yPad * 2);
        };
        auto beatToX = [&] (double localBeat) noexcept
        {
            const double t = juce::jlimit (0.0, 1.0, localBeat / r.lengthBeats);
            return (float) body.getX() + (float) (t * body.getWidth());
        };

        /* Selection brightens the curve (no more hard white -- matches
         * the body-fill brightening above so the region reads as
         * "selected" without three different highlight colours). */
        const juce::Colour lineCol = selected
            ? tint.brighter (0.75f)
            : tint.brighter (0.45f).withAlpha (0.85f);

        /* Build a stroke path for the envelope curve AND an aligned
         * polygon path for the attenuation shading above the curve.
         * Both share the same per-segment subdivision so the shaded
         * polygon's bottom edge sits perfectly on the line. */
        juce::Path stroke;
        juce::Path shadePoly;

        constexpr int kSubdiv = 12;

        auto pushSegment = [&] (double beatA, float dbA,
                                 double beatB, float dbB,
                                 EnvelopeCurve curve,
                                 float curveOffsetT, float curveOffsetDb,
                                 bool first)
        {
            const float xA = beatToX (beatA);
            const float yA = gainToY (dbA);
            if (first)
            {
                stroke.startNewSubPath (xA, yA);
                shadePoly.startNewSubPath ((float) body.getX(), (float) body.getY());
                shadePoly.lineTo (xA, (float) body.getY());
                shadePoly.lineTo (xA, yA);
            }
            if (curve == EnvelopeCurve::Hold)
            {
                /* Hold stays a step; offsets ignored. */
                const float xB = beatToX (beatB);
                stroke.lineTo (xB, yA);
                stroke.lineTo (xB, gainToY (dbB));
                shadePoly.lineTo (xB, yA);
                shadePoly.lineTo (xB, gainToY (dbB));
                return;
            }
            /* Default Bezier (cot=0.5, cod=0) with enum=Linear is just
             * a fast straight line; skip subdivision. */
            const bool useBezier = (curveOffsetT != 0.5f) || (curveOffsetDb != 0.0f);
            if (! useBezier && curve == EnvelopeCurve::Linear)
            {
                const float xB = beatToX (beatB);
                const float yB = gainToY (dbB);
                stroke.lineTo (xB, yB);
                shadePoly.lineTo (xB, yB);
                return;
            }
            /* Quadratic Bezier endpoints A=(0,dbA), B=(1,dbB); control
             * point (cx, cy) derived so the curve passes through
             * (curveOffsetT, chordMidDb + curveOffsetDb) at u=0.5. */
            const double cot = (double) juce::jlimit (0.25f, 0.75f, curveOffsetT);
            const double cx  = 2.0 * cot - 0.5;
            const double chordMidDb = 0.5 * ((double) dbA + (double) dbB);
            const double pinDb = chordMidDb + (double) curveOffsetDb;
            const double cy  = 2.0 * pinDb - chordMidDb;
            for (int k = 1; k <= kSubdiv; ++k)
            {
                double xLocal, yLocal;
                if (useBezier)
                {
                    const double u = (double) k / (double) kSubdiv;
                    const double oneMinusU = 1.0 - u;
                    xLocal = 2.0 * oneMinusU * u * cx + u * u;
                    yLocal = oneMinusU * oneMinusU * (double) dbA
                           + 2.0 * oneMinusU * u * cy
                           + u * u * (double) dbB;
                }
                else
                {
                    const double t = (double) k / (double) kSubdiv;
                    double shaped = t;
                    if (curve == EnvelopeCurve::Exponential)
                        shaped = t * t;
                    else if (curve == EnvelopeCurve::Smooth)
                        shaped = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t);
                    xLocal = t;
                    yLocal = (double) dbA + shaped * ((double) dbB - (double) dbA);
                }
                const double beat = beatA + xLocal * (beatB - beatA);
                const float xK = beatToX (beat);
                const float yK = gainToY ((float) yLocal);
                stroke.lineTo (xK, yK);
                shadePoly.lineTo (xK, yK);
            }
        };

        if (r.volumeEnvelope.empty())
        {
            /* No breakpoints -> render an implicit "constant at 0 dB"
             * line spanning the full region.  Shading above is still
             * meaningful (zero, since 0 dB is at the top window edge,
             * but the surface IS there to double-click). */
            const float y = gainToY (0.0f);
            stroke.startNewSubPath ((float) body.getX(),     y);
            stroke.lineTo            ((float) body.getRight(), y);
            shadePoly.startNewSubPath ((float) body.getX(),     (float) body.getY());
            shadePoly.lineTo            ((float) body.getRight(), (float) body.getY());
            shadePoly.lineTo            ((float) body.getRight(), y);
            shadePoly.lineTo            ((float) body.getX(),     y);
            shadePoly.closeSubPath();
        }
        else
        {
            for (size_t i = 0; i + 1 < r.volumeEnvelope.size(); ++i)
            {
                const auto& a = r.volumeEnvelope[i];
                const auto& b = r.volumeEnvelope[i + 1];
                pushSegment (a.beatOffset, a.gainDb,
                              b.beatOffset, b.gainDb,
                              a.curve, a.curveOffsetT, a.curveOffsetDb,
                              i == 0);
            }
            /* Close the shading polygon: traverse along the top edge
             * back to the starting X. */
            const auto& last = r.volumeEnvelope.back();
            const float xEnd = beatToX (last.beatOffset);
            shadePoly.lineTo (xEnd, (float) body.getY());
            shadePoly.closeSubPath();
        }

        /* Attenuation shading -- translucent black above the curve.
         * Sits below the envelope line so it never hides the curve
         * itself; sits above the waveform so the dimming is visible. */
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillPath (shadePoly);

        /* Envelope line on top of everything. */
        g.setColour (lineCol);
        g.strokePath (stroke, juce::PathStrokeType (selected ? 1.5f : 1.0f));

        /* Breakpoint dots -- larger + ringed when selected. */
        const float dotR = selected ? 3.0f : 2.5f;
        for (const auto& pt : r.volumeEnvelope)
        {
            const float x = beatToX (pt.beatOffset);
            const float y = gainToY (pt.gainDb);
            g.setColour (lineCol);
            g.fillEllipse (x - dotR, y - dotR, dotR * 2, dotR * 2);
            if (selected)
            {
                g.setColour (juce::Colours::black);
                g.drawEllipse (x - dotR, y - dotR, dotR * 2, dotR * 2, 1.0f);
            }
        }

        /* Per-segment midpoint handle.  Only drawn when the region is
         * selected (avoids visual clutter on every clip in the lane).
         * Position: x = geometric midpoint of beatA..beatB; y = the
         * curve's actual y at that x, i.e. it tracks the curve --
         * drag perpendicular to the segment's chord to bend.  Hold
         * segments get no handle (step function isn't bendable). */
        if (selected && r.volumeEnvelope.size() >= 2)
        {
            const float midR = 2.5f;
            for (size_t i = 0; i + 1 < r.volumeEnvelope.size(); ++i)
            {
                const auto& a = r.volumeEnvelope[i];
                const auto& b = r.volumeEnvelope[i + 1];
                if (a.curve == EnvelopeCurve::Hold) continue;

                /* Handle pin point in segment-local coords.  For
                 * default (cot=0.5, cod=0) this is the geometric
                 * midpoint of the chord; non-default positions move
                 * the bend toward the dragged pin. */
                const double cot = (double) juce::jlimit (0.25f, 0.75f, a.curveOffsetT);
                const double chordMidDb = 0.5 * ((double) a.gainDb + (double) b.gainDb);
                const double pinBeat = a.beatOffset + cot * (b.beatOffset - a.beatOffset);
                const double pinDb   = chordMidDb + (double) a.curveOffsetDb;
                const float  mx      = beatToX (pinBeat);
                const float  my      = gainToY ((float) pinDb);

                /* Two-tone fill so it's visually distinct from the
                 * solid breakpoint dots: hollow centre, coloured ring. */
                g.setColour (juce::Colours::black);
                g.fillEllipse (mx - midR, my - midR, midR * 2, midR * 2);
                g.setColour (lineCol);
                g.drawEllipse (mx - midR, my - midR, midR * 2, midR * 2, 1.25f);
            }
        }
    }

    /** Paint a piano-roll style overview of a tracker sequence into
     *  the region body.  Walks every note_on row across every track
     *  of the sequence; maps row index -> X, MIDI pitch -> inverted
     *  Y over the sequence's used pitch range.  Thread-safe: takes
     *  the TrackerNode engine lock for the duration of the iteration
     *  so vht doesn't free the sequence/track arrays underneath. */
    void paintTrackerThumb (Graphics& g,
                              const Rectangle<int>& body,
                              TrackerNode* trk,
                              int sequenceIdx,
                              juce::Colour tint) const
    {
        if (trk == nullptr || sequenceIdx < 0) return;
        if (body.getWidth() < 4 || body.getHeight() < 4) return;

        juce::ScopedLock sl (trk->engineLock());
        auto* mod = trk->modulePtr();
        if (mod == nullptr || sequenceIdx >= mod->nseq) return;
        auto* seq = mod->seq[sequenceIdx];
        if (seq == nullptr || seq->ntrk <= 0) return;

        /* First pass: discover the in-use MIDI pitch range across
         * all tracks/cols/rows.  If no notes, bail. */
        int minNote = 127, maxNote = 0;
        int totalRows = 0;
        for (int t = 0; t < seq->ntrk; ++t)
        {
            auto* trkP = seq->trk[t];
            if (trkP == nullptr) continue;
            if (trkP->nrows > totalRows) totalRows = trkP->nrows;
            for (int c = 0; c < trkP->ncols; ++c)
                for (int r = 0; r < trkP->nrows; ++r)
                {
                    const auto& row = trkP->rows[c][r];
                    if (row.type == note_on && row.note > 0)
                    {
                        if (row.note < minNote) minNote = row.note;
                        if (row.note > maxNote) maxNote = row.note;
                    }
                }
        }
        if (totalRows <= 0 || minNote > maxNote) return;

        /* Give a one-octave minimum range so a single-pitch loop
         * doesn't paint as one flat line at the top of the body. */
        const int pitchPad = juce::jmax (0, 12 - (maxNote - minNote));
        const int loNote = juce::jmax (0, minNote - pitchPad / 2);
        const int hiNote = juce::jmin (127, maxNote + (pitchPad - pitchPad / 2));
        const double pitchRange = (double) juce::jmax (1, hiNote - loNote);

        const int pad = 2;
        const int innerW = juce::jmax (1, body.getWidth() - pad * 2);
        const int innerH = juce::jmax (1, body.getHeight() - pad * 2);
        const int innerX = body.getX() + pad;
        const int innerY = body.getY() + pad;

        g.setColour (tint.withMultipliedBrightness (1.35f)
                          .withMultipliedSaturation (0.85f));

        /* Second pass: paint each note_on row as a small horizontal
         * tick.  Width derives from sequence-row-density so notes
         * read as ticks at low zoom and as bars when zoomed in. */
        for (int t = 0; t < seq->ntrk; ++t)
        {
            auto* trkP = seq->trk[t];
            if (trkP == nullptr || trkP->nrows <= 0) continue;
            const float rowStepPx = (float) innerW / (float) trkP->nrows;
            const int tickW = juce::jmax (1, (int) std::ceil (rowStepPx));

            for (int c = 0; c < trkP->ncols; ++c)
                for (int r = 0; r < trkP->nrows; ++r)
                {
                    const auto& row = trkP->rows[c][r];
                    if (row.type != note_on || row.note <= 0) continue;

                    const float xRel = (float) r / (float) trkP->nrows;
                    const float yRel = (float) (row.note - loNote) / (float) pitchRange;
                    const int dotX = innerX + (int) (xRel * innerW);
                    const int dotY = innerY + innerH - 1
                                     - (int) (yRel * (innerH - 1));
                    g.fillRect (dotX, dotY, tickW, 2);
                }
        }
    }

    void paintLane (Graphics& g, int laneIdx)
    {
        const auto& lane    = owner.lanes_.getReference (laneIdx);
        const auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        const int y = laneClipTopY (laneIdx);
        const Rectangle<int> bounds (0, y, getWidth(), laneClipStripH (laneIdx));

        /* Bitwig-style vertical tint strip on the LEFT edge of the
         * label area, low-alpha wash for the rest, monospace font
         * for the lane name. */
        constexpr int kTintStripW = 6;
        const juce::Colour kGutterColour { 0xff'14'14'14 };
        const juce::Colour kRowDividerColour { 0xff'22'22'22 };
        const juce::Colour kRowTextColour { 0xff'a8'a8'a8 };

        const bool orphan = runtime.isOrphan();
        const bool isAudio = runtime.isAudioLane();
        const juce::Colour fullTint = lane.colour;
        const juce::Colour tint = orphan
            ? fullTint.withSaturation (0.2f).withBrightness (0.4f)
            : fullTint;

        /* Label area background only -- the strip area to the right
         * inherits the Body's contentBackgroundColor fill from paint()
         * so lanes don't repaint the entire row width every tick.
         * Per-row alternating fills + strip-area tint wash were
         * removed per visual-design call 2026-05-24: only the label
         * column carries the gutter / tint identity; the strip body
         * stays uniform background with regions floating on top. */
        const Rectangle<int> labelArea (0, y, kLabelW, laneClipStripH (laneIdx));
        g.setColour (kGutterColour);
        g.fillRect (labelArea);

        /* Vertical tint strip on the LEFT edge + low-alpha wash
         * across the rest of the label area. */
        g.setColour (tint);
        g.fillRect (labelArea.getX(), labelArea.getY(),
                    kTintStripW, labelArea.getHeight() - 1);
        g.setColour (tint.withAlpha (0.12f));
        g.fillRect (labelArea.getX() + kTintStripW, labelArea.getY(),
                    labelArea.getWidth() - kTintStripW - 1,
                    labelArea.getHeight() - 1);

        /* Lane name + kind pill sit in the left column of the label
         * area, between the tint strip and the right-anchored button
         * stack.  Name on top, pill below, both left-aligned. */
        const int labelInset = kTintStripW + 4;
        const int leftColW   = laneButtonX() - labelInset - 2;
        g.setColour (orphan ? tint.withAlpha (0.55f) : tint);
        g.setFont (monoFont (
                                      12.0f, juce::Font::bold));
        const bool isMidi = (lane.kind == Lane::Kind::Midi);
        juce::String label = lane.name.isNotEmpty()
                                ? lane.name
                                : (isMidi  ? juce::String ("MIDI")
                                 : isAudio ? juce::String ("Audio")
                                           : juce::String ("Tracker"));
        if (orphan && ! isMidi)
            /* MIDI lanes carry no graph target in Phase 2 -- they
             * legitimately read as "orphan" through runtime.isOrphan().
             * Suppress the orphan suffix on MIDI lanes so the label
             * doesn't read as broken. */
            label += " (orphan)";
        g.drawText (label,
                    Rectangle<int> (labelArea.getX() + labelInset,
                                     labelArea.getY() + 3,
                                     leftColW, 14),
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (monoFont (
                                      10.0f, juce::Font::plain));
        const juce::String pill = isAudio ? "audio"
                                : runtime.isTrackerLane() ? "trk"
                                : isMidi  ? "midi"
                                : "?";
        g.drawText (pill,
                    Rectangle<int> (labelArea.getX() + labelInset,
                                     labelArea.getY() + 19,
                                     leftColW, 12),
                    juce::Justification::centredLeft);

        /* MUTE / SOLO / ARM cluster at the bottom of the label area.
         * Flat-panel style mirroring SessionView column buttons:
         * fillRect + drawRect, full-word labels, tint-derived idle
         * background so the row reads as a horizontal cousin of a
         * session column. */
        const juce::Colour btnTint = tint.withMultipliedSaturation (0.6f)
                                         .withMultipliedBrightness (0.55f);
        auto drawFlatButton = [&] (const Rectangle<int>& rect,
                                    bool active,
                                    juce::Colour activeFill,
                                    juce::Colour activeText,
                                    const juce::String& label)
        {
            g.setColour (active ? activeFill : btnTint);
            g.fillRect (rect);
            g.setColour (kRowDividerColour);
            g.drawRect (rect, 1);
            g.setColour (active ? activeText
                                : juce::Colours::white.withAlpha (0.70f));
            g.setFont (monoFont (
                                          9.0f, juce::Font::bold));
            g.drawText (label, rect, juce::Justification::centred);
        };

        drawFlatButton (muteToggleRect (laneIdx), lane.muted,
                         juce::Colour { 0xff'c0'30'30 },
                         juce::Colours::white, "MUTE");
        drawFlatButton (soloToggleRect (laneIdx), lane.soloed,
                         juce::Colour { 0xff'd5'b0'30 },
                         juce::Colours::black, "SOLO");

        /* Arm toggle (audio lanes only).  Same pill idiom, with a
         * recording-mode growth + red-200 letter overlay so the user
         * sees capture is in flight without losing the round button
         * style. */
        const bool transportRecording = owner.monitor_ != nullptr
                                      && owner.monitor_->recording.get();
        const bool capturing = isAudio && lane.armed && transportRecording;

        if (isAudio)
        {
            const auto rect = armToggleRect (laneIdx);

            const Colour fillCol = lane.armed
                ? (capturing ? Colour::fromRGB (255, 60, 60)
                             : Colour::fromRGB (220, 70, 70))
                : btnTint;
            g.setColour (fillCol);
            g.fillRect (rect);
            g.setColour (capturing ? Colour::fromRGB (255, 160, 160)
                                    : kRowDividerColour);
            g.drawRect (rect, capturing ? 2 : 1);
            g.setColour (lane.armed ? juce::Colours::white
                                    : juce::Colours::white.withAlpha (0.70f));
            g.setFont (monoFont (
                                          9.0f, juce::Font::bold));
            /* "REC" both at-rest and while capturing -- the fill
             * colour conveys state (dim/bright red when armed-but-
             * idle vs capturing) so the label can stay one word. */
            g.drawText ("REC", rect, juce::Justification::centred);
        }

        /* Automation reveal toggle, bottom-left of the label column.  Shows
         * the param count + a ▾/▸ chevron; click adds the first param (empty)
         * or shows/hides the overlay area.  Adding more + selecting the
         * active param happen in the overlay's chip rail.  Hidden on orphan
         * lanes (nothing to bind a target to). */
        if (! orphan)
        {
            const int autoCount = owner.automationBindingCountForLane (lane.id);
            const bool collapsed = owner.isLaneAutomationCollapsed (lane.id);
            const auto tRect = automationToggleRect (laneIdx);

            const juce::Colour autoTint = btnTint.withMultipliedBrightness (1.1f);
            g.setColour (autoCount > 0 ? autoTint : btnTint.withAlpha (0.6f));
            g.fillRect (tRect);
            g.setColour (kRowDividerColour);
            g.drawRect (tRect, 1);
            g.setColour (juce::Colours::white.withAlpha (autoCount > 0 ? 0.85f : 0.5f));
            g.setFont (monoFont (8.5f, juce::Font::bold));
            const juce::String chevron = (autoCount > 0 && ! collapsed) ? "v" : ">";
            g.drawText ("AUTO" + (autoCount > 0 ? " " + juce::String (autoCount) : juce::String())
                            + " " + chevron,
                        tRect.reduced (3, 0), juce::Justification::centredLeft);
        }

        /* Strip area beat grid only -- no per-lane background fill or
         * tint wash.  Body::paint's fillAll handles the strip's base
         * colour and regions paint themselves on top; what's left for
         * paintLane to draw across the strip is the bar + beat grid
         * lines.  Bar lines slightly darker than the body bg, beat
         * lines (1/4-note in 4/4) one notch dimmer below that so the
         * timeline tempo reads as a glanceable grid behind regions. */
        const Rectangle<int> stripArea (kLabelW, y, getWidth() - kLabelW, laneClipStripH (laneIdx));

        const int gridBeatsPerBar = owner.monitor_ != nullptr
            ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
            : 4;
        const juce::Colour kStripGridBar  { 0xff'2a'2a'2a };
        const juce::Colour kStripGridBeat { 0xff'1c'1c'1c };

        const auto gridClip = g.getClipBounds();
        const int laneBeatStart = juce::jmax (0,
            (gridClip.getX() - stripArea.getX()) / kPxPerBeat - 1);
        const int laneBeatEnd   = juce::jmin (stripArea.getWidth() / kPxPerBeat + 1,
            (gridClip.getRight() - stripArea.getX()) / kPxPerBeat + 1);

        /* Beat lines only show when each beat is at least ~10 px wide;
         * below that the grid would solid-fill the strip with dim
         * gray.  Bar lines always draw. */
        const bool showBeatLines = (kPxPerBeat >= 10);

        for (int beat = laneBeatStart; beat <= laneBeatEnd; ++beat)
        {
            const bool barLine = (beat % gridBeatsPerBar) == 0;
            if (! barLine && ! showBeatLines) continue;

            g.setColour (barLine ? kStripGridBar : kStripGridBeat);
            g.drawVerticalLine (stripArea.getX() + beat * kPxPerBeat,
                                (float) stripArea.getY(),
                                (float) stripArea.getBottom());
        }

        /* Regions painted in the graph-block visual language: filled
         * body in a desaturated lane-tint, then a tint outer stroke
         * + 0.6α-black inner ring with rounded corners.  Matches
         * BlockComponent::paint at ui/block.cpp:915-921 so the
         * arrangement reads as a row of mini graph blocks. */
        constexpr float kCornerSize = 2.0f;
        /* Viewport-clip the regions loop.  Each per-region branch
         * builds 2-3 Paths + sets a clip + walks the audio thumbnail
         * (or paintTrackerThumb) + paintVolumeEnvelope.  Skipping the
         * branch entirely when the region rect is outside the dirty
         * area is the biggest single timeline-zoom + scroll win at
         * high clip counts -- the dirty rect from a playhead tick is
         * a thin vertical strip, so all but a handful of regions per
         * lane drop out.
         *
         * The clip is expanded by a few pixels before the intersect
         * test so a region rect ending just outside the dirty strip
         * is still repainted -- fillRoundedRectangle +
         * drawRoundedRectangle antialias 1-2 px outside the rect's
         * geometric boundary, and skipping a region whose stroke
         * fringe leaks into the dirty strip would leave stale pixels
         * behind on partial repaints (e.g. the playhead-jump
         * artifact reported 2026-05-24). */
        const auto regionClip = g.getClipBounds().expanded (3);
        for (const auto& r : lane.playlist.regions())
        {
            const int xs = stripArea.getX() + (int) (r.positionBeats * kPxPerBeat);
            const int ws = juce::jmax (4, (int) (r.lengthBeats * kPxPerBeat));
            Rectangle<int> rect (xs, stripArea.getY() + 4,
                                 ws, stripArea.getHeight() - 8);

            if (! regionClip.intersects (rect))
                continue;

            const bool selected = isSelected (laneIdx, r.id);

            /* Tint = lane colour, desaturated + dimmed for orphan
             * lanes.  The previous active-region brightening (when
             * the playhead crossed into a region) was removed -- it
             * fired a per-region repaint on every dispatch transition
             * across every lane, which doesn't scale to 32-64 track
             * sessions and the visual delta was modest enough that
             * the playhead line itself reads as the "now playing"
             * marker. */
            juce::Colour borderTint = lane.colour;
            if (orphan)  borderTint = borderTint.withMultipliedSaturation (0.3f)
                                                 .withMultipliedBrightness (0.6f);

            /* Selection visual = slight brightening of the body +
             * title fills.  The earlier white outer stroke read as
             * over-emphasized (and read worse when 32+ tracks were
             * stacked); a small brightness bump keeps the selected
             * region visible without competing with playhead /
             * recording indicators. */
            juce::Colour fill = borderTint.withMultipliedSaturation (0.55f)
                                          .withMultipliedBrightness (0.45f);
            if (selected) fill = fill.brighter (0.30f);

            /* Region body fill (rounded). */
            g.setColour (fill);
            g.fillRoundedRectangle (rect.toFloat(), kCornerSize);

            /* Region title strip: thin band across the top of the
             * region in a SUBTLY-tinted dark grey -- not the lane's
             * full colour.  Border below keeps the full tint, so the
             * region still reads as that lane's colour at a glance,
             * but the title strip itself stays quiet so the white
             * label reads cleanly and several stacked regions don't
             * compete with hard saturation. */
            constexpr int kTitleH = 13;
            const Rectangle<int> titleRect (rect.getX(), rect.getY(),
                                             rect.getWidth(),
                                             juce::jmin (kTitleH, rect.getHeight()));
            const Rectangle<int> bodyRect (rect.getX(),
                                            rect.getY() + titleRect.getHeight(),
                                            rect.getWidth(),
                                            juce::jmax (0, rect.getHeight() - titleRect.getHeight()));
            const juce::Colour titleBase (0xff'14'14'14);
            juce::Colour titleFill = titleBase.interpolatedWith (borderTint, 0.30f);
            if (selected) titleFill = titleBase.interpolatedWith (borderTint, 0.55f);
            {
                /* Clip to the rounded outer rect so the title band's
                 * top corners follow the region's rounding while its
                 * bottom edge stays straight against the body fill. */
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (rect.toFloat(), kCornerSize);
                g.reduceClipRegion (clipPath);
                g.setColour (titleFill);
                g.fillRect (titleRect);

                /* (Envelope-driven attenuation shading is drawn by
                 * paintVolumeEnvelope below -- it shades only the area
                 * ABOVE the envelope curve, so the visible "dimmed"
                 * region maps directly to how much gain is being cut.
                 * No global gradient here.) */
            }

            /* Waveform overlay for audio regions (sequenceIdx < 0),
             * clipped to the *body* rect only so it never overlaps the
             * title band. */
            if (isAudio && r.sequenceIdx < 0 && bodyRect.getHeight() > 2)
            {
                if (auto* thumb = const_cast<Body*> (this)->getThumbnail (r.sourceId))
                {
                    const double totalSeconds = thumb->getTotalLength();
                    if (totalSeconds > 0.0)
                    {
                        const double bpm = owner.monitor_ != nullptr
                                              ? (double) owner.monitor_->tempo.get()
                                              : 120.0;
                        const double srcStartSec = r.startBeats * 60.0 / bpm;
                        const double srcLenSec   = r.lengthBeats * 60.0 / bpm;
                        const double srcEndSec   = juce::jmin (totalSeconds,
                                                                srcStartSec + srcLenSec);

                        juce::Graphics::ScopedSaveState save (g);
                        juce::Path clipPath;
                        clipPath.addRoundedRectangle (
                            rect.toFloat().reduced (2.0f, 2.0f),
                            juce::jmax (0.5f, kCornerSize - 1.0f));
                        g.reduceClipRegion (clipPath);

                        g.setColour (borderTint.withMultipliedBrightness (1.2f)
                                               .withMultipliedSaturation (0.8f));
                        thumb->drawChannels (g,
                                              bodyRect.reduced (3, 2),
                                              srcStartSec, srcEndSec, 0.85f);
                    }
                }
            }
            else if (! isAudio && r.sequenceIdx >= 0 && bodyRect.getHeight() > 2)
            {
                /* Tracker pattern thumbnail: piano-roll-style dots so
                 * each region carries a glanceable shape of its
                 * sequence, paralleling the audio waveform overlay. */
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (
                    rect.toFloat().reduced (2.0f, 2.0f),
                    juce::jmax (0.5f, kCornerSize - 1.0f));
                g.reduceClipRegion (clipPath);
                paintTrackerThumb (g, bodyRect.reduced (3, 2),
                                    runtime.trackerCache,
                                    r.sequenceIdx, borderTint);
            }

            /* Volume envelope -- audio regions only.  Empty envelope
             * still draws a flat line at the static gainDb so the
             * surface is always there to double-click on. */
            if (isAudio && r.sequenceIdx < 0 && bodyRect.getHeight() > 6)
            {
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (
                    rect.toFloat().reduced (2.0f, 2.0f),
                    juce::jmax (0.5f, kCornerSize - 1.0f));
                g.reduceClipRegion (clipPath);
                paintVolumeEnvelope (g, bodyRect.reduced (3, 2),
                                       r, borderTint,
                                       selected || activeTool_ == Tool::Env);
            }

            /* Loop boundary markers -- looped regions whose drawn
             * lengthBeats spans more than one source-length iteration
             * get a dashed-vertical-line cue at every loop boundary.
             * Audio: source length comes from the thumbnail's total
             * seconds at the current tempo.  Tracker: source length
             * comes from the sequence's row count / rpb. */
            if (r.looped && bodyRect.getHeight() > 2)
            {
                double sourceBeats = 0.0;
                if (isAudio && r.sequenceIdx < 0)
                {
                    if (auto* thumb = const_cast<Body*> (this)->getThumbnail (r.sourceId))
                    {
                        const double totalSeconds = thumb->getTotalLength();
                        const double bpm = owner.monitor_ != nullptr
                                              ? (double) owner.monitor_->tempo.get()
                                              : 120.0;
                        if (totalSeconds > 0.0 && bpm > 0.0)
                            sourceBeats = totalSeconds * bpm / 60.0;
                    }
                }
                else if (! isAudio && r.sequenceIdx >= 0
                         && runtime.trackerCache != nullptr)
                {
                    sourceBeats = runtime.trackerCache->getSequenceLengthBeats (
                        r.sequenceIdx);
                }

                if (sourceBeats > 0.0 && sourceBeats < r.lengthBeats)
                {
                    const float dashTopY = (float) bodyRect.getY();
                    const float dashBotY = (float) bodyRect.getBottom();
                    const float dashLengths[] = { 3.0f, 3.0f };
                    g.setColour (borderTint.withAlpha (0.6f));
                    for (double k = sourceBeats;
                          k < r.lengthBeats;
                          k += sourceBeats)
                    {
                        const int bx = rect.getX()
                                     + (int) std::round (k * kPxPerBeat);
                        if (bx >= rect.getRight() - 2) break;
                        if (bx <= rect.getX() + 2)     continue;
                        const juce::Line<float> seg ((float) bx, dashTopY,
                                                       (float) bx, dashBotY);
                        g.drawDashedLine (seg, dashLengths, 2, 1.2f);
                    }
                }
            }

            /* Graph-block borders: tinted outer stroke + black inner
             * ring with rounded corners.  Selection is now signalled
             * by the brighter body / title fills above; the outer
             * stroke stays at the lane tint regardless so multiple
             * stacked tracks don't compete with hard white outlines. */
            constexpr float outerWidth = 1.5f;
            g.setColour (borderTint);
            g.drawRoundedRectangle (rect.toFloat(), kCornerSize, outerWidth);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (
                rect.toFloat().reduced (outerWidth, outerWidth),
                juce::jmax (0.5f, kCornerSize - outerWidth),
                1.0f);

            /* Region label in the title strip.  Bar-position prefix
             * (e.g. "5 Drums") matches Bitwig + Cubase phrasing -- the
             * region is anchored to that bar so the bar number reads
             * as the region's primary identifier.  Pattern regions
             * fall back to their "P<idx>" tag with no bar prefix. */
            const int beatsPerBar = owner.monitor_ != nullptr
                ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
                : 4;
            const int barAt = (int) (r.positionBeats / beatsPerBar) + 1;

            juce::String labelText;
            if (r.sequenceIdx >= 0)
                labelText = "P" + String (r.sequenceIdx);
            else
                labelText = String (barAt) + " " +
                             (r.name.isNotEmpty() ? r.name : String ("Audio"));

            /* Plain (not bold) for a quieter read against the
             * subtly-tinted dark band -- the bold weight was
             * fighting the border + waveform for visual weight. */
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (monoFont (10.0f, juce::Font::plain));
            g.drawText (labelText,
                        titleRect.reduced (5, 0),
                        juce::Justification::centredLeft, true);
        }

        /* MIDI regions: parallel loop to the audio/tracker regions
         * above.  Phase 2 is paint-only -- a coloured block with a
         * note-count badge + a thin piano-roll-style note scatter in
         * the body.  No playback yet (Phase 3 piano-roll lands the
         * audio-thread emit path).  Same clip + selection + rounded-
         * corner pattern as audio so the two kinds read uniformly. */
        for (const auto& mp : lane.playlist.midiRegions())
        {
            if (mp == nullptr) continue;
            const auto& m = *mp;
            const int xs = stripArea.getX() + (int) (m.positionBeats * kPxPerBeat);
            const int ws = juce::jmax (4, (int) (m.lengthBeats * kPxPerBeat));
            Rectangle<int> rect (xs, stripArea.getY() + 4,
                                 ws, stripArea.getHeight() - 8);
            if (! regionClip.intersects (rect))
                continue;

            const bool selected = isSelected (laneIdx, m.id);
            const juce::Colour midiTint = m.colour;
            juce::Colour fill = midiTint.withMultipliedSaturation (
                                              selected ? 0.85f : 0.55f)
                                        .withMultipliedBrightness (
                                              selected ? 0.65f : 0.45f);

            g.setColour (fill);
            g.fillRoundedRectangle (rect.toFloat(), kCornerSize);
            /* Selection outline -- 2 px bright stroke inset slightly
             * so it reads on top of the body fill at every zoom. */
            if (selected)
            {
                const float outerWidth = 1.0f;
                g.setColour (midiTint.brighter (0.50f));
                g.drawRoundedRectangle (rect.toFloat().reduced (outerWidth, outerWidth),
                                         juce::jmax (0.5f, kCornerSize - outerWidth),
                                         1.6f);
            }

            constexpr int kTitleH = 13;
            const Rectangle<int> titleRect (rect.getX(), rect.getY(),
                                             rect.getWidth(),
                                             juce::jmin (kTitleH, rect.getHeight()));
            const Rectangle<int> bodyRect (rect.getX(),
                                            rect.getY() + titleRect.getHeight(),
                                            rect.getWidth(),
                                            juce::jmax (0, rect.getHeight()
                                                                - titleRect.getHeight()));
            const juce::Colour titleBase (0xff'14'14'14);
            const juce::Colour titleFill = titleBase.interpolatedWith (midiTint, 0.30f);
            {
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (rect.toFloat(), kCornerSize);
                g.reduceClipRegion (clipPath);
                g.setColour (titleFill);
                g.fillRect (titleRect);

                /* Piano-roll-style scatter inside the body rect.
                 * Cheap: one drawHorizontalLine per visible note.
                 * Notes are sorted (onBeat, pitch); pitch range is
                 * compacted to the body's vertical extent so even a
                 * narrow lane shows the shape of the pattern.
                 *
                 * For looped regions whose drawn lengthBeats spans
                 * multiple loop iterations, the scatter is painted
                 * once per iteration at successive x-offsets so the
                 * user sees the pattern repeat -- mirrors what the
                 * audio thread will emit at play time. */
                if (bodyRect.getHeight() > 4)
                {
                    if (const auto* snap = m.loadSnapshot())
                    {
                        if (! snap->empty())
                        {
                            /* Compute pitch range for this region's
                             * snapshot.  Single-note pattern degenerates
                             * to a centred row. */
                            int pMin = (*snap)[0].pitch;
                            int pMax = (*snap)[0].pitch;
                            for (const auto& nn : *snap)
                            {
                                pMin = juce::jmin (pMin, nn.pitch);
                                pMax = juce::jmax (pMax, nn.pitch);
                            }
                            const int pSpan = juce::jmax (1, pMax - pMin);
                            const int innerH = bodyRect.getHeight() - 2;
                            const float pxPerPitch = (float) innerH / (float) pSpan;

                            /* Loop iterations: 1 unless looped + loop
                             * period actually fits more than once. */
                            const double loopPeriod = (m.looped
                                                          && m.loopLengthBeats > 0.0)
                                                          ? m.loopLengthBeats
                                                          : m.lengthBeats;
                            const int iterations = (m.looped
                                                       && loopPeriod > 0.0
                                                       && loopPeriod < m.lengthBeats)
                                ? (int) std::ceil (m.lengthBeats / loopPeriod)
                                : 1;

                            g.setColour (midiTint.withMultipliedBrightness (1.4f));
                            for (int iter = 0; iter < iterations; ++iter)
                            {
                                const double iterOffsetBeats = (double) iter * loopPeriod;
                                for (const auto& nn : *snap)
                                {
                                    /* Skip notes that fall past this
                                     * iteration's loop window -- the
                                     * audio thread won't emit them, the
                                     * paint shouldn't show them either. */
                                    if (iterations > 1 && nn.onBeat >= loopPeriod)
                                        continue;

                                    /* x relative to region start +
                                     * iteration offset; clip to region
                                     * width so notes past the end are
                                     * visually trimmed. */
                                    const int nx = rect.getX()
                                                 + (int) ((iterOffsetBeats
                                                           + nn.onBeat)
                                                          * kPxPerBeat);
                                    const int nw = juce::jmax (1,
                                        (int) (nn.lengthBeats * kPxPerBeat));
                                    const int nxEnd = juce::jmin (nx + nw,
                                                                  rect.getRight() - 2);
                                    if (nxEnd <= rect.getX() + 1) continue;
                                    if (nx >= rect.getRight() - 2)  continue;

                                    /* Higher pitch = higher on screen.
                                     * +1 to keep a 1 px margin off the
                                     * title strip; -1 ditto on the
                                     * bottom. */
                                    const int normPitch = pMax - nn.pitch;
                                    const int ny = bodyRect.getY() + 1
                                                 + (int) (normPitch * pxPerPitch);
                                    g.drawHorizontalLine (ny,
                                                           (float) juce::jmax (nx,
                                                                               rect.getX() + 2),
                                                           (float) nxEnd);
                                }
                            }
                        }
                    }
                }
            }

            /* Loop boundary markers -- dashed vertical line at every
             * loop-period repeat boundary inside the region.  Reads as
             * "this section repeats from here" without paying the
             * cost of repainting the full note scatter.  Same tint as
             * the outer border so the visual idiom stays unified. */
            if (m.looped)
            {
                const double loopPeriod = (m.loopLengthBeats > 0.0)
                                              ? m.loopLengthBeats
                                              : m.lengthBeats;
                if (loopPeriod > 0.0 && loopPeriod < m.lengthBeats)
                {
                    const float dashTopY    = (float) bodyRect.getY();
                    const float dashBotY    = (float) bodyRect.getBottom();
                    const float dashLengths[] = { 3.0f, 3.0f };
                    g.setColour (midiTint.withAlpha (0.6f));
                    for (double k = loopPeriod; k < m.lengthBeats; k += loopPeriod)
                    {
                        const int bx = rect.getX() + (int) std::round (k * kPxPerBeat);
                        if (bx >= rect.getRight() - 2) break;
                        if (bx <= rect.getX() + 2)     continue;
                        const juce::Line<float> seg ((float) bx, dashTopY,
                                                       (float) bx, dashBotY);
                        g.drawDashedLine (seg, dashLengths, 2, 1.2f);
                    }
                }
            }

            /* Outer border + inner ring -- same visual idiom as audio
             * regions so MIDI sits naturally beside them in the strip. */
            constexpr float outerWidth = 1.5f;
            g.setColour (midiTint);
            g.drawRoundedRectangle (rect.toFloat(), kCornerSize, outerWidth);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (
                rect.toFloat().reduced (outerWidth, outerWidth),
                juce::jmax (0.5f, kCornerSize - outerWidth),
                1.0f);

            /* Title: bar prefix + name + note-count badge. */
            const int beatsPerBar = owner.monitor_ != nullptr
                ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
                : 4;
            const int barAt = (int) (m.positionBeats / beatsPerBar) + 1;
            const juce::String labelText = juce::String (barAt) + " "
                + (m.name.isNotEmpty() ? m.name : juce::String ("MIDI"));
            const juce::String badgeText = juce::String ((int) m.noteCount()) + " n";

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (monoFont (10.0f, juce::Font::plain));
            /* Reserve right end of title strip for the badge; left
             * label gets the remaining width.  Fixed-width badge
             * estimate (6 chars at 10pt mono ~7 px each + padding)
             * avoids the deprecated Font::getStringWidth path; the
             * jmin cap below keeps the badge from eating the label
             * on a very narrow region. */
            const int badgeW = juce::jmin (titleRect.getWidth() / 2,
                                            6 * 7 + 8);
            const Rectangle<int> badgeRect (titleRect.getRight() - badgeW,
                                             titleRect.getY(),
                                             badgeW,
                                             titleRect.getHeight());
            const Rectangle<int> labelRect (titleRect.getX(),
                                             titleRect.getY(),
                                             titleRect.getWidth() - badgeW,
                                             titleRect.getHeight());
            g.drawText (labelText, labelRect.reduced (5, 0),
                        juce::Justification::centredLeft, true);
            g.setColour (juce::Colours::white.withAlpha (0.65f));
            g.drawText (badgeText, badgeRect.reduced (4, 0),
                        juce::Justification::centredRight, true);
        }

        /* Live-recording placeholder: while transport is capturing
         * + this lane is armed, paint a red translucent rect from
         * recordStartBeat to the current playhead, with a
         * "Recording..." label.  On stop, the AudioClipNode commit
         * handler appends a real Region to the playlist and the
         * placeholder vanishes (its source data lives in the
         * playlist now). */
        if (capturing)
        {
            const double startBeat = owner.recordStartBeat_;
            const double endBeat   = owner.lastBeat_;
            if (endBeat > startBeat)
            {
                const int rx = stripArea.getX() + (int) (startBeat * kPxPerBeat);
                const int rw = juce::jmax (4, (int) ((endBeat - startBeat) * kPxPerBeat));
                const Rectangle<int> recRect (rx, stripArea.getY() + 4,
                                              rw, stripArea.getHeight() - 8);

                g.setColour (Colour::fromRGB (220, 60, 60).withAlpha (0.55f));
                g.fillRect (recRect);
                g.setColour (Colour::fromRGB (255, 120, 120));
                g.drawRect (recRect, 1);
                g.setColour (Colours::white);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText ("Recording...", recRect.reduced (6, 0),
                            juce::Justification::centredLeft, true);
            }
        }

        /* Track separator: a visible 1-px divider across the full width at
         * the lane's bottom border, so track boundaries are obvious AND the
         * resize handle (drag this line -> up/down cursor) is discoverable,
         * exactly like every DAW's track list.  A brighter 1-px highlight
         * sits just above it for a subtle bevel. */
        {
            const int sepY = y + laneClipStripH (laneIdx) - 1;
            g.setColour (juce::Colour (0xff'40'40'40));
            g.drawHorizontalLine (sepY, 0.0f, (float) getWidth());
        }

        /* Automation overlay sub-rows nest directly below the clip strip
         * within this lane's full extent (laneFullHeight). */
        paintLaneAutomationOverlays (g, laneIdx);

        /* Playhead overlay -- drawn LAST so it runs across BOTH the clip
         * strip AND the automation overlay sub-rows (the overlay backdrop
         * paints over anything drawn earlier, so a strip-only playhead
         * vanished inside the automation band).  Spans the lane's full
         * height. */
        const double phb = owner.lastBeat_;
        const int phx = stripArea.getX() + (int) (phb * kPxPerBeat);
        if (phx >= stripArea.getX() && phx < stripArea.getRight())
        {
            g.setColour (Colours::limegreen.withAlpha (0.9f));
            g.drawVerticalLine (phx,
                                (float) laneClipTopY (laneIdx),
                                (float) (laneClipTopY (laneIdx) + laneFullHeight (laneIdx)));
        }
    }

    /** Paint the expanded automation overlay rows for one lane: a label
     *  band (colour swatch + param name + remove [x]) on the left, the
     *  curve over a grid on the right.  Read-only here -- point gestures
     *  land in step 3.  No-op when the lane's stack is collapsed/empty
     *  (forEachOverlayRow handles that). */
    void paintLaneAutomationOverlays (Graphics& g, int laneIdx)
    {
        const auto laneId = owner.lanes_.getReference (laneIdx).id;
        if (owner.collapsedAutomationLanes_.count (laneId) > 0) return;
        if (owner.automationBindingCountForLane (laneId) == 0) return;

        const int areaTop   = overlayAreaTopY (laneIdx);
        const int curveH    = laneOverlayCurveH (laneIdx);
        const Rectangle<int> areaRect (0, areaTop, getWidth(),
                                       kOverlayHeaderH + curveH);
        if (! g.getClipBounds().intersects (areaRect)) return;

        const auto curveArea = overlayCurveArea (laneIdx);
        const int  activeIdx = activeBindingIndexForLane (laneIdx);
        const auto idxs      = bindingIndicesForLane (laneIdx);

        /* Backdrop (curve area) + header strip + centre reference line. */
        g.setColour (juce::Colour (0xff'10'10'12));
        g.fillRect (Rectangle<int> (kLabelW, areaTop + kOverlayHeaderH,
                                    getWidth() - kLabelW, curveH));
        g.setColour (juce::Colour (0xff'17'17'1b));
        g.fillRect (Rectangle<int> (0, areaTop, getWidth(), kOverlayHeaderH));
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine (curveArea.getCentreY(),
                              (float) curveArea.getX(), (float) curveArea.getRight());

        /* Superimposed curves: ghosts first, the active one on top. */
        for (int bi : idxs)
            if (bi != activeIdx)
                paintAutomationCurve (g, owner.automationBindings_.getReference (bi),
                                      curveArea, false);
        if (activeIdx >= 0)
            paintAutomationCurve (g, owner.automationBindings_.getReference (activeIdx),
                                  curveArea, true);

        /* Chip rail: one colour chip per param (active outlined) + "+". */
        const auto layout = overlayChipLayout (laneIdx);
        g.setFont (monoFont (9.0f, juce::Font::bold));
        for (const auto& c : layout.chips)
        {
            const auto& b = owner.automationBindings_.getReference (c.bindingIdx);
            const bool active = (c.bindingIdx == activeIdx);

            /* Active = solid binding colour with a high-contrast (auto
             * black/white) label so it's always readable; inactive = a
             * dim wash of the same colour with the label in that colour
             * so each tab still reads as its curve's hue. */
            const juce::Colour fill = active
                ? b.colour
                : b.colour.withMultipliedBrightness (0.32f).withAlpha (0.85f);
            g.setColour (fill);
            g.fillRect (c.rect);
            /* No white active outline (distracting) -- a subtle same-hue
             * border gives the tab definition; the solid fill already marks
             * it active. */
            g.setColour (active ? b.colour.brighter (0.25f)
                                : b.colour.withAlpha (0.45f));
            g.drawRect (c.rect, 1);

            g.setColour (active ? b.colour.contrasting (0.92f)
                                : b.colour.brighter (0.3f).withAlpha (0.95f));
            g.drawText (chipLabel (b.name), c.rect.reduced (4, 0),
                        juce::Justification::centredLeft, true);
        }
        g.setColour (juce::Colour (0xff'2c'2c'32));
        g.fillRect (layout.addRect);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (monoFont (11.0f, juce::Font::bold));
        g.drawText ("+", layout.addRect, juce::Justification::centred);

        /* Bottom divider for the overlay area. */
        g.setColour (juce::Colour (0xff'2a'2a'2e));
        g.drawHorizontalLine (areaTop + kOverlayHeaderH + curveH - 1,
                              0.0f, (float) getWidth());
    }

    /** Stroke ONE binding's automation curve inside `curveArea`, optionally
     *  as the active (bright, with breakpoint + midpoint handles) or a
     *  dimmed ghost.  Holds flat at the first/last point to the region
     *  edges (lead-in/out).  Mirrors AutomationRegion::sampleAtBeats so the
     *  drawn curve matches playback.  Value 1 = top, 0 = bottom. */
    void paintAutomationCurve (Graphics& g, const AutomationBinding& b,
                               const Rectangle<int>& curveArea, bool active)
    {
        auto* engine = owner.activeAutomationEngine();
        if (engine == nullptr) return;
        auto* track = engine->findTrackById (b.trackId);
        if (track == nullptr) return;
        const auto* regions = track->loadRegionSnapshot();
        if (regions == nullptr) return;

        const int padTop = curveArea.getY();
        const int padBot = curveArea.getBottom();
        auto valueToY = [padTop, padBot] (double v) noexcept
        {
            const double cl = juce::jlimit (0.0, 1.0, v);
            return (float) (padBot - cl * (double) (padBot - padTop));
        };
        auto beatToX = [this] (double beat) noexcept
        {
            return (float) (kLabelW + (int) (beat * kPxPerBeat));
        };

        const float strokeW = active ? 1.6f : 1.2f;
        const juce::Colour curveCol = active ? b.colour.withMultipliedBrightness (1.25f)
                                             : b.colour.withAlpha (0.35f);

        for (auto* region : *regions)
        {
            if (region == nullptr) continue;
            const auto* pts = region->loadSnapshot();
            if (pts == nullptr || pts->empty()) continue;

            const double rPos    = region->positionBeats;
            const float  leftX   = beatToX (rPos);
            const float  rightX  = beatToX (rPos + region->lengthBeats);

            g.setColour (curveCol);
            if (pts->size() == 1)
            {
                const float yv = valueToY ((*pts)[0].valueNormalized);
                g.drawLine (leftX, yv, rightX, yv, strokeW);
            }
            else
            {
                juce::Path path;
                path.startNewSubPath (leftX, valueToY (pts->front().valueNormalized)); // lead-in
                for (size_t i = 0; i + 1 < pts->size(); ++i)
                {
                    const auto& from = (*pts)[i];
                    const auto& to   = (*pts)[i + 1];
                    const float x0 = beatToX (rPos + from.tBeats);
                    const float x1 = beatToX (rPos + to.tBeats);
                    const int   steps = juce::jlimit (1, 96, (int) std::abs (x1 - x0) / 3);
                    for (int s = 0; s <= steps; ++s)
                    {
                        const double f = (double) s / (double) steps;
                        path.lineTo ((float) (x0 + f * (x1 - x0)),
                                     valueToY (autoSegValueAt (from, to, f)));
                    }
                }
                path.lineTo (rightX, valueToY (pts->back().valueNormalized)); // lead-out
                g.strokePath (path, juce::PathStrokeType (strokeW));
            }

            if (! active) continue;   // ghosts: no handles

            /* Segment 2D bend handles (drag anywhere on the line) -- hollow
             * rings sitting ON the curve at the bend pin. */
            for (size_t i = 0; i + 1 < pts->size(); ++i)
            {
                const auto h = autoHandlePx (region, i, curveArea);
                g.setColour (curveCol.withAlpha (0.5f));
                g.drawEllipse (h.x - 3.0f, h.y - 3.0f, 6.0f, 6.0f, 1.2f);
            }
            /* Breakpoint handles -- filled squares on top. */
            g.setColour (b.colour.brighter (0.5f));
            for (const auto& p : *pts)
                g.fillRect (Rectangle<float> (beatToX (rPos + p.tBeats) - 2.5f,
                                              valueToY (p.valueNormalized) - 2.5f, 5.0f, 5.0f));
        }
    }

public:
    /** Returns true if the body's current size doesn't fit the
     *  current lane content extent (e.g. resize pushed a region past
     *  the existing total width, or live recording grew past it).
     *  resizeForLanes() recomputes.  Public so ArrangementView's
     *  timerCallback can gate its growth check on this without
     *  forcing a full body repaint on every 30 Hz tick. */
    bool body_resizeNeeded() const noexcept
    {
        double maxEnd = 0.0;
        for (const auto& l : owner.lanes_)
        {
            for (const auto& r : l.playlist.regions())
                maxEnd = juce::jmax (maxEnd, r.endBeats());
            for (const auto& m : l.playlist.midiRegions())
                if (m != nullptr)
                    maxEnd = juce::jmax (maxEnd, m->positionBeats + m->lengthBeats);
        }
        maxEnd = juce::jmax (maxEnd, liveRecordingEnd());
        const int needW = kLabelW + ((int) maxEnd + kFitPaddingBeats) * kPxPerBeat;
        const int needH = kRulerH
                           + juce::jmax (kLaneH, totalLanesHeight())
                           + kRulerH;
        return needW != getWidth() || needH != getHeight();
    }

private:
    /** Lookup or lazily-create the juce::AudioThumbnail for the given
     *  AudioFileSource uuid.  Returns nullptr if the source isn't
     *  registered (region might point at a missing source, e.g.
     *  imported from a session whose source paths have moved). */
    juce::AudioThumbnail* getThumbnail (juce::Uuid sourceId)
    {
        if (sourceId.isNull()) return nullptr;

        UuidHashKey key { sourceId };
        auto it = thumbnails_.find (key);
        if (it != thumbnails_.end())
            return it->second.get();

        auto src = SourceRegistry::get().findAudioFile (sourceId);
        if (src == nullptr)
            return nullptr;

        auto thumb = std::make_unique<juce::AudioThumbnail> (
            kThumbnailSamplesPerPixel, formatManager_, thumbnailCache_);
        /* setSource owns the InputSource; FileInputSource opens via
         * juce::File which lives on the Linux path under our winelib
         * build (_WIN32 undefined by winelib_compat.h).  Background-
         * thread reader, not the audio thread. */
        thumb->setSource (new juce::FileInputSource (src->file()));
        thumb->addChangeListener (this);

        auto* raw = thumb.get();
        thumbnails_.emplace (key, std::move (thumb));
        return raw;
    }

    ArrangementView& owner;

    /* Drop hover state for visual feedback during external file drag. */
    bool dropHover_         = false;
    int  dropHoverLaneIdx_  = -1;

    /* Region selection state.  selectedLane_ < 0 means no primary
     * selection.  selectedLane_ + selectedRegion_ track the PRIMARY
     * (last-clicked) region; selected_ holds the full multi-selection
     * list including the primary.  Paint sites and the delete /
     * loop / copy / cut / duplicate paths read selected_ (via
     * isSelected) so a no-modifier click still behaves identically
     * to the old single-selection model. */
    int        selectedLane_   = -1;
    juce::Uuid selectedRegion_;
    juce::Array<std::pair<int, juce::Uuid>> selected_;

    /* Tracks the lane most recently interacted with even when the
     * selection is empty.  Ctrl+V uses this as the paste target if
     * the user has just cut every selected region (clearing the
     * primary).  Updated on every mouseDown that lands inside a
     * known lane strip. */
    int lastInteractedLane_ = -1;

    /* Clipboard.  Anchor-relative entries cloned from the selection
     * via Ctrl+C / Ctrl+X / right-click Copy / Cut.  Survives across
     * mutation cycles, cleared only by next Copy/Cut.  std::vector
     * (not juce::Array) because MidiNoteRegion is non-copyable --
     * unique_ptr inside the entry handles the move-only payload. */
    struct ClipboardEntry
    {
        Lane::Kind originLaneKind = Lane::Kind::Audio;
        int        laneOffset     = 0;     /* relative to anchor lane */
        double     beatOffset     = 0.0;   /* relative to anchor beat */
        bool       isAudio        = true;
        Region     audioRegion {};                       /* valid if isAudio */
        std::unique_ptr<MidiNoteRegion> midiRegion;      /* valid if ! isAudio */
    };
    std::vector<ClipboardEntry> clipboard_;

    /** Live drag of an envelope breakpoint.  Set in mouseDown when
     *  the cursor lands on a breakpoint dot; updated in mouseDrag;
     *  cleared in mouseUp.  Decoupled from the region-Gesture so a
     *  Select-tool click on a dot doesn't also start a move/resize. */
    struct EnvGesture {
        int        laneIdx    = -1;
        juce::Uuid regionId;
        juce::Uuid pointId;
        bool       dragActive = false;
        /* Pixel offset from the click point to the dot's painted centre,
         * captured at grab time.  Applied every drag frame so the point
         * tracks the cursor from where it WAS rather than snapping its
         * centre under the cursor (Ardour/Zrythm grab-offset semantics). */
        float      grabDx     = 0.0f;
        float      grabDy     = 0.0f;
    };
    EnvGesture envGesture_;

    /** Live drag of a per-segment midpoint handle.  Bends the segment
     *  between two breakpoints into a power curve.  Distinct gesture
     *  type so a midpoint drag doesn't also move the surrounding
     *  breakpoints.  segIndex is the index of the LEFT breakpoint of
     *  the segment (i.e. the breakpoint whose curve+curveAmount
     *  fields drive the segment's shape). */
    struct EnvMidGesture {
        int        laneIdx        = -1;
        juce::Uuid regionId;
        int        segIndex       = -1;
        bool       dragActive     = false;
        /* Free 2D drag: convert the current mouse position to (cot, cod)
         * each frame by inverting the same mapping the paint maths uses.
         * No baseline / delta state needed -- absolute placement. */
    };
    EnvMidGesture envMidGesture_;

    /* Active tool + loop-range state.  See the comment block on the
     * Tool enum declaration for the per-tool gesture mapping. */
    Tool   activeTool_     = Tool::Select;
    bool   rangeActive_    = false;
    bool   rangeDragging_  = false;

    /* Lane-header drag state.  Mouse-down in the label area (outside
     * M/S/R buttons) stages a potential vertical reorder; promoted
     * past the drag threshold; release commits via owner.moveLane
     * one swap at a time.  Drop indicator paints as a horizontal
     * line on the target lane boundary. */
    int    laneDragSource_   = -1;
    bool   laneDragActive_   = false;
    int    laneDragHoverIdx_ = -1;
    juce::Point<int> laneDragStart_ {};
    double rangeStart_     = 0.0;
    double rangeEnd_       = 0.0;
    double rangeAnchor_    = 0.0;
    bool   loopActive_     = false;

    /* Thumbnail infrastructure.  AudioFormatManager + AudioThumbnailCache
     * are per-Body; thumbnails_ is keyed by AudioFileSource uuid so
     * regions referencing the same source share the same waveform
     * peak data (cheap; one decode per source not per region). */
    struct UuidHashKey {
        juce::Uuid uuid;
        bool operator== (const UuidHashKey& o) const noexcept { return uuid == o.uuid; }
    };
    struct UuidHashKeyHash {
        std::size_t operator() (const UuidHashKey& k) const noexcept
        {
            return std::hash<std::string>{} (k.uuid.toString().toStdString());
        }
    };

    juce::AudioFormatManager   formatManager_;
    juce::AudioThumbnailCache  thumbnailCache_;
    std::unordered_map<UuidHashKey, std::unique_ptr<juce::AudioThumbnail>, UuidHashKeyHash> thumbnails_;
};

/* ===================================================================== */

/* Sticky-ruler scroll follow.  PersistingViewport's visibleAreaChanged
 * lives here (out of the inline hpp body) so it can repaint the body's
 * old + new ruler strips by referencing Body::kRulerH directly.  Each
 * vertical-scroll event invalidates two narrow body regions: the body-
 * coord row where the LCD strip used to sit (so the lane underneath
 * paints back through) and the new row (so the LCD strip paints over
 * the freshly-revealed lane content).  Horizontal-only scrolls bypass
 * this entirely. */
void ArrangementView::PersistingViewport::visibleAreaChanged (const juce::Rectangle<int>& newVisibleArea)
{
    if (owner.body_ == nullptr) return;
    owner.writeViewStateToSession();
    const int newY = newVisibleArea.getY();
    if (newY != lastScrollY_)
    {
        const int w = owner.body_->getWidth();
        const int h = ArrangementView::Body::kRulerH;
        owner.body_->repaint (0, lastScrollY_, w, h);
        owner.body_->repaint (0, newY,         w, h);
        lastScrollY_ = newY;
    }
}

ArrangementView::ArrangementView()
{
    setName (EL_VIEW_ARRANGEMENT);

    addAndMakeVisible (rescanBtn_);
    addAndMakeVisible (addAudioBtn_);
    addAndMakeVisible (addMidiBtn_);
    addAndMakeVisible (toolSelectBtn_);
    addAndMakeVisible (toolRangeBtn_);
    addAndMakeVisible (toolSplitBtn_);
    addAndMakeVisible (toolTrimBtn_);
    addAndMakeVisible (toolAuditionBtn_);
    addAndMakeVisible (toolEnvBtn_);
    addAndMakeVisible (loopBtn_);
    addAndMakeVisible (snapBtn_);
    addAndMakeVisible (snapBox_);
    addAndMakeVisible (snapEventsBtn_);
    addAndMakeVisible (snapOffsetBtn_);
    addAndMakeVisible (zoomOutBtn_);
    addAndMakeVisible (zoomInBtn_);
    addAndMakeVisible (zoomFitBtn_);
    addAndMakeVisible (viewport_);

    /* Horizontal zoom controls in an LCD-style cluster on the
     * toolbar:  [ -  +  Fit ].  -/+ step zoom in/out by 1.20 around
     * the viewport centre; Fit chooses kPxPerBeat so the longest
     * region fits the visible width (overview).  Shift +/- key
     * shortcuts route to the same Body::zoomBy entry point. */
    zoomOutBtn_.onClick = [this]() { if (body_) body_->zoomBy (1.0 / 1.20); };
    zoomInBtn_ .onClick = [this]() { if (body_) body_->zoomBy (1.20); };
    zoomFitBtn_.onClick = [this]() { if (body_) body_->zoomToFit(); };

    /* Snap controls.  snapBtn toggles snap on/off; snapBox picks
     * the snap unit in beats.  Visual highlight = on. */
    snapBtn_.setClickingTogglesState (true);
    snapBtn_.setToggleState (snapEnabled_, juce::dontSendNotification);
    snapBtn_.onClick = [this]()
    {
        snapEnabled_ = snapBtn_.getToggleState();
        snapBox_.setEnabled (snapEnabled_);
    };

    /* Snap divisions.  Triplet entries use exact fractions (1/3, 2/3,
     * 1/6 of a beat) -- piano-roll uses the same lookup so the two
     * pickers stay consistent.  Dotted entries are 1.5x their parent
     * note value.  Ids are stable across versions so persisted picks
     * survive future additions. */
    snapBox_.addItem ("1/32",   7);
    snapBox_.addItem ("1/16",   1);
    snapBox_.addItem ("1/16d",  8);
    snapBox_.addItem ("1/16T",  9);
    snapBox_.addItem ("1/8",    2);
    snapBox_.addItem ("1/8d",  10);
    snapBox_.addItem ("1/8T",  11);
    snapBox_.addItem ("1/4",    3);
    snapBox_.addItem ("1/4T",  12);
    snapBox_.addItem ("Beat",   4);
    snapBox_.addItem ("1/2",    5);
    snapBox_.addItem ("Bar",    6);
    snapBox_.setSelectedId (4, juce::dontSendNotification);   // Beat default
    snapBox_.onChange = [this]()
    {
        switch (snapBox_.getSelectedId())
        {
            case 7:  snapDivision_ = 0.125;            break;   // 1/32
            case 1:  snapDivision_ = 0.25;             break;   // 1/16
            case 8:  snapDivision_ = 0.375;            break;   // 1/16 dotted
            case 9:  snapDivision_ = 1.0 / 6.0;        break;   // 1/16 triplet
            case 2:  snapDivision_ = 0.5;              break;   // 1/8
            case 10: snapDivision_ = 0.75;             break;   // 1/8 dotted
            case 11: snapDivision_ = 1.0 / 3.0;        break;   // 1/8 triplet
            case 3:  snapDivision_ = 1.0;              break;   // 1/4
            case 12: snapDivision_ = 2.0 / 3.0;        break;   // 1/4 triplet
            case 4:  snapDivision_ = 1.0;              break;   // Beat (alias)
            case 5:  snapDivision_ = 2.0;              break;   // 1/2
            case 6:  snapDivision_ = 4.0;              break;   // Bar (4/4)
            default: break;
        }
    };

    snapEventsBtn_.setClickingTogglesState (true);
    snapEventsBtn_.setToggleState (snapToEvents_, juce::dontSendNotification);
    snapEventsBtn_.setTooltip ("Snap to nearby region edges (trigger snap)");
    snapEventsBtn_.onClick = [this]()
    {
        snapToEvents_ = snapEventsBtn_.getToggleState();
    };
    snapOffsetBtn_.setClickingTogglesState (true);
    snapOffsetBtn_.setToggleState (snapKeepOffset_, juce::dontSendNotification);
    snapOffsetBtn_.setTooltip ("Keep original offset within snap division");
    snapOffsetBtn_.onClick = [this]()
    {
        snapKeepOffset_ = snapOffsetBtn_.getToggleState();
    };

    body_ = std::make_unique<Body> (*this);
    viewport_.setViewedComponent (body_.get(), false);
    viewport_.setScrollBarsShown (true, true);

    /* Playhead repaint runs on the peer's vblank tick (vsync-cadenced),
     * not the 30Hz transport timer -- see playheadVBlank_ in the header.
     * Attached to the Body so it only fires while the arranger is on
     * screen; the transport timer keeps dispatching when it isn't. */
    playheadVBlank_ = juce::VBlankAttachment (body_.get(),
                                              [this] { updatePlayheadPaint(); });

    rescanBtn_.onClick    = [this]() { rescanLaneTargets(); };
    addAudioBtn_.onClick  = [this]() { createEmptyAudioLane (true /*stereo*/); };
    addMidiBtn_.onClick   = [this]() { createEmptyMidiLane(); };

    /* Tool buttons: radio-group toggles; one tool active at a time.
     * Select is default + sticky on start. */
    toolSelectBtn_   .setClickingTogglesState (true);
    toolRangeBtn_    .setClickingTogglesState (true);
    toolSplitBtn_    .setClickingTogglesState (true);
    toolTrimBtn_     .setClickingTogglesState (true);
    toolAuditionBtn_ .setClickingTogglesState (true);
    toolEnvBtn_      .setClickingTogglesState (true);
    toolSelectBtn_.setToggleState (true, juce::dontSendNotification);

    toolSelectBtn_  .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Select);   syncToolToggleStates(); };
    toolRangeBtn_   .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Range);    syncToolToggleStates(); };
    toolSplitBtn_   .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Split);    syncToolToggleStates(); };
    toolTrimBtn_    .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Trim);     syncToolToggleStates(); };
    toolAuditionBtn_.onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Audition); syncToolToggleStates(); };
    toolEnvBtn_     .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Env);      syncToolToggleStates(); };

    loopBtn_.setClickingTogglesState (true);
    loopBtn_.setToggleState (false, juce::dontSendNotification);
    loopBtn_.onClick = [this]() { onLoopToggled(); };

    /* Active-state green tint -- mirrors tracker FOLLOW button so a
     * row of tool buttons reads as "this one is currently armed". */
    const juce::Colour kActiveTint { 0xff'4a'a5'5a };
    toolSelectBtn_  .setActiveTint (kActiveTint);
    toolRangeBtn_   .setActiveTint (kActiveTint);
    toolSplitBtn_   .setActiveTint (kActiveTint);
    toolTrimBtn_    .setActiveTint (kActiveTint);
    toolAuditionBtn_.setActiveTint (kActiveTint);
    toolEnvBtn_     .setActiveTint (kActiveTint);
    loopBtn_        .setActiveTint (kActiveTint);
    snapBtn_        .setActiveTint (kActiveTint);
    snapEventsBtn_  .setActiveTint (kActiveTint);
    snapOffsetBtn_  .setActiveTint (kActiveTint);

    /* Tool icons (vector paths, foreground-coloured so they pop in
     * both active + idle states).  Drawn in a square chunk on the
     * left of the button; label sits beside. */
    toolSelectBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Mouse-pointer arrow: thin filled triangle pointing
             * upper-left to lower-right.  Standard cursor glyph. */
            juce::Path p;
            const float x0 = b.getX() + b.getWidth() * 0.20f;
            const float y0 = b.getY() + b.getHeight() * 0.15f;
            const float x1 = b.getX() + b.getWidth() * 0.55f;
            const float y1 = b.getY() + b.getHeight() * 0.55f;
            const float x2 = b.getX() + b.getWidth() * 0.35f;
            const float y2 = b.getY() + b.getHeight() * 0.55f;
            const float x3 = b.getX() + b.getWidth() * 0.55f;
            const float y3 = b.getY() + b.getHeight() * 0.85f;
            const float x4 = b.getX() + b.getWidth() * 0.35f;
            const float y4 = b.getY() + b.getHeight() * 0.85f;
            const float x5 = b.getX() + b.getWidth() * 0.20f;
            const float y5 = b.getY() + b.getHeight() * 0.65f;
            p.startNewSubPath (x0, y0);
            p.lineTo (x1, y1);
            p.lineTo (x2, y2);
            p.lineTo (x3, y3);
            p.lineTo (x4, y4);
            p.lineTo (x5, y5);
            p.closeSubPath();
            g.setColour (fg);
            g.fillPath (p);
        });

    toolRangeBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Two vertical bookends + horizontal line spanning between
             * them: classic "range" / "loop bounds" glyph. */
            const float pad = b.getHeight() * 0.20f;
            const float top = b.getY() + pad;
            const float bot = b.getBottom() - pad;
            const float xL  = b.getX() + b.getWidth() * 0.20f;
            const float xR  = b.getRight() - b.getWidth() * 0.20f;
            const float yMid = b.getCentreY();
            g.setColour (fg);
            g.drawLine (xL, top, xL, bot, 1.5f);
            g.drawLine (xR, top, xR, bot, 1.5f);
            g.drawLine (xL, yMid, xR, yMid, 1.0f);
        });

    toolSplitBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Vertical blade with two outward arrowheads at the
             * midline -- "cut here, two pieces". */
            const float xMid = b.getCentreX();
            const float yMid = b.getCentreY();
            const float pad = b.getHeight() * 0.15f;
            g.setColour (fg);
            g.drawLine (xMid, b.getY() + pad, xMid, b.getBottom() - pad, 1.5f);
            const float arrowLen = b.getWidth() * 0.22f;
            const float arrowH   = b.getHeight() * 0.18f;
            juce::Path leftArrow, rightArrow;
            leftArrow.startNewSubPath (xMid - 1.5f, yMid);
            leftArrow.lineTo (xMid - arrowLen, yMid - arrowH);
            leftArrow.lineTo (xMid - arrowLen, yMid + arrowH);
            leftArrow.closeSubPath();
            rightArrow.startNewSubPath (xMid + 1.5f, yMid);
            rightArrow.lineTo (xMid + arrowLen, yMid - arrowH);
            rightArrow.lineTo (xMid + arrowLen, yMid + arrowH);
            rightArrow.closeSubPath();
            g.fillPath (leftArrow);
            g.fillPath (rightArrow);
        });

    toolTrimBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Two brackets pointing inward -- ][ -- "trim from both
             * sides into the middle". */
            const float pad = b.getHeight() * 0.18f;
            const float top = b.getY() + pad;
            const float bot = b.getBottom() - pad;
            const float xL  = b.getX() + b.getWidth() * 0.22f;
            const float xR  = b.getRight() - b.getWidth() * 0.22f;
            const float armLen = b.getWidth() * 0.18f;
            g.setColour (fg);
            g.drawLine (xL, top, xL, bot, 1.5f);
            g.drawLine (xL, top, xL + armLen, top, 1.5f);
            g.drawLine (xL, bot, xL + armLen, bot, 1.5f);
            g.drawLine (xR, top, xR, bot, 1.5f);
            g.drawLine (xR, top, xR - armLen, top, 1.5f);
            g.drawLine (xR, bot, xR - armLen, bot, 1.5f);
        });

    toolAuditionBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Speaker glyph -- trapezoid body + two arc lines for
             * sound waves.  Standard "preview" affordance. */
            const float yMid = b.getCentreY();
            const float bodyL = b.getX() + b.getWidth() * 0.18f;
            const float bodyR = b.getX() + b.getWidth() * 0.42f;
            const float coneR = b.getX() + b.getWidth() * 0.62f;
            const float bodyTop = yMid - b.getHeight() * 0.15f;
            const float bodyBot = yMid + b.getHeight() * 0.15f;
            const float coneTop = yMid - b.getHeight() * 0.30f;
            const float coneBot = yMid + b.getHeight() * 0.30f;
            juce::Path sp;
            sp.startNewSubPath (bodyL, bodyTop);
            sp.lineTo (bodyR, bodyTop);
            sp.lineTo (coneR, coneTop);
            sp.lineTo (coneR, coneBot);
            sp.lineTo (bodyR, bodyBot);
            sp.lineTo (bodyL, bodyBot);
            sp.closeSubPath();
            g.setColour (fg);
            g.fillPath (sp);
            const float waveX = b.getX() + b.getWidth() * 0.72f;
            g.drawLine (waveX, yMid - 3, waveX + b.getWidth() * 0.10f, yMid - 6, 1.2f);
            g.drawLine (waveX, yMid + 3, waveX + b.getWidth() * 0.10f, yMid + 6, 1.2f);
        });

    loopBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Open circular arrow -- loop / wrap glyph. */
            const float cx = b.getCentreX();
            const float cy = b.getCentreY();
            const float rad = juce::jmin (b.getWidth(), b.getHeight()) * 0.32f;
            juce::Path arc;
            arc.addCentredArc (cx, cy, rad, rad, 0.0f,
                                0.25f, 6.0f, true);
            g.setColour (fg);
            g.strokePath (arc, juce::PathStrokeType (1.4f));
            /* Tiny arrowhead at the end of the arc. */
            const float ax = cx + rad * 0.7f;
            const float ay = cy - rad * 0.7f;
            juce::Path tip;
            tip.startNewSubPath (ax, ay);
            tip.lineTo (ax - rad * 0.45f, ay - rad * 0.15f);
            tip.lineTo (ax - rad * 0.10f, ay - rad * 0.55f);
            tip.closeSubPath();
            g.fillPath (tip);
        });
}

ArrangementView::~ArrangementView()
{
    stopTimer();
    detachFromActiveGraph();

    /* Drop record-committed handlers so AudioClipNodes don't try to
     * invoke us during teardown.  Bound lambdas captured `this`; the
     * AudioClipNode instances outlive the ArrangementView while the
     * graph is being torn down. */
    for (auto& rs : laneRuntime_)
        if (rs.audioClipCache != nullptr)
            rs.audioClipCache->setRecordingCommittedHandler (nullptr);
}

void ArrangementView::initializeView (Services& s)
{
    services_ = &s;
    if (auto* eng = s.context().audio().get())
        monitor_ = eng->getTransportMonitor();
    /* Capture the session VT identity we were built against.
     * writeViewStateToSession compares against the live session and
     * skips the write if they differ -- prevents stale-state writes
     * after a session reload (see initialSessionTree_ in the header). */
    if (auto sess = s.context().session())
        initialSessionTree_ = sess->data();
}

void ArrangementView::didBecomeActive()
{
    attachToActiveGraph();
    rescanLaneTargets();

    /* Zoom restored synchronously BEFORE the first paint so the user
     * never sees the default kPxPerBeat/kLaneH flash through.
     * Previously the entire view-state was deferred via callAsync
     * which left one paint frame of unstyled content visible. */
    loadZoomFromSession();

    startTimerHz (30);

    /* Scroll restore deferred to the next message-thread tick: the
     * parent setContentView path has not yet invoked our resized(),
     * so viewport_ has zero dimensions at this point and an immediate
     * setViewPosition would clamp to (0, 0).  callAsync defers past
     * resized() so loadViewStateFromSession (scroll only) applies
     * against the real viewport area.  After scroll restore we also
     * try an auto-fit-to-content -- no-op when the session has a
     * saved pxPerBeat, otherwise fits a fresh-load arrangement into
     * the visible viewport instead of leaving it at the default
     * (overflowing) kPxPerBeat = 24. */
    juce::Component::SafePointer<ArrangementView> self (this);
    juce::MessageManager::callAsync ([self]()
    {
        if (auto* v = self.getComponent())
        {
            v->loadViewStateFromSession();
            v->maybeAutoFitOnLoad();
        }
    });
}

void ArrangementView::willBeRemoved()
{
    /* No writeViewStateToSession here.  Zoom + scroll persist via
     * Body::zoomBy / zoomToFit + PersistingViewport::visibleAreaChanged
     * which write on the actual user action.  Removing the
     * detach-time write eliminates the session-reload race entirely
     * (sigSessionLoaded fires after the context's session pointer
     * swaps, and a willBeRemoved write would overwrite the new
     * session's view state). */
    cancelPendingUpdate();
    stopTimer();
    detachFromActiveGraph();
}

void ArrangementView::stabilizeContent()
{
    /* Coalesce multiple stabilize requests fired in one event-loop
     * tick into one rescan on the next tick.  Triggers include
     * post-rescan callbacks + ValueTree-listener cascades + outer
     * GuiService stabilizeContent fan-outs.  handleAsyncUpdate does
     * the actual attach + rescan once. */
    triggerAsyncUpdate();
}

void ArrangementView::handleAsyncUpdate()
{
    attachToActiveGraph();
    rescanLaneTargets();
}

void ArrangementView::attachToActiveGraph()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    auto tree = graph.data();
    if (tree == attachedGraphTree_) return;

    detachFromActiveGraph();
    attachedGraphTree_ = tree;
    if (attachedGraphTree_.isValid())
        attachedGraphTree_.addListener (this);
}

void ArrangementView::detachFromActiveGraph()
{
    if (attachedGraphTree_.isValid())
        attachedGraphTree_.removeListener (this);
    attachedGraphTree_ = juce::ValueTree();
}

void ArrangementView::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    /* Graph mutations may fire a burst of child-added events (e.g.
     * adding a subgraph with several nodes drops one event per
     * node).  Route to triggerAsyncUpdate so a burst collapses to a
     * single rescan on the next tick. */
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        triggerAsyncUpdate();
}

void ArrangementView::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        triggerAsyncUpdate();
}

void ArrangementView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kHeaderH).reduced (4, 4);
    rescanBtn_      .setBounds (top.removeFromLeft (60)); top.removeFromLeft (4);
    addAudioBtn_    .setBounds (top.removeFromLeft (70)); top.removeFromLeft (4);
    addMidiBtn_     .setBounds (top.removeFromLeft (60)); top.removeFromLeft (12);

    toolSelectBtn_  .setBounds (top.removeFromLeft (76)); top.removeFromLeft (2);
    toolRangeBtn_   .setBounds (top.removeFromLeft (72)); top.removeFromLeft (2);
    toolSplitBtn_   .setBounds (top.removeFromLeft (64)); top.removeFromLeft (2);
    toolTrimBtn_    .setBounds (top.removeFromLeft (64)); top.removeFromLeft (2);
    toolAuditionBtn_.setBounds (top.removeFromLeft (72)); top.removeFromLeft (2);
    toolEnvBtn_     .setBounds (top.removeFromLeft (60)); top.removeFromLeft (12);

    loopBtn_        .setBounds (top.removeFromLeft (64)); top.removeFromLeft (12);

    snapBtn_        .setBounds (top.removeFromLeft (52)); top.removeFromLeft (4);
    snapBox_        .setBounds (top.removeFromLeft (64)); top.removeFromLeft (4);
    snapEventsBtn_  .setBounds (top.removeFromLeft (40)); top.removeFromLeft (2);
    snapOffsetBtn_  .setBounds (top.removeFromLeft (40)); top.removeFromLeft (12);

    /* Zoom cluster -- LCD-framed group of three buttons.  Inner
     * geometry: [ -  +  Fit ] with 2 px between buttons + 4 px
     * inset from the frame on every side.  The frame itself is
     * painted in paint() around this rect (zoomFrameBounds_). */
    constexpr int kFrameInset = 4;
    constexpr int kBtnGap     = 2;
    const int btnH      = top.getHeight() - kFrameInset * 2;
    const int dashW     = 22;
    const int plusW     = 22;
    const int fitW      = 32;
    const int clusterW  = kFrameInset * 2 + dashW + kBtnGap + plusW + kBtnGap + fitW;

    auto cluster = top.removeFromLeft (clusterW);
    zoomFrameBounds_ = cluster;
    auto inside = cluster.reduced (kFrameInset);
    zoomOutBtn_.setBounds (inside.removeFromLeft (dashW));
    inside.removeFromLeft (kBtnGap);
    zoomInBtn_ .setBounds (inside.removeFromLeft (plusW));
    inside.removeFromLeft (kBtnGap);
    zoomFitBtn_.setBounds (inside.removeFromLeft (fitW));
    juce::ignoreUnused (btnH);

    viewport_.setBounds (r);
    if (body_ != nullptr) body_->resizeForLanes();
}

/* =====================================================================
 * Outer FileDragAndDropTarget on the ContentView itself.  Belt-and-
 * suspenders alongside Body's same interface (Body lives inside a
 * Viewport which can rarely swallow X11 XDND routing in some peer
 * configurations).
 * ===================================================================== */

bool ArrangementView::isInterestedInFileDrag (const juce::StringArray& files)
{
    const bool interested = anyDroppableFileIn (files);
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView] isInterestedInFileDrag count=")
        + juce::String (files.size())
        + (interested ? " ACCEPT" : " REJECT"));
    return interested;
}

void ArrangementView::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView] fileDragEnter x=") + juce::String (x)
        + " y=" + juce::String (y)
        + " count=" + juce::String (files.size()));
}

void ArrangementView::fileDragExit (const juce::StringArray&)
{
    juce::Logger::writeToLog ("[ArrangementView] fileDragExit");
}

void ArrangementView::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView] filesDropped x=") + juce::String (x)
        + " y=" + juce::String (y)
        + " count=" + juce::String (files.size()));

    /* Translate ArrangementView-local coords to Body-local coords:
     * subtract header height + add viewport scroll offset.  body_'s
     * (0,0) is at the top of the lanes area. */
    if (body_ == nullptr) return;

    const int bodyY = (y - kHeaderH) + viewport_.getViewPositionY();
    const int bodyX =  x              + viewport_.getViewPositionX();

    const int laneIdx = laneIdxFromY (bodyY);
    const double dropBeats =
        juce::jmax (0.0, (double) (bodyX - kLabelW) / (double) body_->kPxPerBeat);

    /* Partition by kind first; audio and MIDI files take divergent
     * lane-create paths.  Mixed drops produce one lane each. */
    juce::StringArray audioPaths;
    juce::StringArray midiPaths;
    for (const auto& path : files)
    {
        if      (isAcceptableAudioFile (path)) audioPaths.add (path);
        else if (isAcceptableMidiFile  (path)) midiPaths .add (path);
    }

    if (! audioPaths.isEmpty())
    {
        int targetLane = laneIdx;
        if (targetLane >= 0 && targetLane < lanes_.size())
        {
            const auto& runtime = laneRuntime_.getReference (targetLane);
            if (! runtime.isAudioLane())
                targetLane = -1;
        }
        else
        {
            targetLane = -1;
        }

        double cursor = dropBeats;
        for (const auto& path : audioPaths)
        {
            const juce::File f (path);
            if (! f.existsAsFile())
            {
                juce::Logger::writeToLog (
                    juce::String ("[ArrangementView] skipping non-existent file: ") + path);
                continue;
            }

            const bool ok = importAudioFileToLane (f, targetLane, cursor);
            juce::Logger::writeToLog (
                juce::String ("[ArrangementView] importAudioFileToLane(")
                + f.getFileName() + ", " + juce::String (targetLane) + ", "
                + juce::String (cursor, 2) + ") = " + (ok ? "OK" : "FAIL"));
            if (! ok) continue;

            if (targetLane < 0)
                targetLane = lanes_.size() - 1;

            if (targetLane >= 0 && targetLane < lanes_.size())
            {
                const auto& regs = lanes_.getReference (targetLane).playlist.regions();
                if (! regs.empty())
                    cursor = regs.back().endBeats();
            }
        }
    }

    if (! midiPaths.isEmpty())
    {
        int midiTarget = laneIdx;
        if (midiTarget >= 0 && midiTarget < lanes_.size())
        {
            if (lanes_.getReference (midiTarget).kind != Lane::Kind::Midi)
                midiTarget = -1;
        }
        else
        {
            midiTarget = -1;
        }

        double cursor = dropBeats;
        for (const auto& path : midiPaths)
        {
            const juce::File f (path);
            if (! f.existsAsFile()) continue;

            const bool ok = importMidiFileToLane (f, midiTarget, cursor);
            juce::Logger::writeToLog (
                juce::String ("[ArrangementView] importMidiFileToLane(")
                + f.getFileName() + ", " + juce::String (midiTarget) + ", "
                + juce::String (cursor, 2) + ") = " + (ok ? "OK" : "FAIL"));
            if (! ok) continue;

            if (midiTarget < 0)
                midiTarget = lanes_.size() - 1;

            if (midiTarget >= 0 && midiTarget < lanes_.size())
            {
                const auto& midis = lanes_.getReference (midiTarget)
                                          .playlist.midiRegions();
                if (! midis.empty() && midis.back() != nullptr)
                    cursor = midis.back()->positionBeats
                           + midis.back()->lengthBeats;
            }
        }
    }
}

void ArrangementView::paint (Graphics& g)
{
    g.fillAll (Colors::contentBackgroundColor);
    g.setColour (Colors::backgroundColor);
    g.fillRect (getLocalBounds().removeFromTop (kHeaderH));

    /* LCD-style frame around the zoom cluster -- mirrors the
     * TransportBar's matte-black bezel + cool-grey vertical gradient
     * inset.  Three buttons (-/+/Fit) sit inside; the frame reads as
     * a single hardware-style group.  zoomFrameBounds_ is computed
     * in resized(). */
    if (! zoomFrameBounds_.isEmpty())
    {
        const auto frect = zoomFrameBounds_.toFloat();
        g.setColour (juce::Colour (0xff'08'08'08));
        g.fillRoundedRectangle (frect, 4.0f);
        g.setColour (juce::Colour (0xff'3a'3a'3a));
        g.drawRoundedRectangle (frect.reduced (0.5f), 4.0f, 1.0f);

        const auto inner = frect.reduced (3.0f);
        juce::ColourGradient lcdGrad (
            juce::Colour (0xff'14'19'1e),
            inner.getX(), inner.getY(),
            juce::Colour (0xff'0c'0f'12),
            inner.getX(), inner.getBottom(),
            false);
        g.setGradientFill (lcdGrad);
        g.fillRoundedRectangle (inner, 3.0f);
    }
}

/* ===================================================================== */

namespace {

/** Recursive: collects lane targets reachable from `graph`.  Output
 *  arrays are parallel: outNodes[i] -> outTrackers[i] OR
 *  outAudioClips[i] OR outMidiPlayers[i] (exactly one non-null per
 *  index).
 *
 *  Surface ownership rules (TrackerNode-style unified model):
 *   - TrackerNode: collected at any nesting.  One node = one
 *     "clip source" auto-shown by BOTH session view (as a column)
 *     AND arrangement view (as a lane).  Graph appears once; both
 *     surfaces project onto it.
 *   - MidiPlayerNode: same shape -- collected at any nesting.
 *     Root-level instances spawned via "+ MIDI" from either surface
 *     appear in both views simultaneously, matching the modular-
 *     graph "one node per clip source" abstraction.
 *   - AudioClipNode: still collected ONLY inside the Multi-Track
 *     subgraph.  Audio clips need the subgraph's bus routing
 *     infrastructure (audio sums onto the outer face); they don't
 *     follow the unified-clip-source pattern. */
void collectLaneTargetsFromGraph (const Node& graph,
                                  juce::Array<Node>&            outNodes,
                                  juce::Array<TrackerNode*>&    outTrackers,
                                  juce::Array<AudioClipNode*>&  outAudioClips,
                                  juce::Array<MidiPlayerNode*>& outMidiPlayers,
                                  bool                          insideArrangementSubgraph = false)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (auto* proc = child.getObject())
        {
            /* TrackerNode IS-A element::Processor directly so the
             * cast is on `proc` itself.  Collected at any nesting. */
            if (auto* t = dynamic_cast<TrackerNode*> (proc))
            {
                outNodes.add (child);
                outTrackers.add (t);
                outAudioClips.add (nullptr);
                outMidiPlayers.add (nullptr);
                continue;
            }
            /* MidiPlayerNode -- unified clip source.  Collected at
             * any nesting; appears in BOTH session view (as a column)
             * AND arrangement view (as a lane). */
            if (auto* mp = dynamic_cast<MidiPlayerNode*> (proc))
            {
                outNodes.add (child);
                outTrackers.add (nullptr);
                outAudioClips.add (nullptr);
                outMidiPlayers.add (mp);
                continue;
            }
            /* AudioClipNode is a juce::AudioPluginInstance wrapped by
             * element::Processor -- Node::getObject() returns the
             * wrapper, so we have to go through proc->getAudioProcessor()
             * to reach the underlying AudioClipNode.  Still scoped to
             * Multi-Track because audio clips need the subgraph bus. */
            if (auto* ap = proc->getAudioProcessor())
            {
                if (auto* a = dynamic_cast<AudioClipNode*> (ap))
                {
                    if (insideArrangementSubgraph)
                    {
                        outNodes.add (child);
                        outTrackers.add (nullptr);
                        outAudioClips.add (a);
                        outMidiPlayers.add (nullptr);
                    }
                    continue;
                }
            }
        }
        if (child.isGraph())
        {
            const bool nowInside = insideArrangementSubgraph
                || child.isA (EL_NODE_FORMAT_NAME, EL_NODE_ID_ARRANGEMENT_TRACKS);
            collectLaneTargetsFromGraph (child, outNodes, outTrackers,
                                          outAudioClips, outMidiPlayers,
                                          nowInside);
        }
    }
}

/* Walk the active graph (recursing into subgraphs) collecting every
 * node name that starts with `prefix + " "` so we can compute the
 * lowest free integer suffix.  Mirrors the "graph is the canonical
 * naming source" rule -- both views read node.getName() to display
 * lane / column labels, so a unique numbered name on the node keeps
 * the graph block + arrangement lane + session column in sync. */
void collectUsedNumbersForPrefix (const Node&             graph,
                                  const juce::String&     prefix,
                                  juce::Array<int>&       outUsed)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        const auto name = child.getName();
        if (name.startsWith (prefix + " "))
        {
            const auto tail = name.substring (prefix.length() + 1).trim();
            const int  num  = tail.getIntValue();
            if (num > 0 && tail.containsOnly ("0123456789"))
                outUsed.add (num);
        }
        if (child.isGraph())
            collectUsedNumbersForPrefix (child, prefix, outUsed);
    }
}

/* "prefix N" where N is the lowest positive integer such that no
 * existing graph node has that name.  Sessions tend to fewer than
 * a dozen of each kind, so the linear scan / array contains is fine. */
juce::String nextNumberedNodeName (const Node&         rootGraph,
                                   const juce::String& prefix)
{
    juce::Array<int> used;
    collectUsedNumbersForPrefix (rootGraph, prefix, used);
    int n = 1;
    while (used.contains (n)) ++n;
    return prefix + " " + juce::String (n);
}

Node findNodeByUuid (const Node& graph, juce::Uuid target)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (child.getUuid() == target) return child;
        if (child.isGraph())
        {
            Node nested = findNodeByUuid (child, target);
            if (nested.isValid()) return nested;
        }
    }
    return Node();
}

} // namespace

TrackerNode* ArrangementView::resolveTrackerByUuid (juce::Uuid targetNodeUuid) const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return nullptr;

    const Node target = findNodeByUuid (active, targetNodeUuid);
    if (! target.isValid()) return nullptr;
    return dynamic_cast<TrackerNode*> (target.getObject());
}

AudioClipNode* ArrangementView::resolveAudioClipByUuid (juce::Uuid targetNodeUuid) const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return nullptr;

    const Node target = findNodeByUuid (active, targetNodeUuid);
    if (! target.isValid()) return nullptr;

    /* Two-step unwrap: AudioClipNode is a juce::AudioPluginInstance,
     * so Element wraps it in an AudioProcessorNode (element::Processor).
     * Node::getObject() returns the wrapper -- we have to go through
     * its getAudioProcessor() to reach the underlying AudioClipNode.
     * (TrackerNode IS an element::Processor directly, so the
     * resolveTrackerByUuid path doesn't need this dance.) */
    auto* proc = target.getObject();
    if (proc == nullptr) return nullptr;
    return dynamic_cast<AudioClipNode*> (proc->getAudioProcessor());
}

MidiNoteRegion* ArrangementView::findMidiRegion (const juce::Uuid& regionId) noexcept
{
    /* Linear scan across lanes -> playlist.findMidiRegion (which is
     * itself a linear scan of midiRegions_).  Lane counts are small
     * (tens at most), region counts per lane similarly small, so the
     * per-paint cost from the piano-roll resolver lambda is in the
     * noise compared to the paint pass itself.  Returns the first
     * hit; uuids are unique by construction. */
    for (int i = 0; i < lanes_.size(); ++i)
    {
        auto& lane = lanes_.getReference (i);
        if (auto* r = lane.playlist.findMidiRegion (regionId))
            return r;
    }
    return nullptr;
}

void ArrangementView::autoFillLaneForTracker (Lane& lane, TrackerNode* trk)
{
    if (trk == nullptr) return;
    const int numPatterns = trk->numPatterns();
    double cursor = 0.0;
    for (int p = 0; p < numPatterns; ++p)
    {
        Region r;
        r.id            = juce::Uuid();
        r.sourceId      = lane.targetNodeUuid;
        r.sequenceIdx   = p;
        r.positionBeats = cursor;
        r.lengthBeats   = 4.0;
        /* Lane tint propagates at paint time and is bulk-refreshed by
         * rescanLaneTargets after this method returns (palette by
         * lane index).  Seed to the current lane colour so the brief
         * window between create + the palette pass paints correctly. */
        r.colour        = lane.colour;
        lane.playlist.addRegion (std::move (r));
        cursor += 4.0;
    }
}

void ArrangementView::rescanLaneTargets()
{
    /* Suppress undo tracking during rescan: any auto-fill of newly-
     * discovered tracker lanes is a side effect of graph state, not
     * a user mutation -- the user already has graph-side undo for
     * the underlying add-node action.  ScopedValueSetter restores
     * the prior flag on function exit so nested calls (rescan
     * invoked from applyLaneSnapshot during undo replay) keep their
     * own suppression intact. */
    juce::ScopedValueSetter<bool> suppressGuard (applyingUndoAction_, true);

    if (! lanesLoadedFromSession_)
    {
        loadLanesFromSession();
        lanesLoadedFromSession_ = true;
        /* Seed the undo baseline: future writeLanesToSession calls
         * diff against this initial post-load lanes_ state. */
        lastCommittedSnapshot_ = lanes_;
    }

    juce::Array<Node>             foundNodes;
    juce::Array<TrackerNode*>     foundTrackers;
    juce::Array<AudioClipNode*>   foundAudioClips;
    juce::Array<MidiPlayerNode*>  foundMidiPlayers;

    if (services_ != nullptr)
    {
        if (auto sess = services_->context().session())
        {
            const Node active = sess->getActiveGraph();
            if (active.isValid())
            {
                collectLaneTargetsFromGraph (active, foundNodes,
                                              foundTrackers, foundAudioClips,
                                              foundMidiPlayers);

                /* Auto-rename: any clip-source node still carrying
                 * its default factory name gets bumped to the
                 * canonical "TrkSeq N" / "MidiSeq N" / "AudioTrk N".
                 * Triggers ONLY on exact default match so user-
                 * customised names are preserved. */
                for (int i = 0; i < foundNodes.size(); ++i)
                {
                    Node n = foundNodes.getReference (i);
                    const auto cur = n.getName();
                    const juce::String prefix =
                        (foundTrackers[i]    != nullptr) ? juce::String ("TrkSeq")
                      : (foundMidiPlayers[i] != nullptr) ? juce::String ("MidiSeq")
                      : (foundAudioClips[i]  != nullptr) ? juce::String ("AudioTrk")
                      : juce::String();
                    const bool defaultName =
                        (cur == "Tracker"     && prefix == "TrkSeq")
                     || (cur == "MIDI Player" && prefix == "MidiSeq")
                     || (cur == "Audio Clip"  && prefix == "AudioTrk");
                    if (! defaultName || prefix.isEmpty()) continue;
                    n.setName (nextNumberedNodeName (active, prefix));
                }
            }
        }
    }

    bool mutated = false;   /* shared with the auto-fill loop below */

    /* Auto-fill: clip-source nodes (TrackerNode + MidiPlayerNode) get
     * a default lane on first discovery.  Mirrors how session view
     * auto-creates columns from the same nodes -- one node, two
     * surface projections.
     *
     * - TrackerNodes seed the lane with one region per existing
     *   pattern (autoFillLaneForTracker), since patterns pre-exist
     *   on the node.
     * - MidiPlayerNodes auto-create an empty MIDI lane; the user adds
     *   regions via drop / paint.  No auto-seeding because new MIDI
     *   players don't carry pre-authored content.
     *
     * AudioClipNodes do NOT auto-fill -- they're created explicitly
     * via "+ Audio Track" or file drop, both of which create the lane
     * inline. */
    for (int i = 0; i < foundNodes.size(); ++i)
    {
        const bool isTracker = (foundTrackers[i]   != nullptr);
        const bool isMidi    = (foundMidiPlayers[i] != nullptr);
        if (! isTracker && ! isMidi) continue;

        const juce::Uuid uuid = foundNodes.getReference (i).getUuid();
        bool alreadyBound = false;
        for (const auto& l : lanes_)
            if (l.targetNodeUuid == uuid) { alreadyBound = true; break; }
        if (alreadyBound) continue;

        Lane lane;
        lane.id             = juce::Uuid();
        lane.targetNodeUuid = uuid;
        /* Lane::Kind enum is {Audio, Midi}; tracker lanes share the
         * Audio default + are runtime-distinguished by trackerCache
         * in laneRuntime_ (per the enum docstring). */
        if (isMidi) lane.kind = Lane::Kind::Midi;
        lane.name           = foundNodes.getReference (i).getName().isNotEmpty()
                                ? foundNodes.getReference (i).getName()
                                : (isMidi ? juce::String ("MIDI")
                                          : juce::String ("Tracker"));
        if (isTracker)
            autoFillLaneForTracker (lane, foundTrackers[i]);
        lanes_.add (std::move (lane));
        mutated = true;
    }

    /* Assign palette tint to every lane by its index.  Lanes
     * created before the palette landed default to dark-gray; this
     * line replaces those with the shared tracker palette.  Future
     * "lane colour picker" UI will introduce a separate override
     * flag so user-customised colours aren't reset on rescan.
     *
     * Propagate the freshly-assigned tint to every audio + MIDI
     * region on the lane so paint sites that read region.colour /
     * m.colour (piano-roll paintNotes, arrangement MIDI strip) track
     * the lane.  Single source of truth is lane.colour; per-region
     * .colour is a cached copy maintained here + at region-creation
     * sites. */
    for (int i = 0; i < lanes_.size(); ++i)
    {
        auto& l = lanes_.getReference (i);
        l.colour = laneTintForIndex (i);
        l.playlist.setAllRegionColours (l.colour);
    }

    /* Rebuild runtime state in lockstep.  For each persisted lane,
     * resolve which kind of node it binds to + wire up the
     * AudioLaneAdapter / record commit handler if applicable.
     *
     * Optimisation: replace the per-lane resolveTrackerByUuid +
     * resolveAudioClipByUuid calls (each a recursive findNodeByUuid
     * graph walk = O(M) for M=graph node count) with a linear scan
     * over the foundNodes array that collectLaneTargetsFromGraph
     * already populated.  Lookup cost drops from O(N*M) to O(N*F)
     * where F=N tracker+audio nodes (typically <50), eliminating
     * most of the graph -> timeline switch latency on dense sessions.
     * collectLaneTargetsFromGraph now properly unwraps
     * proc->getAudioProcessor() for AudioClipNodes so foundAudioClips
     * is correctly populated.
     *
     * Per-lane Logger::writeToLog (sync disk IO on the message
     * thread) is also gone -- the other dominant cost in this loop. */
    laneRuntime_.clearQuick();
    laneRuntime_.ensureStorageAllocated (lanes_.size());
    for (int i = 0; i < lanes_.size(); ++i)
    {
        const auto& l = lanes_.getReference (i);
        LaneRuntimeState s;

        for (int j = 0; j < foundNodes.size(); ++j)
        {
            if (foundNodes.getReference (j).getUuid() == l.targetNodeUuid)
            {
                s.trackerCache    = foundTrackers   [j];
                s.audioClipCache  = foundAudioClips [j];
                s.midiPlayerCache = foundMidiPlayers[j];
                break;
            }
        }

        if (s.audioClipCache != nullptr)
        {
            s.audioAdapter.setTargetNode (s.audioClipCache);

            /* Propagate persisted Lane.armed onto the freshly-bound
             * node so re-opening a session restores arm state. */
            s.audioClipCache->setArmed (l.armed);

            /* Capture lane id by value (survives lane-index shifts)
             * and the ArrangementView via Component::SafePointer so
             * the lambda is safe to invoke after this view has been
             * destroyed (graph-teardown race: AudioClipNode's Timer
             * may fire after the view is already gone). */
            const juce::Uuid laneId = l.id;
            juce::Component::SafePointer<ArrangementView> safe (this);
            s.audioClipCache->setRecordingCommittedHandler (
                [safe, laneId] (const juce::File& file)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;

                    /* Resolve lane by id (linear scan).  If user
                     * deleted the lane mid-capture, drop the file
                     * silently -- the .wav stays on disk for
                     * recovery but no Region is created. */
                    int idx = -1;
                    for (int k = 0; k < self->lanes_.size(); ++k)
                        if (self->lanes_.getReference (k).id == laneId) { idx = k; break; }
                    if (idx < 0) return;

                    auto src = SourceRegistry::get().importFromFile (file);
                    if (src == nullptr) return;

                    auto& lane = self->lanes_.getReference (idx);

                    /* Position the new Region at recordStartBeat_ --
                     * where the transport playhead was when record
                     * started -- so the audio lands at the spot the
                     * user actually triggered, not appended after
                     * the last existing region.  If the target slot
                     * overlaps an existing region, nudge forward to
                     * the next free spot (defensive). */
                    double position = self->recordStartBeat_;
                    for (const auto& r : lane.playlist.regions())
                        if (r.containsBeat (position))
                            position = juce::jmax (position, r.endBeats());

                    const double bpm = self->monitor_ != nullptr
                                          ? (double) self->monitor_->tempo.get()
                                          : 120.0;
                    const double lengthSeconds = src->sourceSampleRate() > 0
                        ? (double) src->durationSamples() / (double) src->sourceSampleRate()
                        : 0.0;
                    const double lengthBeats = juce::jmax (
                        0.25,
                        lengthSeconds * (bpm / 60.0));

                    Region r;
                    r.id            = juce::Uuid();
                    r.sourceId      = src->uuid();
                    r.positionBeats = position;
                    r.lengthBeats   = lengthBeats;
                    r.name          = file.getFileNameWithoutExtension();
                    /* Inherit lane tint -- single source of truth at
                     * lane.colour; this is the cached per-region copy
                     * read by piano-roll + MIDI strip paint. */
                    r.colour        = lane.colour;
                    lane.playlist.addRegion (std::move (r));

                    self->writeLanesToSession();
                    if (self->body_ != nullptr)
                    {
                        self->body_->resizeForLanes();
                        self->body_->repaintLane (idx);
                    }
                });
        }
        else if (s.trackerCache == nullptr && s.midiPlayerCache == nullptr)
        {
            /* Orphan lane: defensively detach any previous adapter
             * binding so dispatch silently skips. */
            s.audioAdapter.setTargetNode (nullptr);
        }

        laneRuntime_.add (std::move (s));
    }

    /* Orphan-lane purge.  When the Multi-Track subgraph (or any
     * single tracker host) is destroyed, lanes_ retains entries
     * whose targetNodeUuid no longer resolves.  Walk the freshly-
     * computed laneRuntime_ + drop lanes whose target is gone AND
     * whose playlist is empty (no audio regions + no MIDI regions).
     * Content-bearing orphan lanes are kept so the user doesn't
     * silently lose authored regions -- they show with a "?" badge
     * + the user can manually delete them. */
    for (int i = lanes_.size(); --i >= 0;)
    {
        const auto& l = lanes_.getReference (i);
        const auto& s = laneRuntime_.getReference (i);
        const bool noTarget =
            (s.trackerCache == nullptr
             && s.audioClipCache == nullptr
             && s.midiPlayerCache == nullptr);
        const bool empty =
            l.playlist.regions().empty()
            && l.playlist.midiRegions().empty();
        if (noTarget && empty)
        {
            lanes_.remove (i);
            laneRuntime_.remove (i);
            mutated = true;
        }
    }

    /* Republish MIDI region bindings for every MIDI lane with a live
     * MidiPlayerNode.  Does this AFTER the laneRuntime_ population
     * loop so publishMidiBindingsForLane can read the freshly-bound
     * midiPlayerCache from laneRuntime_. */
    for (int i = 0; i < lanes_.size(); ++i)
    {
        const auto& l = lanes_.getReference (i);
        if (l.kind == Lane::Kind::Midi)
            publishMidiBindingsForLane (i);
    }

    if (mutated) writeLanesToSession();
    if (body_ != nullptr) body_->resizeForLanes();

    /* After any rescan, re-apply persisted mute / solo state to the
     * freshly-bound target processors (a graph rebind clears the
     * underlying setMuted state). */
    propagateMuteSolo();
}

void ArrangementView::loadLanesFromSession()
{
    lanes_.clearQuick();
    markerTrack_.clear();
    automationBindings_.clearQuick();
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;

    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return;
    auto lanesTree = tree.getChildWithName ("lanes");
    if (lanesTree.isValid())
    {
        for (int i = 0; i < lanesTree.getNumChildren(); ++i)
        {
            const auto laneTree = lanesTree.getChild (i);
            if (laneTree.getType() != juce::Identifier ("lane")) continue;
            lanes_.add (Lane::fromValueTree (laneTree));
        }
    }

    /* Markers sit beside <lanes> under <arrangement>.  Older sessions
     * predating Q6 simply have no <markers> child; loadFromValueTree
     * leaves the in-memory list empty in that case. */
    auto markersTree = tree.getChildWithName ("markers");
    if (markersTree.isValid())
        markerTrack_.loadFromValueTree (markersTree);

    /* Automation bindings sit beside <lanes> + <markers>.  Presentation
     * only -- the engine tracks they reference (by trackId) are populated
     * separately + view-independently at session-load (EngineService). */
    auto bindingsTree = tree.getChildWithName ("automationBindings");
    if (bindingsTree.isValid())
    {
        for (int i = 0; i < bindingsTree.getNumChildren(); ++i)
        {
            const auto bt = bindingsTree.getChild (i);
            if (bt.getType() != juce::Identifier ("automationBinding")) continue;
            automationBindings_.add (AutomationBinding::fromValueTree (bt));
        }
    }
}

/* Undoable snapshot action for ArrangementView mutations.  Stored
 * in the global GuiService::UndoManager.  Holds copies of the lanes_
 * array on either side of one user mutation; perform() / undo() swap
 * via ArrangementView::applyLaneSnapshot.  SafePointer guards the
 * cached-view-destroyed case (session reload clears the cache + the
 * undo history at the same time, but defence in depth). */
class ArrangementSnapshotAction : public juce::UndoableAction
{
public:
    ArrangementSnapshotAction (juce::Component::SafePointer<ArrangementView> v,
                                juce::Array<Lane> before,
                                juce::Array<Lane> after)
        : view_ (v),
          before_ (std::move (before)),
          after_  (std::move (after))
    {}

    bool perform() override
    {
        if (auto* v = view_.getComponent())
        {
            v->applyLaneSnapshot (after_);
            return true;
        }
        return false;
    }

    bool undo() override
    {
        if (auto* v = view_.getComponent())
        {
            v->applyLaneSnapshot (before_);
            return true;
        }
        return false;
    }

private:
    juce::Component::SafePointer<ArrangementView> view_;
    juce::Array<Lane> before_;
    juce::Array<Lane> after_;
};

void ArrangementView::writeLanesToSession()
{
    if (services_ == nullptr) return;

    /* Diff against the last-committed snapshot.  If we're inside an
     * undo/redo replay, OR we haven't loaded from session yet (initial
     * baseline state), OR the snapshot is unchanged, skip the action
     * push.  Otherwise enqueue a new undo step and update the
     * committed snapshot to the post-mutation state.
     *
     * The natural choke point is writeLanesToSession itself: every
     * arrangement mutation (region add / remove / move / resize /
     * split, lane add / remove, envelope edits, gain / fade edits,
     * ARM / MUTE / SOLO toggles, loop toggle) writes via this method,
     * so wiring undo here covers all current and future mutation
     * sites without per-callsite refactoring. */
    if (! applyingUndoAction_ && lanesLoadedFromSession_)
    {
        if (auto* gui = services_->find<GuiService>())
        {
            auto& undo = gui->getUndoManager();
            undo.beginNewTransaction();
            undo.perform (new ArrangementSnapshotAction (this,
                                                          lastCommittedSnapshot_,
                                                          lanes_));
        }
        lastCommittedSnapshot_ = lanes_;
    }

    auto sess = services_->context().session();
    if (sess == nullptr) return;

    auto tree = sess->data().getOrCreateChildWithName (tags::arrangement, nullptr);
    auto lanesTree = tree.getOrCreateChildWithName ("lanes", nullptr);
    lanesTree.removeAllChildren (nullptr);
    for (const auto& l : lanes_)
        lanesTree.appendChild (l.toValueTree(), nullptr);

    writeMarkersToSessionTree (tree);
    writeAutomationBindingsToSessionTree (tree);
}

void ArrangementView::flushLanesToSession()
{
    /* Serialise lanes_ to the session tree, refresh the committed
     * baseline, and skip the ArrangementSnapshotAction push.  Mutation
     * sources that already own a global UndoManager entry (the
     * piano-roll's MidiNoteDiffCommand, the velocity-lane edit
     * command) call this instead of writeLanesToSession so Ctrl+Z
     * lands on the granular diff and not on a wrapper snapshot. */
    if (services_ == nullptr) return;

    auto sess = services_->context().session();
    if (sess == nullptr) return;

    auto tree = sess->data().getOrCreateChildWithName (tags::arrangement, nullptr);
    auto lanesTree = tree.getOrCreateChildWithName ("lanes", nullptr);
    lanesTree.removeAllChildren (nullptr);
    for (const auto& l : lanes_)
        lanesTree.appendChild (l.toValueTree(), nullptr);

    writeMarkersToSessionTree (tree);
    writeAutomationBindingsToSessionTree (tree);

    lastCommittedSnapshot_ = lanes_;
}

void ArrangementView::writeAutomationBindingsToSessionTree (juce::ValueTree& arrTree)
{
    auto bindingsTree = arrTree.getOrCreateChildWithName ("automationBindings", nullptr);
    bindingsTree.removeAllChildren (nullptr);
    for (const auto& b : automationBindings_)
        bindingsTree.appendChild (b.toValueTree(), nullptr);
}

automation::AutomationEngine* ArrangementView::activeAutomationEngine() const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return nullptr;
    if (auto* gn = dynamic_cast<GraphNode*> (active.getObject()))
        return gn->automationEngine();
    return nullptr;
}

juce::String ArrangementView::automationTargetDisplayName (
    const dsp::automation::AutomationTargetKey& key) const
{
    if (key.isMidi())
        return "CC " + juce::String (key.midiCcNumber);

    if (services_ != nullptr)
    {
        if (auto sess = services_->context().session())
        {
            const Node node = findNodeByUuid (sess->getActiveGraph(), key.nodeId);
            if (node.isValid())
            {
                if (auto* proc = node.getObject())
                {
                    const int idx = automation::decodeNodeParamId (key.paramId);
                    const auto& params = proc->getParameters (true);
                    if (idx >= 0 && idx < params.size())
                        if (auto p = params.getUnchecked (idx))
                        {
                            /* "Node: Param" -- in a modular graph the target
                             * usually isn't the lane's own source node, so the
                             * node name disambiguates which device is driven. */
                            const juce::String nodeName = node.getDisplayName();
                            const juce::String paramName = p->getName (40);
                            return nodeName.isNotEmpty()
                                       ? nodeName + ": " + paramName
                                       : paramName;
                        }
                }
            }
        }
    }
    return "Param";
}

juce::Uuid ArrangementView::addAutomationLane (const dsp::automation::AutomationTargetKey& key,
                                               juce::Uuid ownerLaneId)
{
    auto* engine = activeAutomationEngine();
    if (engine == nullptr || services_ == nullptr) return juce::Uuid::null();
    auto sess = services_->context().session();
    if (sess == nullptr) return juce::Uuid::null();

    using namespace element::dsp::automation;

    /* Default span: cover existing arrangement content, min 16 beats, so
     * the new curve is immediately visible + editable.  The user resizes
     * / extends the region in the overlay editor. */
    double span = 16.0;
    for (const auto& l : lanes_)
        for (const auto& r : l.playlist.regions())
            span = juce::jmax (span, r.positionBeats + r.lengthBeats);

    auto track = std::make_unique<AutomationTrack>();
    track->id        = juce::Uuid();
    track->targetKey = key;

    /* Seed a flat default region at neutral 0.5 spanning the timeline. */
    auto region = std::make_unique<AutomationRegion>();
    region->id            = juce::Uuid();
    region->positionBeats = 0.0;
    region->lengthBeats   = span;
    AutomationRegion::PointList pts {
        { 0.0,  0.5, {} },
        { span, 0.5, {} }
    };
    region->setPoints (pts);
    track->addRegion (std::move (region));
    track->setMode (AutomationMode::Read);

    auto* live = engine->addTrack (std::move (track));
    if (live == nullptr) return juce::Uuid::null();

    /* Resolve + bind the target now (internal-node params resolve
     * synchronously; async-loaded plugin params bind on a later pass). */
    automation::rebindEngineTargets (*engine, sess->getActiveGraph());

    AutomationBinding b;
    b.id          = juce::Uuid();
    b.trackId     = live->id;
    b.ownerLaneId = ownerLaneId;
    b.name        = automationTargetDisplayName (key);
    b.colour      = automationBindingColour (automationBindings_.size());
    automationBindings_.add (b);

    /* Persist: bindings ride tags::arrangement (writeLanesToSession's
     * binding write); engine tracks -> tags::automationTracks. */
    writeLanesToSession();
    automation::saveAutomationToSession (*engine, sess->data());

    /* A freshly-added overlay reveals expanded (the owner lane is not in
     * the collapsed set by default), so resize + repaint so the new
     * sub-row appears immediately. */
    if (! ownerLaneId.isNull())
        collapsedAutomationLanes_.erase (ownerLaneId);
    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaint();
    }

    return b.id;
}

void ArrangementView::removeAutomationBinding (juce::Uuid bindingId)
{
    int idx = -1;
    for (int i = 0; i < automationBindings_.size(); ++i)
        if (automationBindings_.getReference (i).id == bindingId)
        {
            idx = i;
            break;
        }
    if (idx < 0) return;

    const auto trackId = automationBindings_.getReference (idx).trackId;
    automationBindings_.remove (idx);

    /* Drop the backing engine track (epoch-gated trash reclaim inside)
     * then re-persist both presentation + engine state. */
    auto* engine = activeAutomationEngine();
    if (engine != nullptr && ! trackId.isNull())
        engine->removeTrack (trackId);

    writeLanesToSession();
    if (engine != nullptr && services_ != nullptr)
        if (auto sess = services_->context().session())
            automation::saveAutomationToSession (*engine, sess->data());

    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaint();
    }
}

int ArrangementView::automationBindingCountForLane (juce::Uuid laneId) const noexcept
{
    int n = 0;
    for (const auto& b : automationBindings_)
        if (b.ownerLaneId == laneId)
            ++n;
    return n;
}

bool ArrangementView::isLaneAutomationCollapsed (juce::Uuid laneId) const noexcept
{
    return collapsedAutomationLanes_.count (laneId) > 0;
}

void ArrangementView::setLaneAutomationCollapsed (juce::Uuid laneId, bool collapsed)
{
    if (collapsed)
        collapsedAutomationLanes_.insert (laneId);
    else
        collapsedAutomationLanes_.erase (laneId);

    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaint();
    }
}

namespace {
/** One automatable destination discovered in the active graph: a node +
 *  one of its automatable input params.  Flat list backs the picker; the
 *  menu groups consecutive same-node entries into a submenu. */
struct AutoParamTarget
{
    juce::Uuid   nodeId;
    juce::String nodeName;
    int          paramIndex = -1;
    juce::String paramName;
    juce::String paramLabel;
};

/** Recursively gather every automatable node param reachable from `root`
 *  (descends into subgraphs).  Node-by-node order, so the picker can group
 *  without re-sorting.  This is the modular-graph answer to "what can I
 *  automate": ANY node's param, not just the lane's own source node -- the
 *  overlay's ownerLaneId only governs which lane it's *drawn* under. */
void collectAutomatableTargets (const Node& root, std::vector<AutoParamTarget>& out)
{
    const int n = root.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = root.getNode (i);
        if (! child.isValid()) continue;

        if (auto* proc = child.getObject())
        {
            const auto params =
                automation::enumerateAutomatableParams (proc->getParameters (true));
            for (const auto& ap : params)
                out.push_back ({ child.getUuid(), child.getDisplayName(),
                                 ap.index, ap.name, ap.label });
        }

        if (child.isGraph())
            collectAutomatableTargets (child, out);
    }
}
} // namespace

void ArrangementView::showAddAutomationMenuForLane (int laneIdx,
                                                    juce::Rectangle<int> screenAnchor)
{
    if (laneIdx < 0 || laneIdx >= lanes_.size()) return;
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;

    const juce::Uuid laneId = lanes_.getReference (laneIdx).id;

    /* Enumerate EVERY automatable param in the active graph (modular: the
     * target is rarely the lane's own source node -- it's an effect / VST
     * param somewhere downstream).  Grouped per node into submenus. */
    std::vector<AutoParamTarget> targets;
    collectAutomatableTargets (sess->getActiveGraph(), targets);

    /* Pre-compute which targets are already bound on THIS lane so the menu
     * can tick them (avoids silently double-adding the same destination). */
    auto isBoundOnLane = [this, laneId] (const dsp::automation::AutomationTargetKey& key)
    {
        auto* engine = activeAutomationEngine();
        if (engine == nullptr) return false;
        for (const auto& b : automationBindings_)
            if (b.ownerLaneId == laneId)
                if (auto* trk = engine->findTrackById (b.trackId))
                    if (trk->targetKey == key) return true;
        return false;
    };

    /* Menu id scheme (full symmetry with the piano-roll #11): 1..N = a
     * graph param (index into `targets`); kCcIdBase + cc = a MIDI CC.  The
     * engine already drives BOTH kinds (AutomationTargetKey.isMidi()); a CC
     * track emits a controllerEvent into the graph MIDI buffer, channel-
     * routed to whatever consumes it (e.g. outboard gear / a synth on that
     * channel) -- same path the live MappingEngine uses. */
    constexpr int kCcIdBase = 100000;
    static const struct { int cc; const char* name; } kCommonCcs[] = {
        {  1, "Mod Wheel" }, {  2, "Breath" }, {  7, "Volume" }, { 10, "Pan" },
        { 11, "Expression" }, { 64, "Sustain" }, { 71, "Resonance" },
        { 74, "Cutoff" }, { 91, "Reverb" }, { 93, "Chorus" },
    };

    juce::PopupMenu menu;

    /* Params first, grouped per node. */
    size_t i = 0;
    while (i < targets.size())
    {
        const juce::Uuid nodeId = targets[i].nodeId;
        juce::PopupMenu sub;
        const juce::String nodeName = targets[i].nodeName.isNotEmpty()
                                        ? targets[i].nodeName : juce::String ("Node");
        while (i < targets.size() && targets[i].nodeId == nodeId)
        {
            const auto& t = targets[i];
            const auto key = automation::makeNodeParamKey (nodeId, t.paramIndex);
            juce::String label = t.paramName;
            if (t.paramLabel.isNotEmpty())
                label += " (" + t.paramLabel + ")";
            sub.addItem ((int) i + 1, label, true, isBoundOnLane (key));
            ++i;
        }
        menu.addSubMenu (nodeName, sub);
    }

    /* MIDI CC submenu (channel 1). */
    juce::PopupMenu ccMenu;
    for (const auto& c : kCommonCcs)
        ccMenu.addItem (kCcIdBase + c.cc,
                        "CC " + juce::String (c.cc) + "  " + juce::String (c.name),
                        true, isBoundOnLane (automation::makeMidiCcKey (1, c.cc)));
    menu.addSubMenu ("MIDI CC", ccMenu);

    /* Synchronous showAt -- the reliable pattern in this build (mouse
     * works, positions at the anchor).  withTargetScreenArea + async
     * mis-rendered detached at the window bottom under this windowing. */
    const int result = menu.showAt (screenAnchor);
    if (result <= 0) return;

    /* Confirm the lane still exists; ownerLaneId is presentation only. */
    bool laneStillThere = false;
    for (const auto& l : lanes_)
        if (l.id == laneId) { laneStillThere = true; break; }
    if (! laneStillThere) return;

    if (result >= kCcIdBase)
    {
        addAutomationLane (automation::makeMidiCcKey (1, result - kCcIdBase), laneId);
    }
    else if (result <= (int) targets.size())
    {
        const auto& t = targets[(size_t) (result - 1)];
        addAutomationLane (automation::makeNodeParamKey (t.nodeId, t.paramIndex), laneId);
    }
}

void ArrangementView::writeMarkersToSessionTree (juce::ValueTree& arrTree)
{
    /* Mirror the lanes-tree shape: clear + rebuild the <markers>
     * child every write so deletions / renames flush cleanly.  Marker
     * mutations themselves (add/rename/recolour/delete) go through
     * writeMarkersOnly so a marker edit doesn't accidentally push an
     * ArrangementSnapshotAction for the unchanged lanes_. */
    for (int i = arrTree.getNumChildren() - 1; i >= 0; --i)
    {
        const auto child = arrTree.getChild (i);
        if (child.hasType (juce::Identifier ("markers")))
            arrTree.removeChild (i, nullptr);
    }
    if (! markerTrack_.markers().empty())
        arrTree.appendChild (markerTrack_.toValueTree(), nullptr);
}

void ArrangementView::writeMarkersOnly()
{
    /* Marker mutations skip the lanes_ undo snapshot.  Markers ride
     * outside the per-action undo system for v1 -- they're cheap to
     * recreate manually and not in the hot lane-edit loop. */
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto tree = sess->data().getOrCreateChildWithName (tags::arrangement, nullptr);
    writeMarkersToSessionTree (tree);
}

void ArrangementView::applyLaneSnapshot (const juce::Array<Lane>& snap)
{
    /* Re-entrancy guard: writeLanesToSession runs inside this method
     * (for persistence) and rescanLaneTargets may also call it via
     * its "mutated" path -- both must skip pushing a new action since
     * the change came from the UndoManager replay, not the user. */
    applyingUndoAction_ = true;
    lanes_ = snap;
    lastCommittedSnapshot_ = snap;
    writeLanesToSession();
    rescanLaneTargets();
    applyingUndoAction_ = false;

    if (body_ != nullptr)
    {
        /* Selection may point at regions that no longer exist in the
         * restored snapshot.  Cheaper + safer to clear than to walk
         * the new lanes_ confirming each uuid still resolves. */
        body_->clearSelection();
        body_->resizeForLanes();
        body_->repaint();
    }
}

void ArrangementView::loadZoomFromSession()
{
    if (services_ == nullptr || body_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return;
    auto vs = tree.getChildWithName ("viewState");
    if (! vs.isValid()) return;

    /* Horizontal + vertical zoom are clamped to Body's defined limits
     * so a session edited under a different build can't park kPxPerBeat
     * outside [4, 256] or kLaneH outside [40, 240]. */
    body_->kPxPerBeat = juce::jlimit (Body::kPxPerBeatMin, Body::kPxPerBeatMax,
        (int) vs.getProperty ("pxPerBeat", body_->kPxPerBeat));
    body_->kLaneH = juce::jlimit (Body::kLaneHMin, Body::kLaneHMax,
        (int) vs.getProperty ("laneH", body_->kLaneH));
    body_->resizeForLanes();
}

bool ArrangementView::sessionHasSavedZoom() const
{
    if (services_ == nullptr) return false;
    auto sess = services_->context().session();
    if (sess == nullptr) return false;
    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return false;
    auto vs = tree.getChildWithName ("viewState");
    if (! vs.isValid()) return false;
    return vs.hasProperty ("pxPerBeat");
}

void ArrangementView::maybeAutoFitOnLoad()
{
    if (body_ == nullptr) return;

    double maxEndBeats = 0.0;
    for (const auto& l : lanes_)
    {
        for (const auto& r : l.playlist.regions())
            maxEndBeats = juce::jmax (maxEndBeats, r.endBeats());
        for (const auto& m : l.playlist.midiRegions())
            if (m != nullptr)
                maxEndBeats = juce::jmax (maxEndBeats,
                                            m->positionBeats + m->lengthBeats);
    }
    if (maxEndBeats <= 0.0) return;

    /* Two trigger conditions for auto-fit:
     *   1. No saved zoom at all (fresh session, never been fit).
     *   2. Saved zoom but content OVERFLOWS the viewport (the saved
     *      value is stale -- e.g. user added regions since last
     *      save, or the window is narrower than when it was saved,
     *      OR the saved value was wrong because the old buggy
     *      zoomToFit chronically undershot).
     * When the saved zoom DOES fit, leave it alone -- the user
     * picked it deliberately. */
    const bool hasSaved = sessionHasSavedZoom();
    bool overflows = false;
    if (hasSaved)
    {
        const int contentW = Body::kLabelW
                              + (int) (maxEndBeats + (double) Body::kFitPaddingBeats)
                                * body_->kPxPerBeat;
        const int viewW = viewport_.getMaximumVisibleWidth();
        overflows = contentW > viewW;
    }

    if (! hasSaved || overflows)
        body_->zoomToFit();
}

void ArrangementView::loadViewStateFromSession()
{
    /* Scroll only -- zoom is applied synchronously by
     * loadZoomFromSession from didBecomeActive so the first paint
     * already has the saved kPxPerBeat / kLaneH.  This deferred path
     * waits for the parent's resized() to size viewport_, then
     * restores the saved scroll offset. */
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return;
    auto vs = tree.getChildWithName ("viewState");
    if (! vs.isValid()) return;

    const int scrollX = (int) vs.getProperty ("scrollX", 0);
    const int scrollY = (int) vs.getProperty ("scrollY", 0);
    viewport_.setViewPosition (scrollX, scrollY);
}

void ArrangementView::writeViewStateToSession()
{
    if (services_ == nullptr || body_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    /* Session-identity guard: sigSessionLoaded fires AFTER the
     * context's session pointer swaps, so willBeRemoved on this
     * cached view runs against the NEW session.  Without the guard
     * we'd overwrite the freshly-loaded NEW session's viewState VT
     * with this OLD view's zoom/scroll.  Mirrors the SessionView
     * fix that resolved the test.sls clip-wipe regression. */
    if (initialSessionTree_.isValid() && sess->data() != initialSessionTree_)
        return;
    auto tree = sess->data().getOrCreateChildWithName (tags::arrangement, nullptr);
    auto vs   = tree.getOrCreateChildWithName ("viewState", nullptr);
    vs.setProperty ("pxPerBeat", body_->kPxPerBeat,           nullptr);
    vs.setProperty ("laneH",     body_->kLaneH,               nullptr);
    vs.setProperty ("scrollX",   viewport_.getViewPositionX(), nullptr);
    vs.setProperty ("scrollY",   viewport_.getViewPositionY(), nullptr);
}

double ArrangementView::computePlayheadBeats() const
{
    if (monitor_ == nullptr) return 0.0;
    return (double) monitor_->getPositionBeats();
}

void ArrangementView::dispatchAtBeat (double beat)
{
    const double bpm = monitor_ != nullptr ? (double) monitor_->tempo.get() : 120.0;

    for (int laneIdx = 0; laneIdx < lanes_.size(); ++laneIdx)
    {
        const auto& lane    = lanes_.getReference (laneIdx);
        auto&       runtime = laneRuntime_.getReference (laneIdx);
        if (runtime.isOrphan()) continue;

        const Region* active = lane.playlist.regionAt (beat);

        /* Case 1: playhead is outside any region.  Stop whatever was
         * playing on this lane.  Tracker lanes used to leak past the
         * region edge (vht sequence_advance always wraps internally)
         * so the looped flag was audibly a no-op.  Now both lane
         * kinds schedule-stop on gap exit. */
        if (active == nullptr)
        {
            if (! runtime.lastDispatchedRegion.isNull())
            {
                if (runtime.isAudioLane())
                    runtime.audioClipCache->scheduleStop (
                        runtime.lastDispatchedRegion, -1.0);
                else if (runtime.isTrackerLane()
                         && runtime.lastDispatchedSeqIdx >= 0)
                    runtime.trackerCache->schedulePlaying (
                        runtime.lastDispatchedSeqIdx, -1.0, false);
                runtime.lastDispatchedRegion = juce::Uuid::null();
                runtime.lastDispatchedSeqIdx = -1;
                /* No repaintLane: per-region visual no longer differs
                 * by active state, so the dispatch transition is
                 * model-only. */
            }
            continue;
        }

        /* Case 2: still inside the same region the lane is already
         * playing.  No state change. */
        if (active->id == runtime.lastDispatchedRegion)
            continue;

        /* Case 3: entering a new region (either first region under
         * the playhead OR transitioning from a different region).
         * Stop the prior + launch the new. */
        if (! runtime.lastDispatchedRegion.isNull())
        {
            if (runtime.isAudioLane())
                runtime.audioClipCache->scheduleStop (
                    runtime.lastDispatchedRegion, -1.0);
            /* Tracker lanes: existing pattern stays "playing" in
             * vht state; the new schedulePlaying below explicitly
             * stops the old sequence + starts the new. */
        }

        if (runtime.isTrackerLane())
        {
            if (active->sequenceIdx >= 0)
            {
                if (runtime.lastDispatchedSeqIdx >= 0
                    && runtime.lastDispatchedSeqIdx != active->sequenceIdx)
                    runtime.trackerCache->schedulePlaying (
                        runtime.lastDispatchedSeqIdx, -1.0, false);
                runtime.trackerCache->schedulePlaying (
                    active->sequenceIdx, -1.0, true);

                /* Non-looped tracker regions: schedule the cutoff at
                 * positionBeats + min(lengthBeats, sequence_length_beats).
                 * Pattern shorter than the region plays once then
                 * silences for the remainder; pattern longer than the
                 * region gets truncated at the region edge.  Looped
                 * regions skip this -- vht wraps naturally and the
                 * Case 1 gap-exit branch handles the stop on region
                 * exit. */
                if (! active->looped)
                {
                    const double seqLenBeats =
                        runtime.trackerCache->getSequenceLengthBeats (active->sequenceIdx);
                    if (seqLenBeats > 0.0)
                    {
                        const double cutoffOffset = juce::jmin (active->lengthBeats,
                                                                seqLenBeats);
                        runtime.trackerCache->schedulePlaying (
                            active->sequenceIdx,
                            active->positionBeats + cutoffOffset,
                            false);
                    }
                }
            }
        }
        else if (runtime.isAudioLane())
        {
            /* Mid-region launch: compute the source-sample offset so
             * the playback head matches where the transport is
             * within the region.  bumping into a region from beat 0
             * fires the source from its beginning (modulo startBeats
             * trim); bumping in at beat 2 of a 4-beat region whose
             * startBeats=0 fires from 2 beats into the source. */
            const double beatIntoRegion = juce::jmax (
                0.0, beat - active->positionBeats);
            const double sourceBeat = active->startBeats + beatIntoRegion;

            juce::int64 sampleOffset = 0;
            if (auto src = SourceRegistry::get().findAudioFile (active->sourceId))
            {
                if (src->sourceSampleRate() > 0 && bpm > 0.0)
                {
                    const double sourceSec = sourceBeat * (60.0 / bpm);
                    sampleOffset = (juce::int64) (sourceSec
                        * (double) src->sourceSampleRate());
                }
            }

            /* Convert region beat-domain envelope params to source
             * samples for the audio-thread envelope code. */
            const double sessionSampleRate = monitor_ != nullptr
                ? (double) monitor_->sampleRate.get()
                : 48000.0;
            const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
            const juce::int64 fadeInSamples
                = (juce::int64) (active->fadeInBeats  * secsPerBeat * sessionSampleRate);
            const juce::int64 fadeOutSamples
                = (juce::int64) (active->fadeOutBeats * secsPerBeat * sessionSampleRate);
            const juce::int64 regionLenSamples
                = (juce::int64) (active->lengthBeats  * secsPerBeat * sessionSampleRate);

            /* beatTarget = the region's positionBeats so AudioClipNode's
             * audio-thread applyPendingForBlock fires at the block
             * whose range contains it (or catches up if we already
             * passed it).  Sample-accurate to +/- one block ~ 5-10 ms,
             * vs. ~33 ms latency on the prior immediate-only path. */
            runtime.audioClipCache->schedulePlay (
                active->id, active->sourceId,
                active->positionBeats,
                sampleOffset,
                active->looped,
                active->gainDb,
                fadeInSamples, fadeOutSamples, regionLenSamples,
                active->fadeInCurve, active->fadeOutCurve);
        }

        runtime.lastDispatchedRegion = active->id;
        runtime.lastDispatchedSeqIdx = active->sequenceIdx;
        /* No repaintLane on dispatch transition -- per-region visual
         * is invariant to playhead-over-region state now. */
    }
}

void ArrangementView::stopAllAudioLanes()
{
    for (auto& rs : laneRuntime_)
        if (rs.isAudioLane())
            rs.audioClipCache->scheduleStop (juce::Uuid::null(), -1.0);
}

void ArrangementView::propagateMuteSolo()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return;

    /* Any-soloed gate: in Bitwig / Ableton / Ardour, the moment one
     * lane is soloed, every non-soloed lane is effectively muted. */
    bool anySoloed = false;
    for (const auto& l : lanes_)
        if (l.soloed) { anySoloed = true; break; }

    for (int i = 0; i < lanes_.size(); ++i)
    {
        const auto& l = lanes_.getReference (i);
        const bool effMuted = l.muted || (anySoloed && ! l.soloed);

        const Node target = findNodeByUuid (active, l.targetNodeUuid);
        if (! target.isValid()) continue;
        if (auto* proc = target.getObject())
            proc->setMuted (effMuted);

        /* TrackerNode + MidiPlayerNode also expose their own
         * session-view mute / solo state so the SessionView grid +
         * editor popups stay in sync.  Audio lanes go through
         * Processor::setMuted only -- the AudioProcessorNode
         * wrapper's setMuted gates the audio graph layer, which is
         * enough.
         *
         * Note: SessionView ALSO writes these flags from its own
         * column header (toggleColumnMute / toggleColumnSolo) +
         * reconciles effective Processor::setMuted via
         * applyMuteAndSoloState in its 30 Hz tick.  The two writers
         * race in the user's favour -- whichever surface they
         * touched most recently wins -- which matches Ableton's
         * "track header mute is a single global thing" UX. */
        const auto& rs = laneRuntime_.getReference (i);
        if (rs.isTrackerLane())
        {
            rs.trackerCache->setUserMuted (l.muted);
            rs.trackerCache->setSoloed (l.soloed);
        }
        else if (rs.isMidiPlayerLane())
        {
            rs.midiPlayerCache->setUserMuted (l.muted);
            rs.midiPlayerCache->setSoloed (l.soloed);
        }
    }
}

int ArrangementView::createEmptyAudioLane (bool stereo)
{
    if (services_ == nullptr)
    {
        juce::Logger::writeToLog ("[ArrangementView::createEmptyAudioLane] services_ null");
        return -1;
    }
    auto sess = services_->context().session();
    if (sess == nullptr)
    {
        juce::Logger::writeToLog ("[ArrangementView::createEmptyAudioLane] session null");
        return -1;
    }
    auto* engineService = services_->find<EngineService>();
    if (engineService == nullptr)
    {
        juce::Logger::writeToLog ("[ArrangementView::createEmptyAudioLane] EngineService null");
        return -1;
    }

    Node subgraph = ArrangementTracksService::findOrCreateSubgraph (*engineService, *sess);
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView::createEmptyAudioLane] subgraph valid=")
        + (subgraph.isValid() ? "yes" : "no")
        + " isGraph=" + (subgraph.isValid() && subgraph.isGraph() ? "yes" : "no")
        + " uuid=" + (subgraph.isValid() ? subgraph.getUuid().toString() : juce::String ("(none)"))
        + " numChildren=" + juce::String (subgraph.isValid() ? subgraph.getNumNodes() : 0));
    if (! subgraph.isValid()) return -1;

    Node clip = ArrangementTracksService::addAudioClipNode (*engineService, subgraph, stereo);
    if (! clip.isValid()) return -1;

    /* Canonical "AudioTrk N" naming -- N is lowest-free integer
     * across the full graph so the graph block + lane label match.
     * Scoped to the active graph (root) since AudioClipNodes live
     * inside Multi-Track; nextNumberedNodeName recurses into it. */
    auto activeGraph = sess->getActiveGraph();
    const auto canonicalName = nextNumberedNodeName (activeGraph, "AudioTrk");
    clip.setName (canonicalName);

    Lane lane;
    lane.id             = juce::Uuid();
    lane.kind           = Lane::Kind::Audio;
    lane.targetNodeUuid = clip.getUuid();
    lane.name           = canonicalName;
    lane.colour         = laneTintForIndex (lanes_.size());
    lanes_.add (std::move (lane));

    rescanLaneTargets();   // resolves the new lane's audioClipCache
    writeLanesToSession();
    if (body_ != nullptr) body_->resizeForLanes();
    return lanes_.size() - 1;
}

void ArrangementView::moveLane (int laneIdx, int delta)
{
    if (laneIdx < 0 || laneIdx >= lanes_.size()) return;
    const int target = laneIdx + delta;
    if (target < 0 || target >= lanes_.size()) return;

    /* Swap lane + runtime entries together so audio-thread emission
     * keeps the right cache binding.  Palette tint reflows on the
     * next rescan; for now just swap colours too so the visual order
     * stays stable until rescan repaints. */
    lanes_.swap (laneIdx, target);
    laneRuntime_.swap (laneIdx, target);

    writeLanesToSession();
    if (body_ != nullptr) body_->resizeForLanes();
    repaint();
}

void ArrangementView::removeLane (int laneIdx)
{
    if (laneIdx < 0 || laneIdx >= lanes_.size()) return;
    if (services_ == nullptr) return;
    auto* engine = services_->find<EngineService>();
    if (engine == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;

    /* Resolve the host node by uuid (stored on the lane).  If the
     * node still exists, remove it through the engine; rescanLane-
     * Targets + the orphan-purge pass will then drop the lane.  If
     * the node is already gone (orphan-with-content case) we still
     * need to drop the lane directly here so the user can manually
     * clean up an "?" badge they no longer want. */
    const juce::Uuid targetUuid = lanes_.getReference (laneIdx).targetNodeUuid;
    const Node target = findNodeByUuid (graph, targetUuid);
    if (target.isValid())
    {
        engine->removeNode (target.getNodeId());
        rescanLaneTargets();   // orphan-purge drops the lane
        return;
    }

    /* Direct lane removal for orphan lanes. */
    lanes_.remove (laneIdx);
    if (laneIdx < laneRuntime_.size())
        laneRuntime_.remove (laneIdx);
    writeLanesToSession();
    if (body_ != nullptr) body_->resizeForLanes();
    repaint();
}

int ArrangementView::createEmptyMidiLane()
{
    /* MIDI lane creation spawns a MidiPlayerNode at the ROOT graph
     * level (NOT inside the Multi-Track subgraph -- matches the
     * TrackerNode "clip source" pattern).  One MidiPlayerNode appears
     * in BOTH the session view (as a column) and the arrangement
     * view (as a lane).  The node's MIDI output is left unwired;
     * the user routes it into a Sampler / synth via the main graph
     * (same workflow as TrackerNode).  Lane auto-fill happens via
     * rescanLaneTargets right after the node lands. */
    if (services_ == nullptr) return -1;
    auto sess = services_->context().session();
    if (sess == nullptr) return -1;
    auto* engineService = services_->find<EngineService>();
    if (engineService == nullptr) return -1;

    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return -1;

    juce::PluginDescription desc;
    desc.fileOrIdentifier = EL_NODE_ID_MIDI_PLAYER;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.name             = "MIDI Player";

    Node player = engineService->addPlugin (graph, desc);
    if (! player.isValid()) return -1;

    /* Rename to the canonical "MidiSeq N" form so the graph block,
     * arrangement lane, and session column all show the same label.
     * N = lowest free integer across the entire graph (handles
     * deletes + reuse). */
    player.setName (nextNumberedNodeName (graph, "MidiSeq"));

    /* rescanLaneTargets auto-fills an empty MIDI lane for the new
     * node (via the auto-fill loop covering both tracker + midi clip
     * sources).  Find the lane by uuid afterwards to return its
     * index. */
    rescanLaneTargets();
    const auto uuid = player.getUuid();
    for (int i = 0; i < lanes_.size(); ++i)
        if (lanes_.getReference (i).targetNodeUuid == uuid)
            return i;
    return -1;
}

bool ArrangementView::importAudioFileToLane (const juce::File& file,
                                             int               laneIdx,
                                             double            positionBeats)
{
    if (! file.existsAsFile())
        return false;

    auto src = SourceRegistry::get().importFromFile (file);
    if (src == nullptr) return false;

    /* Create lane if requested.  -1 = always create new. */
    if (laneIdx < 0 || laneIdx >= lanes_.size())
    {
        const int newIdx = createEmptyAudioLane (true /*stereo*/);
        if (newIdx < 0)
            return false;
        laneIdx = newIdx;
    }
    else
    {
        const auto& runtime = laneRuntime_.getReference (laneIdx);
        if (! runtime.isAudioLane())
            return false;
    }

    auto& lane = lanes_.getReference (laneIdx);

    /* Compute lengthBeats from file duration + current session bpm. */
    const double bpm = monitor_ != nullptr
                          ? (double) monitor_->tempo.get()
                          : 120.0;
    const double lengthSeconds = src->sourceSampleRate() > 0
        ? (double) src->durationSamples() / (double) src->sourceSampleRate()
        : 0.0;
    const double lengthBeats = juce::jmax (
        0.25,
        lengthSeconds * (bpm / 60.0));

    Region r;
    r.id            = juce::Uuid();
    r.sourceId      = src->uuid();
    r.positionBeats = juce::jmax (0.0, positionBeats);
    r.lengthBeats   = lengthBeats;
    r.name          = file.getFileNameWithoutExtension();
    /* Inherit lane tint -- single source of truth at lane.colour. */
    r.colour        = lane.colour;

    if (! lane.playlist.addRegion (Region (r)))
    {
        /* Overlap rejected; nudge to next free spot beyond the last
         * region's end. */
        double cursor = 0.0;
        for (const auto& existing : lane.playlist.regions())
            cursor = juce::jmax (cursor, existing.endBeats());
        r.positionBeats = cursor;
        if (! lane.playlist.addRegion (Region (r)))
            return false;   // shouldn't happen; defensive
    }

    /* Fire immediately so the user gets audible confirmation.
     * Subsequent transport playback will re-dispatch at the region
     * boundary. */
    auto& runtime = laneRuntime_.getReference (laneIdx);
    if (runtime.isAudioLane())
    {
        const double sessionSR = monitor_ != nullptr
            ? (double) monitor_->sampleRate.get() : 48000.0;
        const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
        runtime.audioClipCache->schedulePlay (
            r.id, r.sourceId, -1.0, 0, r.looped, r.gainDb,
            (juce::int64) (r.fadeInBeats  * secsPerBeat * sessionSR),
            (juce::int64) (r.fadeOutBeats * secsPerBeat * sessionSR),
            (juce::int64) (r.lengthBeats  * secsPerBeat * sessionSR),
            r.fadeInCurve, r.fadeOutCurve);
    }

    writeLanesToSession();
    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaintLane (laneIdx);
    }
    return true;
}

bool ArrangementView::importMidiFileToLane (const juce::File& file,
                                             int               laneIdx,
                                             double            positionBeats)
{
    if (! file.existsAsFile())
        return false;

    auto src = SourceRegistry::get().importMidiFromFile (file);
    if (src == nullptr) return false;

    /* Create lane if requested or the supplied lane is the wrong kind. */
    if (laneIdx < 0 || laneIdx >= lanes_.size()
        || lanes_.getReference (laneIdx).kind != Lane::Kind::Midi)
    {
        const int newIdx = createEmptyMidiLane();
        if (newIdx < 0) return false;
        laneIdx = newIdx;
    }

    auto& lane = lanes_.getReference (laneIdx);

    /* Decode the SMF into a NoteList + region.  juce::MidiFile inside
     * MidiSource holds the source-of-truth bytes; the region owns its
     * own COW snapshot so subsequent user edits don't affect the
     * source.  Beat-domain length = max(noteOff) for now -- callers
     * resizing the region UI-side just adjust lane.midiRegions[i]
     * lengthBeats. */
    const auto mf      = src->toMidiFile();
    auto       notes   = MidiSource::extractNotes (mf);
    const double srcBeats = src->durationBeats (0.0, 0.0);
    const double regionLen = juce::jmax (0.25, srcBeats);

    auto region = std::make_unique<MidiNoteRegion>();
    region->id            = juce::Uuid();
    region->sourceId      = src->uuid();
    region->positionBeats = juce::jmax (0.0, positionBeats);
    region->lengthBeats   = regionLen;
    region->name          = file.getFileNameWithoutExtension();
    /* Inherit lane tint -- piano-roll paintNotes reads region->colour. */
    region->colour        = lane.colour;
    /* setNotesAssigningIds stamps fresh per-note ids so the piano-roll
     * selection model has stable identities across snapshot swaps. */
    region->setNotesAssigningIds (std::move (notes));

    if (! lane.playlist.addMidiRegion (std::move (region)))
        return false;

    /* Republish the lane's region table onto its MidiPlayerNode so
     * the audio thread starts emitting from the new region on the
     * next transport tick. */
    publishMidiBindingsForLane (laneIdx);

    writeLanesToSession();
    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaintLane (laneIdx);
    }
    return true;
}

juce::Uuid ArrangementView::createEmptyMidiRegion (int    laneIdx,
                                                     double positionBeats,
                                                     double lengthBeats)
{
    if (laneIdx < 0 || laneIdx >= lanes_.size())
        return juce::Uuid::null();

    auto& lane = lanes_.getReference (laneIdx);
    if (lane.kind != Lane::Kind::Midi)
        return juce::Uuid::null();

    auto region = std::make_unique<MidiNoteRegion>();
    region->id            = juce::Uuid();
    region->positionBeats = juce::jmax (0.0, positionBeats);
    region->lengthBeats   = juce::jmax (0.25, lengthBeats);
    region->name          = juce::String ("MIDI ") + juce::String (lane.playlist.midiRegions().size() + 1);
    /* Inherit lane tint -- piano-roll paintNotes reads region->colour. */
    region->colour        = lane.colour;

    const juce::Uuid newId = region->id;

    if (! lane.playlist.addMidiRegion (std::move (region)))
        return juce::Uuid::null();

    publishMidiBindingsForLane (laneIdx);
    writeLanesToSession();
    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaintLane (laneIdx);
    }
    return newId;
}

namespace {
/* Resolve a clip-local param lane's (nodeId, paramId) to a live
 * AutomationTarget (NodeParam kind, holding the ParameterPtr by value so
 * it survives transient graph rebuilds).  Mirrors
 * automation::rebindEngineTargets.  Returns false (target stays Invalid)
 * when the node/param can't be resolved -- the audio thread then simply
 * finds no binding and skips applying that lane. */
bool buildClipParamBinding (const Node& graph,
                            const juce::Uuid& nodeId, const juce::String& paramId,
                            MidiPlayerNode::ParamBinding& out)
{
    const int idx = automation::decodeNodeParamId (paramId);
    if (idx < 0) return false;
    const Node node = findNodeByUuid (graph, nodeId);
    if (! node.isValid()) return false;
    auto* proc = node.getObject();
    if (proc == nullptr) return false;
    const auto& params = proc->getParameters (true);
    if (idx >= params.size()) return false;
    auto p = params.getUnchecked (idx);
    if (p == nullptr) return false;

    out.nodeId        = nodeId;
    out.paramId       = paramId;
    out.target.kind   = automation::AutomationTarget::Kind::NodeParam;
    out.target.nodeParam = p;
    return true;
}
} // namespace

void ArrangementView::publishMidiBindingsForLane (int laneIdx)
{
    if (laneIdx < 0 || laneIdx >= lanes_.size()) return;
    if (laneIdx >= laneRuntime_.size()) return;

    auto& runtime = laneRuntime_.getReference (laneIdx);
    if (runtime.midiPlayerCache == nullptr) return;

    const auto& lane = lanes_.getReference (laneIdx);
    std::vector<MidiPlayerNode::RegionEntry> entries;
    entries.reserve (lane.playlist.midiRegions().size());

    /* Param-lane bindings published alongside the region table: walk every
     * region's clip-local param lanes, resolve each target against the live
     * graph, and hand the player node a fresh binding table (#11 Phase 2c).
     * CC lanes need no binding -- the node reads their points live from the
     * region snapshot + emits MIDI; only param lanes need a resolved
     * destination. */
    std::vector<MidiPlayerNode::ParamBinding> paramBindings;
    Node graph;
    if (services_ != nullptr)
        if (auto sess = services_->context().session())
            graph = sess->getActiveGraph();

    for (const auto& mp : lane.playlist.midiRegions())
    {
        if (mp == nullptr) continue;
        MidiPlayerNode::RegionEntry e;
        e.region        = mp.get();
        e.positionBeats = mp->positionBeats;
        e.lengthBeats   = mp->lengthBeats;
        e.startBeats    = mp->startBeats;
        e.looped        = mp->looped;
        e.loopLengthBeats = mp->loopLengthBeats;
        entries.push_back (e);

        if (graph.isValid())
            if (const auto* cc = mp->loadCcSnapshot())
                for (const auto& cl : *cc)
                {
                    if (! cl.isParam()) continue;
                    MidiPlayerNode::ParamBinding b;
                    if (buildClipParamBinding (graph, cl.paramNodeId, cl.paramId, b))
                        paramBindings.push_back (std::move (b));
                }
    }
    runtime.midiPlayerCache->setBoundRegions (std::move (entries));
    runtime.midiPlayerCache->setBoundParams  (std::move (paramBindings));
}

void ArrangementView::republishMidiBindingsForRegion (const juce::Uuid& regionId)
{
    if (regionId.isNull()) return;
    for (int i = 0; i < lanes_.size(); ++i)
        if (lanes_.getReference (i).playlist.findMidiRegion (regionId) != nullptr)
        {
            publishMidiBindingsForLane (i);
            return;
        }
}

int ArrangementView::laneIdxFromY (int yPx) const noexcept
{
    /* Body coordinate; account for the top ruler row.  Negative or
     * inside-ruler y returns -1 (no lane at that y).  Delegates to
     * Body::rowAtY so the cumulative variable-row geometry stays the
     * single source of truth (handles expanded automation overlays). */
    if (body_ == nullptr) return -1;
    const int idx = body_->rowAtY (yPx);
    if (idx < 0 || idx >= lanes_.size()) return -1;
    return idx;
}

void ArrangementView::updateTransportLabel()
{
    /* BPM + Beat read-outs moved to the global transport bar (above
     * the content view), so this is a no-op now.  Kept as a stub so
     * timerCallback's call site stays valid without conditional
     * checks; remove once a follow-up audits timerCallback. */
}

void ArrangementView::syncToolToggleStates()
{
    if (body_ == nullptr) return;
    const auto tool = body_->getActiveTool();
    toolSelectBtn_  .setToggleState (tool == Body::Tool::Select,   juce::dontSendNotification);
    toolRangeBtn_   .setToggleState (tool == Body::Tool::Range,    juce::dontSendNotification);
    toolSplitBtn_   .setToggleState (tool == Body::Tool::Split,    juce::dontSendNotification);
    toolTrimBtn_    .setToggleState (tool == Body::Tool::Trim,     juce::dontSendNotification);
    toolAuditionBtn_.setToggleState (tool == Body::Tool::Audition, juce::dontSendNotification);
    toolEnvBtn_     .setToggleState (tool == Body::Tool::Env,      juce::dontSendNotification);
}

void ArrangementView::onLoopToggled()
{
    if (body_ == nullptr) return;
    body_->setLoopActive (loopBtn_.getToggleState());
}

int64_t ArrangementView::beatsToFrames (double beats) const noexcept
{
    if (monitor_ == nullptr) return 0;
    const double bpm = juce::jmax (1.0, (double) monitor_->tempo.get());
    const double sr  = juce::jmax (1.0, (double) monitor_->sampleRate.get());
    const double secs = beats * 60.0 / bpm;
    return (int64_t) std::llround (secs * sr);
}

double ArrangementView::framesToBeats (int64_t frames) const noexcept
{
    if (monitor_ == nullptr) return 0.0;
    const double bpm = juce::jmax (1.0, (double) monitor_->tempo.get());
    const double sr  = juce::jmax (1.0, (double) monitor_->sampleRate.get());
    const double secs = (double) frames / sr;
    return secs * bpm / 60.0;
}

void ArrangementView::seekToBeat (double beats)
{
    if (services_ == nullptr) return;
    if (auto* eng = services_->context().audio().get())
        eng->seekToAudioFrame (beatsToFrames (juce::jmax (0.0, beats)));
}

void ArrangementView::timerCallback()
{
    if (monitor_ == nullptr) return;

    const bool playing   = monitor_->playing.get();
    const bool recording = monitor_->recording.get();
    const double beat    = computePlayheadBeats();

    /* Transport start edge: clear per-lane "last dispatched" so the
     * first region under the playhead fires when playback resumes. */
    if (! wasPlaying_ && playing)
        for (auto& rs : laneRuntime_)
        {
            rs.lastDispatchedRegion = juce::Uuid::null();
            rs.lastDispatchedSeqIdx = -1;
        }

    /* Transport stop edge: silence audio lanes so playback halts on
     * stop.  Tracker lanes don't get an explicit stop here -- their
     * sequences carry "playing" state in vht state and stop on next
     * launch start.  DAW convention is audio stops with transport. */
    if (wasPlaying_ && ! playing)
        stopAllAudioLanes();

    /* Transport-recording rising edge: snapshot playhead position
     * so Body can paint a placeholder growing rect from here to
     * the current playhead on each armed lane. */
    if (recording && ! wasRecording_)
        recordStartBeat_ = beat;

    /* Repaint armed lanes while transport-recording so the REC
     * indicator + growing-placeholder rect refresh.  The
     * dispatchAtBeat path only repaints lanes that change region
     * state -- which doesn't happen during capture (no region
     * exists until finalise). */
    const bool recordingStateChanged = (recording != wasRecording_);
    if (recording || recordingStateChanged)
    {
        if (body_ != nullptr)
        {
            /* Grow the body when the live recording crosses the
             * current right edge so the ruler + placeholder keep
             * drawing past the previous extent.  Without this, the
             * timeline freezes at the last-region width and the
             * placeholder gets clipped until commit on stop.
             * body_resizeNeeded gates the call so non-growth ticks
             * (30 Hz) don't trigger a full-body repaint. */
            if (recording && body_->body_resizeNeeded())
                body_->resizeForLanes();
            for (int i = 0; i < lanes_.size(); ++i)
            {
                const auto& l = lanes_.getReference (i);
                const auto& rs = laneRuntime_.getReference (i);
                if (rs.isAudioLane() && l.armed)
                    body_->repaintLane (i);
            }
        }
    }
    wasRecording_ = recording;

    /* Loop wrap: if loop is armed and the playhead has crossed the
     * range-end boundary, seek the transport back to range-start.
     * Driven from the 30 Hz UI timer -- one tick of jitter at the
     * wrap is acceptable for v1; future move to engine-side seek
     * for sample-accurate loop. */
    if (playing && body_ != nullptr && body_->isLooping())
    {
        const double loopStart = body_->rangeStart();
        const double loopEnd   = body_->rangeEnd();
        if (beat >= loopEnd - 1e-6 && loopEnd > loopStart)
        {
            seekToBeat (loopStart);
            for (auto& rs : laneRuntime_)
            {
                rs.lastDispatchedRegion = juce::Uuid::null();
                rs.lastDispatchedSeqIdx = -1;
            }
        }
    }

    /* Clip volume envelope -- push the evaluated live gain to each
     * audio lane whose last-dispatched region carries an envelope.
     * 30 Hz update rate; coarser than per-sample but matches Bitwig's
     * display refresh + is plenty for most musical envelopes.  Lanes
     * without an envelope clear the override (NaN) so the per-launch
     * static gain takes back over. */
    if (playing)
    {
        for (int i = 0; i < lanes_.size(); ++i)
        {
            auto& rs = laneRuntime_.getReference (i);
            if (! rs.isAudioLane()) continue;
            if (rs.lastDispatchedRegion.isNull())
            {
                rs.audioClipCache->setLiveGain (std::numeric_limits<float>::quiet_NaN());
                continue;
            }
            const auto& lane = lanes_.getReference (i);
            const auto* r = lane.playlist.findRegion (rs.lastDispatchedRegion);
            if (r == nullptr || r->volumeEnvelope.size() < 2)
            {
                rs.audioClipCache->setLiveGain (std::numeric_limits<float>::quiet_NaN());
                continue;
            }
            const double localBeat = beat - r->positionBeats;
            const float  envDb = r->gainAtBeatOffset (localBeat);
            const float  linear = juce::Decibels::decibelsToGain (envDb);
            rs.audioClipCache->setLiveGain (linear);
        }
    }

    if (playing) dispatchAtBeat (beat);

    /* Playhead repaint is NOT done here -- it runs on the vblank tick
     * (updatePlayheadPaint) so it's vsync-cadenced rather than beating
     * against the peer's vblank at this 30Hz timer's phase.  lastBeat_
     * (the painted position) is owned by the vblank too. */
    wasPlaying_ = playing;
    updateTransportLabel();
}

void ArrangementView::updatePlayheadPaint()
{
    /* Runs on the Body's vblank tick (vsync-cadenced).  Mirrors the old
     * timer playhead block, with its OWN play-edge state so it stays
     * independent of the transport timer's wasPlaying_.  Surgical: repaint
     * only the old + new playhead strips.  When stopped the playhead PARKS
     * at the last beat (one settling repaint on the stop edge, then idle). */
    if (monitor_ == nullptr || body_ == nullptr) return;
    const bool playing = monitor_->playing.get();
    if (! playing && playing == playheadPaintPlaying_) return;   // parked / idle

    const double beat = computePlayheadBeats();
    const int oldPxX = (lastBeat_ > -0.001)
                          ? Body::kLabelW + (int) (lastBeat_ * body_->kPxPerBeat)
                          : -1;
    const int newPxX = Body::kLabelW + (int) (beat * body_->kPxPerBeat);
    if (oldPxX != newPxX || playing != playheadPaintPlaying_)
        body_->repaintPlayhead (oldPxX, newPxX);

    lastBeat_ = beat;
    playheadPaintPlaying_ = playing;
}

} // namespace element
