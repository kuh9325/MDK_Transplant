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
#include <string>
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
  // The fuse-table bound dword for the FUN_004555bc 0x4edcc0 arm
  // (OBSERVED asm chain): `bound = *(u32*)(*(*(u32*)(rec0+0x10)) +
  // 0xc) >> 0x10` — the port flattens that triple deref; this field
  // carries the raw dword (the hi16 is the fuse frame bound, read by
  // SAR so the sign bit propagates). No mode-5 spawn produces a
  // class-table-bound object (OBSERVED: every spawn binds a deep copy
  // or null), so this stays 0 on real data — synthetic fixtures
  // supply the decoded dword.
  std::uint32_t animFuseBound = 0;
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
  // Phase 19A.2F — the two entries of the global HUD image table the
  // mode-5 HUD consumes (FUN_00418688(0x49a828) at 0x42b5ac binds an
  // ENGINE-side table — these are not STREAM records): slot 2 =
  // SC_STAT status icon (img 0x54b3e8, dims 0x54b44c = {w,h,w*h});
  // slot 7 = SNIP_TXT digit strip (img 0x54b3fc, dims 0x54b488 — the
  // record's w field is the strip's full row stride; drawn cells are
  // 8 px wide).
  int hudIconTag = -1, hudDigitTag = -1;
  int hudIconW = 0, hudIconH = 0;
  int hudDigitW = 0, hudDigitH = 0;
};

// --- per-frame input ------------------------------------------------------
// The native FUN_00407f2c folds key/analog state into globals
// 0x4ce758/0x4ce75c (+-180.0 digital, or -analog*const). The fold lives
// at the host; the runtime consumes the two resolved axes.
struct StreamInput {
  float axis0 = 0.0f;  // -> +0x4c roll steer (center 90, clamp [45,135])
  float axis1 = 0.0f;  // -> +0x13c pitch steer (center 0, clamp [-45,45])
  std::uint32_t nowMs = 0;  // host ms clock (FUN_0046c650) feeding the
                            // limiter (FUN_0042fb68 -> 42fcd0). 0 keeps
                            // the native first-call arm (0x49b700==0):
                            // the record re-initializes every step and
                            // the frame delta stays pinned at 1/30.
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
    kPlaySound,      // tag=sound tag (loop flag in aux). The
                     // FUN_004555bc sound-marker arm emits the
                     // name-bound form instead: tag=-1, `name` carries
                     // the +0x140 text the original resolves through
                     // FUN_00402fe8, f[0..2]=+0x10 pos, aux=0x1000e
                     // (the FUN_00402160 mode word; OBSERVED)
    kStopSound,      // tag=sound tag (WIND at teardown)
    kBackdropBlit,   // f[0]=scrollU f[1]=scrollV — the post-wrap i32
                     // accumulators 0x49b5fc/0x49b600; host does the
                     // 600x360 toroidal two-piece copy (e684)
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
    kSpriteDraw,     // f[0..3] = {sx, sy, size, vz} (sx/sy/size are
                     // post-trunc ints; vz = the view z'). tag = sprite
                     // tag (+0x108 -> lightTag/planetTag[0]); aux =
                     // pool index. f[4]/f[5] = the record's tag4/tag5
                     // fields — planetTag[1]/planetTag[2] for the
                     // marker (e55c), 0x40/0x40 otherwise (e49c).
                     // Emit order is the OBSERVED 09a00 flush order:
                     // flag-0 models first in chain order, flag-1
                     // sprites after — sorted DESCENDING by key
                     // vz*3.0f+8000.0f (far-first painter order; the
                     // 0x47dc00 qsort comparator 0x40bd2c returns
                     // keyB-keyA) under the 0x541500==1 deferred path,
                     // 0x499f88==0 key form.
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
    kTeletypeDraw,   // Phase 19A.2D — FUN_0041cb44 line draw. tag =
                     // renderer id (0 = FUN_00414d2c plain — the steady
                     // phase when 0x541548==0, 1 = FUN_0041518c scaled —
                     // slide/page transitions and the 541548 steady
                     // variant); aux = the post-FISTP y coord; f[0] =
                     // scale (the 41518c operand; 0 for the plain call,
                     // which takes none); name = the line text as a
                     // C-string at draw time.
    kHudBlit,        // Phase 19A.2F — FUN_004185fc transparent-keyed
                     // indexed blit into the 600x360 framebuffer
                     // (0x541650). tag = image tag; aux = source byte
                     // offset (digit cells = strip + digit*8);
                     // f[0..5] = {dstX, dstY, w, h, srcStride, key}
                     // (key = the transparent index, always 0 here).
  };
  Kind kind = kPresent;
  int tag = 0;
  int aux = 0;
  float f[12] = {};
  std::string name;          // sound/record name for name-bound events
                             // (empty for tag-bound forms)
};

// --- completion writer tags --------------------------------------------------
// Diagnostic-only reason tag for the 0x4ed748 latch — records WHICH of the
// three proven BUILD_A write sites first set it (the latch is monotone;
// later writes are idempotent no-ops). Not native state — the original
// keeps only the dword latch.
enum class StreamCompletion : std::uint8_t {
  kNone = 0,   // latch clear — no writer has fired
  kHero,       // 0x42ca55 — rescue twin (0x4edac0) +0xe4 > 0x50: the
               // BONESANIM dock sequence played out
  kWindow,     // 0x42ca79 — isFinal && FILD(0x4e74b4) >= 186.0
  kDeath,      // 0x42d95b — heroUpdate drain -> health<=0 (final only —
               // the "counter drained" write and the death write are the
               // SAME site; non-final clamps health to 1 instead)
};

// --- deterministic snapshot ------------------------------------------------
// Flat, allocation-free view of the simulation state for tests/digests.
// Native globals are named after their 0x4edxxx addresses where useful.
struct StreamSnapshot {
  std::int32_t winLo = 0;      // e74b0 — path window low
  std::int32_t winHi = 0;      // e74b4 — path window high
  std::int32_t complete = 0;   // e748
  StreamCompletion completionSrc = StreamCompletion::kNone;  // which
                               // writer (diagnostic only — not hashed)
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
  // Phase 19A.2D — TELETYPE service globals (0x54b7a4 block; all
  // OBSERVED). None of this mixes into stateHash — the canonical
  // traversal/freefall digests are unchanged; ttHash is the service's
  // own digest (FNV-1a over the whole 0x90-byte arena).
  std::uint32_t ttQRead = 0;        // 0x54b7fc ring read index
  std::uint32_t ttQWrite = 0;       // 0x54b800 ring write index
  std::uint32_t ttCurLine = 0;      // 0x54b7f0 — line count (1|2)
  std::uint32_t ttEntryFlags = 0;   // 0x54b7ec — current entry flags
  float ttCharTimer = 0.0f;         // 0x54b7f4 — hold countdown
  float ttHoldTimer = 0.0f;         // 0x54b7f8 — slide/page envelope
  std::uint64_t ttHash = 0;         // FNV-1a over the 0x90 arena bytes
  // Phase 19A.2F — draw-stage state (all diagnostic; none of it mixes
  // into stateHash — the canonical digests are unchanged):
  std::int32_t drawDue = 0;         // 0x5414d4 — limiter's draw gate
  std::int32_t hudBlink = 0;        // 0x49a8dc — HUD digit blink phase
  std::uint64_t paletteDacHash = 0; // FNV-1a over the 768B upload
                                    // surface (the ramped palette the
                                    // native hands FUN_0046d208)
  std::uint32_t limiter[6] = {};    // 0x49b6e4 record — {f32 mul,
                                    // i32 t1, i32 t2, f32 t3, f32 t4,
                                    // i32 t5} (t4 = the fade delta)
  std::uint32_t limiterBase = 0;    // 0x49b700 — ms window base
  std::uint32_t limiterTarget = 0;  // 0x49b704 — ms window target
  // Phase 19A.2H0 — the 0x49bbe8 install contents (diagnostic only,
  // not hashed): the FUN_0046ae60-selected projector variant.
  std::int32_t projectorSel = 0;    // 0 = 46ad20 .. 4 = 46ae1c
};

// --- frame-stage counters -----------------------------------------------------
// FUN_0042c8b0 (the native frame) dispatches per-object updaters and a
// render pipeline. Implemented stages (the updater family, the
// FUN_004555bc animator family, the TELETYPE queue service, the
// Phase 19A.2F draw stage, the 19A.2G ribbon engine, the limiter
// state machine and the ae60 projector install) count their dispatch
// reach; the remaining host-side stages (listener, paletteRamp,
// resourceBind/Free) are counted seams whose bodies only bump the
// counter — Phase 19B presentation fills those in.
struct StreamSeams {
  int heroUpdate = 0;     // FUN_0042d24c — owns winLo++/tunnelExtend feed
  int genericUpdate = 0;  // FUN_0042cf6c — +0x34 swim (no anim call —
                          // OBSERVED the one updater without)
  int escortUpdate = 0;   // FUN_0042d034 — escort lane keeper
  int pickupUpdate = 0;   // FUN_0042d118 — pickup catch/proximity
  int strayUpdate = 0;    // FUN_0042db0c — edabc slot (never spawned in 19A.2A)
  int twinSync = 0;       // FUN_0042dabc — rescue-twin mirror pass
  // FUN_004555bc animator family — IMPLEMENTED (Phase 19A.2C), these
  // are dispatch counters (which terminal body each tick took), not
  // deferred seams. The sum over the body counters == the calls.
  int animCalls = 0;      // total FUN_004555bc dispatches
  int animClassless = 0;  // +0x04==-1 -> FUN_00455500 tail
  int animFuse = 0;       // +0x0c==classRec0 (0x4edcc0) fuse arm
  int animFuseEnd = 0;    //   of those: non-loop fuse teardowns (5828c)
  int animHold = 0;       // latch resync / 0xff00 done-hold
  int animNull = 0;       // +0x114 null or bounded-fail record
  int animAdvance = 0;    // record advance -> FUN_00455890 apply
  int animSound = 0;      //   of those: +0x140/+0x144 marker consumed
  int backdrop = 0;       // FUN_0042e684 — toroidal scroll accumulate+blit
  int drawList = 0;       // FUN_0042e100 — back-to-front object draws
  int drawFlush = 0;      // FUN_00409a00 calls — one per bucket that
                          // appended at least one draw record (model
                          // or sprite — gated sprites still consume
                          // records, OBSERVED)
  int drawListOverflow = 0;  // appends past the 64-record stack arena
                             // (0xc00/0x30) — the native overflows its
                             // frame stack; the port bounds + counts
  int ribbonDraw = 0;     // FUN_0042e620 calls — 32 per drawn bucket
                          // (15x2 loop + 2 unrolled i=15 tail)
  int ribbonEmit = 0;     //   of those: kRibbonTri emitted (plane faces
                          //   the eye + 0ca00 gates passed)
  int ribbonPlaneCull = 0;//   e620's plane test: n.eye + d < 0 — the
                          //   backface skip, returns before 0ca00
  int ribbonGate = 0;     //   0ca00's 0x5414d4==0 draw-gate skip —
                          //   emitDrawList is itself gated on drawDue_,
                          //   reachable only when the fn runs outside
                          //   the frame path
  int ribbonReject = 0;   //   0ca00's trivial reject — the AND of the
                          //   three verts' clip-flag bytes != 0
  int ribbonClip = 0;     //   of the emitted: OR of the flag bytes != 0
                          //   (0ca00 takes the clipper; the port emits
                          //   the pre-clip tri, flags packed in aux)
  int trailUpdate = 0;    // FUN_0042be4c calls — path-history appends
                          // (ring/plane/pen regeneration per slot)
  int trailMax = 0;       // draw-window high water — max live
                          // winHi-winLo slots feeding e100
  int drawListMax = 0;    // 64-record (0xc00/0x30) arena high water
  int hudBlit = 0;        // FUN_004185fc calls — HUD icon + digits
  int listener = 0;       // FUN_004026f8 — audio listener xform update
  int limiter = 0;        // FUN_0042fb68 (frame) + 42fb30 (init) calls
  int fillSelect = 0;     // FUN_0046ae60 — the 0x49bbe8 projector
                          // install (EAX selects the variant; both
                          // mode-5 sites pass 0 -> FUN_0046ad20)
  int paletteRamp = 0;    // init's 64-step DAC crossfade loop
  int resourceBind = 0;   // init MTI/BNI/HUD-table binds (host-side)
  int resourceFree = 0;   // teardown MTI/BNI/HUD-table frees
  int teletype = 0;       // FUN_0041cf5c queue clear (init)
  // Phase 19A.2D — the TELETYPE queue service is IMPLEMENTED; these
  // count its real call sites/behavior (the OBSERVED bodies run —
  // nothing deferred). `teletype` above stays the init clear count.
  int teletypePost = 0;     // FUN_0041cad0 calls (incl. FTI-miss fails)
  int teletypeService = 0;  // FUN_0041cb44 calls (one per draw block;
                          // the 0x541544 stereo branch calls it twice
                          // per frame — unwritten in BUILD_A, untaken)
  int teletypeDraw = 0;     // FUN_00414d2c / FUN_0041518c emits
  int teletypeOverflow = 0; // consume line writes clipped at the
                          // 0x54b834 arena end — the native write is
                          // UNBOUNDED (past the queue block into BSS);
                          // the port keeps it inside the modeled arena
                          // and counts the clip
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

// --- per-tick animator trace -------------------------------------------------
// One entry per FUN_004555bc dispatch (animStep), appended in call order
// and cleared at the head of each step() — the diagnostic surface for
// "which animator body ran on which object". `rec` indexes the bound
// StreamAssets slots {0 escort, 1 bones, 2 kurt, 3 hvr, 4 wave} or -1.
// Post-tick object state (frame/acc/latch) is recorded — a fuse
// teardown therefore reports the wiped record (body 'T').
struct StreamAnimTick {
  int slot = -1;            // pool index of the object
  char body = '?';          // C classless | F fuse | T fuse-teardown |
                            // H latch/done hold | N null record |
                            // A record advance | S advance+sound
  int rec = -1;             // bound anim slot 0..4, or -1
  std::int16_t frame = 0;   // +0xe4 after the body
  float acc = 0.0f;         // +0xdc after the body
  std::int16_t latch = 0;   // +0x118 after the body
  std::uint8_t flags148 = 0;// loop bit lives at &0x8
};

// --- per-call TELETYPE service trace ----------------------------------------
// One entry per FUN_0041cb44 call, appended in call order and cleared
// at the head of each step() — the diagnostic surface for the queue
// service. There is no script PC/opcode in BUILD_A (OBSERVED — the
// "script" is a posted C-string; the only per-entry program state is
// the live str cursor the consume loop mutates, surfaced here as
// `cursor` packed (slot<<20)|byteOffset).
struct StreamTtTick {
  char phase = 'I';       // arm taken this call: 'I' idle | 'C' entry
                          // load+consume | 'S' steady hold | 'N'
                          // slide-in (entryFlags&1, hold<0.5) | 'P'
                          // page-out (hold != 0 after charTimer 0)
  int qRead = 0, qWrite = 0;          // post-call ring indices
  int curLine = 0;                    // 0x54b7f0 post-call
  std::uint32_t flags = 0;            // 0x54b7ec post-call
  float charTimer = 0.0f;             // 0x54b7f4 post-call
  float holdTimer = 0.0f;             // 0x54b7f8 post-call
  std::uint32_t cursor = 0;           // queue[slot].str post-call
  int chars = 0;                      // source bytes consumed (phase C)
  int draws = 0;                      // 414d2c/41518c emits this call
  int overflow = 0;                   // arena-clipped writes this call
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
  // Which proven writer latched complete_ (diagnostic-only — BUILD_A
  // stores only the dword). kNone while complete_ == 0 or when the
  // latch was forced from outside the three write sites.
  StreamCompletion completionReason() const { return completionSrc_; }

  StreamSnapshot snapshot() const;

  const std::vector<StreamEvent>& events() const { return events_; }
  void clearEvents() { events_.clear(); }
  // Stage tags appended during the last step() call (cleared each
  // step) — the frame skeleton's recorded control-flow spine.
  const std::vector<StreamStage>& stepLog() const { return stepLog_; }
  // Per-tick animator trace (cleared at the head of each step).
  const std::vector<StreamAnimTick>& animLog() const { return animLog_; }
  // Per-call TELETYPE service trace (cleared at the head of each step;
  // one record per FUN_0041cb44 call in the draw block).
  const std::vector<StreamTtTick>& ttLog() const { return ttLog_; }
  // Phase 19A.2D — TELETYPE queue post (FUN_0041cad0). The native
  // resolves a NAME through FTI (FUN_00414890) and stores the resolved
  // char*; the FTI table is process-global host state, so the port
  // takes the resolved text — text==nullptr models the resolve miss
  // and returns false (0). flags bits OBSERVED: 0x1 = slide-in intro
  // (holdTimer ramps 0->0.5 before the steady hold, then pages out),
  // 0x2 = front-push (qRead decremented BEFORE the resolve — a failed
  // front-push still keeps the decrement, OBSERVED quirk). rate =
  // seconds on screen. The 4-deep ring has NO full check — posting
  // into a full ring silently overwrites the unread tail.
  bool teletypePost(const char* text, std::uint32_t flags, float rate);
  const StreamSeams& seams() const { return seams_; }
  const StreamAssets& assets() const { return assets_; }
  // DAT_004edcc0 — the shared class-table record-0 identity the
  // FUN_004555bc dispatch compares +0x0c against. A stable sentinel —
  // never dereferenced (the fuse arm reads the bound through
  // assets_.animFuseBound instead). Tests bind col.elements to it.
  static const CollisionElementSet* classRec0();

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
  // The ramped 768B the frame's fade stage hands the DAC upload
  // (FUN_0046d208) — the paletteDacHash snapshot field digests it.
  const std::uint8_t* paletteDac() const { return paletteDac_; }
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
  // FUN_004555bc — the shared per-object animator dispatch. Order
  // (OBSERVED asm): +0x04==-1 classless -> +0x0c==classRec0 fuse ->
  // the ordinary record driver (objectAnimTickDt).
  void animStep(DynamicObject& o, float dt);
  void logAnimTick(const DynamicObject& o, char body);
  void paletteRamp(int mode);                   // fill paletteDac_ per
                                                // the OBSERVED fade arms
  void project6b4f8(const float v[3], float out[6]) const;
                                                // {x',y',z',sx,sy,flags}
                                                // — point x camProj_ +
                                                // clip flags + fill fn
  void fillSelect(int mode);                    // FUN_0046ae60 — the
                                                // 0x49bbe8 install
  void backdropScroll();                        // e684
  void emitDrawList();                          // e100 — records + flush
  // The 32x FUN_0042e620 band for slot `seg` — a = the higher slot's
  // projected ring records (native bufA), b = this bucket's (bufB);
  // penBase = the distance-banded negative material index.
  void ribbonEmit(std::uint32_t seg,
                  const float a[kStreamRingPts][6],
                  const float b[kStreamRingPts][6], int penBase);
  void emitHud();                               // 17e20
  void hudBlit(int tag, int srcOff, float x, float y,
               float w, float h, float srcStride);  // 185fc seam
  void limiterRun(std::uint32_t nowMs);         // 2fb68 -> 2fcd0
  void limiterInit();                           // 2fb30 — table reset
  void emitFrameDraw();                         // e684 + e100 + 1cb44 +
                                                // 17e20 + present
  // -- TELETYPE queue service (Phase 19A.2D — OBSERVED 0x41cxxx) ------
  // The native owns the flat block 0x54b7a4..0x54b834: two 36-byte
  // line buffers, four scalars, the ring indices and the 4x0x0c queue
  // records — one contiguous region, which the port keeps verbatim as
  // `ttMem_` so the consume loop's unbounded line write aliases the
  // trailing globals/queue exactly like the original. Layout:
  //   +0x00 lineBuf[0][36]          +0x24 lineBuf[1][36]
  //   +0x48 entryFlags u32          +0x4c curLine u32
  //   +0x50 charTimer f32           +0x54 holdTimer f32
  //   +0x58 qRead u32               +0x5c qWrite u32
  //   +0x60 queue[4] {f32 rate; u32 flags; u32 strCursor}
  // The str cursor packs (textSlot<<20)|byteOffset — the native's raw
  // char* cannot be reproduced, so resolved payloads live in the
  // ttText_ side table and a corrupt/out-of-range cursor reads as the
  // terminating 0 (the native would chase the wild pointer — the one
  // place the port bounds rather than emulates, counted).
  void teletypeClear();                   // 1cf5c — init clear
  void teletypeService(int drawEnable);   // 1cb44 — arg = 5414d4
  static std::uint32_t ttLd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
  }
  static void ttSt32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
  }
  static constexpr int kTtFlags = 0x48, kTtLine = 0x4c,
                       kTtChar = 0x50, kTtHold = 0x54,
                       kTtQRead = 0x58, kTtQWrite = 0x5c,
                       kTtQueue = 0x60;
  std::uint32_t ttU32(int off) const { return ttLd32(&ttMem_[off]); }
  void ttSetU32(int off, std::uint32_t v) { ttSt32(&ttMem_[off], v); }
  float ttF32(int off) const {
    return std::bit_cast<float>(ttLd32(&ttMem_[off]));
  }
  void ttSetF32(int off, float v) {
    ttSt32(&ttMem_[off], std::bit_cast<std::uint32_t>(v));
  }
  // Line write — bounded at the arena end (0x54b834); past it the
  // native corrupts BSS, the port clips and counts.
  void ttPutByte(unsigned pos, std::uint8_t v) {
    if (pos < ttMem_.size()) ttMem_[pos] = v;
    else ++seams_.teletypeOverflow, ++ttTickOv_;
  }
  // One byte of the consume loop's str cursor (packed u32).
  std::uint8_t ttStrByte(std::uint32_t cursor) const;
  std::string ttLineStr(int line) const;    // C-string at line*36
  void ttDraw(int renderer, int line, int y, float scale);

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
  StreamCompletion completionSrc_ =             // port-side diagnostic:
      StreamCompletion::kNone;                  //   first writer to latch
  std::int32_t penBase_ = 0, penTarget_ = 0;    // e74c/e750
  float penT_ = 0.0f;                           // e754
  std::uint8_t palette_[768] = {};              // e758 (+0xc0 PAL half)
  std::uint8_t paletteDac_[768] = {};           // the ramped 768B the
                                                // native hands
                                                // FUN_0046d208
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
  float camViewPrev_[12] = {};                  // 49b604 — the saved
                                                // pre-update copy e684
                                                // diffs against (init'd
                                                // at 0x42b4d0)
  float camProj_[12] = {};                      // 540b80 proj-scaled view
  float camPos_[3] = {};                        // 540b28 eye
  float projScale_[2] = {};                     // 540bf0/540bf4
  // 0x540b68/0x540b58 — sprite-size numerator (i32 600) and the 2.4f
  // zoom denominator the camera writes every frame (2de28:4e..5a).
  int spriteSizeNum_ = 600;
  float projSizeF_ = 2.4f;
  std::int32_t bgScroll_[2] = {};               // e684 accumulators —
                                                // [0]=U 0x49b5fc,
                                                // [1]=V 0x49b600 (the
                                                // post-wrap toroidal
                                                // i32s)
  float lastLookT_ = 0.0f;                      // last cameraAt t arg
  // Phase 19A.2F draw-stage globals.
  std::int32_t drawDue_ = 1;                    // 0x5414d4 — draw gate.
                                                // Mode-5 init leaves it
                                                // untouched (no write in
                                                // 42b270's span); BSS
                                                // cold-boot is 0 but the
                                                // dispatcher's 4346e8
                                                // inits set it first —
                                                // the established harness
                                                // contract arms it.
  std::int32_t hudBlink_ = 0;                   // 0x49a8dc
  // The 0x49bbe8 projector slot — FUN_0046ae60's write target, modeled
  // as the installed variant index: 0 = FUN_0046ad20 (full view),
  // 1..4 = FUN_0046ad58/9c/e0/46ae1c (sub-window variants). Both
  // mode-5 ae60 sites pass 0, so the runtime value is always 0.
  int projectorSel_ = 0;
  // Limiter (FUN_0042fb68 path). limiter_[6] = the 0x49b6e4 tick record
  // {mul f32, t1 divisor, t2 tick-delta, t3 EMA, t4 = t3*(1/30) fade
  // delta, t5 remainder}; base_/target_ = 0x49b700/0x49b704 ms window;
  // lastMs_ = the 46c650 clock sample 2fcd0 consumes; limSpan_ counts
  // the draw-off frames the 2fb68 wrapper observes (the accum+50<budget
  // stall accounting); limInSeq_/limTick_ are the 42fcc0 armed-timing
  // bookkeeping the wrapper adds.
  std::uint32_t limiter_[6] = {};
  std::uint32_t limiterBase_ = 0, limiterTarget_ = 0;
  std::uint32_t lastMs_ = 0;
  int limSpan_ = 0, limInSeq_ = 0, limTick_ = 0;
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
  StreamSeams seams_;                           // call census + deferred-
                                                // hook counters
  std::vector<StreamStage> stepLog_;            // per-step stage spine
  std::vector<StreamAnimTick> animLog_;         // per-step 555bc trace
  // TELETYPE service state (0x54b7a4..0x54b834 flat block — OBSERVED
  // contiguous in BUILD_A). ttMem_ is byte-addressed so the consume
  // loop's unbounded line write aliases the trailing scalars and queue
  // entries exactly like the original; ttText_ holds the resolved
  // payload each slot's str cursor indexes (the native stores a raw
  // char* into FTI memory).
  std::array<std::uint8_t, 0x90> ttMem_{};      // 0x54b7a4..0x54b834
  std::array<std::string, 4> ttText_{};         // posted payloads
  std::vector<StreamTtTick> ttLog_;             // per-call trace
  int ttTickDraws_ = 0, ttTickOv_ = 0;          // per-call accumulators
  // The service's two suppress/draw gates: 0x4999d0 is the pause/debug
  // word, 0x541548 has no writer anywhere in BUILD_A (stays 0 — both
  // gate paths live but untaken). Staged as fields so synthetic tests
  // can exercise the gated arm.
  std::int32_t flag4999d0_ = 0;                 // 0x4999d0 pause word
  std::int32_t flag541548_ = 0;                 // 0x541548 (no writers)
};

} // namespace mdk
