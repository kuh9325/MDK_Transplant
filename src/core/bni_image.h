// BNI interior: indexed bitmap payloads (Phase 4A).
//
// EVIDENCE — the original's own readers (instruction-level disasm +
// decompilation; real-byte agreement in BUILD_A):
//
//   Indexed-only bitmap  {u16le w, u16le h, u8 px[w*h]}:
//     FUN_00403a00 reads the two payload-head u16s, returns the pixel
//     pointer at payload+4 and computes w*h. Byte agreement 8/8
//     candidates (BG 600x360, SPACE 600x360, PLANET/MOON 128x128,
//     EARTH 512x512, SKULL 256x256, TRAVSPRT SKULL 256x256 — each
//     payload tiles exactly: size == 4 + w*h).
//
//   Palette-embedded bitmap {u8 rgb[768], u16le w, u16le h, u8 px[w*h]}:
//     the MDKOPT / L1_INTRM / L1..L5_MAP class. The options-screen
//     orchestrator FUN_0041dc90 selects the payload HEAD as the
//     palette source (FUN_00416700 consumes 0x300 = 768 bytes ->
//     FUN_0046d208 expands RGB triplets into 4-byte entries ->
//     IDirectDrawPalette::SetEntries — PALETTEENTRY order proves
//     file byte order is R,G,B); FUN_0041d7b4 derives the pixel
//     pointer as payload+0x304 (768 + 4) and the blit copies
//     0x34bc0 = 216000 = 600*360 index bytes verbatim into the
//     600x360 work surface DAT_00541650. FUN_0041ebf4 repeats the
//     same +0x304 / 54000-dword copy. Payload tiles exactly:
//     size == 772 + w*h (OBSERVED 4/4: MDKOPT, L1_INTRM, L1_MAP,
//     L5_MAP all 600x360).
//
//   Row order is top-down (FUN_0046c86c copies row i of the work
//   surface to surface row 60+i; the blit is a contiguous copy so
//   payload row order == surface row order). Stride == width (the
//   54000-dword copy has no row padding). No transparency: the blit
//   overwrites every work-surface byte. The indexed-only class is
//   the same shape (the BG blit FUN_0042e684 copies with stride 600
//   == width, top-down, overwriting every byte).
//
// The 768-byte head is only a palette when the consumer uses it as
// one — probe order checks the paletted shape FIRST because a
// palette head could itself carry u16s that mimic the indexed-only
// shape.
//
#ifndef MDK_CORE_BNI_IMAGE_H
#define MDK_CORE_BNI_IMAGE_H

#include "core/indexed_image.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace mdk {

// Header sizes within the resource payload.
inline constexpr std::size_t kBniImagePaletteBytes = 768;  // 256 x RGB
inline constexpr std::size_t kBniImageDimHeaderBytes = 4;  // u16 w, u16 h
inline constexpr std::size_t kBniPalettedHeaderBytes =
    kBniImagePaletteBytes + kBniImageDimHeaderBytes;

enum class BniImageShape {
  kPaletted,     // {rgb[768], u16 w, u16 h, px[w*h]}
  kIndexedOnly,  // {u16 w, u16 h, px[w*h]}
  kOther,        // payload does not match a proven image layout
};

struct BniImageProbe {
  BniImageShape shape = BniImageShape::kOther;
  int width = 0;
  int height = 0;
  std::size_t headerBytes = 0;   // bytes before the first pixel
  std::size_t pixelBytes = 0;    // == width*height when an image shape
};

// Classify a BNI record payload by the two proven image layouts.
// Pure bounds/shape check — no pixel interpretation.
BniImageProbe probeBniImage(std::span<const std::byte> payload);

std::string_view bniImageShapeName(BniImageShape s);

// Decode a palette-embedded BNI image (the MDKOPT class). Requires
// the payload to tile EXACTLY: size == 772 + w*h, w/h > 0 — no silent
// truncation. The pixel indices and all 256 RGB entries are copied
// verbatim (R,G,B order per the proven SetEntries path).
std::optional<IndexedImage> decodeBniPalettedImage(
    std::span<const std::byte> payload, std::string* error = nullptr);

// Decode an indexed-only BNI image (the BG/SPACE/PLANET class) with
// an externally supplied palette. `imagePayload` must tile EXACTLY:
// size == 4 + w*h, w/h > 0. `palettePayload` must be exactly the
// 768-byte effective RGB table (256 x {R,G,B}) the consumer uploads —
// this decoder never invents colors and never reaches for a sibling
// record itself; pairing an image with its palette is the caller's
// (context) responsibility (see core/stream_context.h for the one
// proven binding so far).
std::optional<IndexedImage> decodeBniIndexedImage(
    std::span<const std::byte> imagePayload,
    std::span<const std::byte> palettePayload,
    std::string* error = nullptr);

} // namespace mdk

#endif // MDK_CORE_BNI_IMAGE_H
