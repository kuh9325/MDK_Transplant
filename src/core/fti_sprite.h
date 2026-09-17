// .FTI sprite record decoder (Phase 4D) — the ARROW cursor format.
//
// EVIDENCE (bytes: MISC/MDKFONT.FTI record ARROW, 96-byte content at
// file offset 0xecc; statics: Ghidra disasm of the original consumer
// chain — see docs/ENGINE_RECONSTRUCTION.md Phase 4D and
// analysis-private/logs/phase4d-evidence.md):
//
//   Consumer chain (OBSERVED, instruction-level):
//     FUN_004236c0(x, y) — the cursor-arrow draw used by the front-end
//       screens. Lazily resolves FTI record "ARROW" via the name lookup
//       FUN_00414890 (returns fileBase+4+dirOffset), caches
//       content+4 in DAT_0049ac78, then draws frame 0:
//         sprite = (content+4) + u32@(content+8)
//         FUN_00409760(x, y, sprite)
//     FUN_00409760 — generic sprite-header reader shared by several
//       draw sites: w=u16@+0, h=u16@+2, hotspot=(s16@+4, s16@+6);
//       calls FUN_00415ff0(x - hx, y - hy, &{w,h}, sprite+8).
//     FUN_00415ff0 — the command-stream blitter into the 600-stride
//       indexed work buffer DAT_00541650.
//
//   Record payload layout (CODE-CORROBORATED by the resolver +
//   byte-corroborated by the real ARROW payload):
//
//     +0x00  u32 blockBytes — bytes following this field (ARROW: 91 =
//            4 count + 4 offset + 8 header + 75 stream). NOT read by
//            the draw path; reported as metadata only.
//     +0x04  u32 frameCount
//     +0x08  u32 frameOffset[frameCount] — each relative to +0x04
//            (the frameCount field), i.e. frame i sits at
//            payload+4+frameOffset[i].
//     frame: +0 u16 width, +2 u16 height,
//            +4 s16 hotspotX, +6 s16 hotspotY, +8 stream
//
//   Stream commands (FUN_00415ff0, OBSERVED):
//     0x00-0x7f  literal packet: cmd+1 pixel bytes follow; each byte is
//              a FINAL palette index — nonzero overwrites the dest,
//              byte 0 is skipped (transparent). Dest advances 1 per
//              byte regardless.
//     0x80-0xfd  run packet: count = cmd-0x7c (4..129), one value byte
//              follows; value 0 advances the dest by count without
//              writing (transparent run), nonzero writes count copies.
//     0xfe       row break: next row down; the column resets to the
//              sprite x. If the row reaches the framebuffer bottom
//              (360) the draw returns.
//     0xff       end of stream.
//
//   Original clipping (OBSERVED — reproduced by blitFtiSpriteFrame):
//     entry:  x>=600 or y>=360 or x+w<=0 or y+h<=0 -> nothing drawn.
//             x>=0 && x+w>600 -> nothing drawn (right edge is
//             all-or-nothing; there is NO per-pixel right clip).
//             y<0: commands are consumed without writes until the row
//             counter reaches 0 (top clip). x<0: pixels left of the
//             left edge are skipped per packet (left clip).
//     packets are allowed to spill past the declared row width into
//     the next row — the stream is trusted; there is no per-row width
//     check and no explicit bottom clip beyond the 0xfe row counter.
//
//   HARDENING (native-only, behavior-identical on well-formed data):
//   the original would run off the record on a truncated stream and
//   could write past the framebuffer on a malformed one. This decoder
//   validates that every stream terminates with 0xff inside the
//   record span, and the blitter bounds every destination write to
//   the framebuffer rectangle.
//
//   Color/transparency (CORROBORATED): stream values are palette
//   indices — no caller color, no mask. ARROW uses only index 1
//   (white under both the MDKOPT palette head and SYS_PAL). Byte 0 is
//   the only transparency mechanism; it skips destination writes.
//
#ifndef MDK_CORE_FTI_SPRITE_H
#define MDK_CORE_FTI_SPRITE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdk {

class IndexedFramebuffer;

// CODE-CORROBORATED layout constants.
inline constexpr std::uint64_t kFtiSpriteBlockSizeOffset = 0x00; // u32
inline constexpr std::uint64_t kFtiSpriteCountOffset = 0x04;     // u32
inline constexpr std::uint64_t kFtiSpriteOffsetsOffset = 0x08;   // u32[count]
inline constexpr std::size_t kFtiSpriteFrameHeaderBytes = 8;     // u16,u16,s16,s16

// Stream command classes (FUN_00415ff0).
inline constexpr std::uint8_t kFtiSpriteRowBreak = 0xfe;  // next row
inline constexpr std::uint8_t kFtiSpriteStreamEnd = 0xff; // terminator
// <0x80: literal packet, cmd+1 pixel bytes. >=0x80 && <=0xfd: run
// packet, cmd-0x7c copies of one value byte.
inline constexpr std::uint8_t kFtiSpriteRunBase = 0x7c;   // count bias

// One decoded sprite frame. `stream` is the verbatim command byte
// range including the terminating 0xff; the blitter interprets it
// with the original rules (above) — pixels are NOT pre-expanded so
// the run/literal semantics stay byte-exact.
struct FtiSpriteFrame {
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::int16_t hotspotX = 0;   // dest x = caller x - hotspotX
  std::int16_t hotspotY = 0;   // dest y = caller y - hotspotY
  std::vector<std::uint8_t> stream;   // commands through 0xff
  std::uint64_t frameFileOffset = 0;  // diagnostic only

  // Stream statistics gathered by the terminating parse (metadata).
  std::uint32_t literalPackets = 0;
  std::uint32_t runPackets = 0;
  std::uint32_t transparentRuns = 0;  // run packets with value 0
  std::uint32_t rowBreaks = 0;
  std::uint64_t pixelAdvances = 0;    // dest columns stepped over
  std::uint64_t opaqueWrites = 0;     // nonzero pixel writes
  std::uint8_t minPixelIndex = 0;     // lowest nonzero index (0 = none)
  std::uint8_t maxPixelIndex = 0;
};

struct FtiSprite {
  std::uint32_t blockBytes = 0;                 // u32 @+0 (metadata)
  std::vector<FtiSpriteFrame> frames;           // frameCount entries
  std::uint64_t payloadBytes = 0;
  std::uint64_t trailingBytes = 0;  // bytes after the last frame's end

  const FtiSpriteFrame* frame(std::size_t i) const {
    return i < frames.size() ? &frames[i] : nullptr;
  }
};

// Decode one sprite-table record (ARROW and siblings). `payload` is
// the record span only — the caller resolves the FTI directory.
// Validates: header fields present, count nonzero and offsets in
// bounds, each frame header readable, and every stream reaching 0xff
// inside the payload. Fills `err` and returns nullopt on malformed
// input (short table, bad count/offset, truncated header or stream).
std::optional<FtiSprite> decodeFtiSprite(
    std::span<const std::byte> payload, std::string* err);

// Deterministic FNV-1a64 digest over the decoded sprite: block size,
// per frame {w, h, hx, hy, stream}. Never hashes filesystem data.
std::uint64_t ftiSpriteDigest(const FtiSprite& sprite);

// FUN_00409760 + FUN_00415ff0 combined: draw frame `f` so that its
// hotspot lands at (x, y) — i.e. the stream is interpreted at
// (x - hotspotX, y - hotspotY). Mirrors the original entry checks and
// stream semantics exactly; the only deviation is bounds-hardening
// (writes outside the framebuffer are dropped — unreachable for
// well-formed content on the 600x360 work buffer).
void blitFtiSpriteFrame(const FtiSpriteFrame& f, IndexedFramebuffer& fb,
                        int x, int y);

} // namespace mdk

#endif // MDK_CORE_FTI_SPRITE_H
