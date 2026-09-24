// progression_runtime.h — Phase 13B: the freefall→traversal handoff.
//
// EVIDENCE (instruction-level, BUILD_A — docs/reverse-engineering/
// EXECUTABLE_MAP.md §"mode dispatch"):
//
//   The master loop FUN_0040103c dispatches on the primary-mode byte
//   DAT_00541492 (sub-mode byte DAT_00541493 = frontend/pause overlays,
//   0 while a gameplay mode runs). Mode 2 is freefall:
//
//     0x4014aa  CALL FUN_004103d8        ; freefall frame
//     0x4014af  TEST EAX,EAX
//     0x4014b1  JZ   next-iteration      ; still running
//     0x4014b7  CALL FUN_0040fa68        ; freefall teardown
//     0x4014bc  CMP  [0x541554],0        ; health — the sole predicate
//     0x4014c3  JLE  0x4014e8            ; <=0 → frontend
//     ; success: 5414a0/a4/a8 = 1000.0f (transition-fade timers),
//     ; EAX=0 → FUN_004346e8 (arg bit1 clear → spawn scan runs)
//     0x4014de  CALL FUN_004346e8        ; writes 541492=3, runs the
//                                        ; FUN_00433c4c gate/inventory
//                                        ; resets + FUN_00433d40 loader
//     ; failure:
//     0x4014ea  CALL FUN_0041d85c        ; writes 541492=0, rebuilds the
//                                        ; frontend/menu state
//
//   DAT_00541498 (level id) is NOT modified on this path — it was set
//   upstream (FUN_0041b724 new-game reset → 0, or the mode-6 loader's
//   ++). FUN_00433d40 maps it through the dword table at 0x4999e8 to
//   build "TRAVERSE\LEVEL<n>\LEVEL<n>.DTI" (et al.). FUN_0040ef28 uses
//   the same id +1 for FALL3D_%d/FALLPU_%d, so freefall course N and
//   traversal LEVEL table[N] share the id.
//
//   Carried across the handoff (no writer in FUN_0040fa68 /
//   FUN_004346e8 / FUN_00433d40): health (541554), skill (54147a), the
//   ammo block (54161f..33), and the shared CRT rand stream
//   (FUN_0047d2b5). Reset at traversal entry (FUN_00433c4c): the
//   weapon/fire indicator block 541618=0, 541619=0, 54161a=3,
//   54161b=0. Player pose/camera are not carried — traversal spawns
//   from the level's s0 record.
//
#ifndef MDK_CORE_PROGRESSION_RUNTIME_H
#define MDK_CORE_PROGRESSION_RUNTIME_H

#include "core/freefall_runtime.h"
#include "core/traversal_runtime.h"

#include <array>
#include <cstdint>
#include <string>

namespace mdk {

class DataRoot;

// OBSERVED 0x4999e8 — internal level id → TRAVERSE\LEVEL<n> directory
// number. Ids 0..4 are the freefall courses (LEVEL7, 6, 3, 4, 8); ids
// 5..7 are traversal-only levels (LEVEL5, 2, 1) reached via the
// mode-7 path. Returns -1 when the id is outside the 8-entry table.
int progressionLevelDir(int levelId);

// The outer-orchestrator globals that outlive a mode transition.
// FreefallRuntime/TraversalRuntime hold the same values while their
// mode is active; the session is the canonical store between modes —
// the same role DAT_00541492/98/541554/54147a play in the original.
struct ProgressionSession {
  int mode = 0;              // 0x541492 — frontend at boot
  int levelId = 0;           // 0x541498 — internal level id
  int health = 100;          // 0x541554
  int skill = 0;             // 0x54147a
  std::uint32_t rng = 1;     // shared CRT rand stream (FUN_0047d2b5)
  std::array<int, 6> ammo{}; // 0x54161f..0x541633 — survives modes
  bool freefallHandedOff = false;  // the one-shot handoff latch
};

enum class ProgressionRoute : int {
  kTraversal = 0,  // health > 0  → FUN_004346e8, mode 3
  kFrontend,       // health <= 0 → FUN_0041d85c, mode 0
};

enum class ProgressionError : int {
  kOk = 0,
  kNotFinished,        // freefall frame never returned nonzero
  kAlreadyHandedOff,   // a second handoff on the same session
  kBadLevelId,         // levelId outside the 0x4999e8 table
  kTraversalLoad,      // traversalRuntimeLoad failed
};

const char* progressionErrorName(ProgressionError e);

// Structured handoff record — what the diagnostic prints and tests
// assert. All fields are gameplay-authoritative or OBSERVED metadata.
struct ProgressionHandoff {
  ProgressionRoute route = ProgressionRoute::kFrontend;
  int levelId = -1;          // 0x541498 at the transition
  int traversalDir = -1;     // LEVEL<n> directory number (table value)
  int healthBefore = 0;      // 541554 sampled after teardown
  int healthAfter = 0;       // value handed to traversal (identical)
  int skill = 0;             // 54147a carried
  std::uint32_t rng = 0;     // CRT rand state carried into traversal
  int ammoGranted = 0;       // kFfEvGrantAmmo events applied to ammo[]
  std::string dtiPath;       // TRAVERSE/LEVEL<n>/LEVEL<n>.DTI
  std::string cmiPath;
  std::string mtoPath;
  int spawnArena = -1;       // s0 record: arena index + pos + yaw
  float spawnPos[3] = {};
  float spawnYawDeg = 0;
  TraversalLoadError loadError = TraversalLoadError::kOk;
};

// FUN_0040fa68 teardown + the 0x4014bc success/failure branch, then
// the FUN_004346e8 traversal entry or the FUN_0041d85c frontend route.
//
//   - `ff` must have completed (finished || died) — else kNotFinished.
//   - Teardown drains the freefall object lists (0x4edaec / 0x4edaf0).
//   - `sess.health`/`sess.rng`/`sess.ammo` sync FROM the freefall
//     runtime (grant events are applied to the shared ammo block —
//     the FUN_0046a790 write target, rows 0..4 → ammo[1..5]).
//   - Success: mode=3, traversal loads TRAVERSE/LEVEL<table[levelId]>
//     via traversalRuntimeLoad; health/rng/ammo carry verbatim.
//   - Failure: mode=0, traversal is not touched.
//   - `trav` may be null only when the caller knows the route will be
//     kFrontend; on the traversal route it is required.
//   - One-shot: a second call returns kAlreadyHandedOff with no side
//     effects. progressionResetHandoffLatch (called on the next
//     freefall entry) clears it.
ProgressionError progressionFreefallHandoff(
    const DataRoot& root, ProgressionSession& sess,
    FreefallRuntime& ff, TraversalRuntime* trav,
    ProgressionHandoff& out, std::string* detail);

// FUN_0041b724's new-game reset subset: levelId=0, health=100, the
// latch cleared (the original resets 541498/541554 before the mode-6
// briefing chain; ammo/inventory reset at new game lives in the same
// reset block upstream — modeled here for a coherent new campaign).
void progressionNewGame(ProgressionSession& sess, int skill);

// Marks mode 2 entered (FUN_0040ef28's 541492=2 write) and clears the
// handoff latch so the next completed freefall can transition.
void progressionEnterFreefall(ProgressionSession& sess);

} // namespace mdk

#endif // MDK_CORE_PROGRESSION_RUNTIME_H
