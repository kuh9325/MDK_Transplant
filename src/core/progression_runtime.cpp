// progression_runtime.cpp — Phase 13B freefall→traversal handoff.
// See the header for the evidence summary; addresses cited inline are
// BUILD_A (MDK95.EXE) instruction addresses.

#include "core/progression_runtime.h"

#include "core/data_root.h"

#include <cstdio>

namespace mdk {

namespace {

// OBSERVED — the dword table at 0x4999e8, read by FUN_00433d40
// (0x433d77/0x433d7d) and FUN_0041b7b4 (0x41b7d1) as
// table[DAT_00541498] when building the LEVEL%n directory names.
// Entries 8..11 in the image (0,0,-1,0) are outside the level range.
constexpr int kLevelDirTable[8] = {7, 6, 3, 4, 8, 5, 2, 1};

// OBSERVED — FUN_0046a790 rows 0..4 (SW_HOME/SW_SGREN/SW_HGREN/
// SW_LGREN/SW_BONES) write ammo[1+i] += amount; the source amounts
// live in the 0x49bb04 table and halve on skill 2 when > 1.
constexpr int kGrantAmmoRows = 5;  // grant-table rows that feed ammo

} // namespace

int progressionLevelDir(int levelId) {
  if (levelId < 0 || levelId > 7) return -1;
  return kLevelDirTable[levelId];
}

const char* progressionErrorName(ProgressionError e) {
  switch (e) {
    case ProgressionError::kOk: return "ok";
    case ProgressionError::kNotFinished: return "not-finished";
    case ProgressionError::kAlreadyHandedOff: return "already-handed-off";
    case ProgressionError::kBadLevelId: return "bad-level-id";
    case ProgressionError::kTraversalLoad: return "traversal-load";
    case ProgressionError::kBadMode: return "bad-mode";
    case ProgressionError::kNoTraversalExit: return "no-traversal-exit";
    case ProgressionError::kAlreadyEnded: return "already-ended";
    case ProgressionError::kStageRunning: return "stage-running";
    case ProgressionError::kTerminalConsumed: return "terminal-consumed";
  }
  return "?";
}

void progressionNewGame(ProgressionSession& sess, int skill) {
  // FUN_0041b724 (new-game reset): 541498 = 0, 541554 = 100. The
  // ammo block is campaign state — cleared here because the original
  // reset path runs before any grant can exist.
  sess.levelId = 0;
  sess.health = 100;
  sess.skill = skill;
  sess.ammo.fill(0);
  sess.freefallHandedOff = false;
  sess.victoryPhase = 0;
  sess.loaderSub = 0;
  sess.terminalDone = false;
  sess.transitionCount = 0;
  // mode is left for the entry path (frontend/menu at boot).
}

void progressionEnterFreefall(ProgressionSession& sess) {
  sess.mode = 2;  // FUN_0040ef28 writes 541492 = 2
  sess.freefallHandedOff = false;
}

ProgressionError progressionFreefallHandoff(
    const DataRoot& root, ProgressionSession& sess,
    FreefallRuntime& ff, TraversalRuntime* trav,
    ProgressionHandoff& out, std::string* detail) {
  auto fail = [&](ProgressionError e, const char* msg) {
    if (detail) *detail = msg;
    return e;
  };

  // One-shot guard — a completed course transitions exactly once.
  if (sess.freefallHandedOff)
    return fail(ProgressionError::kAlreadyHandedOff,
                "handoff already consumed");
  // FUN_004103d8's nonzero return is the gate; the runtime records it
  // as finished (timeline > 33s) or died (death fade end).
  if (!ff.finished && !ff.died)
    return fail(ProgressionError::kNotFinished,
                "freefall frame has not completed");

  // FUN_0040fa68 — the freefall teardown: both object lists
  // (0x4edaec live list, 0x4edaf0 bones entry) are drained; the
  // 80-slot model table and SNI/presentation release are a
  // presentation seam with no gameplay state.
  ff.listHead = -1;
  ff.bonesIdx = -1;

  // Sync the shared globals from the completed runtime — 541554 is
  // sampled AFTER teardown in the original (0x4014bc).
  sess.health = ff.health;
  sess.rng = ff.rng;
  sess.skill = ff.skill;
  // The level id is the orchestrator global; ff.course is the
  // runtime's copy of the same value from init.
  const int levelId = sess.levelId;

  // The ammo block (0x54161f..33) outlives the mode: freefall pickup
  // grants wrote it live in the original via FUN_0046a790; the port
  // emits grant events instead, so the coordinator applies the
  // equivalent writes at the boundary — identical observable state.
  int granted = 0;
  for (const auto& e : ff.events) {
    if (e.kind != kFfEvGrantAmmo) continue;
    if (e.a >= 0 && e.a < kGrantAmmoRows) {
      sess.ammo[1 + e.a] += e.b;
      ++granted;
    }
    // rows 10/11 (SW_EWJ/BONEFLC) run FUN_0046aa30 / no-op in the
    // original — the non-ammo inventory write stays a named seam.
  }

  out = ProgressionHandoff{};
  out.levelId = levelId;
  out.healthBefore = sess.health;
  out.skill = sess.skill;
  out.rng = sess.rng;
  out.ammoGranted = granted;
  sess.freefallHandedOff = true;

  if (sess.health <= 0) {
    // Failure route — FUN_0041d85c: mode 0, frontend rebuild. Health
    // is left at <= 0; the next new-game reset (FUN_0041b724) writes
    // 100. No traversal load happens on this path.
    sess.mode = 0;
    out.route = ProgressionRoute::kFrontend;
    out.healthAfter = sess.health;
    return ProgressionError::kOk;
  }

  // Success route — FUN_004346e8(EAX=0): mode 3, then the
  // FUN_00433c4c gate/inventory resets and the FUN_00433d40 loader.
  sess.mode = 3;
  out.route = ProgressionRoute::kTraversal;
  out.traversalDir = progressionLevelDir(levelId);
  if (out.traversalDir < 0)
    return fail(ProgressionError::kBadLevelId,
                "level id outside the 0x4999e8 table");
  if (!trav)
    return fail(ProgressionError::kTraversalLoad,
                "traversal route requires a runtime");

  // "TRAVERSE\LEVEL%d\LEVEL%d.*" — the 0x4975e4-family formats with
  // %s = 0x541524 ("TRAVERSE") and %d = table[541498] twice.
  char base[64];
  std::snprintf(base, sizeof base, "TRAVERSE/LEVEL%d/LEVEL%d",
                out.traversalDir, out.traversalDir);
  out.dtiPath = std::string(base) + ".DTI";
  out.cmiPath = std::string(base) + ".CMI";
  out.mtoPath = std::string(base) + "O.MTO";

  const auto le = traversalRuntimeLoad(root, out.dtiPath, out.cmiPath,
                                       out.mtoPath, *trav, detail);
  out.loadError = le;
  if (le != TraversalLoadError::kOk)
    return fail(ProgressionError::kTraversalLoad,
                detail ? detail->c_str() : "traversal load failed");

  // Carried state — globals the original path leaves untouched:
  trav->fieldHealth = sess.health;   // 0x541554 carries verbatim
  trav->rngState = sess.rng;         // shared CRT rand stream
  trav->ammo = sess.ammo;            // 0x54161f..33 carries verbatim
  // (The 541618/19/1a/1b indicator reset lives in the loader — it is
  // part of FUN_00433c4c, which traversalRuntimeLoad folds in.)

  out.healthAfter = sess.health;
  out.spawnArena = trav->cur ? trav->cur->index : -1;
  for (int i = 0; i < 3; ++i) out.spawnPos[i] = trav->cs.pos[i];
  out.spawnYawDeg = trav->motion.yawDeg;
  return ProgressionError::kOk;
}

// ---------------------------------------------------------------------------
// Phase 14A — traversal → intermission → next level / terminal loop
// ---------------------------------------------------------------------------

void progressionStartCampaign(ProgressionSession& sess, int skill) {
  progressionNewGame(sess, skill);
  // FUN_0041b630: reset → FUN_0041b7b4 preload → FUN_00429200(EAX=1).
  // EAX != 0 selects sub-state 3 — the briefing stage alone; the
  // levelId is already 0 so no advance runs.
  sess.mode = 6;       // FUN_00429200's 541492 = 6 write
  sess.loaderSub = 3;  // 0x54bef8 = 3 (EAX != 0 branch, 0x4295ae)
  // The first state-3 frame runs FUN_00429cb4's init arm — health
  // floor to 100. No-op here (new game already set 100), but the
  // rule applies to any direct sub-3 entry (save restore).
  if (sess.health < 100) sess.health = 100;
}

ProgressionError progressionRequestTraversalEnd(ProgressionSession& sess) {
  if (sess.mode != 3) return ProgressionError::kBadMode;
  // 540ebc == -1 consumed by the FUN_00436100 tail → FUN_0040dde0.
  if (sess.victoryPhase != 0) return ProgressionError::kAlreadyEnded;
  // FUN_0040dde0: 541554 = max(1, 541554) — victory floors health so
  // the mode-5 exit's <= 0 check cannot fire on a completed level.
  if (sess.health < 1) sess.health = 1;
  sess.victoryPhase = 1;  // 540d9c = 1 — END_LEVEL sequence running
  return ProgressionError::kOk;
}

ProgressionError progressionAdvanceVictory(ProgressionSession& sess) {
  if (sess.mode != 3) return ProgressionError::kBadMode;
  if (sess.victoryPhase == 0 || sess.victoryPhase == 3)
    return ProgressionError::kBadMode;
  // 1 → 2: the END_LEVEL object's countdown completes
  //     (FUN_00461954 / FUN_00463608 write 540da0 = 1).
  // 2 → 3: FUN_0040e958's white-out counter passes 300 → 49a030 = 1.
  ++sess.victoryPhase;
  return ProgressionError::kOk;
}

ProgressionError progressionTraversalTeardown(ProgressionSession& sess) {
  if (sess.mode != 3) return ProgressionError::kBadMode;
  // Dispatcher 0x401497: CMP [0x49a030],0 — the only mode-3 exit.
  if (sess.victoryPhase != 3) return ProgressionError::kNoTraversalExit;
  // 49a030 cleared (0x4014fb), fades = 1000.0f, FUN_004371bc
  // teardown — object/arena/script destruction only; health, ammo,
  // inventory, RNG and levelId all carry (no writer in its call
  // graph). FUN_0042b270 then writes 541492 = 5.
  sess.victoryPhase = 0;
  sess.mode = 5;
  return ProgressionError::kOk;
}

ProgressionError progressionTraversalComplete(ProgressionSession& sess) {
  auto e = progressionRequestTraversalEnd(sess);
  if (e != ProgressionError::kOk) return e;
  progressionAdvanceVictory(sess);
  progressionAdvanceVictory(sess);
  return progressionTraversalTeardown(sess);
}

ProgressionError progressionStepIntermission(ProgressionSession& sess,
                                             bool tallyDone) {
  if (sess.mode != 5) return ProgressionError::kBadMode;
  // FUN_0042c8b0 returns 0 while the tally/fade runs — the session
  // holds in mode 5 until the caller reports the presentation done.
  if (!tallyDone) return ProgressionError::kStageRunning;
  // Dispatcher exit (0x4015c3): FUN_0046ca84 + FUN_0042c824 teardown.
  if (sess.health <= 0) {
    sess.mode = 0;  // FUN_0041d85c — frontend
    return ProgressionError::kOk;
  }
  if (sess.levelId < 4) {
    // FUN_00429200(EAX=0) → mode 6, sub-state 2 (full advance).
    sess.mode = 6;
    sess.loaderSub = 2;
    return ProgressionError::kOk;
  }
  // 0x4015ef: MOV [541498],5 — the literal store; FUN_00422bc0 arms
  // the score-entry overlay (a presentation seam), then mode 7.
  sess.levelId = 5;
  ++sess.transitionCount;
  sess.mode = 7;
  return ProgressionError::kOk;
}

ProgressionError progressionStepLoader(ProgressionSession& sess,
                                       bool stageDone) {
  if (sess.mode != 6) return ProgressionError::kBadMode;
  if (!stageDone) return ProgressionError::kStageRunning;
  switch (sess.loaderSub) {
    case 2:  // FUN_00429f40 — level-intro card → sub 4
      sess.loaderSub = 4;
      return ProgressionError::kStageRunning;
    case 4:  // FUN_00429984 — debrief pages → sub 1
      sess.loaderSub = 1;
      return ProgressionError::kStageRunning;
    case 1: {
      // FUN_00429fe4 load-bar done → 0x4297d9: levelId++ — the only
      // normal-campaign advance write.
      ++sess.levelId;
      ++sess.transitionCount;
      if (sess.levelId == 6) break;  // ==6 → early exit, no briefing
      // FUN_00422bc0(1,3) arms the score overlay (seam); the store
      // of ECX=3 selects the briefing stage.
      sess.loaderSub = 3;
      // FUN_00429cb4 init effects (first state-3 frame): health
      // floored to 100; the 541618..1b weapon indicators reset.
      if (sess.health < 100) sess.health = 100;
      return ProgressionError::kStageRunning;
    }
    case 3:
      break;  // FUN_00429cb4 briefing done → mode-6 exit
    default:
      return ProgressionError::kBadMode;
  }
  // Dispatcher exit (0x40155d): teardown, fades = 1000.0f,
  // FUN_0041b7b4(levelId) preload — FALL3D_<id> assets are queued
  // only for id < 5 — then the next-mode select.
  sess.loaderSub = 0;
  if (sess.levelId < 5) {
    sess.mode = 2;  // FUN_0040ef28 — freefall init re-arms the latch
    sess.freefallHandedOff = false;
  } else {
    sess.mode = 3;  // FUN_004346e8 — traversal-only entry
  }
  return ProgressionError::kOk;
}

ProgressionError progressionStepMode7(ProgressionSession& sess) {
  if (sess.mode != 7) return ProgressionError::kBadMode;
  // 0x40160c: FUN_0046ca84 + FUN_00413b20 cleanup, FUN_0041b7b4
  // (levelId unchanged — no FALL3D assets queued since 5 >= 5),
  // FUN_004346e8 → mode 3.
  sess.mode = 3;
  return ProgressionError::kOk;
}

ProgressionError progressionEnterCinematic(ProgressionSession& sess) {
  if (sess.mode != 3) return ProgressionError::kBadMode;
  // FUN_0047b038: 541492 = 8, 49bd40 = 1 — the next mode-8 frame runs
  // FUN_004371bc teardown before the cinematic. A pending victory
  // latch is consumed by the teardown path either way.
  sess.victoryPhase = 0;
  sess.mode = 8;
  return ProgressionError::kOk;
}

ProgressionError progressionStepCinematic(ProgressionSession& sess,
                                        bool cinematicDone) {
  if (sess.terminalDone) return ProgressionError::kTerminalConsumed;
  if (sess.mode != 8) return ProgressionError::kBadMode;
  if (!cinematicDone) return ProgressionError::kStageRunning;
  // FUN_0047b3f4 finished → FUN_0047b674 + FUN_0041d85c: mode 0.
  sess.mode = 0;
  sess.terminalDone = true;
  return ProgressionError::kOk;
}

ProgressionError progressionLoadTraversalForCurrentLevel(
    const DataRoot& root, ProgressionSession& sess,
    TraversalRuntime& trav, std::string* detail) {
  auto fail = [&](ProgressionError e, const char* msg) {
    if (detail) *detail = msg;
    return e;
  };
  if (sess.mode != 3) return fail(ProgressionError::kBadMode,
                                  "traversal load requires mode 3");
  const int dir = progressionLevelDir(sess.levelId);
  if (dir < 0)
    return fail(ProgressionError::kBadLevelId,
                "level id outside the 0x4999e8 table");
  char base[64];
  std::snprintf(base, sizeof base, "TRAVERSE/LEVEL%d/LEVEL%d", dir, dir);
  const auto le = traversalRuntimeLoad(root, std::string(base) + ".DTI",
                                       std::string(base) + ".CMI",
                                       std::string(base) + "O.MTO",
                                       trav, detail);
  if (le != TraversalLoadError::kOk)
    return fail(ProgressionError::kTraversalLoad,
                detail ? detail->c_str() : "traversal load failed");
  // Same carry rule as the freefall handoff — the globals outlive
  // FUN_0041b7b4 / FUN_004346e8 / FUN_00433d40.
  trav.fieldHealth = sess.health;
  trav.rngState = sess.rng;
  trav.ammo = sess.ammo;
  return ProgressionError::kOk;
}

const CampaignLevelInfo* progressionCampaignTable(int& count) {
  // id, dir(0x4999e8), freefallFirst(id<5), viaMode7(id>=4 write),
  // terminal. Ids 6/7 have no BUILD_A data — see the header notes.
  static const CampaignLevelInfo kTable[8] = {
      {0, 7, true,  false, false},
      {1, 6, true,  false, false},
      {2, 3, true,  false, false},
      {3, 4, true,  false, false},
      {4, 8, true,  false, false},  // mode-5 exit writes id = 5
      {5, 5, false, true,  true},   // mode 7 → traversal; ends via mode 8
      {6, 2, false, true,  false},  // unreachable — LEVEL2 absent
      {7, 1, false, true,  false},  // unreachable — LEVEL1 absent
  };
  count = 8;
  return kTable;
}

} // namespace mdk
