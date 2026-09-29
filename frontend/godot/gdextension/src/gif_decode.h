// GIF87a/89a still-image decoder for the attract slides
// (MISC/MDKS_%03d.GIF). Presentation-side only — the core has no
// GIF decoder by design (kStandardExternalFormat; the original's
// own decoder is FUN_00416e98, which writes indexed pixels + a
// palette pointer into the slide bindings 0x49aaa0/0x49aa9c).
//
// Scope (corpus-proven): GIF87a, single image descriptor,
// non-interlaced, global color table only, 8-bit LZW, logical
// screen 600x360. Anything outside that shape (interlace flag,
// local color table, multi-image/animation, trailer-less short
// reads) decodes as failure — the caller keeps the
// probe-failure/menu-visible fallback the original uses for
// unbindable slides.
//
#ifndef MDK_BRIDGE_GIF_DECODE_H
#define MDK_BRIDGE_GIF_DECODE_H

#include "core/indexed_image.h"

#include <cstdint>
#include <optional>
#include <span>

namespace mdkbridge {

// Decodes the FIRST image of a GIF stream into an indexed image
// (palette = the resolved global color table). Returns nullopt on
// any structural failure or on bounds violations.
std::optional<mdk::IndexedImage> decodeGifImage(
    std::span<const std::uint8_t> bytes);

}  // namespace mdkbridge

#endif  // MDK_BRIDGE_GIF_DECODE_H
