// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/quantize_dialog.hpp"

#include <element/settings.hpp>
#include <element/ui.hpp>
#include "ui/pianoroll/pianoroll_keyboard.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/pianoroll/velocity_lane.hpp"
#include "ui/pianoroll/cc_lane.hpp"
#include "ui/fontcache.hpp"

#include "services/timeline/midi_note_region.hpp"

namespace element {

/* Bridge from juce::Viewport's view-position change -> VelocityLane.
 * Viewport doesn't expose a public listener interface so we attach a
 * juce::ScrollBar::Listener to the horizontal scrollbar.  Fires on
 * every visible-area change (drag, wheel, programmatic). */
class PianoRollView::ViewportScrollMirror : public juce::ScrollBar::Listener
{
public:
    ViewportScrollMirror (juce::Viewport& vp, VelocityLane& lane, CcLane& cc)
        : viewport_ (vp), lane_ (lane), cc_ (cc)
    {
        viewport_.getHorizontalScrollBar().addListener (this);
    }
    ~ViewportScrollMirror() override
    {
        viewport_.getHorizontalScrollBar().removeListener (this);
    }
    void scrollBarMoved (juce::ScrollBar* sb, double /*newRange*/) override
    {
        /* The HORIZONTAL scrollbar's range start IS the viewport's
         * view-position-x.  Push it to both under-grid lanes so they
         * re-paint aligned with the notes above. */
        if (sb == &viewport_.getHorizontalScrollBar())
        {
            const int x = viewport_.getViewPositionX();
            lane_.setScrollX (x);
            cc_.setScrollX (x);
        }
    }
private:
    juce::Viewport& viewport_;
    VelocityLane&   lane_;
    CcLane&         cc_;
};

/* Top-edge drag handle.  Vertical-only resize -- forwards the pixel
 * delta to the dock's onResizeDrag callback so StandardContent owns
 * the height field + layout invalidation.  Mirror of
 * TrackerSideDock::DragHandle rotated 90 degrees. */
class PianoRollView::DragHandle : public juce::Component
{
public:
    DragHandle (PianoRollView& d) : dock_ (d)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff'1a'1a'1a));
        g.setColour (juce::Colour (0xff'3a'3a'3a));
        g.drawHorizontalLine (getHeight() / 2,
                               0.0f, (float) getWidth());
        g.setColour (juce::Colour (0xff'5a'5a'5a));
        const int cx = getWidth() / 2;
        const int cy = getHeight() / 2;
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse ((float) (cx + i * 8) - 1.0f,
                            (float) cy - 1.0f, 2.0f, 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartY_ = e.getScreenY();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int dy = e.getScreenY() - dragStartY_;
        dragStartY_ = e.getScreenY();
        if (dock_.onResizeDrag) dock_.onResizeDrag (dy);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dock_.onResizeDragEnd) dock_.onResizeDragEnd();
    }

private:
    PianoRollView& dock_;
    int dragStartY_ { 0 };
};

PianoRollView::PianoRollView (Services& services)
    : services_ (services)
{
    dragHandle_ = std::make_unique<DragHandle> (*this);
    addAndMakeVisible (*dragHandle_);

    regionLabel_.setJustificationType (juce::Justification::centredLeft);
    regionLabel_.setColour (juce::Label::textColourId,
                              juce::Colours::white.withAlpha (0.85f));
    regionLabel_.setFont (monoFont (11.0f, juce::Font::plain));
    refreshLabel();
    addAndMakeVisible (regionLabel_);

    /* Active-state green tint mirrors ArrangementView's tool buttons
     * + tracker FOLLOW button.  Single source of truth so the dock
     * reads as part of the same toolbar family. */
    const juce::Colour kActiveTint { 0xff'4a'a5'5a };

    /* Tool palette: radio toggle.  Buttons are togglable BlockToolButtons
     * styled to match ArrangementView's tool row.  Manual radio
     * handling -- juce::Button's radio group machinery doesn't
     * cooperate with BlockToolButton's custom paint. */
    auto initToolBtn = [this, kActiveTint] (BlockToolButton& btn, Tool t)
    {
        btn.setClickingTogglesState (true);
        btn.setActiveTint (kActiveTint);
        btn.onClick = [this, t]() { setActiveTool (t); };
        addAndMakeVisible (btn);
    };
    initToolBtn (selectBtn_, Tool::Select);
    initToolBtn (pencilBtn_, Tool::Pencil);
    initToolBtn (eraseBtn_,  Tool::Erase);
    initToolBtn (brushBtn_,  Tool::Brush);

    /* Vector tool icons -- same drawing style as ArrangementView's tool
     * row so the user reads them as siblings. */
    selectBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            juce::Path p;
            const float w = b.getWidth(), h = b.getHeight();
            p.startNewSubPath (b.getX() + w * 0.20f, b.getY() + h * 0.15f);
            p.lineTo          (b.getX() + w * 0.55f, b.getY() + h * 0.55f);
            p.lineTo          (b.getX() + w * 0.35f, b.getY() + h * 0.55f);
            p.lineTo          (b.getX() + w * 0.55f, b.getY() + h * 0.85f);
            p.lineTo          (b.getX() + w * 0.35f, b.getY() + h * 0.85f);
            p.lineTo          (b.getX() + w * 0.20f, b.getY() + h * 0.65f);
            p.closeSubPath();
            g.setColour (fg);
            g.fillPath (p);
        });
    pencilBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Diagonal pencil glyph -- short angled line + tip wedge. */
            const float w = b.getWidth(), h = b.getHeight();
            const float x0 = b.getX() + w * 0.20f;
            const float y0 = b.getY() + h * 0.85f;
            const float x1 = b.getX() + w * 0.80f;
            const float y1 = b.getY() + h * 0.20f;
            g.setColour (fg);
            g.drawLine (x0, y0, x1, y1, 1.8f);
            juce::Path tip;
            tip.startNewSubPath (x1, y1);
            tip.lineTo (x1 + w * 0.08f, y1 + h * 0.06f);
            tip.lineTo (x1 - w * 0.05f, y1 + h * 0.16f);
            tip.closeSubPath();
            g.fillPath (tip);
        });
    eraseBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* X glyph -- two crossing diagonals. */
            const float w = b.getWidth(), h = b.getHeight();
            const float pad = juce::jmin (w, h) * 0.22f;
            g.setColour (fg);
            g.drawLine (b.getX() + pad, b.getY() + pad,
                        b.getRight() - pad, b.getBottom() - pad, 1.6f);
            g.drawLine (b.getRight() - pad, b.getY() + pad,
                        b.getX() + pad, b.getBottom() - pad, 1.6f);
        });
    brushBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Stylised brush: angled handle + wider bristle tip. */
            const float w = b.getWidth(), h = b.getHeight();
            const float x0 = b.getX() + w * 0.22f;
            const float y0 = b.getY() + h * 0.85f;
            const float x1 = b.getX() + w * 0.72f;
            const float y1 = b.getY() + h * 0.30f;
            g.setColour (fg);
            g.drawLine (x0, y0, x1, y1, 1.6f);
            juce::Path tip;
            tip.startNewSubPath (x1 - w * 0.08f, y1 + h * 0.04f);
            tip.lineTo          (x1 + w * 0.10f, y1 - h * 0.12f);
            tip.lineTo          (x1 + w * 0.22f, y1 + h * 0.06f);
            tip.lineTo          (x1 + w * 0.04f, y1 + h * 0.22f);
            tip.closeSubPath();
            g.fillPath (tip);
        });

    syncToolToggles();

    /* Snap controls -- mirror ArrangementView's pair.  snapBtn toggles
     * snap enable/disable; snapBox picks the resolution.  Default is
     * 1/16 (Ableton + Zrythm default). */
    snapBtn_.setClickingTogglesState (true);
    snapBtn_.setToggleState (true, juce::dontSendNotification);
    snapBtn_.setActiveTint (kActiveTint);
    snapBtn_.onClick = [this]() {
        if (grid_ != nullptr)
            grid_->setSnapEnabled (snapBtn_.getToggleState());
        snapBox_.setEnabled (snapBtn_.getToggleState());
    };
    addAndMakeVisible (snapBtn_);

    /* Snap divisions -- standard DAW menu including triplets.  IDs
     * are stable across versions so persisted picks (future Phase
     * 4) stay valid.  Triplet entries express N notes in the time
     * of N-1 of the next-larger duple division (1/4T = 2/3 beat;
     * 1/8T = 1/3 beat; 1/16T = 1/6 beat) -- standard music notation
     * convention. */
    snapBox_.addItem ("Bar",     1);
    snapBox_.addItem ("1/2",     2);
    snapBox_.addItem ("1/4",     3);
    snapBox_.addItem ("1/8",     4);
    snapBox_.addItem ("1/16",    5);
    snapBox_.addItem ("1/32",    6);
    snapBox_.addItem ("1/4T",   10);   /* quarter triplet -- 2/3 beat */
    snapBox_.addItem ("1/8T",   11);   /* eighth triplet  -- 1/3 beat */
    snapBox_.addItem ("1/16T",  12);   /* sixteenth triplet -- 1/6 beat */
    snapBox_.setSelectedId (5, juce::dontSendNotification);   /* 1/16 */
    snapBox_.onChange = [this]() { applySnapFromComboBox(); };
    snapBox_.setColour (juce::ComboBox::backgroundColourId,
                         juce::Colour (0xff'2c'2c'2c));
    snapBox_.setColour (juce::ComboBox::textColourId,
                         juce::Colour (0xff'd0'd0'd0));
    snapBox_.setColour (juce::ComboBox::outlineColourId,
                         juce::Colour (0xff'5a'5a'5a));
    snapBox_.setColour (juce::ComboBox::arrowColourId,
                         juce::Colour (0xff'a0'a0'a0));
    addAndMakeVisible (snapBox_);

    /* Bulk-edit ops -- Quantize + Humanize + Scale.  Click opens the
     * three-tab dialog at the matching tab so the user can dial in
     * parameters with live preview; Ctrl+Q remains the fast-replay
     * hotkey that uses the last-applied settings (or snap-derived
     * defaults pre-first-dialog-open). */
    quantizeBtn_.setClickingTogglesState (true);
    quantizeBtn_.setTooltip ("Quantize... (Ctrl+Q applies last settings)");
    quantizeBtn_.setActiveTint (kActiveTint);
    quantizeBtn_.onClick = [this]() {
        toggleQuantizePanel (0);
    };
    quantizeBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Stepped staircase glyph -- reads as "snap to grid":
             * three rising right-angle steps. */
            const float w = b.getWidth(), h = b.getHeight();
            const float pad = juce::jmin (w, h) * 0.18f;
            const float x0 = b.getX() + pad;
            const float x3 = b.getRight() - pad;
            const float y0 = b.getBottom() - pad;
            const float y3 = b.getY() + pad;
            const float stepW = (x3 - x0) / 3.0f;
            const float stepH = (y0 - y3) / 3.0f;
            juce::Path p;
            p.startNewSubPath (x0,                y0);
            p.lineTo          (x0 + stepW,        y0);
            p.lineTo          (x0 + stepW,        y0 - stepH);
            p.lineTo          (x0 + 2.0f * stepW, y0 - stepH);
            p.lineTo          (x0 + 2.0f * stepW, y0 - 2.0f * stepH);
            p.lineTo          (x3,                y0 - 2.0f * stepH);
            p.lineTo          (x3,                y3);
            g.setColour (fg);
            g.strokePath (p, juce::PathStrokeType (1.6f));
        });
    addAndMakeVisible (quantizeBtn_);

    humanizeBtn_.setClickingTogglesState (true);
    humanizeBtn_.setTooltip ("Humanize...");
    humanizeBtn_.setActiveTint (kActiveTint);
    humanizeBtn_.onClick = [this]() {
        toggleQuantizePanel (1);
    };
    humanizeBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Uneven-height bar glyph -- reads as "varied velocities":
             * four vertical bars with different heights. */
            const float w = b.getWidth(), h = b.getHeight();
            const float pad   = juce::jmin (w, h) * 0.18f;
            const float baseY = b.getBottom() - pad;
            const float topY  = b.getY() + pad;
            const float totalW = b.getWidth() - 2.0f * pad;
            const float barW   = totalW / 7.0f;            /* 4 bars + 3 gaps */
            const float heights[] = { 0.55f, 0.85f, 0.40f, 0.70f };
            g.setColour (fg);
            for (int i = 0; i < 4; ++i)
            {
                const float x = b.getX() + pad + barW * (2.0f * i);
                const float y = baseY - (baseY - topY) * heights[i];
                g.fillRect (x, y, barW, baseY - y);
            }
        });
    addAndMakeVisible (humanizeBtn_);

    scaleBtn_.setClickingTogglesState (true);
    scaleBtn_.setTooltip ("Scale-snap...");
    scaleBtn_.setActiveTint (kActiveTint);
    scaleBtn_.onClick = [this]() {
        toggleQuantizePanel (2);
    };
    scaleBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Treble-clef-ish stack of three short dashes ascending --
             * reads as "notes in a scale".  Simple + recognisable at
             * 24 px. */
            const float w = b.getWidth(), h = b.getHeight();
            const float pad   = juce::jmin (w, h) * 0.20f;
            const float dashW = (w - 2.0f * pad) * 0.85f;
            const float baseY = b.getBottom() - pad;
            const float topY  = b.getY() + pad;
            const float stepH = (baseY - topY) / 4.0f;
            g.setColour (fg);
            for (int i = 0; i < 3; ++i)
            {
                const float y = baseY - stepH * (1.0f + (float) i);
                const float x = b.getX() + pad + (3 - i) * 1.5f;
                g.fillRect (x, y, dashW * (0.7f + 0.10f * (float) i), 1.4f);
            }
        });
    addAndMakeVisible (scaleBtn_);

    /* Zoom controls -- X axis (beat span).  Mirrors ArrangementView's
     * [- + Fit] triplet. */
    zoomOutBtn_.onClick = [this]() { if (grid_) grid_->zoomBy (1.0 / 1.20); };
    zoomInBtn_ .onClick = [this]() { if (grid_) grid_->zoomBy (1.20); };
    zoomFitBtn_.onClick = [this]() { if (grid_) grid_->zoomToFit(); };
    addAndMakeVisible (zoomOutBtn_);
    addAndMakeVisible (zoomInBtn_);
    addAndMakeVisible (zoomFitBtn_);

    /* Y zoom -- visible pitch span.  Step factor 1.20 matches the X
     * zoom; the keyboard component clamps to >= 12 semitones visible. */
    yZoomOutBtn_.setTooltip ("Show more octaves (zoom out vertically)");
    yZoomInBtn_ .setTooltip ("Show fewer octaves (zoom in vertically)");
    yZoomOutBtn_.onClick = [this]() {
        if (keyboard_) { keyboard_->zoomVertically (1.20); if (grid_) grid_->repaint(); }
    };
    yZoomInBtn_.onClick = [this]() {
        if (keyboard_) { keyboard_->zoomVertically (1.0 / 1.20); if (grid_) grid_->repaint(); }
    };
    addAndMakeVisible (yZoomOutBtn_);
    addAndMakeVisible (yZoomInBtn_);

    addAndMakeVisible (closeBtn_);
    closeBtn_.setTooltip ("Hide piano-roll dock");
    closeBtn_.onClick = [this]() {
        if (onCloseClicked) onCloseClicked();
    };

    keyboard_ = std::make_unique<PianoRollKeyboard>();
    addAndMakeVisible (*keyboard_);

    grid_ = std::make_unique<PianoRollGrid> (*this, services_);

    /* Wrap the grid in a juce::Viewport so the user can horizontally
     * scroll through long regions (e.g. a 424-beat .mid import that
     * doesn't fit in the dock's visible width at the default zoom).
     * Vertical scrollbar is disabled because the pitch range is fixed
     * to the keyboard column's [36, 96] span, which is laid out to
     * fill the dock height. */
    gridViewport_ = std::make_unique<juce::Viewport>();
    gridViewport_->setScrollBarsShown (false /*vertical*/, true /*horizontal*/);
    gridViewport_->setViewedComponent (grid_.get(), false /*deleteOnDelete*/);
    addAndMakeVisible (*gridViewport_);

    /* Velocity lane under the grid.  Lives OUTSIDE the viewport (so
     * its height doesn't get eaten by the viewport's content sizing)
     * + mirrors the viewport's horizontal scroll position via a
     * ScrollBar listener so lollipops stay aligned with notes. */
    velocityLane_ = std::make_unique<VelocityLane> (*this, services_);
    addAndMakeVisible (*velocityLane_);
    /* CC automation lane, stacked beneath the velocity lane.  Hidden by
     * default (added as a child component, not made visible) so the grid
     * keeps its full height until the user toggles "CC" on -- the dock's
     * 240 px default can't fit grid + both lanes at once. */
    ccLane_ = std::make_unique<CcLane> (*this, services_);
    addChildComponent (*ccLane_);
    scrollMirror_ = std::make_unique<ViewportScrollMirror> (*gridViewport_,
                                                              *velocityLane_,
                                                              *ccLane_);

    /* "CC" toggle -- reveals/hides the CC lane + re-lays-out. */
    ccBtn_.setClickingTogglesState (true);
    ccBtn_.setToggleState (false, juce::dontSendNotification);
    ccBtn_.setActiveTint (kActiveTint);
    ccBtn_.onClick = [this]() {
        if (ccLane_ != nullptr)
        {
            ccLane_->setVisible (ccBtn_.getToggleState());
            resized();
            if (ccLane_->isVisible()) ccLane_->repaint();
        }
    };
    addAndMakeVisible (ccBtn_);

    /* Seed the grid with the comboBox's default snap pick so the
     * displayed division + the runtime division agree from frame 0. */
    applySnapFromComboBox();

    /* Hydrate last-used quantize/humanize/scale options from the
     * user's settings file so Ctrl+Q replays the user's most recent
     * dialed-in parameters across Element restarts (A.6 fix). */
    loadLastUsedFromSettings();
}

PianoRollView::~PianoRollView()
{
    /* Tear down the panel before our other members vanish so its
     * clearPreview() destructor hook can still reach the grid. */
    quantizePanel_.reset();
}

void PianoRollView::setLastQuantizeOptions (const dsp::quantize::QuantizeOptions& o) noexcept
{
    lastQuantize_      = o;
    lastQuantizeDirty_ = true;
    persistLastUsedToSettings();
}

void PianoRollView::setLastHumanizeOptions (const dsp::quantize::HumanizeOptions& o) noexcept
{
    lastHumanize_      = o;
    lastHumanizeDirty_ = true;
    persistLastUsedToSettings();
}

void PianoRollView::setLastScale (dsp::quantize::Scale s, int root) noexcept
{
    lastScale_      = s;
    lastScaleRoot_  = juce::jlimit (0, 11, root);
    lastScaleDirty_ = true;
    persistLastUsedToSettings();
}

namespace {

constexpr const char* kPropPrefix = "pianoRoll.quantize.";

juce::PropertiesFile* getUserProps (Services& svc) noexcept
{
    if (auto* gui = svc.find<GuiService>())
        return gui->settings().getUserSettings();
    return nullptr;
}

} // namespace

void PianoRollView::persistLastUsedToSettings()
{
    auto* props = getUserProps (services_);
    if (props == nullptr) return;

    if (lastQuantizeDirty_)
    {
        const auto& q = lastQuantize_;
        props->setValue (juce::String (kPropPrefix) + "noteLength", (int) q.noteLength);
        props->setValue (juce::String (kPropPrefix) + "noteType",   (int) q.noteType);
        props->setValue (juce::String (kPropPrefix) + "amount",     q.amount);
        props->setValue (juce::String (kPropPrefix) + "adjustStart",(bool) q.adjustStart);
        props->setValue (juce::String (kPropPrefix) + "adjustEnd",  (bool) q.adjustEnd);
        props->setValue (juce::String (kPropPrefix) + "swing",      q.swing);
        props->setValue (juce::String (kPropPrefix) + "randomBeats",q.randomBeats);
    }
    if (lastHumanizeDirty_)
    {
        const auto& h = lastHumanize_;
        props->setValue (juce::String (kPropPrefix) + "velRange", h.velocityRange);
        props->setValue (juce::String (kPropPrefix) + "velBias",  h.velocityBias);
    }
    if (lastScaleDirty_)
    {
        props->setValue (juce::String (kPropPrefix) + "scale",    (int) lastScale_);
        props->setValue (juce::String (kPropPrefix) + "root",     lastScaleRoot_);
    }
}

void PianoRollView::loadLastUsedFromSettings()
{
    auto* props = getUserProps (services_);
    if (props == nullptr) return;

    /* Quantize options.  Missing keys land as defaults via the
     * struct initialiser, then we overlay whatever the user
     * previously committed.  Dirty flag goes true if ANY key was
     * present, so subsequent Ctrl+Q hotkeys use last-used. */
    bool anyQuantize = false;
    const auto getDouble = [&] (const juce::String& key, double dflt) {
        if (! props->containsKey (key)) return dflt;
        anyQuantize = true;
        return props->getDoubleValue (key, dflt);
    };
    const auto getInt = [&] (const juce::String& key, int dflt) {
        if (! props->containsKey (key)) return dflt;
        anyQuantize = true;
        return props->getIntValue (key, dflt);
    };
    const auto getBool = [&] (const juce::String& key, bool dflt) {
        if (! props->containsKey (key)) return dflt;
        anyQuantize = true;
        return props->getBoolValue (key, dflt);
    };

    lastQuantize_.noteLength = static_cast<dsp::quantize::NoteLength> (
        juce::jlimit (0, 6, getInt (juce::String (kPropPrefix) + "noteLength",
                                      (int) lastQuantize_.noteLength)));
    lastQuantize_.noteType   = static_cast<dsp::quantize::NoteType> (
        juce::jlimit (0, 2, getInt (juce::String (kPropPrefix) + "noteType",
                                      (int) lastQuantize_.noteType)));
    lastQuantize_.amount      = getDouble (juce::String (kPropPrefix) + "amount",      lastQuantize_.amount);
    lastQuantize_.adjustStart = getBool   (juce::String (kPropPrefix) + "adjustStart", lastQuantize_.adjustStart);
    lastQuantize_.adjustEnd   = getBool   (juce::String (kPropPrefix) + "adjustEnd",   lastQuantize_.adjustEnd);
    lastQuantize_.swing       = getDouble (juce::String (kPropPrefix) + "swing",       lastQuantize_.swing);
    lastQuantize_.randomBeats = getDouble (juce::String (kPropPrefix) + "randomBeats", lastQuantize_.randomBeats);
    if (anyQuantize) lastQuantizeDirty_ = true;

    if (props->containsKey (juce::String (kPropPrefix) + "velRange")
        || props->containsKey (juce::String (kPropPrefix) + "velBias"))
    {
        lastHumanize_.velocityRange = props->getIntValue (
            juce::String (kPropPrefix) + "velRange", lastHumanize_.velocityRange);
        lastHumanize_.velocityBias  = props->getIntValue (
            juce::String (kPropPrefix) + "velBias",  lastHumanize_.velocityBias);
        lastHumanizeDirty_ = true;
    }

    if (props->containsKey (juce::String (kPropPrefix) + "scale")
        || props->containsKey (juce::String (kPropPrefix) + "root"))
    {
        lastScale_     = static_cast<dsp::quantize::Scale> (
            juce::jlimit (0, 11, props->getIntValue (
                juce::String (kPropPrefix) + "scale", (int) lastScale_)));
        lastScaleRoot_ = juce::jlimit (0, 11,
            props->getIntValue (juce::String (kPropPrefix) + "root", lastScaleRoot_));
        lastScaleDirty_ = true;
    }
}

void PianoRollView::notifyRegionEdited()
{
    if (onRegionEdited) onRegionEdited();
    if (velocityLane_ != nullptr)
        velocityLane_->repaint();
    if (ccLane_ != nullptr)
        ccLane_->repaint();
}

void PianoRollView::repaintControllerLanes()
{
    if (velocityLane_ != nullptr && velocityLane_->isVisible())
        velocityLane_->repaint();
    if (ccLane_ != nullptr && ccLane_->isVisible())
        ccLane_->repaint();
}

void PianoRollView::setRegion (const juce::Uuid& regionId)
{
    if (regionId == boundRegionId_) return;
    boundRegionId_ = regionId;
    refreshLabel();
    if (grid_ != nullptr)
    {
        /* Stale preview rings would survive a region rebind otherwise
         * -- a panel left open against region A would have its
         *  highlight ids still painted when the user navigates to
         *  region B.  Drop them at the rebind boundary (A.4 fix). */
        grid_->clearPreviewAffectedNotes();
        grid_->boundRegionChanged();
    }
    /* Re-run preview against the new region's selection so the panel
     * stays useful if it's open. */
    if (quantizePanel_ != nullptr && quantizePanelVisible_)
        quantizePanel_->refreshPreviewFromExternal();
    repaint();
}

void PianoRollView::setActiveTool (Tool t)
{
    if (t == activeTool_) return;
    activeTool_ = t;
    syncToolToggles();
    if (grid_ != nullptr)
        grid_->activeToolChanged();
}

void PianoRollView::syncToolToggles()
{
    selectBtn_.setToggleState (activeTool_ == Tool::Select, juce::dontSendNotification);
    pencilBtn_.setToggleState (activeTool_ == Tool::Pencil, juce::dontSendNotification);
    eraseBtn_ .setToggleState (activeTool_ == Tool::Erase,  juce::dontSendNotification);
    brushBtn_ .setToggleState (activeTool_ == Tool::Brush,  juce::dontSendNotification);
}

void PianoRollView::applySnapFromComboBox()
{
    if (grid_ == nullptr) return;
    /* Snap-in-beats lookup table.  Beat = quarter note convention so
     * 1/4 = 1.0 beat.  Triplet values are exact fractions (1/3, 2/3,
     * 1/6) -- the grid's snapBeat() rounding handles the math
     * correctly so the user gets pixel-perfect snap at any zoom. */
    double div = 0.25;
    switch (snapBox_.getSelectedId())
    {
        case 1:  div = 4.0;       break;   /* Bar (4/4 assumption) */
        case 2:  div = 2.0;       break;   /* 1/2 */
        case 3:  div = 1.0;       break;   /* 1/4 */
        case 4:  div = 0.5;       break;   /* 1/8 */
        case 5:  div = 0.25;      break;   /* 1/16 (default) */
        case 6:  div = 0.125;     break;   /* 1/32 */
        case 10: div = 2.0 / 3.0; break;   /* 1/4T  -- 2/3 beat */
        case 11: div = 1.0 / 3.0; break;   /* 1/8T  -- 1/3 beat */
        case 12: div = 1.0 / 6.0; break;   /* 1/16T -- 1/6 beat */
        default: break;
    }
    grid_->setSnapDivision (div);
}

void PianoRollView::refreshLabel()
{
    juce::String text ("Piano Roll -- no region bound");
    if (! boundRegionId_.isNull())
    {
        /* Prefer the region's name (.mid file basename, set by
         * importMidiFileToLane).  Falls back to the uuid prefix if
         * the region can't be resolved or is unnamed. */
        juce::String name;
        if (regionResolver_)
        {
            if (auto* region = regionResolver_ (boundRegionId_))
            {
                if (region->name.isNotEmpty())
                    name = region->name;
            }
        }
        if (name.isEmpty())
            name = boundRegionId_.toString().substring (0, 8);
        text = "MIDI -- " + name;
    }
    regionLabel_.setText (text, juce::dontSendNotification);
}

void PianoRollView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff'0a'0a'0a));

    /* Header strip backdrop -- matches ArrangementView's toolbar tone
     * so the dock reads as a sibling row. */
    g.setColour (juce::Colour (0xff'14'14'14));
    g.fillRect (0, kDragHandleH, getWidth(), kHeaderH);

    /* Top-left corner above the keyboard column: the same tone as the
     * grid's internal ruler so there's a continuous horizontal band
     * across the dock width (keyboard top-cap + grid ruler).  Without
     * this the corner reads as a darker square that looks like a
     * paint bug. */
    const int rulerTop = kDragHandleH + kHeaderH;
    g.setColour (juce::Colour (0xff'0c'0c'0c));
    g.fillRect (0, rulerTop, kKeyboardW, PianoRollGrid::kRulerH);
    /* 1 px brightener at the bottom of that corner cell, lining up
     * with the grid's ruler-bottom divider. */
    g.setColour (juce::Colour (0xff'30'30'30));
    g.drawHorizontalLine (rulerTop + PianoRollGrid::kRulerH - 1,
                           0.0f, (float) kKeyboardW);
}

void PianoRollView::resized()
{
    auto r = getLocalBounds();
    dragHandle_->setBounds (r.removeFromTop (kDragHandleH));

    /* Header layout LEFT -> RIGHT (mirrors ArrangementView's row):
     *   Select Pencil Erase | Snap snapBox | X+/-/Fit | Y+ Y-
     *   ...(label fills middle)... | close X
     */
    auto header = r.removeFromTop (kHeaderH);
    const int yPad = 3;
    header = header.reduced (4, yPad);

    auto layoutLeftBtn = [&header] (juce::Component& c, int w, int gap = 3) {
        c.setBounds (header.removeFromLeft (w));
        header.removeFromLeft (gap);
    };

    layoutLeftBtn (selectBtn_, kToolBtnW);
    layoutLeftBtn (pencilBtn_, kToolBtnW);
    layoutLeftBtn (eraseBtn_,  kToolBtnW);
    layoutLeftBtn (brushBtn_,  kToolBtnW, 12);

    layoutLeftBtn (snapBtn_,   kToolBtnW);
    layoutLeftBtn (snapBox_,   kSnapBoxW, 12);

    /* Bulk-edit ops: Q (Quantize), H (Humanize), S (Scale).  Click
     * opens the dialog at the matching tab; Ctrl+Q applies last
     * settings without the dialog. */
    layoutLeftBtn (quantizeBtn_, kZoomBtnW);
    layoutLeftBtn (humanizeBtn_, kZoomBtnW);
    layoutLeftBtn (scaleBtn_,    kZoomBtnW, 12);

    layoutLeftBtn (zoomOutBtn_, kZoomBtnW);
    layoutLeftBtn (zoomInBtn_,  kZoomBtnW);
    layoutLeftBtn (zoomFitBtn_, kZoomBtnW + 6, 12);

    layoutLeftBtn (yZoomOutBtn_, kZoomBtnW + 6);
    layoutLeftBtn (yZoomInBtn_,  kZoomBtnW + 6, 12);

    layoutLeftBtn (ccBtn_,       kZoomBtnW + 6, 12);

    /* Close X on the far right. */
    closeBtn_.setBounds (header.removeFromRight (kHeaderH - 2 * yPad));
    header.removeFromRight (6);

    /* Region label fills whatever's left in the middle.  Justify left
     * so the title doesn't drift around as the dock width changes. */
    regionLabel_.setBounds (header);

    /* Body: keyboard column on the left, grid viewport fills the rest.
     * Keyboard is offset DOWN by the grid's ruler height so the first
     * key (highest visible pitch) lines up vertically with the grid's
     * first note row.  The dock paints the corner above the keyboard
     * with the same colour as the grid's ruler strip so the gap reads
     * as intentional. */
    /* Quantize / Humanize / Scale panel docks on the RIGHT edge of
     * the body when visible -- claims the FULL body height (above
     * the velocity lane reservation so the panel + velocity lane
     * never leave an empty cell beneath the panel).  Reserved first
     * so the grid viewport gets what's left between the keyboard
     * column and the panel. */
    if (quantizePanel_ != nullptr && quantizePanelVisible_)
    {
        const int panelW = juce::jmin (r.getWidth() / 2, kQuantizePanelW);
        quantizePanel_->setBounds (r.removeFromRight (panelW));
    }

    /* Reserve kVelocityLaneH at the BOTTOM of what remains -- the
     * velocity strip lives only beneath the grid viewport, NOT
     * beneath the docked panel.  Lane spans the same horizontal
     * extent as the grid viewport so its lollipops line up with the
     * notes above; horizontal scroll is mirrored from the viewport
     * via the ScrollBar listener installed in the ctor. */
    /* CC lane sits at the very bottom, velocity lane just above it.
     * Remove the CC strip FIRST (bottom-most), then velocity. */
    juce::Rectangle<int> ccLaneArea;
    if (ccLane_ != nullptr && ccLane_->isVisible())
        ccLaneArea = r.removeFromBottom (kCcLaneH);

    juce::Rectangle<int> velLaneArea;
    if (velocityLane_ != nullptr && velocityLane_->isVisible())
        velLaneArea = r.removeFromBottom (kVelocityLaneH);

    if (keyboard_ != nullptr)
    {
        auto kbCol = r.removeFromLeft (kKeyboardW);
        keyboard_->setBounds (kbCol.withTrimmedTop (PianoRollGrid::kRulerH));
        /* PianoRollKeyboard::resized() recomputes per-key Y extent
         * from the new height + current visible range. */
    }
    else
    {
        r.removeFromLeft (kKeyboardW);
    }
    if (gridViewport_ != nullptr)
        gridViewport_->setBounds (r);

    if (velocityLane_ != nullptr && ! velLaneArea.isEmpty())
    {
        /* Lane occupies the SAME X+W as the grid viewport so notes
         * + lollipops align vertically. */
        velocityLane_->setBounds (velLaneArea.withLeft (r.getX())
                                                .withWidth (r.getWidth()));
        velocityLane_->setScrollX (gridViewport_ != nullptr
                                     ? gridViewport_->getViewPositionX()
                                     : 0);
    }

    if (ccLane_ != nullptr && ! ccLaneArea.isEmpty())
    {
        ccLane_->setBounds (ccLaneArea.withLeft (r.getX())
                                       .withWidth (r.getWidth()));
        ccLane_->setScrollX (gridViewport_ != nullptr
                                 ? gridViewport_->getViewPositionX()
                                 : 0);
    }

    /* Grid height tracks the viewport's inner height so each pitch
     * row stays aligned with the keyboard.  Width is updated by the
     * grid itself in updateSizeForRegion (called from
     * boundRegionChanged + when zoom changes). */
    if (grid_ != nullptr && gridViewport_ != nullptr)
        grid_->updateSizeForViewport (gridViewport_->getMaximumVisibleWidth(),
                                       gridViewport_->getMaximumVisibleHeight());
}

void PianoRollView::toggleQuantizePanel (int tabIndex)
{
    /* Map tabIndex -> enum.  Defensive clamp keeps a stray call from
     * landing on an out-of-range value. */
    auto tab = QuantizeDialog::Tab::Quantize;
    if      (tabIndex == 1) tab = QuantizeDialog::Tab::Humanize;
    else if (tabIndex == 2) tab = QuantizeDialog::Tab::Scale;

    /* If panel exists + visible + click is on the SAME tab, hide it
     * (button acts as a true toggle).  If the click is on a DIFFERENT
     * tab while already open, stay open + switch tab. */
    if (quantizePanel_ != nullptr && quantizePanelVisible_)
    {
        const bool sameTab = (quantizePanel_->getActiveTab() == tab);
        if (sameTab)
        {
            hideQuantizePanel();
            return;
        }
        quantizePanel_->switchTab (tab);
        syncToolbarTabToggles();
        return;
    }

    /* No panel yet, or hidden -- (re)build + show on `tab`. */
    if (quantizePanel_ == nullptr)
    {
        quantizePanel_ = std::make_unique<QuantizeDialog> (*this, tab);
        quantizePanel_->onCloseRequested = [this]() { hideQuantizePanel(); };
        addChildComponent (*quantizePanel_);
    }
    else
    {
        quantizePanel_->switchTab (tab);
    }
    quantizePanel_->setVisible (true);
    quantizePanelVisible_ = true;
    syncToolbarTabToggles();
    resized();
}

void PianoRollView::hideQuantizePanel()
{
    if (quantizePanel_ != nullptr)
    {
        quantizePanel_->setVisible (false);
        if (auto* grid = grid_.get())
            grid->clearPreviewAffectedNotes();
    }
    quantizePanelVisible_ = false;
    syncToolbarTabToggles();
    resized();
}

void PianoRollView::syncToolbarTabToggles()
{
    using Tab = QuantizeDialog::Tab;
    const bool vis = quantizePanelVisible_ && quantizePanel_ != nullptr;
    const Tab active = vis ? quantizePanel_->getActiveTab() : Tab::Quantize;
    quantizeBtn_.setToggleState (vis && active == Tab::Quantize, juce::dontSendNotification);
    humanizeBtn_.setToggleState (vis && active == Tab::Humanize, juce::dontSendNotification);
    scaleBtn_   .setToggleState (vis && active == Tab::Scale,    juce::dontSendNotification);
}

} // namespace element
