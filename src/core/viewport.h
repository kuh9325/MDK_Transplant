// Window -> presentation-canvas -> working-image geometry.
//
// ORIGINAL ENGINE OBSERVATION: 600x360 working image, 640x480 8bpp display
// surface, 360 rows copied at present (Phase 2B). Original in-surface
// placement offset is UNKNOWN.
//
// NATIVE PORT PROJECT DECISION: treat 640x480 as a 4:3 presentation canvas
// that is aspect-fitted into the window (letterbox/pillarbox, never
// stretched). The 600x360 image is centered inside the canvas.

#ifndef MDK_CORE_VIEWPORT_H
#define MDK_CORE_VIEWPORT_H

namespace mdk {

struct RectD {
  double x = 0, y = 0, w = 0, h = 0;
};

// Largest centered rectangle with the given aspect ratio (wPerH) that fits
// inside (winW x winH). Returns an empty rect for non-positive input.
RectD aspectFit(double winW, double winH, double aspect);

// The drawable-space rect, in pixels, where the working image should be
// presented: aspectFit(4:3 canvas) then the centered 600x360 sub-rect.
RectD presentationRect(double winW, double winH);

} // namespace mdk

#endif // MDK_CORE_VIEWPORT_H
