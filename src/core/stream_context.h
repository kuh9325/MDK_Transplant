// STREAM context resources (Phase 4B) — the proven external-palette
// binding behind the STREAM.BNI backdrop.
//
// EVIDENCE — instruction-level disasm of the original stream-mode
// init/frame functions (BUILD_A MDK95.EXE) plus real-byte agreement:
//
//   FUN_0042b270 (stream init):
//     * loads "STREAM\STREAM.BNI" into the per-context BNI slot
//       DAT_004a1e38 (FUN_00403928 -> whole-blob loader);
//     * resolves record "PAL" through the BNI name lookup
//       (FUN_004039c8 -> FUN_004039a4 -> FUN_00403958) and copies
//       0x240 bytes from PAL+0xc0 to DAT_004ed818;
//     * copies 0xc0 bytes from DAT_00540820 — the engine's
//       current-palette head — to DAT_004ed758. The two destinations
//       are adjacent, so the composed 768-byte table is:
//         entries 0-63    <- DAT_00540820[0:192]
//         entries 64-255  <- PAL payload bytes [0xc0, 0x300)
//     * DAT_00540820's head is seeded at startup by FUN_0040163c from
//       the "SYS_PAL" record (resolved through the FTI directory
//       lookup FUN_00414890; the record's entry 0 is forced black) —
//       it is the only writer of that 192-byte head;
//     * resolves "BG" via FUN_00403a00: payload+0 u16le w, +2 u16le h,
//       +4 pixel base, w*h count (stored to DAT_004eda88..98). BG and
//       PAL come out of the SAME BNI image.
//
//   FUN_0042c8b0 (stream frame):
//     * fade factor DAT_004eda9c; at 1.0 the stable path calls
//       FUN_00413b40(DAT_004ed758) -> FUN_0046d208 uploads entries
//       0-63 from DAT_00540820 and 64-255 from the argument's +0xc0 —
//       the same RGB-triplet -> PALETTEENTRY -> SetEntries path the
//       paletted decoder documents (file order R,G,B);
//     * fade != 1.0 scales/clamps a stack copy of the composed table
//       (fade-to/from-black and brighten transitions) — animation
//       state, not required for the stable displayed image;
//     * a global brightness offset (DAT_0054147e * 16, default 0) is
//       a user-setting path inside FUN_0046d208.
//
//   FUN_0042e684 (backdrop draw): wrapping blit of BG into the
//   600x360 work surface — source and destination stride 600 ==
//   width, rows top-down, NO transparency (every byte overwritten);
//   the scroll offsets are stream camera state and are 0 for a
//   verbatim copy.
//
//   Real-byte agreement (BUILD_A): PAL record span = 768; its first
//   192 bytes equal SYS_PAL except entry 0 (magenta vs black — never
//   read by the consumer, and BG's minimum pixel index is 1); BG =
//   4 + 600*360 bytes exactly, pixel indices 1-255.
//
// SCOPE: this is the ONE proven indexed-only + external-palette
// binding. It does not generalize to other BNI files — other
// contexts (FALL3D etc.) have their own unproven palette handling.
//
#ifndef MDK_CORE_STREAM_CONTEXT_H
#define MDK_CORE_STREAM_CONTEXT_H

#include "core/bni_image.h"
#include "core/indexed_image.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mdk {

// Proven names in the stream context (the path is DOS-style as the
// original passes it; '/' and '\' are equivalent under DataRoot).
inline constexpr std::string_view kStreamBniFile = "STREAM/STREAM.BNI";
inline constexpr std::string_view kStreamImageRecord = "BG";
inline constexpr std::string_view kStreamPaletteRecord = "PAL";
inline constexpr std::string_view kStreamSystemFile = "MISC/MDKFONT.FTI";
inline constexpr std::string_view kStreamSystemRecord = "SYS_PAL";

// Byte layout of the composed palette table (FUN_0042b270).
inline constexpr std::size_t kStreamSystemHeadBytes = 0xc0;  // 64 RGB
inline constexpr std::size_t kStreamPaletteTailOffset = 0xc0;
inline constexpr std::size_t kStreamPaletteTailBytes = 0x240;  // 192 RGB

// Does (relFile, record) name the proven stream backdrop? ASCII
// case-insensitive; '/' and '\' are equivalent.
bool isStreamBackdropRequest(std::string_view relFile,
                             std::string_view record);

// Compose the effective 768-byte RGB palette exactly as the original
// stream init does: entries 0-63 from the SYS_PAL record head,
// entries 64-255 from the PAL record's bytes [0xc0, 0x300).
// `sysPalRecord` must be exactly the 192-byte SYS_PAL record and
// `palRecord` exactly the 768-byte PAL record — no silent slack.
std::optional<std::array<std::byte, kBniImagePaletteBytes>>
composeStreamPalette(std::span<const std::byte> sysPalRecord,
                     std::span<const std::byte> palRecord,
                     std::string* error = nullptr);

// Full stream-context resolution: parse the STREAM.BNI file bytes,
// resolve the BG and PAL records by their proven names, parse the
// system FTI file bytes for SYS_PAL, compose the palette, and decode
// the indexed-only image. Both arguments are complete file bytes.
// Fails (nullopt + error) when a required record is missing, the
// image record is not the proven indexed-only shape, or a palette
// record is malformed.
std::optional<IndexedImage> decodeStreamBackdrop(
    std::span<const std::byte> streamBniFile,
    std::span<const std::byte> systemFtiFile,
    std::string* error = nullptr);

} // namespace mdk

#endif // MDK_CORE_STREAM_CONTEXT_H
