#include "app/diagnostic_scene.h"

#include "core/compat.h"
#include "core/framebuffer.h"
#include "input/input_state.h"

namespace mdk {

namespace {

// Palette layout (synthetic; not original data):
//   0..15    : grayscale ramp
//   16..31   : hue sweep used for the background gradient
//   32..47   : checkerboard pair ramp
//   48..63   : accent colors for the box / markers
constexpr int kGrayBase = 0;
constexpr int kGradBase = 16;
constexpr int kCheckBase = 32;
constexpr int kAccentBase = 48;

} // namespace

DiagnosticScene::DiagnosticScene() = default;

void DiagnosticScene::buildPalette(Palette& palette) const {
  for (int i = 0; i < 16; ++i) {
    const auto v = static_cast<std::uint8_t>(i * 16);
    palette.set(kGrayBase + i, {v, v, v, 255});
  }
  for (int i = 0; i < 16; ++i) {
    // Simple hue sweep: rotate R/G/B dominance in 3 phases.
    const int phase = i / 6; // 0,1,2 (last partial)
    const int t = (i % 6) * 51;
    Palette::Color c{};
    switch (phase) {
    case 0: c = {static_cast<std::uint8_t>(255 - t), static_cast<std::uint8_t>(t), 0, 255}; break;
    case 1: c = {0, static_cast<std::uint8_t>(255 - t), static_cast<std::uint8_t>(t), 255}; break;
    default: c = {static_cast<std::uint8_t>(t), 0, static_cast<std::uint8_t>(255 - t), 255}; break;
    }
    palette.set(kGradBase + i, c);
  }
  for (int i = 0; i < 16; ++i) {
    const auto a = static_cast<std::uint8_t>(32 + i * 8);
    const auto b = static_cast<std::uint8_t>(160 + i * 4);
    palette.set(kCheckBase + i, {a, a, b, 255});
  }
  for (int i = 0; i < 16; ++i) {
    palette.set(kAccentBase + i,
                {static_cast<std::uint8_t>(255 - i * 8),
                 static_cast<std::uint8_t>(64 + i * 8),
                 static_cast<std::uint8_t>(i * 16), 255});
  }
}

void DiagnosticScene::update(std::uint64_t frameIndex,
                             const InputState& input) {
  frame_ = frameIndex;
  mouseDx_ = input.mouseDx();
  mouseDy_ = input.mouseDy();
  wheelX_ = input.wheelX();
  wheelY_ = input.wheelY();
}

void DiagnosticScene::render(IndexedFramebuffer& fb,
                             const Palette&) const {
  const int w = fb.width();
  const int h = fb.height();

  // 1) Horizontal palette gradient background.
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      fb.pixels()[static_cast<std::size_t>(y) * fb.stride() + x] =
          static_cast<std::uint8_t>(kGradBase + (x * 16) / w);
    }
  }

  // 2) Animated checkerboard band across the middle.
  const int bandTop = h / 2 - 40;
  const int bandBottom = h / 2 + 40;
  const int cell = 20;
  const int scroll = static_cast<int>(frame_ % (2 * cell));
  for (int y = bandTop; y < bandBottom; ++y) {
    for (int x = 0; x < w; ++x) {
      const bool on = (((x + scroll) / cell) + (y / cell)) & 1;
      fb.pixels()[static_cast<std::size_t>(y) * fb.stride() + x] =
          static_cast<std::uint8_t>(kCheckBase + (on ? 8 : 0));
    }
  }

  // 3) Bouncing box — position is a pure function of the frame index.
  const int boxW = 48, boxH = 36;
  const int spanX = w - boxW, spanY = h - boxH;
  const int period = 240;
  const int p = static_cast<int>(frame_ % period);
  const int tri = p < period / 2 ? p : period - p; // 0..120..0
  const int bx = (tri * spanX) / (period / 2);
  const int by = (tri * spanY) / (period / 2);
  for (int y = by; y < by + boxH; ++y) {
    for (int x = bx; x < bx + boxW; ++x) {
      fb.put(x, y, kAccentBase + 4);
    }
  }
  // Box outline.
  for (int x = bx; x < bx + boxW; ++x) {
    fb.put(x, by, kAccentBase + 12);
    fb.put(x, by + boxH - 1, kAccentBase + 12);
  }
  for (int y = by; y < by + boxH; ++y) {
    fb.put(bx, y, kAccentBase + 12);
    fb.put(bx + boxW - 1, y, kAccentBase + 12);
  }

  // 4) Frame counter: 16 binary dots, top-left corner.
  for (int bit = 0; bit < 16; ++bit) {
    const bool set = (frame_ >> bit) & 1;
    const int cx = 12 + bit * 12;
    for (int dy = 0; dy < 6; ++dy) {
      for (int dx = 0; dx < 6; ++dx) {
        fb.put(cx + dx, 10 + dy,
               set ? kAccentBase + 12 : kGrayBase + 4);
      }
    }
  }

  // 5) Input indicators, top-right: raw mouse/wheel deltas as bars.
  const int cx = w - 60, cy = 18;
  const int mx = cx + static_cast<int>(mouseDx_);
  const int my = cy + static_cast<int>(mouseDy_);
  for (int y = cy - 2; y <= cy + 2; ++y) {
    for (int x = cx - 2; x <= cx + 2; ++x) {
      fb.put(x, y, kGrayBase + 15);
    }
  }
  for (int y = my - 3; y <= my + 3; ++y) {
    for (int x = mx - 3; x <= mx + 3; ++x) {
      fb.put(x, y, kAccentBase + 8);
    }
  }
  const int wheelBar = static_cast<int>(wheelY_ * 8);
  for (int y = 0; y < (wheelBar < 0 ? -wheelBar : wheelBar); ++y) {
    const int yy = cy + (wheelBar < 0 ? y : -y);
    for (int x = w - 20; x < w - 12; ++x) {
      fb.put(x, yy, kAccentBase + 14);
    }
  }
}

} // namespace mdk
