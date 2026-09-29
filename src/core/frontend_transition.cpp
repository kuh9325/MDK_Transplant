#include "core/frontend_transition.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mdk {

namespace {

// FUN_0041e500 (OBSERVED): PackBits-style RLE into the bound slide
// buffer. Returns false when the stream overruns the input or the
// 600x360 output; a stream that ends early leaves the tail at 0 —
// the original writes into a cleared allocation.
bool decodeTransitionRle(std::span<const std::uint8_t> in,
                         std::uint8_t* out, std::size_t outSize) {
  std::size_t si = 0, di = 0;
  while (si < in.size()) {
    const std::uint8_t c = in[si++];
    if (c == 0) return true;
    if (c < 0x80) {
      if (si >= in.size() || di + c > outSize) return false;
      std::memset(out + di, in[si++], c);
      di += c;
    } else {
      const std::size_t n = 256u - c;
      if (si + n > in.size() || di + n > outSize) return false;
      std::memcpy(out + di, in.data() + si, n);
      si += n;
      di += n;
    }
  }
  return true;  // input exhausted without terminator (as original)
}

// k = round-half-even(frac * 256), clamped to [0,256].
int blendKey(double frac) {
  frac = std::clamp(frac, 0.0, 1.0);
  const long k = std::lrint(frac * 256.0);
  return static_cast<int>(std::clamp(k, 0L, 256L));
}

}  // namespace

std::optional<FrontendTransitionImage>
decodeFrontendTransitionRecord(std::span<const std::uint8_t> record) {
  constexpr std::size_t kPal = compat::kPaletteEntries * 3;
  constexpr std::size_t kPixels = 600u * 360u;
  if (record.size() <= kPal * 2) return std::nullopt;

  FrontendTransitionImage t;
  std::memcpy(t.paletteA.data(), record.data(), kPal);
  std::memcpy(t.paletteB.data(), record.data() + kPal, kPal);

  t.image.width = 600;
  t.image.height = 360;
  t.image.stride = 600;
  t.image.pixels.assign(kPixels, 0);
  t.image.hasPalette = false;
  if (!decodeTransitionRle(record.subspan(kPal * 2),
                           t.image.pixels.data(), kPixels)) {
    return std::nullopt;
  }
  return t;
}

void frontendPaletteFadeToBase(std::uint8_t* dst,
                               const std::uint8_t* src,
                               const std::uint8_t base[3],
                               double frac) {
  const int k = blendKey(frac);
  const int inv = 256 - k;
  for (int i = 0; i < compat::kPaletteEntries; ++i) {
    dst[i * 3 + 0] =
        static_cast<std::uint8_t>((src[i * 3 + 0] * k + base[0] * inv) >> 8);
    dst[i * 3 + 1] =
        static_cast<std::uint8_t>((src[i * 3 + 1] * k + base[1] * inv) >> 8);
    dst[i * 3 + 2] =
        static_cast<std::uint8_t>((src[i * 3 + 2] * k + base[2] * inv) >> 8);
  }
}

void frontendPaletteCrossfade(std::uint8_t* dst, const std::uint8_t* a,
                              const std::uint8_t* b, double frac) {
  const int k = blendKey(frac);
  const int inv = 256 - k;
  for (int i = 0; i < compat::kPaletteEntries * 3; ++i) {
    dst[i] = static_cast<std::uint8_t>((a[i] * inv + b[i] * k) >> 8);
  }
}

void frontendTransitionPalette(const FrontendTransitionImage& img,
                               double seconds, std::uint8_t* out768) {
  static constexpr std::uint8_t kBlack[3] = {0, 0, 0};
  const double t =
      std::clamp(seconds, 0.0, kFrontendTransitionSeconds);

  if (t >= kFrontendTransitionSeconds || t <= 0.0) {
    // Phase 0 / initial frame: black (phase 0 fades palB->black to
    // frac 0 and phase 1 uploads before the first fade-in tick).
    frontendPaletteFadeToBase(out768, img.paletteB.data(), kBlack, 0.0);
  } else if (t < kFrontendTransitionFadeInEnd) {
    frontendPaletteFadeToBase(out768, img.paletteA.data(), kBlack, t);
  } else if (t < kFrontendTransitionHoldAEnd) {
    std::memcpy(out768, img.paletteA.data(), compat::kPaletteEntries * 3);
  } else if (t < kFrontendTransitionCrossEnd) {
    frontendPaletteCrossfade(out768, img.paletteA.data(),
                             img.paletteB.data(),
                             (t - kFrontendTransitionHoldAEnd) /
                                 (kFrontendTransitionCrossEnd -
                                  kFrontendTransitionHoldAEnd));
  } else if (t < kFrontendTransitionHoldBEnd) {
    std::memcpy(out768, img.paletteB.data(), compat::kPaletteEntries * 3);
  } else {
    // frac = 1.0 - accum == (end - t) / 1s.
    frontendPaletteFadeToBase(out768, img.paletteB.data(), kBlack,
                              kFrontendTransitionSeconds - t);
  }
}

}  // namespace mdk
