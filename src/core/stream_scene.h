// stream_scene.h — Phase 19A: mode-5 "stream" cinematic scene runtime.
//
// Port of the native mode-5 engine (the steerable tunnel/intermission
// scene): FUN_0042b270 (init) / FUN_0042c8b0 (per-tick frame) /
// FUN_0042c824 (teardown). Resources: STREAM\STREAM.BNI + STREAM.MTI
// (OBSERVED — the init binds them by absolute path; records PAL, BG,
// PLANET, WIND, HITSIDE, RESCUE, APPLE, HURT1-7, LIGHT, KURT, BONES,
// PROFSHIP, GUNTA|SWH150, GUNTANIM|SWHANM, BONESANIM, KURTANIM,
// FL_HVR, FL_WAVE). Mode-6 (FUN_00429200) owns STATS.BNI — do not
// conflate; mode-8 (FUN_0047b038) is the FLIC->MVE ending pipeline and
// shares only the generic palette/framebuffer plumbing.
//
// Evidence (all OBSERVED unless noted): analysis-private/logs
//   p19a_b270.asm (init), p18c_42c8b0.asm (frame), p19a_asm1-5.txt
//   (helpers/updaters/lifecycle/render). Disassembly-verified against
//   MDK95.EXE; behavior below is the original's, quirks included —
//   each is labeled at the point of use.
//
// Native layout: one 0x6630-byte state block at 0x4e74b0 +
//   32-entry ring 0x4e74b8 (+0xc0: 16 vec3) +
//   32 plane sets 0x4e8cb8 (+0x200: 32x {n.xyz, d}) +
//   32 node mats 0x4eccb8 (+0x30: 3x4 row-major, t in col 3) +
//   32 pen sets 0x4ed2b8 (+0x20) + bucket heads 0x4ed6b8 +
//   drift/radius/pen/palette/sound/proto/slot globals +
//   a 400-record (0x32e) object pool with a freelist head.
//
// The port keeps the same architecture as freefall_runtime /
// enemy_runtime: a headless deterministic runtime, native global
// fields as named members, presentation emitted as typed events,
// and a snapshot/digest surface for tests. Original resources are
// injected via StreamAssets — the runtime never touches files.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "dynamic_objects.h"
#include "object_animation.h"

namespace mdk {

// --- record geometry (native strides; OBSERVED) -------------------------
inline constexpr int kStreamPoolSize = 400;  // records, stride 0x32e
inline constexpr int kStreamSegs = 32;       // ring slots (the window)
inline constexpr int kStreamRingPts = 16;    // path samples per slot
inline constexpr int kStreamPlanes = 32;     // plane records per slot
inline constexpr int kStreamPens = 32;       // pen bytes per slot
inline constexpr int kStreamSounds = 11;     // eda58..eda80 sound slots

// --- injected resource bindings ------------------------------------------
// The native init resolves STREAM.BNI records by name (OBSERVED). The
// port takes the resolved objects — models as parsed RuntimeModel protos
// (deep-copied per spawn, FUN_00403720), anims as ObjectAnimView spans,
// sprites/sounds/backdrop as opaque host tags carried through events.
struct StreamAssets {
  // Palette: the 768-byte scene palette = 192B global copy + 576B from
  // the PAL record's +0xc0 (OBSERVED: contiguous at 0x4ed758).
  const std::uint8_t* paletteGlobal = nullptr;  // 192B
  const std::uint8_t* palettePal = nullptr;     // 576B (record+0xc0)
  // Model protos (FUN_00428400-parsed RuntimeModel).
  const RuntimeModel* protoKurt = nullptr;      // KURT     -> hero edab4
  const RuntimeModel* protoBones = nullptr;     // BONES    -> rescue twin edac0
  const RuntimeModel* protoProfship = nullptr;  // PROFSHIP -> loaded, no
                                                // spawn site found in mode 5
                                                // (documented dead resource)
  const RuntimeModel* protoEscort = nullptr;    // GUNTA (final) |
                                                // SWH150 (non-final)
  // Anim records (ObjectAnimView-compatible spans; same +0x4/+0x8
  // channel/frame header as traversal records).
  const std::uint8_t* animEscort = nullptr;  // GUNTANIM | SWHANM -> edab0
  const std::uint8_t* animBones = nullptr;   // BONESANIM -> edaa0 (twin)
  const std::uint8_t* animKurt = nullptr;    // KURTANIM  -> edaa4 (hero)
  const std::uint8_t* animHvr = nullptr;     // FL_HVR    -> edaa8
  const std::uint8_t* animWave = nullptr;    // FL_WAVE   -> edaac
  const std::uint8_t* animLimit = nullptr;   // shared rec bound
  // Presentation tags (opaque to the runtime; echoed in draw/sound
  // events so the host can bind real images/audio).
  int bgTag = -1;               // BG bitmap (600x360 indexed backdrop)
  int planetTag[4] = {-1, -1, -1, -1};  // PLANET sub-images eda8c..98
  int lightTag = -1;            // LIGHT sprite -> debris +0x108
  int sndWind = -1;             // eda58 (looped during scene)
  int sndHitside = -1;          // eda5c (wall ricochet)
  int sndRescue = -1;           // eda7c (twin spawn)
  int sndApple = -1;            // eda80 (pickup catch)
  int sndHurt[7] = {-1, -1, -1, -1, -1, -1, -1};  // eda60..78 random hurt
};

// --- per-frame input ------------------------------------------------------
// The native FUN_00407f2c folds key/analog state into globals
// 0x4ce758/0x4ce75c (+-180.0 digital, or -analog*const). The fold lives
// at the host; the runtime consumes the two resolved axes.
struct StreamInput {
  float axis0 = 0.0f;  // -> +0x4c roll steer (center 90, clamp [45,135])
  float axis1 = 0.0f;  // -> +0x13c pitch steer (center 0, clamp [-45,45])
};

// --- presentation events ---------------------------------------------------
// The renderer/audio seams: the native frame fn blits the backdrop
// (FUN_0042e684 toroidal scroll), walks slots back-to-front emitting
// ribbon tris + per-slot object draw calls in bucket order
// (FUN_0042e100 -> FUN_00409a00; OBSERVED: the draw list is NOT
// z-sorted), applies fades, then presents. All side effects become
// events here so the runtime stays headless; order is preserved.
struct StreamEvent {
  enum Kind : std::uint8_t {
    kPlaySound,      // tag=sound tag (loop flag in aux)
    kStopSound,      // tag=sound tag (WIND at teardown)
    kBackdropBlit,   // f[0]=scrollU f[1]=scrollV (600x360 toroidal)
    kRibbonTri,      // one e620/0ca00 emission. f[0..8] = the three
                     // view-space verts (vx,vy,vz — pre-divide, pre-clip);
                     // tag = pen (depth-tint base - pens_[2i|2i+1], the
                     // native negative offset); aux = per-vert clip flags
                     // packed f0|f1<<8|f2<<16 {1:top 2:bot 4:right 8:left
                     // 0x10:near(0.05)}. Emitted only when the e620 plane
                     // test faces the eye AND 0ca00's trivial-reject
                     // (AND of flags) passes; near-clipping is the host's.
    kModelDraw,      // f[0..11] = camProj o object xform (the +0x7c
                     // precompute, native 3x4 order); aux = pool index
    kSpriteDraw,     // f[0..3] = {sx, sy, size, vz} (post-trunc ints);
                     // tag = sprite tag (+0x108 -> lightTag/planetTag[0]);
                     // aux = pool index. Marker variant additionally
                     // carries f[4]/f[5] = planetTag[1]/planetTag[2]
    kPaletteSet,     // aux = transform mode {0 install base palette,
                     // 1 black ramp (pal*fade — isFinal fade),
                     // 2 dark-in ramp (pal*fade + (fade-1)*255 —
                     // non-final fade), 3 red ramp (death fade<=1:
                     // R=trunc(fade*256), G/B=pal*fade), 4 red
                     // saturate (death fade>1: R=min(255,pal.R+
                     // trunc((2-fade)*255)), G/B unchanged)};
                     // f[0] = fade. Host applies to the base palette
                     // via palette().
    kPresent,        // end of frame — host displays the assembled image
    kExitMode,       // aux = exit-frame palette fill (0xff alive non-
                     // final, 0x00 final-or-dead); the mode dispatcher
                     // picks the next mode (course>=4 -> 7, else return)
  };
  Kind kind = kPresent;
  int tag = 0;
  int aux = 0;
  float f[12] = {};
};

// --- deterministic snapshot ------------------------------------------------
// Flat, allocation-free view of the simulation state for tests/digests.
// Native globals are named after their 0x4edxxx addresses where useful.
struct StreamSnapshot {
  std::int32_t winLo = 0;      // e74b0 — path window low
  std::int32_t winHi = 0;      // e74b4 — path window high
  std::int32_t complete = 0;   // e748
  std::int32_t isFinal = 0;    // edad0 — course>=4
  std::int32_t health = 0;     // 541554 — hero health (incoming global;
                               // the scene drains/clamps/sets it)
  float fade = 0.0f;           // eda9c — single accumulator: rises
                               // 0->1 while !complete (fade-in), falls
                               // ->0 while complete (fade-out->exit).
                               // heroUpdate's final-course terminal
                               // path writes 2.0 (OBSERVED 0x42d961:
                               // health=0, complete=1, fade=2.0 — the
                               // long ramp into the death/dead palette
                               // transform before the black exit)
  float radius = 0.0f;         // e744 — tunnel ribbon radius
  float drift[3] = {};         // e738/73c/740 — node drift walk
  std::int32_t penBase = 0;    // e74c
  std::int32_t penTarget = 0;  // e750
  float penT = 0.0f;           // e754 — pen sawtooth lerp
  float driftMax = 0.0f;       // edad4 (skill/course-seeded)
  float radiusMin = 0.0f;      // edad8
  float radiusMax = 0.0f;      // edadc
  int heroIdx = -1, escortIdx = -1, markerIdx = -1;
  int pickupIdx = -1, twinIdx = -1;   // edab4/b8/c4/c8/c0 pool indices
  float heroPathT = 0.0f, heroRoll = 0.0f, heroPitch = 0.0f;
  float heroSpeed = 0.0f, heroOfsX = 0.0f, heroOfsY = 0.0f;
  std::int16_t twinAnimFrame = -1;    // completion gate (>0x50)
  float eye[3] = {}, lookT = 0.0f;
  float camView[12] = {};      // 540bb0 [side|up|fwd]+t
  float upRef[3] = {};         // 49b5a8 — temporal up
  float bgScroll[2] = {};      // e684 accumulators (pre-wrap)
  std::int32_t liveObjects = 0, freeObjects = 0;
  std::uint64_t stateHash = 0; // FNV-1a over the sim fields
};

// --- deferred-stage seam counters --------------------------------------------
// FUN_0042c8b0 (the native frame) dispatches per-object updaters and a
// render pipeline that Phase 19A.2A does NOT implement. Rather than
// silently skipping them, the skeleton calls the real hook functions,
// whose bodies currently only bump these counters — tests verify call
// order/count, later phases fill the bodies in place.
struct StreamSeams {
  int heroUpdate = 0;     // FUN_0042d24c — owns winLo++/tunnelExtend feed
  int genericUpdate = 0;  // FUN_0042cf6c — +0x34 swim + anim tick
  int escortUpdate = 0;   // FUN_0042d034 — escort lane keeper
  int pickupUpdate = 0;   // FUN_0042d118 — pickup catch/proximity
  int strayUpdate = 0;    // FUN_0042db0c — edabc slot (never spawned in 19A.2A)
  int twinSync = 0;       // FUN_0042dabc — rescue-twin mirror pass
  int animStep = 0;       // FUN_004555bc family ticks inside updaters
  int backdrop = 0;       // FUN_0042e684 — toroidal scroll accumulate+blit
  int drawList = 0;       // FUN_0042e100 — back-to-front object draws
  int listener = 0;       // FUN_004026f8 — audio listener xform update
  int limiter = 0;        // FUN_0042fb68 — frame limiter wait
  int fillSelect = 0;     // FUN_0046ae60 — scanline filler mode select
  int paletteRamp = 0;    // init's 64-step DAC crossfade loop
  int resourceBind = 0;   // init MTI/BNI/HUD-table binds (host-side)
  int resourceFree = 0;   // teardown MTI/BNI/HUD-table frees
  int teletype = 0;       // FUN_0041cf5c script-queue clear (init)
};

// The stage tag sequence step() records into stepLog_ — the frame's
// control-flow spine minus the stages a branch skips (e.g. the twin
// gate only appears when entered; kExitMode is the terminal stage).
enum class StreamStage : std::uint8_t {
  kTick,         // 0x49b5a4++ — global frame counter
  kTwinGate,     // guarded rescue-twin spawn + completion gates
  kFade,         // fade accumulator + palette transform / exit emit
  kObjectWalk,   // slot scan +0x11c-stamped updater dispatch
  kTwinSync,     // twinSync() hook
  kCamera,       // eye blend + cameraAt
  kDraw,         // backdrop/draw/present seams + kPresent emit
  kLimiter,      // frame limiter wait
  kExit,         // fade-out terminal — kExitMode emitted, step ends
};

// --- the runtime ------------------------------------------------------------
class StreamScene {
public:
  StreamScene();                 // pool wipe + freelist link (the FUN_0042b270
                                 // head — 0x4f0740 memset + 0x540ed0 chain)

  // Init (FUN_0042b270). course = 0x541498 (>=4 -> final), skill =
  // 0x54147a (0/1/2 — counter seeds + drain formula), rng = the mode's
  // rand state (FUN_0047d2b5 seed stream — same as enemyRandNext).
  // health is the incoming 0x541554 value (native global).
  bool init(const StreamAssets& a, int course, int skill,
            std::uint32_t rng, int health);

  // One native tick (FUN_0042c8b0). dtSec = the 0x49b6f4 frame delta in
  // seconds (native uses the global frame dt — 1/30 nominal).
  // Returns false once the scene has signaled mode-exit (the native
  // returns 1 to the dispatcher; we emit kExitMode once).
  bool step(const StreamInput& in, float dtSec);

  void teardown();             // FUN_0042c824 — frees protos/objects,
                               // stops WIND, emits nothing visual
  bool finished() const { return exited_; }

  StreamSnapshot snapshot() const;

  const std::vector<StreamEvent>& events() const { return events_; }
  void clearEvents() { events_.clear(); }
  // Stage tags appended during the last step() call (cleared each
  // step) — the frame skeleton's recorded control-flow spine.
  const std::vector<StreamStage>& stepLog() const { return stepLog_; }
  const StreamSeams& seams() const { return seams_; }
  const StreamAssets& assets() const { return assets_; }

  // Test hooks (private-state access without friendship).
  const DynamicObject& objectAt(int i) const { return pool_[i]; }
  int bucketHead(int b) const;                 // pool index or -1
  int poolFreeCount() const;                   // freelist_ chain length
  int poolBucketCount(int b) const;            // buckets_[b] chain length
  int poolLinkIndex(const DynamicObject& o) const;  // +0x00 target | -1
  const float* nodeMat(int seg) const { return nodeMat_[seg & 0x1f]; }
  const float* ringPts(int seg) const { return ringPts_[seg & 0x1f]; }
  const float* planeSet(int seg) const { return planes_[seg & 0x1f]; }
  const std::uint8_t* penSet(int seg) const {
    return pens_[seg & 0x1f];
  }
  // Base 768B scene palette (e758) — the source kPaletteSet transforms.
  const std::uint8_t* palette() const { return palette_; }
  std::uint32_t& rng() { return rng_; }

private:
  // -- native helpers (0x42xxxx / 0x46xxxx ports; see .cpp comments) --
  static float streamFrndInt(float v);          // 47d59a — RC=11 FRNDINT
  static void euler6b180(float a3, float a2, float a1, float s,
                         const float t[3], float out[12]);
  static void euler6b2f8(float a1, float a2, float a3, float s,
                         const float t[3], float out[12]);
  static void compose6aeb0(const float A[12], const float B[12],
                           float out[12]);
  static void point6afe4(const float v[3], const float M[12],
                         float out[3]);
  static void normalizeE9c4(float v[3]);
  static void normalizeColsE978(float M[12]);
  static void sincos37f98(float deg, float* s, float* c);
  void pathPos(float out[3], float t) const;    // dc68
  void pathFrame(float out[12], float t0, float t1) const;  // dcf4
  void cameraAt(const float eye[3], float t);   // de28
  float wallProbe(const float pos[3], float t) const;  // d97c
  bool migrate(DynamicObject& o, int target);   // da40
  DynamicObject* alloc(int bucket, float t);    // bdc4
  void reap(DynamicObject& o);                  // c7b4
  DynamicObject* spawnDebris(float t, const void* vecHdr,
                             float speed);      // c578
  DynamicObject* spawnMarker(float t);          // c6f0
  void poolReset();                             // b270 head — wipe + link
  void tunnelExtend();                          // be4c
  void heroUpdate(float dt);                    // d24c
  void genericUpdate(DynamicObject& o, float dt);   // cf6c
  void escortUpdate(DynamicObject& o, float dt);    // d034
  void pickupUpdate(DynamicObject& o, float dt);    // d118
  void strayUpdate(DynamicObject& o, float dt);     // db0c (edabc slot)
  void twinSync();                              // dabc
  void animStep(DynamicObject& o, float dt);    // 555bc via objectAnimTickDt
  void backdropScroll();                        // e684
  void emitFrameDraw();                         // e684 + e100 + present
  void emit(StreamEvent::Kind kind, int tag, int aux, float f0) {
    StreamEvent ev;
    ev.kind = kind;
    ev.tag = tag;
    ev.aux = aux;
    ev.f[0] = f0;
    events_.push_back(ev);
  }
  int objIndex(const DynamicObject* o) const {
    return o ? static_cast<int>(o - pool_.data()) : -1;
  }
  DynamicObject* objAt(int i) {
    return i >= 0 ? &pool_[i] : nullptr;
  }

  // -- state block (0x4e74b0 layout) --
  std::int32_t winLo_ = 0, winHi_ = 0;          // e74b0/e74b4
  float ringPts_[kStreamSegs][kStreamRingPts * 3] = {};
  float planes_[kStreamSegs][kStreamPlanes * 4] = {};
  float nodeMat_[kStreamSegs][12] = {};
  std::uint8_t pens_[kStreamSegs][kStreamPens] = {};
  DynamicObject* buckets_[kStreamSegs] = {};    // e6b8 heads
  float drift_[3] = {};                         // e738/73c/740 (a3,a1,a2)
  float radius_ = 0.0f;                         // e744
  std::int32_t complete_ = 0;                   // e748
  std::int32_t penBase_ = 0, penTarget_ = 0;    // e74c/e750
  float penT_ = 0.0f;                           // e754
  std::uint8_t palette_[768] = {};              // e758 (+0xc0 PAL half)
  float fade_ = 0.0f;                           // ea9c
  DynamicObject* hero_ = nullptr;               // eab4
  DynamicObject* escort_ = nullptr;             // eab8
  DynamicObject* stray_ = nullptr;              // eabc (never spawned —
                                              // the db0c dispatch slot)
  DynamicObject* twin_ = nullptr;               // eac0 (rescue clone)
  DynamicObject* marker_ = nullptr;             // eac4 (end gate, final)
  DynamicObject* pickup_ = nullptr;             // eac8 (non-final)
  std::int32_t isFinal_ = 0;                    // ead0 (course >= 4)
  float driftMax_ = 0.0f;                       // ead4
  float radiusMin_ = 0.0f, radiusMax_ = 0.0f;   // ead8/eadc
  int windHandle_ = -1;                         // ea84 — WIND instance
  std::int32_t health_ = 0;                     // 541554
  std::int32_t skill_ = 0;                      // 54147a
  std::int32_t course_ = 0;                     // 541498
  // Camera/view globals.
  float upRef_[3] = {0.0f, 0.0f, -1.0f};        // 49b5a8 (init {0,0,-1})
  float camView_[12] = {};                      // 540bb0 world->view
  float camProj_[12] = {};                      // 540b80 proj-scaled view
  float camPos_[3] = {};                        // 540b28 eye
  float projScale_[2] = {};                     // 540bf0/540bf4
  float bgScroll_[2] = {};                      // e684 accumulators
  float lastLookT_ = 0.0f;                      // last cameraAt t arg
  // Proto/anim bindings (asset views).
  StreamAssets assets_;
  // Pool + freelist (0x4f0740 / 0x540ed0).
  std::array<DynamicObject, kStreamPoolSize> pool_{};
  DynamicObject* freelist_ = nullptr;
  std::uint32_t rng_ = 1;
  std::vector<StreamEvent> events_;
  // Diagnostic seams. poolErrorCalls_ counts the FUN_00408eb0 reports
  // ("No Aliens available in stream!" / "Alien not in list to be
  // freed") — the original prints+aborts; the port counts and returns.
  // quit_ records the alloc-failure DAT_0054148e = 1 write (the host
  // wires it to the mode dispatcher when the frame loop lands).
  int poolErrorCalls_ = 0;                      // FUN_00408eb0 seam
  bool quit_ = false;                           // DAT_0054148e
  bool exited_ = false;
  bool tornDown_ = false;
  int frameTick_ = 0;                           // 0x49b5a4 — +0x11c stamp source
  // d24c/dabc frame context — the native reads the resolved input axes
  // from the +0x4ce758/+0x4ce75c globals (written by the 407f2c fold
  // inside heroUpdate) and the frame delta from the 0x49b6f4 constant;
  // the port stages both at the head of step() so heroUpdate/twinSync
  // share the same frame values.
  StreamInput input_;
  float stepDt_ = 0.0f;
  StreamSeams seams_;                           // deferred-hook counters
  std::vector<StreamStage> stepLog_;            // per-step stage spine
};

} // namespace mdk
