#pragma once
// traversal_runtime.h — Phase 5G: native traversal runtime host.
//
// Assembles the proven 5A–5F systems over a real BUILD_A traversal
// level (LEVELn.DTI + LEVELn.CMI + LEVELnO.MTO) and drives them in
// the original per-frame order. Headless: no SDL or renderer — the
// Phase 5K camera pose (player_camera.*) is pure scalar math and the
// 5H script VM is platform-neutral.
//
// Original ownership (MDK95.EXE, OBSERVED via disassembly):
//
//   FUN_004346e8  traversal entry: FUN_00433d40(0) level load,
//                 FUN_00436100 init+loop, FUN_004362bc teardown.
//   FUN_00433d40  level loader: reads the sibling triple, builds the
//                 arena table (0x466-record pool), resolves type-2/4
//                 record names against the CMI/MTO model table,
//                 pairs type-6 connect records (FUN_00434e54 —
//                 "Unmatched connect" / "Mismatched connect type"),
//                 then spawns the player at s0:
//                   arena = s0[0] (clamped to arena count),
//                   pos   = {s0[1], s0[2], s0[3]}, yaw = s0[4].
//   FUN_00436100  the traversal frame. Order (OBSERVED):
//                 stream-drain gates -> player dispatch (5A/5B/5C/5D
//                 in FUN_00463608, input merge at its tail ->
//                 one-frame control latency) -> FUN_00432f84 object
//                 prepass -> FUN_004572ac object updates (current
//                 then carrier) -> FUN_0045cf18 pending-arena
//                 transfers -> FUN_004388d8 script-object calls ->
//                 FUN_00435178 portal test -> partner/current swap +
//                 FUN_00432d9c attach -> FUN_00434b44 type-1/3
//                 trigger scan -> FUN_004301e0 view blend +
//                 orientation tail + 0x540c08 commit + camera pose/
//                 matrix pair + eye->camPos portal tail (5K) ->
//                 timers -> primary/secondary
//                 surface updates (FUN_0040e19c scripted-move gate,
//                 FUN_00404cd4 event list, FUN_004134a0 +0x45e
//                 records) -> FUN_00436d60 world tick ->
//                 FUN_0040b4dc(0) pending re-arms -> FUN_00435eec
//                 floor probe -> arena LRU bookkeeping.
//
// Arena slots (single source of truth, mirrored in CollisionState):
//   0x540c48  current arena            -> rt.cur / cs.arena
//   0x540ca4  partner/carrier arena    -> rt.partner / cs.carrier
//   0x540ca8  partner-active flag      -> rt.partnerActive /
//                                         cs.carrierValid
//   0x540d3c  carrier-busy flag        -> cs.carrierBusy
//   0x540d30  master stream-load slot  -> rt.loadArena
//   0x540d60/0x540d64  arena LRU pair  -> rt.lruA / rt.lruB
//
// Bounded stream model: the original loads arena geometry through a
// staged stream machine (FUN_0041b654/FUN_00431cf4/FUN_00432404/
// FUN_00432534/FUN_00432740/FUN_0043209c) gated by 0x540d68/0x540d40.
// Those gates start cleared (FUN_00433c4c writes them at traversal
// init) and this host resolves synchronously — attach/request calls
// complete instantly, so the stage functions are equivalent to
// no-ops and are counted in seams.streamStageCalls only.
//
// Explicit deferred seams (counted, never emulated):
//   FUN_004388d8 script-object calls (tr_alcmd VM)
//   FUN_0040e19c scripted move (gated by 0x540d9c)
//   FUN_00404cd4 arena event/timer list (script-created, empty)
//   FUN_00436d60 world tick (particles/HUD/event machinery)
//   FUN_0046603c slide helper, FUN_0046a5b8 mantle,
//   FUN_0046ae60 timers, FUN_00432f84 object prepass,
//   FUN_00430bf8 camera obstruction (5K — call site proven,
//   gated 0x49b710 && |0x540d58|==0, may move player AND camera),
//   0x540cdc teleport block, 0x540ebc pending view snap.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cmi_directory.h"
#include "core/collision_query.h"
#include "core/dti_structure.h"
#include "core/traversal_script.h"
#include "core/dynamic_objects.h"
#include "core/frontend_machines.h"
#include "core/gameplay_input.h"
#include "core/player_camera.h"
#include "core/player_look.h"
#include "core/mto_directory.h"
#include "core/player_motion.h"
#include "core/player_surface.h"
#include "core/player_vertical.h"

namespace mdk {

class DataRoot;

// ---------------------------------------------------------------------------
// Level + arena runtime types
// ---------------------------------------------------------------------------

// One runtime arena — the native 0x466-record equivalent. Storage is
// stable (unique_ptr in TraversalRuntime::arenas) because collision
// tokens, ride state, and the +0x68 object list hold raw pointers.
struct TraversalArena {
  std::string name;                    // DTI arena name (8 chars)
  int index = -1;                      // DTI arena-table index
  const DtiArenaRecord* rec = nullptr; // resolved work record
  DynamicArena dyn;                    // col view + +0x68 object list
  SurfaceObjectState surface;          // +0x6c..+0x45e block (script-
                                       // configured -> starts zeroed)
  bool geometryLoaded = false;         // +0x24 landed (eager model)
  bool objectsSpawned = false;         // +0x44 bit2 (spawn-once)
  bool hasScriptObject = false;        // +0x220 — CMI table-3 lookup
                                       // (FUN_00458550) matched this
                                       // arena -> CMI bytecode record
                                       // consumed by the FUN_004388d8
                                       // tr_alcmd VM (PC=+0x108,
                                       // wait=+0x22c, altPC=+0x230)
  TraversalScriptState script;         // +0x118 ctx block — the
                                       // FUN_004388d8 interpreter
                                       // state (pc=+0x108/+0x220
                                       // gate+PC, wait=+0x22c,
                                       // resume=+0x230, stack +0x248+)
  std::uint32_t flags58 = 0;           // +0x58 — bound-object flag
                                       // dword (script flag group 1)
  std::uint8_t objFlag148 = 0;         // +0x148 — script byte (0x61)
  float objVars48[4] = {};             // +0x48 — object f32 vars
                                       // (operand mode 1)
  float scalar = 0.0f;                 // +0x462 — view scalar blended
                                       // into 0x540b54 by FUN_004301e0
  TraversalArena() = default;
  TraversalArena(const TraversalArena&) = delete;
  TraversalArena& operator=(const TraversalArena&) = delete;
  ~TraversalArena();
};

// Owned level package: file bytes plus parsed views. Model/collision
// pointers alias these buffers — the level must outlive arenas.
struct TraversalLevel {
  std::vector<std::byte> dtiBytes;
  std::vector<std::byte> cmiBytes;
  std::vector<std::byte> mtoBytes;
  DtiStructure dti;
  CmiDirectory cmi;
  MtoDirectory mto;
  EnemyTable enemies;                  // CMI table-1 + MTO region-A
  std::vector<DtiArenaRecord> work;    // post name-resolve + connect
                                       // pairing (FUN_00433d40 body)
  // Lazy runtime-model cache — the FUN_004286c8 deferred-geometry
  // table: CMI u32 is a .MAT offset whose flag bit marks unresolved.
  std::vector<std::optional<RuntimeModel>> models;
  std::vector<bool> modelTried;
  int modelsResolved = 0;
  int modelsFailed = 0;
  std::vector<std::string> unresolvedNames; // spawn names not in table
};

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

enum class TraversalLoadError : int {
  kOk = 0,
  kDtiRead,
  kCmiRead,
  kMtoRead,
  kDtiParse,
  kCmiParse,
  kMtoParse,
  kArenaNotFound,
  kCollisionBlobMissing,   // arena has no same-named MTO block
  kCollisionBlobParse,     // region-C parse failed
  kConnectUnmatched,       // FUN_00434e54 error path
  kConnectMismatch,
  kBadSpawnArena,          // s0[0] outside arena table after clamp
  kBadStart,               // diagnostic override invalid
};
const char* traversalLoadErrorName(TraversalLoadError e);

// ---------------------------------------------------------------------------
// Frame accounting + result
// ---------------------------------------------------------------------------

// Deferred-seam call counters — each increments where the original
// would have invoked the named system. Nothing is emulated.
struct TraversalSeams {
  int streamStageCalls = 0;     // stream machine stages (eager model)
  int objectPrepass = 0;        // FUN_00432f84 (0x540c74 gate)
  int scriptObjectCalls = 0;    // FUN_004388d8 per arena (+0x220 set)
  int scriptedMoveCalls = 0;    // FUN_0040e19c (0x540d9c gate)
  int arenaEventListCalls = 0;  // FUN_00404cd4 (+0x5c list, empty)
  int slideHelperCalls = 0;     // FUN_0046603c (slope-assist vec)
  int mantleCalls = 0;          // FUN_0046a5b8 (forwardIntent gate)
  int timersCalls = 0;          // FUN_0046ae60
  int worldTickCalls = 0;       // FUN_00436d60
  int extraWorldTickCalls = 0;  // FUN_0042b20c + FUN_00436d60(-1)
  int profilerHooks = 0;        // FUN_0042fecc rdtsc probe sites
  int postTailCalls = 0;        // FUN_0042ff9c + FUN_0042fef4 walk
  int teleportCalls = 0;        // 0x540cdc teleport block
  int pendingViewSnaps = 0;     // 0x540ebc snap consumed
  int arenaReleases = 0;        // 0x540d60/64 LRU drops
  int type1Triggers = 0;        // FUN_00434b44 attach/detach fires
  int type3Prefetches = 0;      // FUN_00434b44 prefetch fires
  int objectMigrations = 0;     // FUN_004574d0 pending transfers
  int portalsCrossed = 0;       // FUN_00435178 passes
  int deepFloorFallbacks = 0;   // sweep failsafe fired
  int overheadViewCalls = 0;    // FUN_00431100 overhead view block
                                // (0x49b740 path — Phase 5K)
  int cameraObstructionCalls = 0; // FUN_00430bf8 gate fired
                                // (0x49b710 && |0x540d58|==0 —
                                // Phase 5K boundary seam)
};

struct TraversalFrameResult {
  int frame = 0;
  int curArenaIndex = -1;
  int partnerArenaIndex = -1;
  float pos[3] = {0, 0, 0};
  float posPrev[3] = {0, 0, 0};      // 0x540c08
  float yawDeg = 0.0f, pitchDeg = 0.0f, bankDeg = 0.0f;
  float moveVel = 0.0f, strafeVel = 0.0f, turnVel = 0.0f;
  float vertVel = 0.0f;
  bool grounded = false;
  bool rideActive = false;
  bool positionChanged = false;
  bool collisionIssued = false;
  float appliedDispZ = 0.0f;
  std::uint32_t contactObj = 0;      // 0x540e4c token
  const CollisionPoly* contactPoly = nullptr;
  float contactNormal[3] = {0, 0, 0};
  float playerBox[6] = {0, 0, 0, 0, 0, 0}; // 0x540c30..44
  int locoState = 0;                 // 0x540cac
  int eventType = 0, eventMag = 0;   // 0x54cb00/08 — the pending
                                     // event slots (cleared each
                                     // dispatch head, OBSERVED)
  int slideChannel = 0;              // 0x540e24
  bool viewOnPartner = false;        // 0x49b714
  bool partnerActive = false;        // 0x540ca8
  bool currentArenaSwapped = false;  // portal swap ran this frame
  int portalCandidate = -1;          // dest arena idx (fields[0])
  float eventTimer = 0.0f;           // 0x540eb0
  float viewScalar = 0.0f;           // 0x540b54
  float lookOffsetDeg = 0.0f;        // 0x540d58 — look offset
  float viewYawDeg = 0.0f;           // 0x540b50 — 90 - yaw
  float viewPitchDeg = 0.0f;         // 0x540be0 — effective pitch
  float viewZDelta = 0.0f;           // 0x49b718
  float viewPitchLift = 0.0f;        // 0x49b71c
  bool overheadViewActive = false;   // FUN_00431100 ran this frame
                                     // (0x540c9c==0 && 0x49b740!=0)
  PlayerCameraPose camera;           // Phase 5K — 0x540b28..0x540bdc
                                     // pose + matrix pair + scalars
  TraversalSeams seams;              // cumulative snapshot
};

// ---------------------------------------------------------------------------
// Runtime host
// ---------------------------------------------------------------------------

struct TraversalRuntime {
  TraversalLevel level;
  // Stable arena storage — NATIVE PORT infrastructure. Pointers are
  // semantically meaningful (collision tokens, ride state, +0x68
  // list links, pendingArena), so arenas are unique_ptrs whose
  // addresses never move once created.
  std::vector<std::unique_ptr<TraversalArena>> arenas;

  // --- arena slots (original globals) ---
  TraversalArena* cur = nullptr;       // 0x540c48
  TraversalArena* partner = nullptr;   // 0x540ca4
  bool partnerActive = false;          // 0x540ca8
  TraversalArena* loadArena = nullptr; // 0x540d30
  TraversalArena* lruA = nullptr;      // 0x540d60
  TraversalArena* lruB = nullptr;      // 0x540d64

  // --- player subsystem state (single ownership) ---
  // cs.pos / vert.posX..Z mirror one original vec (0x540bfc..0x540c04);
  // the step function keeps them synchronized at the proven write
  // points — no other copies exist.
  CollisionState cs;
  PlayerMotionState motion;
  PlayerVerticalState vert;
  GameplayInputState inputState;
  GameplayInputFrame prevFrame;        // merged control N-1 — the
                                       // one-frame-latency block

  // --- mirrored runtime globals (documented per field) ---
  int locoState = 0;              // 0x540cac — dispatched state code
  int eventType = 0, eventMag = 0;// 0x54cb00 / 0x54cb08
  int slideChannel = 0;           // 0x540e24 — slide-mode channel;
                                  // portal pass sets it to -15
                                  // when positive (OBSERVED)
  int eventPriority = 0;          // 0x540cbc — current event
                                  // priority (cbc): the dispatcher
                                  // latches cac/cbc = cb08/cb00
                                  // when cbc < cb00; cleared for the
                                  // transient states 300/400/500/
                                  // 600/601 at the dispatch head, by
                                  // the slide clear, and by the
                                  // look-anim end (OBSERVED set of
                                  // writers — the earlier "slide
                                  // aux" guess is resolved)
  bool viewOnPartner = false;     // 0x49b714 — surface-update select
  int flag49b740 = 0;             // 0x49b740 — FUN_004301e0 gate
  float viewScalar = 4.0f;        // 0x540b54 — blended arena scalar;
                                  // FUN_00433c4c inits it to 4.0
                                  // (0x433c9d: MOV EAX,0x40800000 —
                                  // Phase 5K evidence correction)
  PlayerLookState look;           // Phase 5J — 0x540d58 (FUN_00465c4c)
  PlayerViewTail view;            // Phase 5J — 0x49b718/0x49b71c/
                                  // 0x540b50/0x540be0 (FUN_004301e0)
  PlayerCameraState camera;       // Phase 5K — 0x540b58/0x540db4/
                                  // 0x540db8/0x540b5c/0x49b74c/
                                  // 0x540ce4..0x540cfc/0x49b710 +
                                  // the 0x540b28..0x540bdc pose
                                  // (FUN_004301e0/FUN_00431100)
  int flagBec = 0;                // 0x540bec — blend gate
  int pendingViewSnap = 0;        // 0x540ebc
  float pendingView[4] = {0, 0, 0, 0}; // 0x540ec0..0x540ecc
  float eventTimer = 0.0f;        // 0x540eb0
  const DynamicObject* eventTimerObj = nullptr; // 0x540eb4
  int flagC9c = 0;                // 0x540c9c — transition/death gate
  int transitionPhase = 0;        // 0x540ca0
  bool flag541548 = false;        // 0x541548 — extra world-tick gate
  bool flag5414bc = false;        // 0x5414bc — FUN_0046ae60 arg
  int fieldCc8 = 0;               // 0x540cc8 — cleared per frame
  int fieldE14 = 0;               // 0x540e14 — cleared per frame
  bool masterMoveGate = false;    // 0x540d9c — FUN_0040e19c gate
  int fieldC74 = 0;               // 0x540c74 — FUN_00432f84 gate
  int teleportFlag = 0;           // 0x540cdc — teleport block gate
  float teleportVec[6] = {0, 0, 0, 0, 0, 0}; // 0x540ce0..0x540cf4
  int flagE72 = 0;                // 0x540e72 — bit1 silences hard
                                  // landing (fed to vert env)
  float bankAux = 0.0f;           // 0x540b60 — aux bank term (the
                                  // writer is UNKNOWN; folded into
                                  // the bankIdle test)
  bool bankIdle = false;          // 0x540bcc — bank+aux == 0
  int frameCounter = 0;           // 0x540ce0 — step counter

  // The last collisionApply contact token as a poly pointer — the
  // surface the player most recently touched (0x540e4c's EAX is the
  // same token; contactObj on `vert` carries the u32 form).
  const CollisionPoly* lastContactPoly = nullptr;

  // tr_alcmd script VM (Phase 5H) — bounded diagnostics + spawn
  // accounting collected across the frame's arena invocations.
  std::vector<std::string> scriptDiag;
  int scriptSpawned = 0;             // objects created by 0x95 family
  int scriptInsnTotal = 0;           // instructions executed (total)
  int scriptRuns = 0;                // FUN_004388d8 invocations

  TraversalSeams seams;
};

// ---------------------------------------------------------------------------
// Load + assemble
// ---------------------------------------------------------------------------

// Loads LEVELn.DTI + sibling .CMI + O.MTO through DataRoot (read-
// only), parses all three, builds the enemy/model table, resolves
// arena work records (type-sorted sub-record table + name fixups),
// pairs type-6 connect records (FUN_00434e54), resolves the s0 spawn
// (arena index + position + yaw — OBSERVED loader write), attaches
// the initial arena (geometry + spawn-once), and initializes the
// player/collision state. `detail` receives a human-readable
// diagnostic string on failure.
TraversalLoadError traversalRuntimeLoad(const DataRoot& root,
                                        const std::string& dtiPath,
                                        const std::string& cmiPath,
                                        const std::string& mtoPath,
                                        TraversalRuntime& rt,
                                        std::string* detail);

// NATIVE DIAGNOSTIC OVERRIDE — not original behavior. Re-anchors the
// player at `pos`/yaw and re-attaches `arenaIndex` as current
// (partner cleared). For selftests only; the original spawn comes
// from s0 (see traversalRuntimeLoad).
TraversalLoadError traversalRuntimeDiagnosticStart(
    TraversalRuntime& rt, int arenaIndex, const float pos[3],
    float yawDeg, std::string* detail);

// ---------------------------------------------------------------------------
// Arena services (original equivalents in comments)
// ---------------------------------------------------------------------------

// FUN_00419ee0 — resolve arena's MTO collision block by name and
// parse region C into dyn.col (zero-copy into level.mtoBytes).
TraversalLoadError traversalArenaLoadGeometry(TraversalRuntime& rt,
                                              TraversalArena& arena,
                                              std::string* detail);

// FUN_00432d9c tail — attach side-effects on an already-slotted
// arena: geometry ensure + stream drain (eager no-op) + spawn-once.
void traversalAttachSideEffects(TraversalRuntime& rt,
                                TraversalArena& arena);

// FUN_00432d9c — partner-slot attach. arg==cur -> side effects only;
// arg!=partner -> partner=arg + side effects; always ca8=1.
void traversalAttachPartner(TraversalRuntime& rt, TraversalArena& a);

// FUN_00432980 cold prefetch (type-3): partner=arg (if different),
// geometry ensure, ca8=0 — no spawn.
void traversalPrefetchPartner(TraversalRuntime& rt, TraversalArena& a);

// FUN_00432bf8 — detach partner (type-1 fields[0]==-1).
void traversalDetachPartner(TraversalRuntime& rt);

// FUN_00435178 — scan an arena's type-6 records for a portal
// crossing of the from->to segment; returns the destination arena
// on pass, else nullptr. OBSERVED pure-read: the original writes
// neither endpoint. The camera tail calls it with
// (cur, eye=pos+3z, camPos); the player path uses (cur, prev, pos).
TraversalArena* traversalPortalScanSegment(TraversalRuntime& rt,
                                           const TraversalArena& arena,
                                           const float from[3],
                                           const float to[3]);

// Player-call wrapper — (cur, entryPos -> pos) as the original does.
TraversalArena* traversalPortalTest(TraversalRuntime& rt);

// FUN_00457738 — arena-connector (door) state update, gated by
// col.flags14a & 0x10. Runs first in the per-object dispatch: self-
// migration, proximity open/close, partner attach/detach, element
// mask rebuild, +0x148 bit4 collision toggle.
void traversalConnectorUpdate(DynamicObject& o, TraversalRuntime& rt);

// FUN_004555bc — per-object animation advance for the +0x114
// connector anim record. Advances +0xdc by rate*+0xe0/30 per frame,
// applies FRNDINT(+0xdc) to +0xe4, latches +0x118=0xff00 at
// frameCount-1 (non-looping). Inert when +0x114 == nullptr.
void traversalObjectAnimUpdate(DynamicObject& o);

// FUN_00434b44 — type-1/3 trigger scan on the current arena.
void traversalTriggerScan(TraversalRuntime& rt);

// FUN_00412d04 — the tr_alcmd volume-activation seam: find the
// arena's type-7 record whose fields[0] == id, create the +0x45e
// record with script-supplied kind/rate/queryMask (+0x10 no-falloff
// flag). Returns false when no such record exists (original prints
// a "cannot find fan hotspot" error and continues).
bool traversalVolumeActivate(TraversalArena& arena, int id, int kind,
                             float rate, std::uint32_t mask,
                             bool noFalloff);

// FUN_00434e54 — pair type-6 records across arenas (connect id in
// fields[0]: side codes must XOR to 1, boxes must match; type-6 may
// pair with type-7 records). Errors are load-fatal in the original.
TraversalLoadError traversalConnectPairing(
    std::vector<DtiArenaRecord>& arenas, std::string* detail);

// ---------------------------------------------------------------------------
// Frame driver
// ---------------------------------------------------------------------------

// One traversal frame — FUN_00436100's traversal-active section in
// original order (see header comment). `raw` is the 5A raw input
// snapshot for THIS frame; `env` carries the frame timing (smoothed,
// deltaSeconds, frameStep). Returns observable end-of-frame state.
TraversalFrameResult stepTraversalRuntime(
    TraversalRuntime& rt, const RawGameplayInput& raw,
    const GameplayInputBindings& bindings,
    const FrontendTimingState& timing);

} // namespace mdk
