// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/automation/curve.hpp"

#include <algorithm>
#include <cmath>

namespace element::dsp::automation {

double evaluateSegment (double x, double v0, double v1, CurveOptions opts) noexcept
{
    x = std::clamp (x, 0.0, 1.0);

    /* Straight chord (default handle) -- the common case, no sqrt. */
    const bool centred = std::abs (opts.offsetT - 0.5) < 1e-9;
    if (centred && std::abs (opts.offsetV) < 1e-12)
        return std::clamp (v0 + x * (v1 - v0), 0.0, 1.0);

    /* Value-space quadratic Bezier: endpoints P0=(0,v0), P2=(1,v1),
     * control point P1 derived so the curve passes through the handle
     * pin (offsetT, chordMid + offsetV) at Bezier parameter u=0.5.
     *   B(0.5) = 0.25 P0 + 0.5 P1 + 0.25 P2 = pin
     *   => P1 = 2*pin - 0.5*(P0 + P2)
     * Mirrors paintVolumeEnvelope's construction exactly. */
    const double chordMid = 0.5 * (v0 + v1);
    const double pinX = std::clamp (opts.offsetT, 0.25, 0.75);
    const double pinY = chordMid + opts.offsetV;
    const double cx = 2.0 * pinX - 0.5;          // P1.x
    const double cy = 2.0 * pinY - chordMid;     // P1.y  (= chordMid + 2*offsetV)

    /* Invert x(u) = (1-2cx) u^2 + 2cx u  for u in [0,1]. */
    const double a = 1.0 - 2.0 * cx;
    double u;
    if (std::abs (a) < 1e-9)
    {
        /* Linear in u (cx == 0.5): x = 2cx u = u. */
        u = (std::abs (cx) < 1e-12) ? x : x / (2.0 * cx);
    }
    else
    {
        /* a u^2 + 2cx u - x = 0  ->  u = (-cx + sqrt(cx^2 + a x)) / a.
         * The '+' root is the one in [0,1] for a monotonic segment
         * (offsetT clamped to [0.25,0.75] keeps x(u) monotonic). */
        const double disc = std::max (0.0, cx * cx + a * x);
        u = (-cx + std::sqrt (disc)) / a;
    }
    u = std::clamp (u, 0.0, 1.0);

    const double omu = 1.0 - u;
    const double y = omu * omu * v0 + 2.0 * omu * u * cy + u * u * v1;
    return std::clamp (y, 0.0, 1.0);
}

} // namespace element::dsp::automation
