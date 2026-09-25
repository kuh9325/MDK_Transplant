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

#include <array>
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
#include "core/player_fire.h"
#include "core/player_projectiles.h"
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
  // +0x118 doubles as a pseudo-object for the punch element event
  // latch (FUN_00432f84): the latch writes +0x06 named / +0x08 health
  // / +0x2a2 threshold here when no real object is available, so the
  // 0x540eb4 timer validation reads them through the object layout.
  DynamicObject eventLatch;
  std::uint32_t flags44 = 0;           // +0x44 — arena flag dword;
                                       // bit2 mirrors objectsSpawned
                                       // (the FUN_00432d9c spawn gate),
                                       // bit0 gates FUN_00432c34's
                                       // activation calls. Persisted
                                       // verbatim by full saves.
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
  // TRAVSPRT.BNI — the traversal-context anim bank (the original's
  // DAT_004a1e38 image while traversing). Loaded when present; the
  // mover's SW_H150 records resolve into it.
  std::vector<std::byte> travsprtBytes;
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
  // --- Phase 5L — sniper + mounted reticle seams (counted stubs) ---
  int scopeOverlayCalls = 0;      // FUN_0041664c scope-overlay anim
  int sniperExitCalls = 0;        // FUN_0046ca84 unscope seam
  int weaponScanCalls = 0;        // FUN_00469b98 scoped weapon-select
  int hudEventCalls = 0;          // FUN_0040210c/FUN_00402388/
                                // FUN_00402014 HUD-resource seams
  int sniperFireCalls = 0;        // FUN_0045f138 projectile spawn
  int animEventCalls = 0;         // FUN_00469668 anim-event seam
  int itemUseCalls = 0;           // FUN_00459d28 item activation
  int reticleSpawnCalls = 0;      // FUN_0046153c + FUN_00454794 +
                                // FUN_00454af8 + FUN_0045612c +
                                // FUN_00402fe8 + FUN_00402160
                                // (ballistic-object spawn seams)
  int reticleDrawCalls = 0;       // FUN_0046911c reticle HUD seam
  int reticleDeathCalls = 0;      // FUN_004581a4 mount-death seam
  int animDriverCalls = 0;        // FUN_00431300/FUN_00461954
  int mountUpdateCalls = 0;       // FUN_00467ac4/FUN_00467ed0 class 1/2
  int weaponSlotCalls = 0;        // FUN_00469cd0 inventory scanner
  int mountUnmountCalls = 0;      // unrecognised-class unmount log
                                  // (FUN_00408eb0) + e6c release
  // --- Phase 5N — player weapon fire (see player_fire.h) ---
  int shotSpawnCalls = 0;         // FUN_0045f138 0..4 pool spawns
  int weapon5SpawnCalls = 0;      // FUN_0045a4dc thrown-object spawn
  int bombCinematicCalls = 0;     // FUN_0042f310 — the X_STRIKB/X_STRIKD
                                  // presentation prop manager invoked
                                  // at the top of FUN_0045a4dc(0) and
                                  // on the cinematic path (arg=1).
  int fireDenyCalls = 0;          // FUN_00402388(1,0x54c650) no-fire
  int fireNotifyCalls = 0;        // FUN_00469668 fire on/off seam
  int fireSoundCalls = 0;         // FUN_004022b8(0x54c5d0) fire sound
  int classLookupCalls = 0;       // FUN_00454794 class-name lookups
  int punchHitCalls = 0;          // punch target hit -> damage tail
  int punchWallCalls = 0;         // punch miss -> wall impact seam
  int moverSfxCalls = 0;          // FUN_00402388(0x54c644 "RUNNER",0)
                                  // — the SW_H150 reaction sound
                                  // (FUN_004585c4; audio seam)
  int chargeProbeCalls = 0;       // FUN_00437aa8 -> FUN_0046145c
  int shotRenderCalls = 0;        // FUN_0045f030 shot-pool render pass
  int shotPoolTickCalls = 0;      // FUN_004572ac shot-pool update seam
  int punchDeathCalls = 0;        // FUN_0042ac90 punch-kill seam
  int punchEffectCalls = 0;       // FUN_00437444/FUN_00458140/etc fx
  // --- Phase 10A — projectile lifecycle (player_projectiles.h) ---
  int shotImpactFxCalls = 0;      // FUN_00437444 wall/impact fx seam
  int remnantSpawnCalls = 0;      // FUN_004575fc detonation remnant seam
  int objectDeathCalls = 0;       // FUN_00458140 deaths run
  int objectTeardownCalls = 0;    // FUN_00457cf4 teardown seam
  int mountDamageCalls = 0;       // FUN_0046771c mount-redirect hits
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
  float playerBox[6] = {0, 0, 0, 0, 0, 0}; // 0x540c30..44 — the LAST
                                         // per-query AABB collisionApply
                                         // wrote this frame (volatile)
  float playerBodyBox[6] = {0, 0, 0, 0, 0, 0}; // 0x540c30..44 captured at
                                             // the mode-3 tail rebuild —
                                             // the standing {pos+-1.25,
                                             // pos.z, +1.25, +4.25} box
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

// The 0x54155c inventory table record (0x24-stride, 5 slots). OBSERVED
// (FUN_0046a3d8 delete, opcode-0xaf scan, FUN_00432f84 type-6 consume):
// id + charges are gameplay-authoritative; the anim/slot fields are the
// HUD slide-row state (48px slots — rec+0x18 drops 0x30 on delete,
// rec+0x10 recomputes as (slotX - animX) * 2.0).
struct InventoryRecord {
  std::int32_t id = 0;          // +0x00 item type (5,6 OBSERVED)
  std::int32_t charges = 0;     // +0x04 amount — opcode 0xaf sums this
  float animX = 0.0f;           // +0x08 HUD slide x (float)
  float animY = 0.0f;           // +0x0c bar row y (float)
  float animVel = 0.0f;         // +0x10 slide-in velocity
  float animAux = 0.0f;         // +0x14
  std::int32_t slotX = 0;       // +0x18 HUD target x
  std::int32_t slotY = 0;       // +0x1c bar row y (int mirror)
  std::int32_t aux = 0;         // +0x20
};

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
  int fieldD2c = 0;               // 0x540d2c — frameStep-decayed
                                  // countdown; set to 10 by the
                                  // teardown when a +0x11e==0xf
                                  // object dies (FUN_00457cf4 +
                                  // FUN_0045828c, OBSERVED)
  const DynamicObject* fieldB85c = nullptr; // 0x49b85c — the last
                                  // +0x07==1 object seen in the
                                  // FUN_004572ac update loop (the
                                  // camera view-anchor source);
                                  // cleared when that object dies
  int flagC9c = 0;                // 0x540c9c — transition/death gate
  int transitionPhase = 0;        // 0x540ca0
  bool flag541548 = false;        // 0x541548 — extra world-tick gate
  bool flag5414bc = false;        // 0x5414bc — FUN_0046ae60 arg
  bool flag4999d0 = false;        // 0x4999d0 — pause/debug gate; with
                                  // flag541548 it suppresses the anim
                                  // machine + the 54161b world-tick decay
  int fieldCc8 = 0;               // 0x540cc8 — cleared per frame
  int fieldE14 = 0;               // 0x540e14 — cleared per frame
  bool masterMoveGate = false;    // 0x540d9c — FUN_0040e19c gate
  bool vertEnable = true;         // 0x540c6c — vertical master gate;
                                  // set at traversal init. Read by the
                                  // sniper abort/entry gates and fed
                                  // to vert env.
  int fieldC74 = 0;               // 0x540c74 — FUN_00432f84 gate
  int teleportFlag = 0;           // script-side teleport seam gate;
                                  // 0x540cdc is float-only (focusDist)
                                  // — no int DAMP home (Phase 14C.1)
  float fieldD00 = 0.0f;          // 0x540d00 — FUN_00463608 exclude
                                  // latch (mounted-idle event-100
                                  // variant); restored, consumer
                                  // deferred with that variant
  float bankAux = 0.0f;           // 0x540b60 — aux bank term (the
                                  // writer is UNKNOWN; folded into
                                  // the bankIdle test)
  bool bankIdle = false;          // 0x540bcc — bank+aux == 0
  int frameCounter = 0;           // 0x540ce0 — step counter

  // --- Phase 11C — mover anim-record globals (FUN_004585c4) ---
  // 0x54c6a4/0x54c6b0 — TRAVSPRT.BNI "H150_I"/"H150_R" record
  // payload+4 pointers bound once at context init (0x434510..,
  // FUN_004039ec). Null when the bank is absent; the mover compares
  // +0x114 against them verbatim.
  const void* animH150I = nullptr;    // 0x54c6a4
  const void* animH150R = nullptr;    // 0x54c6b0

  // --- Phase 5L — sniper mode + mounted reticle (OBSERVED globals) ---
  // Shared counter + fire cadence (0x540d0c family): the sniper fire
  // gate reads d0c>=5 && 54161b==0; the reticle uses d0c as its
  // semi-auto latch (999 armed, drains by frameStep while held).
  int fieldD0c = 0;               // 0x540d0c — fire cadence counter
  float fireCadence = 0.0f;       // 0x54161b — fire-cadence/blend timer
  int burstIndex = 0;             // 0x54161a — burst counter (FUN_0045f138)
  int wpnSel0 = 0;                // 0x541618 — weapon-select current
  int wpnSel1 = 0;                // 0x541619 — weapon-select pending
  // Scope-phase + HUD (FUN_00436100 head / FUN_00461954 / FUN_00461878):
  int scopeAnimLatch = 0;         // 0x540e74 — scope overlay anim latch
  float scopeBlend94 = 0.0f;      // 0x540e94 — scope overlay blends
  float scopeBlend98 = 0.0f;      // 0x540e98
  float scopeChanC = 0.0f;        // 0x540ccc — scope-request channels
  float scopeChanD0 = 0.0f;       // 0x540cd0   (consumers UNKNOWN)
  float scopeChanD4 = 0.0f;       // 0x540cd4
  int scopeHudOffset = -101;      // 0x540d34 — scope HUD y-offset / the
                                  // anim machine's d34 write target
  int scopeScale = 0;             // 0x540dbc — scope FOV multiplier
                                  // (FILD'd by the 0x323 anim handler)
  const DynamicObject* focusObj = nullptr; // 0x540cd8 — zoom-floor focus
  float focusDist = 0.0f;         // 0x540cdc — focus distance (also the
                                  // 0x540cdc teleport-block writer alias —
                                  // kept distinct by teleportFlag)
  // Mounted/reticle (FUN_00463608 mount-scan + FUN_004691c4):
  std::uint32_t mountClass = 0;   // 0x540e70 — mounted class dword
  float mountYaw = 0.0f;          // 0x540e64 — mount yaw (class-1 entry)
  int reticleAux = 0;             // 0x540d08 — reticle aux / FUN_00461954
                                  // tail gate
  int bombs = 0;                  // 0x540ea0 — reticle bomb count
  float bombRecharge = 0.0f;      // 0x540ea4 — bomb recharge timer
  float overheadAux744 = 0.0f;    // 0x49b744 — overhead-view aux; the
  float overheadAux748 = 0.0f;    // 0x49b748   X_STRIKE mount entry
  float overheadAux76c = 0.0f;    // 0x49b76c   clears all three to 0

  // --- Phase 5N — player weapon fire (player_fire.h) ---
  // 0x54161f..0x541633 — the six-dword ammo block: ammo[0] doubles
  // as the punch-charge resource; ammo[1..5] gate the weapons.
  std::array<int, 6> ammo = {};
  // The 0x54155c inventory table — NOT reset by the save loader
  // (FUN_00427218 xref-clean; the FUN_00433c4c reset only rewrites
  // 0x541618/19/1a/1b). Persisted authoritative state.
  InventoryRecord inventory[5]; // 0x54155c..0x54160f
  int inventoryCount = 0;       // 0x541610 — live record count
  int inventorySel = 0;         // 0x541614 — HUD selection
  int invSelAux = 0;            // 0x541616 — low u16 of the packed
                              // select dword (survives the reset)
  int invHudTimer = 0;          // 0x541558 — FUN_00469f7c HUD anim
                              // countdown (60-tick writes)
  int field541498 = 0;            // 0x541498 — weapon-5 charge level
  int field54163b = 0;            // 0x54163b — weapon-5 fire latch
  int weapon5Probe = 0;           // PORT test hook: when nonzero the
                                  // charge probe is forced live
                                  // (FUN_0046145c skipped). The real
                                  // probe runs whenever this is 0.
  float weapon5Aim[3] = {0, 0, 0};  // 0x540e18 — the FUN_0046145c
                                  // probe hit point (+ camera basis
                                  // bump); FUN_0045a4dc reads it as
                                  // the trajectory apex.
  std::int32_t weapon5Path[1 + 5 * 10] = {}; // 0x54ca00 — the SINGLE
                                  // shared 5-key path record rebuilt
                                  // on every weapon-5 throw (OBSERVED:
                                  // one global, not per-bomb).
  int shotSerial = 0;             // 0x540e80 — per-spawn serial
  int fieldE88 = 0;               // 0x540e88 — script-incremented
                                  // counter; FUN_00429200 mode
                                  // dispatch reads it (<5/<0x11
                                  // tiering) — OBSERVED
  int fieldE9c = 0;               // 0x540e9c — FUN_00436100 head /
                                  // FUN_00422bc0 gate; FUN_0047b7e0
                                  // init-writes 0x47
  std::uint32_t flagEac = 0;      // 0x540eac — FUN_00423ca0 cheat
                                  // flag bits (persisted cheat state)
  std::array<PlayerShot, 3> shots{};  // 0x540ed4 — the 3-slot pool
  int punchTime = 0;              // 0x540e78 — punch jitter accum
  int punchHitTime = 0;           // 0x540e7c — hit-time accumulator
  // Phase 10A — projectile/combat globals (player_projectiles.h):
  int shotHitCount = 0;           // 0x540e84 — w0/1 direct-hit counter
  int killTally = 0;              // 0x540e90 — FUN_0042ac90 kill tally
  PlayerShot* lobbedShot = nullptr; // 0x49b8e4 — last type-4 shot to
                                  // touch a wall (the ribbon-binder
                                  // gate); cleared on slot release
  float fieldE10 = 0.0f;          // 0x540e10 — player-damage suppress
                                  // window (>0 suppresses, float)
  // Damage/energy (FUN_00467a00 difficulty-scaled drain):
  int fieldDac = 0;               // 0x540dac — damage accumulator (cap 180)
  int fieldHealth = 0;            // 0x541554 — player health (drained)
  int fieldHealthGate = 0;        // 0x541510 — health/difficulty gate
  int difficulty = 1;             // 0x54147a — difficulty (0 easy/1/2 hard)
  int hudActive = 0;              // 0x5414d4 — HUD gate for d0c++/blit
  // Phase 10B — combat presentation event log (player_projectiles.h).
  // One CombatFxEvent per original FUN_00437444/FUN_004575fc/
  // FUN_00458140 callsite, pushed alongside the seams.* counters.
  // Presentation-only — the frontend drains it; nothing consumes it
  // in-game. (std::move + clear to drain.)
  std::vector<CombatFxEvent> combatFx;
  // Animation machine (FUN_00461954) + slide vector + scripted gates:
  int animPrev = -1;              // 0x540cb0 — previous anim state (the
                                  // first-frame detect latch)
  int animFrame = 0;              // 0x540cb4 — anim frame counter
  float animPhase = 0.0f;         // 0x540cb8 — FUN_00464308 move-anim
                                  // phase accumulator (integrates
                                  // moveVel; restored, port consumer
                                  // deferred with the anim machine)
  float slideState[6] = {};       // 0x540e2c..0x540e43 — FUN_0046603c
                                  // slide-mode internals; restored,
                                  // consumer deferred with the
                                  // slide port (seam)
  float animE44 = 0.0f;           // 0x540e44 — slide vector X
  float animE48 = 0.0f;           // 0x540e48 — slide vector Y
  float ambientFades[3] = {};     // 0x540d18/0x540d24/0x540d28 — live
                                  // ambient-sound fade accumulators;
                                  // restored, no port consumer yet
                                  // (audio seam)
  std::int32_t ambientChan[6] = {}; // 0x540d70..0x540d87 — two 12-byte
                                  // ambient-sound channel records;
                                  // restored raw (audio seam)
  int fieldDa0 = 0;               // 0x540da0 — scripted transition gate
  int fieldDa4 = 0;               // 0x540da4 — post-tick decay target
  int fieldEb8 = 0;               // 0x540eb8 — death-fade mode byte

  // The last collisionApply contact token as a poly pointer — the
  // surface the player most recently touched (0x540e4c's EAX is the
  // same token; contactObj on `vert` carries the u32 form).
  const CollisionPoly* lastContactPoly = nullptr;

  // --- Phase 11B — enemy runtime globals (enemy_runtime.*) ---
  std::uint32_t rngState = 1;       // CRT rand() __threadseed — the
                                    // FUN_0047d2b5 LCG state (default
                                    // 1 = the CRT unseeded start).
  // FUN_0045897c command-registry globals:
  int cmdFlag54 = 0;                // 0x540e54 — set by most command
                                    // paths (idle/exec ran this tick)
  int cmdDetonate58 = 0;            // 0x540e58 — cmd-5 arming counter
                                    // (the first two armed detonators
                                    // don't set e54)
  DynamicObject* cmdObj5c = nullptr;// 0x540e5c — active cmd-1 lunge
                                    // object registry
  DynamicObject* cmdObj60 = nullptr;// 0x540e60 — active cmd-2 spin
                                    // object registry

  // tr_alcmd script VM (Phase 5H) — bounded diagnostics + spawn
  // accounting collected across the frame's arena invocations.
  std::vector<std::string> scriptDiag;
  int scriptSpawned = 0;             // objects created by 0x95 family
  int scriptInsnTotal = 0;           // instructions executed (total)
  int scriptRuns = 0;                // FUN_004388d8 invocations

  // --- Phase 14C — persisted globals restored by full saves ---
  // These are original globals the full-save MORE/PLAY/DAMP packets
  // carry; the port parks them here so a restore is byte-faithful and
  // the script VM sees its persistent operands.
  float fadeTimer5414a0 = 0.0f;      // 0x5414a0 — transition fade
  float fadeTimer5414a4 = 0.0f;      // 0x5414a4   (MORE+0x04..+0x0c;
  float fadeTimer5414a8 = 0.0f;      // 0x5414a8   both a4/a8 take +0x08)
  int field5414d8 = 0;               // 0x5414d8 — MORE+0x14 counter
  int field541518 = 0;               // 0x541518 — MORE+0x18; the
                                     // frontend tick mirror (the live
                                     // counter is frontend-owned)
  std::int8_t g541534 = 0;           // 0x541534 — script byte (op 0xca)
  float scriptGVars[8] = {};         // 0x540d88 — script operand
                                     // group 0 (slots 4..7 alias
                                     // scriptGFlags/masterMoveGate/
                                     // fieldDa0/fieldDa4)
  std::uint32_t scriptGFlags = 0;    // 0x540d98 — script flag group 0
  // The verbatim 0x541554 image (PLAY, 239B). Typed fields above
  // mirror the proven slots; +0x04..+0xc3 is the stats/inventory
  // region with no typed consumer yet — kept raw, not guessed.
  std::array<std::byte, 239> savePlayBlock = {};

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
//
// `flags` — bit0 suppresses the tail's initial-arena ensure/spawn,
// mirroring FUN_004346e8(param=2): the save-load path needs the fresh
// level data + s0 binds WITHOUT the eager spawn (restored objects
// arrive through ALIE records; unactivated arenas keep lazy spawn).
inline constexpr std::uint32_t kTraversalLoadSuppressSpawn = 1;
TraversalLoadError traversalRuntimeLoad(const DataRoot& root,
                                        const std::string& dtiPath,
                                        const std::string& cmiPath,
                                        const std::string& mtoPath,
                                        TraversalRuntime& rt,
                                        std::string* detail,
                                        std::uint32_t flags = 0);

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

// FUN_0046a3d8 — inventory record delete: shifts records above `idx`
// left one slot (recomputing each shifted record's HUD slide fields:
// slotX -= 0x30, animVel = (slotX - animX) * 2.0), decrements the
// count, then applies the selection fixup — `sel--` when it pointed
// past the new tail, plus the OBSERVED type-6 GATT quirk (a selected
// id-6 record nudges sel to 1 when sel==0, else sel--).
void inventoryRemove(TraversalRuntime& rt, int idx);

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

// Geometry ensure + the +0x44 bit-2 spawn-once gate (FUN_00432d9c's
// tail). Exposed for the save restore, which drives the same
// attach sequence on restored arenas.
void traversalEnsureLoaded(TraversalRuntime& rt, TraversalArena& arena);

// FUN_00432980 tail — connector pull-in from type-6 peer arenas.
void traversalMigrateInto(TraversalRuntime& rt, TraversalArena& a);

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

// FUN_004286c8 — the lazy runtime-model cache (deferred geometry
// table). Resolves enemy-table index -> RuntimeModel, parsing on
// first touch (ctx = &rt.level).
const RuntimeModel* traversalModelFor(int idx, void* ctx);

// FUN_004555bc — per-object animation advance (the generic driver —
// any object may bind +0x114 via ops 0x03/0x3b). Advances +0xdc by
// rate*+0xe0/30, applies FRNDINT(+0xdc)-+0xe4 steps through
// FUN_00455890 (vertex deltas + ref points + root impulse), latches
// +0x118=0xff00 at frameCount-1 (non-looping). `recLimit` bounds the
// record walk — pass the containing image end (nullptr = unchecked,
// test path). Full implementation: object_animation.cpp.
void traversalObjectAnimUpdate(DynamicObject& o,
                               const std::uint8_t* recLimit);

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
