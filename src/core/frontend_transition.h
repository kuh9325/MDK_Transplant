// Returning-entry transition record + palette timeline
// (FUN_0041d85c arm, FUN_0041e554 dispatcher, FUN_0041e500 pixel
// decoder — all OBSERVED for BUILD_A).
//
// OPTIONS.BNI record layout (INTRO1A; INTRO2 exists only in the
// nine-phase variant FUN_0041e838 and is absent from the BUILD_A
// corpus, so it is not implemented):
//   [+0x000] palette A — 768 raw r,g,b triplets
//   [+0x300] palette B — 768 raw r,g,b triplets
//   [+0x600] PackBits-style RLE -> 600x360 indexed pixels
//
// RLE control byte c (FUN_0041e500):
//   c == 0        -> end of stream
//   1..127        -> repeat the next byte c times
//   128..255      -> copy the next (256 - c) bytes literally
//
// Six-phase machine (FUN_0041e554; accumulator steps 1/30 per
// frontend tick, phases advance on overflow):
//   1  decode + bind record, then 2
//   2  fade in:   pal = black->A, frac += 1/30 until 1.0  (30 ticks)
//   3  hold A:    frac += 1/30 until > 3.0                (90 ticks)
//   4  crossfade: pal = lerp(A,B,frac), frac += 1/60 until 1.0
//                                                          (60 ticks)
//   5  hold B:    frac += 1/30 until > 3.0                (90 ticks)
//   6  fade out:  pal = B->black, frac += 1/30 until 1.0  (30 ticks)
//   0  done: black palette, menu resources reload
//
// Blend math (FUN_004164d0 fade-to-base / FUN_0041657c crossfade):
//   k = round-half-even(frac * 256)
//   fade:      out[i] = (src[i]*k + base[i]*(256-k)) >> 8
//   crossfade: out[i] = (a[i]*(256-k) + b[i]*k) >> 8
// (arithmetic shift, no clamp — frac stays within [0,1] by phase
// construction; this port clamps defensively on boundary input).
//
// Total nominal duration: (30+90+60+90+30)/30 = 10.0 seconds. The
// original skips to the end on any key edge (0x54b57c); the port
// surfaces that through the bridge's frontend_transition_complete.
//
#ifndef MDK_CORE_FRONTEND_TRANSITION_H
#define MDK_CORE_FRONTEND_TRANSITION_H

#include "core/compat.h"
#include "core/indexed_image.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace mdk {

struct FrontendTransitionImage {
  IndexedImage image;  // 600x360 indexed pixels (hasPalette=false —
                       // the record's twin palettes blend it)
  std::array<std::uint8_t, compat::kPaletteEntries * 3> paletteA{};
  std::array<std::uint8_t, compat::kPaletteEntries * 3> paletteB{};
};

// Decodes an INTRO1A-family record ({palA, palB, RLE pixels}).
// Rejects truncated/short RLE streams and wrong dimensions.
std::optional<FrontendTransitionImage>
decodeFrontendTransitionRecord(std::span<const std::uint8_t> record);

// Nominal timeline duration in seconds (OBSERVED: 300 ticks of 1/30).
inline constexpr double kFrontendTransitionSeconds = 10.0;

inline constexpr double kFrontendTransitionFadeInEnd = 1.0;
inline constexpr double kFrontendTransitionHoldAEnd = 4.0;
inline constexpr double kFrontendTransitionCrossEnd = 6.0;
inline constexpr double kFrontendTransitionHoldBEnd = 9.0;

inline bool frontendTransitionDone(double seconds) {
  return seconds >= kFrontendTransitionSeconds;
}

// Evaluates the blended 256-entry palette at `seconds` since arm
// (input clamped to [0, 10]). `out768` receives 768 r,g,b bytes.
void frontendTransitionPalette(const FrontendTransitionImage& img,
                               double seconds,
                               std::uint8_t* out768);

// Raw primitives — exposed for tests that pin the OBSERVED formulas.
void frontendPaletteFadeToBase(std::uint8_t* dst,
                               const std::uint8_t* src,
                               const std::uint8_t base[3],
                               double frac);
void frontendPaletteCrossfade(std::uint8_t* dst,
                              const std::uint8_t* a,
                              const std::uint8_t* b, double frac);

}  // namespace mdk

#endif  // MDK_CORE_FRONTEND_TRANSITION_H
