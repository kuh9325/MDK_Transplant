// Save-file THMB record capture (FUN_00427e8c + FUN_0046d614,
// OBSERVED).
//
// The save-name dialog (FUN_00422bc0) captures a thumbnail at arm
// time — before the dialog itself draws — by nearest-sampling the
// live 600x360 indexed framebuffer:
//
//   out[768 + r*64 + c] = fb[(r*8)*600 + 44 + c*8]
//     for r in 0..44, c in 0..63
//
// (source offset 0x2c = 44, sample pitch 8 in both axes), then
// snapshots the current 256-entry DAC palette into out[0..767]
// (FUN_0046d614 copies the staged palette verbatim, pre-brightness).
// The 3648-byte record is written into the save's THMB chunk on
// confirm (FUN_00422d84) and is what the save list blits at
// (418,103) for full saves.
//
#ifndef MDK_CORE_THMB_CAPTURE_H
#define MDK_CORE_THMB_CAPTURE_H

#include "core/compat.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace mdk {

class IndexedFramebuffer;

inline constexpr int kThmbWidth = 64;
inline constexpr int kThmbHeight = 45;
inline constexpr std::size_t kThmbRecordBytes =
    compat::kPaletteEntries * 3 + kThmbWidth * kThmbHeight;  // 3648

// FUN_00427e8c geometry (OBSERVED).
inline constexpr int kThmbSrcOffsetX = 44;  // 0x2c
inline constexpr int kThmbSamplePitch = 8;

// Samples `fb` (must be the frontend 600x360 indexed buffer) into
// out[768..3647] and copies `palette768` (768 raw r,g,b triplets —
// the staged DAC palette, same byte order the THMB/LBB records use)
// into out[0..767]. `out` must point to kThmbRecordBytes bytes.
void captureThumbnail(const IndexedFramebuffer& fb,
                      std::span<const std::uint8_t> palette768,
                      std::uint8_t* out);

}  // namespace mdk

#endif  // MDK_CORE_THMB_CAPTURE_H
