// save_full_restore.h — Phase 14C: the FUN_00427218 full-save load.
//
// Route (OBSERVED): FUN_004278c0 handles the GAME packet; when its
// mode field is >= 1000 (the writer stamps 0x3eb = 1003 = mode+1000)
// it calls FUN_00427218, which performs the complete world restore
// inline — FUN_00427f94 then returns early (0x54be2c == 0).
//
// FUN_00427218 packet order (OBSERVED):
//   MORE (52B)  -> staged globals (applied after the level load)
//   PLAY (239B) -> the 0x541554 player block verbatim
//   FUN_0041b7b4 + FUN_004346e8(2) — fresh level load with the
//     arena-activation/spawn block suppressed (param bit1)
//   MORE[0] == CMI blob length — the save/level identity check
//   DAMP (724B) -> 0x540bfc..0x540ecf player/arena-state block
//   CAME (200B or 212B) -> 0x540b28 camera block
//   per arena: AREN (1126B) + ALIE (814B) x objectCount
//              + FAND (72B) x fanCount — stream order
//   BULL (252B) x 3 -> the 0x540ed4 shot pool
//   relocation tail: FUN_00426f34 per embedded record + object,
//   fan owner/name fixups, BULL rebinds, pending-list clears,
//   FUN_00432c34(loadArena) -> FUN_00432d9c(cur) ->
//   FUN_00432d9c(partner) -> FUN_004348d4 (audio seam)
//
// Saved references (OBSERVED writer FUN_00426a0c/00426738):
//   arena ptr   -> byte offset into the 0x466-stride arena array
//                  (-1 = NULL; FUN_004262b0/004262c8 inverse pair)
//   CMI ptr     -> byte offset into the loaded .CMI image (-1 = NULL)
//   object ptr  -> the record's sequential +0x7c save id (1-based,
//                  each arena's embedded +0x118 record counted first);
//                  FUN_004282dc resolves by scanning arenas + lists
//   +0x0c/+0x158/+0x160..0x17f — cleared on BOTH sides (the deep-copied
//                  model record is not persisted). The loader's attach
//                  tail reaches FUN_004321dc -> FUN_0045a3b0, the lazy
//                  +0x0c rebind for every named object in cur+partner
//                  (port: objectArenaActivate inside
//                  traversalMigrateInto) — required before the first
//                  frame: FUN_004138d8 derefs +0x0c unguarded and real
//                  saves DO carry flags149&1 standable objects.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/save_game.h"

namespace mdk {

class DataRoot;
struct ProgressionSession;
struct TraversalRuntime;

// Diagnostics for --save-restore and the golden tests.
struct FullRestoreReport {
  int modeField = 0;
  int mode = 0;
  int levelId = -1;
  bool identityOk = false;   // MORE+0x00 == loaded .CMI byte length

  int arenPackets = 0;       // AREN records seen in stream order
  int arenApplied = 0;       // ...matched to runtime arenas by index
  int arenNameMismatch = 0;  // AREN+0x00 name != runtime arena name
  int objectsAllocated = 0;  // ALIE records -> DynamicArena lists
  int scriptedObjects = 0;   // +0x108 != -1 after restore
  int fansAllocated = 0;     // FAND records -> SurfaceRecord lists
  int shotSlots = 0;         // BULL records applied
  int shotsActive = 0;       // ...with +0x00 state != 0

  int curArenaIndex = -1;    // DAMP+0x4c resolved
  int partnerArenaIndex = -1;// DAMP+0xa8 resolved
  int loadArenaIndex = -1;   // DAMP+0x130 resolved (FUN_00432c34 arg)
  float playerPos[3] = {0, 0, 0};
  float playerYawDeg = 0.0f;
  int health = 0;

  int arenaRefsResolved = 0;
  int arenaRefsFailed = 0;
  int objectRefsResolved = 0;
  int objectRefsFailed = 0;
  int cmiRefsResolved = 0;
  int cmiRefsFailed = 0;

  // Bounded-coverage accounting: packet byte spans with no proven
  // native consumer (the original lands them on globals the port does
  // not model). Logged, not guessed.
  struct UnmappedSpan {
    const char* packet;
    std::uint32_t offset;
    std::uint32_t size;
  };
  std::vector<UnmappedSpan> unmapped;
  std::vector<std::string> warnings;
};

// FUN_00427218 — restore a parsed full save into a TraversalRuntime.
// Requires save.game.full() (modeField >= 1000); header-only saves
// take progressionApplyGamePacket + a fresh load instead.
// `sess` receives the GAME/PLAY session globals (mode, level, health,
// deathCount, field54163b). `root` supplies the level's DTI/CMI/MTO
// (resolved from the saved levelId through the campaign table).
SaveError applyFullSaveToTraversal(const SaveGame& save,
                                   const DataRoot& root,
                                   ProgressionSession& sess,
                                   TraversalRuntime& rt,
                                   FullRestoreReport* report,
                                   std::string* detail);

// FUN_004278c0's own route, for callers holding a parsed save:
//   modeField >= 1000 -> full restore (this file)
//   else              -> header-only fields only (progression side)
inline bool saveNeedsFullRestore(const SaveGame& save) {
  return save.game.full();
}

} // namespace mdk
