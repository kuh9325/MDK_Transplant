// save_full_write.h — Phase 14D: the FUN_00426a0c full-save writer.
//
// Route (OBSERVED — FUN_00427ed4/FUN_00426e98/FUN_00426a0c):
//   manual F2 save -> FUN_00427ed4(headerOnly=0) arms the stream;
//   FUN_00426e98 writes GAME with modeField = 0x3eb (the +1000 full-
//   save flag — hard-coded, not mode+1000 arithmetic); FUN_00426a0c
//   writes MORE/PLAY/DAMP/CAME, then iterates the arena array writing
//   AREN + one ALIE per +0x68 object + one FAND per +0x45e record,
//   then the three BULL shot slots, then SEND.
//
// Token forms (the FUN_004262b0/FUN_00426738 writer-side conversions —
// the exact inverse of the loader's FUN_004262c8 resolves):
//   arena ptr   -> index * 0x466 (the record-array stride; -1 = NULL)
//   CMI ptr     -> image offset into .CMI file+4 (-1 = NULL)
//   CMI string  -> image offset of the char data (-1 = none/empty)
//   object ptr  -> the record's +0x7c sequential save id (0 = NULL);
//                  ids run 1..N across arenas, each arena's embedded
//                  +0x118 record counted first, then its +0x68 list in
//                  order (OBSERVED — one increment per store)
//   +0x114      -> CMI offset of the live anim record; a record with
//                  no CMI home emits -1 (the stale-pointer policy —
//                  OBSERVED saves carry -1 there)
//   +0x2b0/2b4  -> nonzero boolean (the loader rebinds to statics)
//   BULL +0xdc  -> nonzero boolean gating the element rebind
//
// The writer serializes native fields back into the fixed original
// record images; nothing about the native layout leaks into the file.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/save_game.h"

namespace mdk {

struct ProgressionSession;
struct TraversalRuntime;

// Diagnostics for --save-write-full and the golden tests.
struct FullWriteReport {
  int arenasWritten = 0;    // AREN packets emitted
  int objectsWritten = 0;   // ALIE packets emitted
  int fansWritten = 0;      // FAND packets emitted
  int shotSlots = 3;        // BULL packets (always 3 — fixed pool)
  int saveIds = 0;          // highest stamped +0x7c id
  std::vector<std::string> warnings;  // unrepresentable refs -> sentinels
};

// Writer inputs beside the runtime/session: the cipher seed (pin it
// for deterministic test output; 0 is a valid production seed) and
// the THMB payload (3648B — empty emits a zeroed thumbnail; the load
// path only previews it).
struct SaveWriteFullInput {
  std::uint16_t seed = 0;
  std::span<const std::byte> thumbnail;
};

// Serialize the live traversal state into the original full-save
// stream (SAVE THMB GAME MORE PLAY DAMP CAME (AREN ALIE* FAND*)*
// BULLx3 SEND), ciphered and enveloped — parseable by saveGameParse
// and restorable by applyFullSaveToTraversal. Returns an empty vector
// with `detail` set when the state is structurally unrepresentable
// (no arenas, no CMI image, invalid level id); reference-resolution
// misses degrade to the null token + a warning instead.
std::vector<std::byte> saveGameWriteFull(const TraversalRuntime& rt,
                                       const ProgressionSession& sess,
                                       const SaveWriteFullInput& in,
                                       FullWriteReport* report,
                                       std::string* detail);

} // namespace mdk
