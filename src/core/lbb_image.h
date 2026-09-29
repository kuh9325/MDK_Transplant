// LOAD_<n>.LBB loading-screen bitmaps (FUN_004258a0, OBSERVED).
//
// On-disk layout (verified against all six corpus files, each
// 40772 bytes = 768 + 4 + 200*200):
//   [768-byte palette: 256 x {r,g,b} 8-bit triplets]
//   [u16le width][u16le height]
//   [width*height indexed pixels, row-major top-down]
//
// The save-load path FUN_004090fc loads one LBB for the destination
// level; the save-list resource load FUN_004202cc preloads all six
// (level-id table at rodata 0x4999e8 = {7,6,3,4,8,5}) into pixel
// slots indexed by the save's level field and palette slots at
// levelId+1 (slot 0 holds a DAC snapshot for restore).
//
// The save-list detail pane (FUN_004206d0) draws the LBB centered at
// (450 - w/2, 103) when the inspected slot's GAME mode field < 1000
// (a quick/header-only save has no THMB; the level image is the
// fallback preview).
//
#ifndef MDK_CORE_LBB_IMAGE_H
#define MDK_CORE_LBB_IMAGE_H

#include "core/compat.h"
#include "core/indexed_image.h"

#include <optional>
#include <span>

namespace mdk {

class DataRoot;

// FUN_004202cc's level-id table (rodata 0x4999e8, OBSERVED): the
// save's levelId indexes this table to pick LOAD_<id>.LBB.
inline constexpr int kSaveListLbbLevelIds[6] = {7, 6, 3, 4, 8, 5};
inline constexpr int kSaveListLbbCount = 6;

// Strict parser — rejects wrong sizes/dimensions rather than
// tolerating (evidence-first: the corpus format is exact).
std::optional<IndexedImage> decodeLbbImage(
    std::span<const std::uint8_t> bytes);

// Loads MISC/LOAD_<kSaveListLbbLevelIds[levelId]>.LBB for a save's
// levelId (0..5); nullopt when out of range or the file is absent
// or malformed.
std::optional<IndexedImage> loadSaveListLbb(const DataRoot& root,
                                            int levelId);

}  // namespace mdk

#endif  // MDK_CORE_LBB_IMAGE_H
