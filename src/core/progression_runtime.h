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
// Phase 14A — the traversal→intermission→next-level campaign loop.
// Instruction-level evidence (BUILD_A, master dispatcher FUN_0040103c):
//
//   Traversal completion edge (mode 3 → mode 5):
//     level scripts write the mailbox DAT_00540ebc = -1 (script-VM
//     arm at 0x43d411 fires for script opcodes <= 0x32; the >0x32
//     range routes to FUN_0047baf4 where opcode 0x51 enters mode 8).
//     The traversal frame tail (FUN_00436100) consumes -1 →
//     FUN_0040dde0: 541554 floored to >= 1, DAT_00540d9c = 1, the
//     END_LEVEL effect spawns. FUN_00461954 / FUN_00463608 then set
//     DAT_00540da0 = 1 → the player dispatcher calls FUN_0040e958
//     (victory white-out) → after its >300 counter, 0x49a030 = 1.
//     Dispatcher mode-3 tail (0x401497): 49a030 → cleared, fades =
//     1000.0f, FUN_004371bc traversal teardown, FUN_0042b270 →
//     mode 5. FUN_004371bc writes no health/ammo/levelId/RNG —
//     everything carries.
//
//   Mode 5 (FUN_0042c8b0 frame): statistics/tally screen; its fade
//   completes or is skipped → return 1 → dispatcher 0x4015c3:
//     health <= 0 → FUN_0041d85c frontend;
//     541498 < 4  → FUN_00429200(0) → mode 6;
//     541498 >= 4 → 541498 = 5 (literal store 0x4015ef),
//                   FUN_00422bc0 overlay arm, mode 7.
//
//   Mode 6 (FUN_00429200 entry → sub-state 0x54bef8; FUN_004296f0
//   frame): EAX==0 → sub 2 (full advance), EAX!=0 → sub 3 (briefing
//   only — new game / save restore). Sub-states: 2→FUN_00429f40→4;
//   4→FUN_00429984→1; 1→FUN_00429fe4 then levelId++ (0x4297d9) —
//   if the new id == 6 mode 6 exits immediately, else FUN_00422bc0
//   arms the score overlay and sub=3; 3→FUN_00429cb4 briefing
//   (541554 floored to 100, 541618/19/1a/1b reset) → exit.
//   Exit (0x40155d): teardown, fades=1000, FUN_0041b7b4(levelId)
//   preload — which queues FALL3D_<id> assets only when id < 5 —
//   then id < 5 → FUN_0040ef28 (mode 2 freefall) else FUN_004346e8
//   (mode 3 traversal).
//
//   Mode 7 (0x40160c): one-shot — FUN_0046ca84 cleanup,
//   FUN_0041b7b4(levelId) preload, FUN_004346e8 → mode 3. This is
//   the traversal-only entry used for id 5 (LEVEL5); the mode-5
//   exit wrote id=5 literally, so mode 7 never advances the id.
//
//   Mode 8 (FUN_0047b038 ← script opcode 0x51 or the ending cheat):
//   finish.c ending. First frame tears down traversal
//   (FUN_004371bc), plays the cinematic (FUN_0047b3f4), then
//   FUN_0041d85c → mode 0 frontend. This is the campaign's terminal
//   route — LEVEL5 (id 5) ends through it, so the mode-5 exit's
//   id>=4 write only ever fires for id==4 in normal play.
//
//   levelId (541498) writers, complete census: =0 new game
//   (FUN_0041b724), ++ in FUN_004296f0 mode-6 sub-state 1, =5 literal
//   at the mode-5 exit, =arg in FUN_0041b7b4 (idempotent), =packet in
//   FUN_004278c0 save restore (validated < 6). BUILD_A ships only
//   TRAVERSE/LEVEL3..8 — ids 6,7 (LEVEL2/LEVEL1) have no data and are
//   unreachable in normal campaign play.
//
//   Death boundary: traversal death (541554==0) does not write
//   49a030 — FUN_00463608 sets 5414d0 instead → the dispatcher's
//   demo.c loop (FUN_004090fc) restores the last save and re-enters
//   the saved mode (3 → reload traversal, 6 → FUN_00429200). Death
//   never advances levelId.
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

  // Phase 14A — campaign-loop latches. The original runs a timed
  // victory sequence (540d9c end-level effect → 540da0 victory mode →
  // 49a030 dispatcher-visible exit); the presentation duration is a
  // seam, so the port models it as a three-stage latch. victoryPhase
  // is consumed by progressionTraversalTeardown and cleared for the
  // next level.
  int victoryPhase = 0;    // 0 none · 1 = 540d9c · 2 = 540da0 · 3 = 49a030
  int loaderSub = 0;       // 0x54bef8 — mode-6 sub-state (0 when not in 6)
  bool terminalDone = false; // mode-8 finish consumed → frontend reached
  int transitionCount = 0;   // diagnostic: level advances applied
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
  kBadMode,            // call requires a different current mode
  kNoTraversalExit,    // teardown requested without the 49a030 latch
  kAlreadyEnded,       // duplicate end-level request on this level
  kStageRunning,       // the current mode-5/6 stage has not finished
  kTerminalConsumed,   // the mode-8 finish already ran
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

// ---------------------------------------------------------------------------
// Phase 14A — traversal → intermission → next level / terminal loop
// ---------------------------------------------------------------------------

// FUN_0041b630 — the frontend "new game" action: FUN_0041b724 reset
// plus FUN_00429200(EAX=1). Mode 6 is entered at the briefing stage
// (loaderSub 3) — no levelId advance, since the id is already 0.
void progressionStartCampaign(ProgressionSession& sess, int skill);

// The script-VM end-level request (540ebc = -1) → FUN_0040dde0:
// health is floored to >= 1 (541554 = max(1, 541554)) and the
// end-level sequence latches (540d9c = 1). Requires mode 3 and no
// pending end request — a second request returns kAlreadyEnded.
ProgressionError progressionRequestTraversalEnd(ProgressionSession& sess);

// One deterministic step of the victory sequence. victoryPhase 1 → 2
// (the END_LEVEL effect completing — 540da0 = 1) then 2 → 3
// (FUN_0040e958's >300 white-out finished — 49a030 = 1). Anything
// else returns kBadMode.
ProgressionError progressionAdvanceVictory(ProgressionSession& sess);

// The dispatcher mode-3 tail (0x4014f4): consumes 49a030
// (victoryPhase must be 3), runs FUN_004371bc teardown — none of
// health/ammo/levelId/RNG are touched — then FUN_0042b270 → mode 5.
ProgressionError progressionTraversalTeardown(ProgressionSession& sess);

// Convenience: request + advance + teardown in one call — the
// traversal → mode-5 semantic edge for tests/diagnostics that do not
// need the individual latches.
ProgressionError progressionTraversalComplete(ProgressionSession& sess);

// FUN_0042c8b0's completion edge. `tallyDone` is the presentation
// seam input — the original waits for the statistics fade
// (0x4eda9c) / a skip; pass true when the intermission is complete.
// Returns kStageRunning while the intermission is still up (mode
// stays 5). On completion the dispatcher exit (0x4015c3) runs:
// health <= 0 → mode 0 frontend; levelId < 4 → mode 6 with the full
// advance sequence (loaderSub = 2); levelId >= 4 → levelId = 5
// (the literal store) + mode 7. Returns kOk after the transition.
ProgressionError progressionStepIntermission(ProgressionSession& sess,
                                             bool tallyDone);

// FUN_004296f0's sub-state machine, one stage per call. `stageDone`
// is the presentation seam — each stage (intro card, debrief pages,
// load bar, briefing) holds until its animation/input completes.
// Sub-state flow: 2 → 4 → 1 → (levelId++; ==6 exits early; else
// overlay arm + sub 3) → 3 → exit. Entering sub 3 applies the
// FUN_00429cb4 briefing effects: health floored to 100.
// On exit the dispatcher (0x40155d) runs teardown + preload then
// selects mode 2 (levelId < 5) or mode 3 (levelId >= 5). Returns
// kStageRunning while mode 6 continues, kOk after the exit.
ProgressionError progressionStepLoader(ProgressionSession& sess,
                                       bool stageDone);

// The mode-7 dispatcher body (0x40160c): cleanup + preload +
// FUN_004346e8 — a one-shot traversal-only entry. Never advances
// levelId (the mode-5 exit already wrote it). Requires mode 7;
// returns kOk with mode == 3.
ProgressionError progressionStepMode7(ProgressionSession& sess);

// FUN_0047b038 — the script-opcode-0x51 / ending-cheat edge: mode 8.
// Requires mode 3 (the script VM runs inside the traversal frame).
ProgressionError progressionEnterCinematic(ProgressionSession& sess);

// FUN_0047b06c's completion edge — `cinematicDone` is the
// presentation seam (FUN_0047b3f4 still playing). While running,
// mode stays 8; on completion: traversal teardown already ran on
// entry, then FUN_0047b674 + FUN_0041d85c → mode 0 frontend and
// terminalDone = true. Returns kStageRunning / kOk.
ProgressionError progressionStepCinematic(ProgressionSession& sess,
                                        bool cinematicDone);

// Loads TRAVERSE/LEVEL<table[levelId]> into `trav` — the
// FUN_0041b7b4 + FUN_004346e8 + FUN_00433d40 data path for the
// mode-6-exit (id >= 5) and mode-7 traversal entries. Session
// health/rng/ammo carry verbatim into the runtime. Requires mode 3.
ProgressionError progressionLoadTraversalForCurrentLevel(
    const DataRoot& root, ProgressionSession& sess,
    TraversalRuntime& trav, std::string* detail);

// Static campaign table — the full internal sequence the BUILD_A
// data supports. For each entry: internal id, the 0x4999e8 directory
// number, whether a freefall course precedes the traversal (id < 5),
// and how the id is reached. Ids 6/7 map to LEVEL2/LEVEL1 which are
// absent from BUILD_A — reachable in the model only through the
// mode-6 ==6 arm (a crafted-save edge) and reported as such.
struct CampaignLevelInfo {
  int levelId;
  int dir;             // 0x4999e8 table value
  bool freefallFirst;  // mode-6 exit selects mode 2 (id < 5)
  bool viaMode7;       // mode-5 exit literal-write path (id >= 4)
  bool terminal;       // the campaign's normal end (id 5 → mode 8)
};
const CampaignLevelInfo* progressionCampaignTable(int& count);

} // namespace mdk

#endif // MDK_CORE_PROGRESSION_RUNTIME_H
