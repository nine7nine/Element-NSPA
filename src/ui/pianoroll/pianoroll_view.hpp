// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/quantize_options.hpp"
#include "ui/blocktoolbutton.hpp"

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>

#include <functional>

namespace element {

class MidiNoteRegion;
class PianoRollKeyboard;
class PianoRollGrid;
class QuantizeDialog;
class VelocityLane;
class CcLane;

/** Bottom-attached piano-roll editor dock.  Peer to TrackerSideDock
 *  (right-attached) -- the bottom slot was reserved for piano-roll
 *  back when the tracker editor moved to the right column.
 *
 *  Layout (inside the dock's own bounds):
 *   - Top edge:   thin drag handle for VERTICAL resize.
 *   - Header:     region label ("MIDI -- <filename>") + tool palette
 *                 (Select / Pencil / Erase) + close X.
 *   - Body:       left = PianoRollKeyboard column (fixed width),
 *                 right = juce::Viewport hosting the PianoRollGrid
 *                 (full region beat span, horizontal scroll + zoom).
 *
 *  Tool palette: radio-toggle on the header, default Select.  The
 *  active tool is stored on PianoRollView and read by PianoRollGrid's
 *  mouse handlers via getActiveTool().
 *
 *  Region binding: per-paint resolver lookup (Q3 design pick from
 *  Session 1).  See setRegionResolver. */
class PianoRollView : public juce::Component
{
public:
    /** Brush: drag paints fresh notes as the cursor sweeps cells.
     *  Distinct from Pencil (single-click + drag-resize one note);
     *  brush emits one snap-division-length note per visited
     *  (beat-cell, pitch) pair, then commits the lot as a single
     *  MidiNoteDiffCommand on mouseUp.  B17 in
     *  midi-implementation-audit-20260526.md. */
    enum class Tool : int { Select = 0, Pencil = 1, Erase = 2, Brush = 3 };

    PianoRollView (Services& services);
    ~PianoRollView() override;

    /** Resolver hook -- returns a pointer to the live MidiNoteRegion
     *  for the given uuid, or nullptr.  Called once per paint from
     *  PianoRollGrid + once per gesture commit. */
    using RegionResolver = std::function<MidiNoteRegion* (const juce::Uuid&)>;
    void setRegionResolver (RegionResolver resolver) { regionResolver_ = std::move (resolver); }
    const RegionResolver& getRegionResolver() const noexcept { return regionResolver_; }

    /** Bind the dock to a specific MIDI region. */
    void setRegion (const juce::Uuid& regionId);
    juce::Uuid getBoundRegionId() const noexcept { return boundRegionId_; }

    /** Accessor for the keyboard column. */
    PianoRollKeyboard* getKeyboard() const noexcept { return keyboard_.get(); }

    /** Accessor for the grid (used by gesture-commit code to invoke
     *  region mutation and undo-push paths). */
    PianoRollGrid* getGrid() const noexcept { return grid_.get(); }

    /** Active tool selection.  Mouse handlers in the grid branch on
     *  this; selection paint highlights respect it too. */
    Tool getActiveTool() const noexcept { return activeTool_; }
    void setActiveTool (Tool t);

    /** Drag-handle / close callbacks owned by StandardContent. */
    std::function<void (int /*deltaPx*/)> onResizeDrag;
    std::function<void()>                  onResizeDragEnd;
    std::function<void()>                  onCloseClicked;

    //==========================================================================
    // Last-used quantize / humanize / scale state.  Survives dialog
    // open/close so Ctrl+Q and the toolbar Q/H/S buttons replay the
    // user's most recent settings.  Lives on the view (not the grid)
    // because the grid is re-bound per region; the dialog parameters
    // are per-editor-instance.

    const dsp::quantize::QuantizeOptions& getLastQuantizeOptions() const noexcept { return lastQuantize_; }
    const dsp::quantize::HumanizeOptions& getLastHumanizeOptions() const noexcept { return lastHumanize_; }
    dsp::quantize::Scale                  getLastScale()           const noexcept { return lastScale_; }
    int                                   getLastScaleRoot()       const noexcept { return lastScaleRoot_; }

    void setLastQuantizeOptions (const dsp::quantize::QuantizeOptions& o) noexcept;
    void setLastHumanizeOptions (const dsp::quantize::HumanizeOptions& o) noexcept;
    void setLastScale (dsp::quantize::Scale s, int root) noexcept;

    /** True when the user has opened the dialog at least once and
     *  adjusted the Quantize tab parameters.  Ctrl+Q uses last-used
     *  options when true; otherwise it derives defaults from the
     *  current snap division. */
    bool isLastQuantizeUserAdjusted() const noexcept { return lastQuantizeDirty_; }

    /** Toggle the embedded Quantize / Humanize / Scale panel on the
     *  right edge of the piano-roll body.  If the panel is hidden,
     *  show it on the matching tab.  If it's visible on the SAME tab,
     *  hide it (so the toolbar button acts as a toggle).  If it's
     *  visible on a DIFFERENT tab, stay open and switch tab.  The
     *  panel never floats -- always docked in the editor's layout. */
    void toggleQuantizePanel (int tabIndex);   /* 0 = Q, 1 = H, 2 = S */

    /** True when the quantize panel is currently visible.  Toolbar
     *  buttons + Ctrl+Q hotkey use this to decide whether a click on
     *  the active tab should hide vs reopen. */
    bool isQuantizePanelVisible() const noexcept { return quantizePanelVisible_; }

    static constexpr int kQuantizePanelW = 290;

    /** Fired after every region-mutating gesture commit (Move, Create,
     *  Resize, Erase, Delete-key) on the bound region.  StandardContent
     *  wires this to ArrangementView::body_->repaint() so the lane
     *  strip's note-count badge tracks live edits. */
    std::function<void()>                  onRegionEdited;
    /** Internal fanout for region-edit notifications.  Defined in the
     *  cpp so it can repaint the (forward-declared) VelocityLane in
     *  addition to firing the outbound callback. */
    void notifyRegionEdited();

    void paint (juce::Graphics&) override;
    void resized() override;

    /* Layout constants:
     *   kDragHandleH   thin top edge for vertical resize.
     *   kHeaderH       toolbar row (tool palette + snap + zoom + close).
     *   kKeyboardW     left-side piano key column width.
     */
    static constexpr int kHeaderH      = 28;
    static constexpr int kDragHandleH  = 4;
    static constexpr int kKeyboardW    = 56;
    static constexpr int kToolBtnW     = 56;
    static constexpr int kZoomBtnW     = 24;
    static constexpr int kSnapBoxW     = 70;

private:
    Services&    services_;
    juce::Uuid   boundRegionId_;
    Tool         activeTool_ { Tool::Select };

    RegionResolver regionResolver_;

    class DragHandle;
    std::unique_ptr<DragHandle>         dragHandle_;
    juce::Label                         regionLabel_;
    BlockToolButton                     closeBtn_       { "X" };
    BlockToolButton                     selectBtn_      { "Select" };
    BlockToolButton                     pencilBtn_      { "Pencil" };
    BlockToolButton                     eraseBtn_       { "Erase" };
    BlockToolButton                     brushBtn_       { "Brush"  };
    BlockToolButton                     snapBtn_        { "Snap" };
    juce::ComboBox                      snapBox_;
    /* Bulk-edit ops.  Toolbar entry points for the quantize / humanize
     * helpers in dsp/quantize_ops.hpp -- the same code paths Ctrl+Q
     * exercises.  Surfacing these as buttons (vs hotkey-only) keeps the
     * MIDI editor's editing surface discoverable; the C.2 dialog (still
     * pending) will subsume both buttons + add the scale-snap tab. */
    BlockToolButton                     quantizeBtn_    { "Q" };
    BlockToolButton                     humanizeBtn_    { "H" };
    BlockToolButton                     scaleBtn_       { "S" };
    BlockToolButton                     zoomOutBtn_     { "-" };
    BlockToolButton                     zoomInBtn_      { "+" };
    BlockToolButton                     zoomFitBtn_     { "Fit" };
    /* Y zoom: shrink / grow visible pitch span around the centre.
     * Independent of the X zoom triplet -- piano-roll users routinely
     * change Y span (more octaves) and X span (more beats) on different
     * axes during edits. */
    BlockToolButton                     yZoomOutBtn_    { "Y-" };
    BlockToolButton                     yZoomInBtn_     { "Y+" };
    /* Show/hide the MIDI CC automation lane (hidden by default so the
     * grid keeps its full height until the user wants CC editing). */
    BlockToolButton                     ccBtn_          { "CC" };

    /* Last-used dialog parameters.  Defaults match the C.1 toolbar
     * behaviour: full-amount quantize, +/- 10 velocity humanize, C
     * major scale.  Dirty flags track whether the user has actually
     * touched a tab so Ctrl+Q falls back to snap-derived defaults
     * pre-first-dialog-open. */
    dsp::quantize::QuantizeOptions lastQuantize_;
    dsp::quantize::HumanizeOptions lastHumanize_;
    dsp::quantize::Scale           lastScale_     { dsp::quantize::Scale::Major };
    int                            lastScaleRoot_ { 0 };
    bool                           lastQuantizeDirty_ { false };
    bool                           lastHumanizeDirty_ { false };
    bool                           lastScaleDirty_    { false };

    /* Embedded quantize / humanize / scale panel.  Owned + parented
     * by the view; visibility flips per toolbar Q/H/S click.  Never
     * floats -- always docked on the right edge of the body. */
    std::unique_ptr<QuantizeDialog>  quantizePanel_;
    bool                             quantizePanelVisible_ { false };

    std::unique_ptr<PianoRollKeyboard>  keyboard_;
    /** juce::Viewport hosting the grid.  Horizontal scrolling only --
     *  the keyboard column and grid both fill the dock's body height. */
    std::unique_ptr<juce::Viewport>     gridViewport_;
    std::unique_ptr<PianoRollGrid>      grid_;
    /** Velocity lollipops strip under the grid viewport.  Mirrors the
     *  viewport's horizontal scroll via a Viewport listener so the
     *  lane stays aligned with the notes above. */
    std::unique_ptr<VelocityLane>       velocityLane_;
    /** MIDI CC automation lane, stacked beneath the velocity lane.
     *  Shares the grid's horizontal scroll + pxPerBeat via the same
     *  ViewportScrollMirror. */
    std::unique_ptr<CcLane>             ccLane_;
    class ViewportScrollMirror;
    std::unique_ptr<ViewportScrollMirror> scrollMirror_;

    static constexpr int kVelocityLaneH = 72;
    static constexpr int kCcLaneH       = 84;

    void refreshLabel();
    void syncToolToggles();
    void applySnapFromComboBox();
    void hideQuantizePanel();
    void syncToolbarTabToggles();
    void persistLastUsedToSettings();
    void loadLastUsedFromSettings();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};

} // namespace element
