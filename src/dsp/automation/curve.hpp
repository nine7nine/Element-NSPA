// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace element::dsp::automation {

/** Per-segment curve descriptor stored on each AutomationPoint.  The
 *  curve describes the shape FROM this point TO the next point in time
 *  order.
 *
 *  This is the SAME 2D draggable-handle model the audio-clip volume
 *  envelope uses (EnvelopePoint.curveOffsetT / curveOffsetDb), unified
 *  here so the timeline automation overlay + piano-roll CC lanes shape
 *  curves exactly like the volume envelope (a quadratic Bezier you bend
 *  by dragging a handle in 2D), instead of the old algorithmic 1D
 *  "curviness" families (removed).
 *
 *    offsetT  -- normalised X of the bend handle within the segment,
 *                in [0.25, 0.75] (clamped at the UI layer; outside that
 *                the quadratic Bezier's x(u) goes non-monotonic).
 *                0.5 == handle horizontally centred.
 *    offsetV  -- normalised VALUE offset of the handle from the chord
 *                midpoint (NOT a fraction of the segment): positive
 *                bulges the curve UP, negative DOWN, regardless of
 *                whether the segment rises or falls -- matching the
 *                volume envelope's "drag up = bulge up" feel.
 *
 *  Defaults (0.5, 0.0) == handle on the straight chord == linear.
 *
 *  POD / trivially-copyable so vectors of AutomationPoint stay cheap. */
struct CurveOptions
{
    double offsetT { 0.5 };
    double offsetV { 0.0 };
};

/** Sample the segment curve at normalised x in [0, 1] and return the
 *  actual normalised VALUE in [0, 1].
 *
 *  @param x    Position along the segment, 0 = from-point, 1 = to-point.
 *              Clamped internally.
 *  @param v0   from-point value (normalised [0, 1]).
 *  @param v1   to-point value   (normalised [0, 1]).
 *  @param opts Bend-handle offsets.
 *
 *  Default handle (0.5, 0) returns the straight lerp v0..v1.  A bent
 *  handle evaluates the value-space quadratic Bezier with endpoints
 *  (0, v0), (1, v1) and control point derived so the curve passes
 *  through (offsetT, 0.5*(v0+v1) + offsetV) at Bezier parameter u=0.5 --
 *  identical to paintVolumeEnvelope's construction. */
double evaluateSegment (double x, double v0, double v1, CurveOptions opts) noexcept;

} // namespace element::dsp::automation
