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

} // namespace mdk
