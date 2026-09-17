// .FTI font record decoder (Phase 4C) — FONTSML/FONTBIG glyph format.
//
// EVIDENCE (bytes: MISC/MDKFONT.FTI records FONTSML 0xc7cb bytes and
// FONTBIG 0x1621c bytes; statics: Ghidra disasm/decomp of the original
// font subsystem — see docs/ENGINE_RECONSTRUCTION.md Phase 4C):
//
//   FUN_004149c4 resolves the three MDKFONT.FTI font records:
//     "FONTSML" -> DAT_00541644   (draw: FUN_00414dd4,
//                                  measure: FUN_00414d88,
//                                  centered: FUN_00414f1c)
//     "FONTBIG" -> DAT_00541648   (draw: FUN_00414c34 1:1,
//                                  FUN_00414f64 scaled,
//                                  measure: FUN_00414be8)
//     "F8"      -> DAT_0054164c   (different 8x8 1bpp mask format —
//                                  FUN_00414a08; NOT this decoder)
//
//   Payload layout (OBSERVED bytes + CODE-CORROBORATED draw paths —
//   identical for FONTSML and FONTBIG):
//
//     +0x000  u32 glyphOffset[256] — record-relative offsets indexed
//             DIRECTLY by the input byte (MOVZX char -> *4; no ASCII
//             base subtraction, no case folding, no code-page map).
//             Entry value 0 means "no glyph" for that byte.
//     +off    glyph record:
//       +0    s8 top     — bitmap rows ending ON the pen row:
//                        bitmap row 0 draws at fb row (penY - top)
//       +1    s8 bottom  — bitmap rows below the pen row (may be
//                        negative: '!' keeps its whole body above)
//       +2    u8 width   — bitmap row width AND horizontal advance
//       +3    u8 pixels[width * (top + bottom + 1)] — row-major,
//             top row first; byte 0 = skip (transparent), any
//             nonzero byte is a FINAL 8-bit palette index written
//             verbatim to the framebuffer (no mask, no caller color).
//
//   Draw rule (FUN_00414dd4, FONTSML; FUN_00414c34, FONTBIG):
//     dst = fb + penX + (penY - top)*600; for each row r (0..rows-1)
//     and column c (0..width-1): byte b = px[r*width + c]; if b != 0
//     then dst[c] = b. Rows walk DOWN the framebuffer (+600/row).
//     Pen advance after a glyph = width. A byte with no glyph (table
//     entry 0) advances by a per-consumer constant: 4 for the
//     FONTSML path (FUN_00414d88/FUN_00414dd4), 6 for FONTBIG
//     (FUN_00414be8/FUN_00414c34). These constants live in the draw
//     code, not the payload — pass them explicitly.
//
//   Color binding (CORROBORATED): glyph bytes are palette indices and
//   every index used by the real fonts is <= 62, inside the resident
//   64-entry system palette head SYS_PAL (loaded by FUN_0040163c).
//   The preview binds SYS_PAL[0:64] from the same FTI file; tail
//   entries 64-255 are never referenced by these glyphs.
//
//   Corpus check: all 204 FONTSML / 152 FONTBIG offsets land in
//   [0x400, recordEnd) and each glyph's consumed extent
//   3 + width*rows tiles the payload exactly (FONTSML: to EOF;
//   FONTBIG: 2 pad bytes of slack at EOF).
//
// Scope rule: the decoder validates the offset table and each glyph's
// proven extent, copies pixels verbatim, and reports per-entry bounds
// as metadata. Unknown slack after the last glyph is tolerated (the
// original never reads it) but reported via `trailingBytes`.
//
#ifndef MDK_CORE_FTI_FONT_H
#define MDK_CORE_FTI_FONT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mdk {

class IndexedFramebuffer;

// CODE-CORROBORATED layout constants.
inline constexpr std::size_t kFtiFontGlyphSlots = 256;   // u8 char range
inline constexpr std::uint64_t kFtiFontTableBytes =
    kFtiFontGlyphSlots * 4;                              // 0x400
inline constexpr std::size_t kFtiGlyphHeaderBytes = 3;   // s8,s8,u8

// Per-consumer missing-glyph advances (constants in the original draw
// code — not stored in the payload). OBSERVED.
inline constexpr int kFtiFontSmlMissingAdvance = 4;  // FUN_00414dd4
inline constexpr int kFtiFontBigMissingAdvance = 6;  // FUN_00414c34

// One decoded glyph. `pixels` is the verbatim bitmap: rows() * width
// bytes, row-major top-down; 0 = transparent (skipped by the original
// draw), nonzero = final palette index.
struct FtiGlyph {
  std::uint8_t code = 0;         // table index that mapped here
  std::uint32_t offset = 0;      // record-relative glyph offset (raw)
  std::int8_t top = 0;           // s8 +0x00 — bitmap rows to pen row
  std::int8_t bottom = 0;        // s8 +0x01 — rows below pen row
  std::uint8_t width = 0;        // u8 +0x02 — width AND advance
  std::vector<std::uint8_t> pixels;

  int rows() const { return int(top) + int(bottom) + 1; }
  // Bytes the original draw consumes for this glyph: header plus
  // bitmap. A non-positive row count draws nothing (legal form — the
  // original skips the loop but still advances the pen).
  std::uint64_t consumedBytes() const {
    return kFtiGlyphHeaderBytes +
           (rows() > 0 ? std::uint64_t(width) * std::uint64_t(rows())
                       : 0);
  }
};

struct FtiFont {
  // glyphs[i] is present iff offset-table entry i was nonzero.
  std::vector<std::optional<FtiGlyph>> glyphs;  // size 256
  std::size_t mappedCount = 0;
  int firstMapped = -1;   // lowest mapped code (-1 if none)
  int lastMapped = -1;    // highest mapped code
  std::uint8_t maxPixelIndex = 0;  // highest nonzero byte seen
  std::uint64_t payloadBytes = 0;
  std::uint64_t glyphDataStart = kFtiFontTableBytes;
  std::uint64_t trailingBytes = 0;  // slack after the last glyph end
  // Corpus-OBSERVED properties (reported, not load-bearing):
  bool offsetsSortedAscending = false;
  bool offsetsUnique = false;

  const FtiGlyph* glyphFor(std::uint8_t code) const {
    const auto& g = glyphs[code];
    return g ? &*g : nullptr;
  }
};

// Decode one FONTSML/FONTBIG-style font record. `payload` must be the
// record span only (the caller resolves the FTI directory). Bounds-
// checked throughout: returns nullopt with a human-readable reason in
// `err` on any malformed input — short table, offset inside the table
// region or past the payload, truncated header, or bitmap overrun.
// A payload with no mapped glyphs is rejected (not a font resource).
std::optional<FtiFont> decodeFtiFont(std::span<const std::byte> payload,
                                     std::string* err);

// Deterministic FNV-1a64 digest over the decoded font: for each table
// slot in order {code, mapped?, top, bottom, width, pixels}. Stable
// across the app and the inspector; never hashes filesystem data.
std::uint64_t ftiFontDigest(const FtiFont& font);

// --- Draw helpers (mirror the proven original paths) ---------------

// FUN_00414dd4 / FUN_00414c34 inner blit: glyph bitmap row 0 lands on
// framebuffer row (penY - top); each nonzero byte overwrites the
// destination, zero bytes are skipped; pen advances by `width`.
// The original writes unconditionally — this version bounds-checks
// each pixel against the framebuffer (native hardening only).
void drawFtiGlyph(const FtiGlyph& glyph, IndexedFramebuffer& fb,
                  int penX, int penY);

// Byte-string draw per FUN_00414dd4/FUN_00414c34: each byte indexes
// the table directly; mapped glyphs draw and advance `width`, unmapped
// bytes advance `missingAdvance` (proven constants: 4 for FONTSML, 6
// for FONTBIG). Returns the pen x after the last byte. The original
// stops at NUL; argv input cannot contain one, so all bytes are
// consumed (a 0 byte behaves as an unmapped code either way).
int drawFtiText(const FtiFont& font, std::string_view text,
                IndexedFramebuffer& fb, int penX, int penY,
                int missingAdvance);

// Width of a byte string in pixels per FUN_00414d88/FUN_00414be8:
// sum of glyph widths plus `missingAdvance` per unmapped byte.
int measureFtiText(const FtiFont& font, std::string_view text,
                   int missingAdvance);

// FUN_00414f64 — the FONTBIG scaled draw (Phase 4D). EVIDENCE
// (instruction-level, original binary):
//   scale <= 0.05        -> draws nothing (FCOMP d[0x4950f4], JBE)
//   scale == 1.0         -> identical to drawFtiText (FUN_00414c34)
//   otherwise, per mapped glyph:
//     glyphTopY = trunc(y - top*scale)
//     srcStep   = trunc(65536.0 / scale)          (16.16 source step)
//     srcRow    = (top<<16) - (y - glyphTopY)*srcStep
//     rows while srcRow < (top+bottom+1)<<16:
//       srcLine = (srcRow>0 ? srcRow>>16 : 0) * width
//       srcCol  = 0; while srcCol < width<<16:
//         byte b = px[srcLine + (srcCol>>16)]; if b: dst = b
//         srcCol += srcStep; dst++
//       dstRow += stride; srcRow += srcStep
//     penX = trunc(penX + width*scale)
//   unmapped byte: penX = trunc(penX + missingAdvance*scale)
// All truncations are the original's x87 truncation-toward-zero
// (FUN_0047d59a sets RC=11 then FRNDINT). The original performs NO
// clipping — this version bounds-checks each write (native
// hardening only; unreachable for the proven options state).
// Returns the pen x after the last byte (truncated pen position).
int drawFtiTextScaled(const FtiFont& font, std::string_view text,
                      IndexedFramebuffer& fb, int penX, int penY,
                      float scale, int missingAdvance);

} // namespace mdk

#endif // MDK_CORE_FTI_FONT_H
