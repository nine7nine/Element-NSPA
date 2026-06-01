// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/velocity_lane.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/pianoroll/midi_note_diff_command.hpp"
#include "ui/fontcache.hpp"

#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/midi_note.hpp"

#include <element/services.hpp>
#include <element/ui.hpp>

#include <algorithm>
#include <cmath>

namespace element {

namespace {

constexpr int kHeadRadius   = 4;
constexpr int kHitSlop      = 6;
constexpr int kBaselinePad  = 6;
constexpr int kTopPad       = 8;

} // namespace

VelocityLane::VelocityLane (PianoRollView& parent, Services& services)
    : parent_ (parent), services_ (services)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

VelocityLane::~VelocityLane() = default;

MidiNoteRegion* VelocityLane::resolveBoundRegion() const noexcept
{
    const auto& resolver = parent_.getRegionResolver();
    const auto  regionId = parent_.getBoundRegionId();
    if (! resolver || regionId.isNull()) return nullptr;
    return resolver (regionId);
}

void VelocityLane::setScrollX (int x)
{
    if (x == scrollX_) return;
    scrollX_ = x;
    repaint();
}

void VelocityLane::resized() {}

int VelocityLane::yForVelocity (int vel) const noexcept
{
    const float v = (float) juce::jlimit (1, 127, vel) / 127.0f;
    const int   h = juce::jmax (1, getHeight() - kTopPad - kBaselinePad);
    return kTopPad + (int) std::round ((1.0f - v) * (float) h);
}

int VelocityLane::velocityForY (int y) const noexcept
{
    const int h = juce::jmax (1, getHeight() - kTopPad - kBaselinePad);
    const float t = juce::jlimit (0.0f, 1.0f,
        1.0f - ((float) (y - kTopPad) / (float) h));
    return juce::jlimit (1, 127, (int) std::round (t * 127.0f));
}

int VelocityLane::xForBeat (double localBeat, int pxPerBeat) const noexcept
{
    return (int) std::round (localBeat * (double) pxPerBeat) - scrollX_;
}

void VelocityLane::paint (juce::Graphics& g)
{
    /* Backdrop -- matches the grid's body tone so the lane reads as
     * part of the same surface family.  Top edge has a 1 px LCD-blue
     * divider so the seam against the grid above is unambiguous. */
    g.fillAll (juce::Colour (0xff'10'10'10));
    g.setColour (juce::Colour (0xff'30'30'30));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;

    auto* grid = parent_.getGrid();
    if (grid == nullptr) return;
    const int pxPerBeat = grid->getPxPerBeat();
    if (pxPerBeat <= 0) return;

    const auto* snap = region->loadSnapshot();
    if (snap == nullptr || snap->empty()) return;

    const int baselineY = getHeight() - kBaselinePad;

    /* Baseline track -- thin horizontal divider so empty velocity
     * areas still feel grounded. */
    g.setColour (juce::Colour (0xff'20'20'20));
    g.drawHorizontalLine (baselineY, 0.0f, (float) getWidth());

    /* Region-end fence: matches the grid's "beyond region" divider.
     * Anything past it is unreachable territory. */
    const int regionEndPx = (int) std::round (region->lengthBeats * (double) pxPerBeat);
    const int regionEndX  = regionEndPx - scrollX_;
    if (regionEndX > 0 && regionEndX < getWidth())
    {
        g.setColour (juce::Colour (0xff'08'08'08));
        g.fillRect (regionEndX, 0, getWidth() - regionEndX, getHeight());
        g.setColour (juce::Colour (0xff'40'40'40));
        g.drawVerticalLine (regionEndX, 0.0f, (float) getHeight());
    }

    const juce::Colour stemDefault { 0xff'5a'a5'd0 };   /* matches tracker stroke */
    const juce::Colour stemSelected { 0xff'40'ff'80 };  /* kAccentGreen */

    for (const auto& n : *snap)
    {
        const int x = xForBeat (n.onBeat, pxPerBeat);
        if (x < -kHitSlop || x > getWidth() + kHitSlop) continue;
        const int headY = yForVelocity (n.velocity);
        const bool selected = grid->isSelected (n.id);
        const juce::Colour col = selected ? stemSelected : stemDefault;

        /* Stem -- 2 px wide so it reads at any zoom. */
        g.setColour (col.withMultipliedAlpha (0.75f));
        g.fillRect (juce::Rectangle<int> (x - 1, headY,
                                            2, baselineY - headY));
        /* Head + outline. */
        g.setColour (col);
        g.fillEllipse ((float) (x - kHeadRadius),
                        (float) (headY - kHeadRadius),
                        (float) (kHeadRadius * 2),
                        (float) (kHeadRadius * 2));
        g.setColour (col.brighter (0.45f));
        g.drawEllipse ((float) (x - kHeadRadius),
                        (float) (headY - kHeadRadius),
                        (float) (kHeadRadius * 2),
                        (float) (kHeadRadius * 2),
                        1.0f);
    }

    /* Playhead -- mirror the grid + CC lane so live playback tracks
     * unbroken across the whole editor (the velocity lane sits between
     * them). */
    const double phLocal = grid->playheadLocalBeat();
    if (phLocal >= 0.0)
    {
        const int px = xForBeat (phLocal, pxPerBeat);
        if (px >= 0 && px < getWidth())
        {
            g.setColour (juce::Colour (0xff'40'ff'80).withAlpha (0.85f));
            g.drawVerticalLine (px, 0.0f, (float) getHeight());
        }
    }
}

std::uint64_t VelocityLane::hitTestStem (int x, int y,
                                            const MidiNoteRegion& region) const noexcept
{
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* grid = parent_.getGrid();
    if (grid == nullptr) return 0;
    const int pxPerBeat = grid->getPxPerBeat();
    if (pxPerBeat <= 0) return 0;

    /* Reverse iterate so later-drawn notes win the hit on overlap. */
    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        const int nx = xForBeat (n.onBeat, pxPerBeat);
        const int ny = yForVelocity (n.velocity);
        const int dx = x - nx;
        const int dy = y - ny;
        if (dx * dx + dy * dy <= kHitSlop * kHitSlop * 2)
            return n.id;
    }
    return 0;
}

void VelocityLane::mouseDown (const juce::MouseEvent& e)
{
    auto* region = resolveBoundRegion();
    if (region == nullptr) return;
    auto* grid = parent_.getGrid();
    if (grid == nullptr) return;

    const auto hitId = hitTestStem (e.x, e.y, *region);
    if (hitId == 0)
    {
        /* Empty area click clears selection unless additive mod. */
        if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown())
            grid->selectClear();
        return;
    }

    /* Hit on a stem -- promote to selection if it wasn't (so the
     * subsequent drag operates on the right set), then begin drag. */
    if (! grid->isSelected (hitId))
        grid->selectOnly (hitId);

    drag_.active    = true;
    drag_.anchorY   = e.y;
    drag_.originals.clear();

    if (const auto* snap = region->loadSnapshot())
    {
        for (const auto& n : *snap)
            if (grid->isSelected (n.id))
            {
                drag_.originals.push_back (n);
                if (n.id == hitId)
                    drag_.referenceVel = n.velocity;
            }
    }

    repaint();
    parent_.getGrid()->repaint();   /* sync selection colour on grid */
}

void VelocityLane::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag_.active) return;
    auto* region = resolveBoundRegion();
    if (region == nullptr) return;

    const int targetVel = velocityForY (e.y);
    const int delta     = targetVel - drag_.referenceVel;
    if (delta == 0) return;

    /* Apply the SAME delta to every dragged note (matches Bitwig --
     * preserves the relative velocity differences between notes in a
     * selection).  Ableton scales proportionally; we pick delta for
     * predictability.  Clamp each note independently to [1, 127]. */
    for (auto& before : drag_.originals)
    {
        MidiNote after = before;
        after.velocity = juce::jlimit (1, 127, before.velocity + delta);
        region->updateNoteById (before.id, after);
    }
    repaint();
}

void VelocityLane::mouseUp (const juce::MouseEvent& e)
{
    if (! drag_.active) return;
    drag_.active = false;

    auto* region = resolveBoundRegion();
    if (region == nullptr) { drag_.originals.clear(); return; }

    /* Build a single MidiNoteDiffCommand covering every note's before
     * -> after so undo restores everything in one step.  We've already
     * applied the live preview via updateNoteById; reapply through
     * the undo path so the action history records the change. */
    const int finalVel = velocityForY (e.y);
    const int delta    = finalVel - drag_.referenceVel;
    if (delta == 0) { drag_.originals.clear(); return; }

    if (auto* gui = services_.find<GuiService>())
    {
        auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                            parent_.getRegionResolver());
        /* First, REVERT the live preview so perform() applies the
         * final delta as a clean undoable step.  updateNoteById on
         * the original restores each note's pre-drag velocity. */
        for (const auto& before : drag_.originals)
            region->updateNoteById (before.id, before);

        for (const auto& before : drag_.originals)
        {
            MidiNote after = before;
            after.velocity = juce::jlimit (1, 127, before.velocity + delta);
            cmd->recordUpdate (before.id, before, after);
        }
        juce::Component::SafePointer<PianoRollView> safeView (&parent_);
        cmd->onApplied = [safeView]() {
            if (auto* v = safeView.getComponent())
                v->notifyRegionEdited();
        };
        /* Fresh transaction segregates this edit from any open
         * ArrangementView / SessionView transaction so Ctrl+Z stays
         * scoped to the velocity edit. */
        gui->getUndoManager().beginNewTransaction ("Edit MIDI velocity");
        gui->getUndoManager().perform (cmd.release(), "Edit MIDI velocity");
    }
    else
    {
        /* No undo trail in the fallback path -- but the live preview
         * already applied the velocity changes; still need to flush
         * the session via notifyRegionEdited. */
        parent_.notifyRegionEdited();
    }

    drag_.originals.clear();
    repaint();
}

} // namespace element
