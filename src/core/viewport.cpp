#include "core/viewport.h"

#include "core/compat.h"

namespace mdk {

RectD aspectFit(double winW, double winH, double aspect) {
  if (winW <= 0 || winH <= 0 || aspect <= 0) {
    return {};
  }
  const double winAspect = winW / winH;
  RectD r;
  if (winAspect > aspect) {
    // Window wider than canvas: pillarbox.
    r.h = winH;
    r.w = winH * aspect;
    r.x = (winW - r.w) * 0.5;
  } else {
    // Window taller than canvas: letterbox.
    r.w = winW;
    r.h = winW / aspect;
    r.y = (winH - r.h) * 0.5;
  }
  return r;
}

RectD presentationRect(double winW, double winH) {
  const double canvasAspect =
      static_cast<double>(compat::kPresentWidth) / compat::kPresentHeight;
  const RectD canvas = aspectFit(winW, winH, canvasAspect);
  if (canvas.w <= 0) {
    return canvas;
  }
  // Centered 600x360 sub-rect inside the 640x480 canvas (PROJECT DECISION;
  // original offset UNKNOWN — see compat.h).
  const double sx = static_cast<double>(compat::kWorkWidth) / compat::kPresentWidth;
  const double sy = static_cast<double>(compat::kWorkHeight) / compat::kPresentHeight;
  RectD r;
  r.w = canvas.w * sx;
  r.h = canvas.h * sy;
  r.x = canvas.x + (canvas.w - r.w) * 0.5;
  r.y = canvas.y + (canvas.h - r.h) * 0.5;
  return r;
}

} // namespace mdk
