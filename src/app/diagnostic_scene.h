// Synthetic diagnostic scene — proves the native pipeline with zero
// original data. No MDK imagery, fonts, audio, levels, or extracted
// assets are used anywhere in this file.
//
// Content: palette gradient background, animated checkerboard band, a
// frame-index-driven bouncing box, and a row of binary frame-counter
// dots. All motion derives from the frame index, so a given --frames N
// run produces identical framebuffer contents every time
// (deterministic).

#ifndef MDK_APP_DIAGNOSTIC_SCENE_H
#define MDK_APP_DIAGNOSTIC_SCENE_H

#include <cstdint>

namespace mdk {

class IndexedFramebuffer;
class Palette;
class InputState;

class DiagnosticScene {
public:
  DiagnosticScene();

  void buildPalette(Palette& palette) const;
  void update(std::uint64_t frameIndex, const InputState& input);
  void render(IndexedFramebuffer& fb, const Palette& palette) const;

private:
  // Snapshot of input deltas for on-screen indicators (wheel/mouse).
  float mouseDx_ = 0, mouseDy_ = 0;
  float wheelX_ = 0, wheelY_ = 0;
  std::uint64_t frame_ = 0;
};

} // namespace mdk

#endif // MDK_APP_DIAGNOSTIC_SCENE_H
