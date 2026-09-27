// Phase 16A — Kurt/player sprite animation (FUN_00461954 + helpers).
//
// OBSERVED (BUILD_A disassembly + real data):
//
//   The original drives Kurt's animation from the depth-sorted draw
//   callback FUN_00461954 (registered by FUN_00431300): it is BOTH the
//   animation state machine AND the draw site. Each frame it clears
//   the overlay pointer (0x54cb18), snapshots 0x540cb4 into a local
//   shadow, dispatches on the locomotion state 0x540cac, writes the
//   selected sprite-frame pointer to 0x54cb14 (plus an optional
//   muzzle-flash overlay frame at 0x54cb18 with jitter offsets
//   0x54cb0c/0x54cb10), then latches 0x540cb0 = 0x540cac ("previous
//   state" — the first-frame detect) and hands the frame pair to the
//   sprite blitter.
//
//   Sprite animation tables are name-bound at traversal context init
//   (FUN_0046445c): 23 records out of TRAVSPRT.BNI via FUN_004039ec
//   (payload+4) plus 6 slide/surf records out of the level's
//   LEVEL<n>S.SNI sprite-name bank via FUN_004289a0
//   (imageBase + record[+0x10] + 4; that bank is built by
//   FUN_0042891c from records flagged +0x0D & 0x80 — the parser's
//   "sentinel" class, all six are K_* records).
//
//   Record layout (OBSERVED on real bytes, identical to the FTI
//   sprite records fti_sprite.h already decodes):
//     +0x00 u32 block byte count (skipped by the +4 bind)
//     +0x04 u32 frame count N
//     +0x08 u32 offsets[N], each relative to the +0x04 field
//     frame: +0 u16 w, +2 u16 h, +4 s16 hotX, +6 s16 hotY, +8 stream
//
//   Timing (OBSERVED):
//     - FUN_00464278 integer advance: cb4 += 0x49b6e8 (frameStep,
//       [1,4]) wrapping cb4=0 at >= count (dir>0) or cb4 -= step,
//       wrapping to count-1 at <0 (dir<=0). First frame resets
//       cb4/shadow to 0.
//     - FUN_00464308 float phase advance: cb8 += 0x49b6f0 (smoothed)
//       * rate(1.5*moveVel) — piecewise-linear rate with gear
//       thresholds at moveVel +-2/3 and nonzero floors +-0.25 at
//       standstill; wraps once at [0,count); cb4 = rint(cb8)
//       (round-to-nearest, FUN_0047d59a + FISTP).
//     - FUN_004375f4 frame-crossing test (wrap-aware): forward
//       interval (prev,curr], reverse [curr,prev) — used for footstep
//       and action triggers.
//     - Jump states index frames by vertVel against constant
//       threshold tables; look/turn index by pitch/yaw; mantle and
//       crash run at half rate (cb4/2).
//
// NATIVE PORT DECISIONS:
//   - The machine owns the SAME globals the original owns: locoState,
//     animPrev, animFrame, animPhase, eventPriority, the frame pair,
//     the muzzle index, the foot-alternate flag, plus the handler
//     side-effects the original performs (camera/HUD writes, sounds,
//     the 0x320 mantle root-motion nudge, the 0x325 interact trigger).
//   - The draw call itself (FUN_00409724 sprite blit) stays deferred;
//     the selected frame pointers + offsets + visibility gate are
//     surfaced on the runtime for the presentation layer.
//   - Missing tables (tests/ports without the banks) degrade to the
//     same "draw whatever cb14 holds" state the original would have
//     with a null record — the machine still advances/latches so
//     gameplay-visible writes (locoState, eventPriority, position)
//     stay authoritative.

#ifndef MDK_CORE_PLAYER_ANIMATION_H
#define MDK_CORE_PLAYER_ANIMATION_H

#include <cstddef>
#include <cstdint>

namespace mdk {

struct TraversalRuntime;

// The 29 name-bound sprite tables, in original slot order
// (0x49b9a8..0x49ba18). The first 23 resolve from TRAVSPRT.BNI, the
// trailing 6 (bSlide/fSlide/slide/slip/surf/surfJ) from the level's
// LEVEL<n>S.SNI bank — null when the record is absent (real levels
// only carry the records they need, e.g. LEVEL6S.SNI has the four
// slide records and no surf pair).
struct PlayerAnimTables {
  const std::byte* bang = nullptr;   // 0x49b9a8 K_BANG   — 0x385/0x3ea
  const std::byte* bFlip = nullptr;  // 0x49b9ac K_BFLIP  — 0x385 tail
  const std::byte* bSlide = nullptr; // 0x49b9b0 K_BSLIDE — 0x32a (SNI)
  const std::byte* chute = nullptr;  // 0x49b9b4 K_CHUTE  — 0x2bd
  const std::byte* chuteC = nullptr; // 0x49b9b8 K_CHUTEC — 0x2bd sustain
  const std::byte* crashL = nullptr; // 0x49b9bc K_CRASHL — 0x326
  const std::byte* fall = nullptr;   // 0x49b9c0 K_FALL   — 0x2bc
  const std::byte* floatC = nullptr; // 0x49b9c4 K_FLOATC — 0x3e9
  const std::byte* fSlide = nullptr; // 0x49b9c8 K_FSLIDE — 0x329 (SNI)
  const std::byte* hang = nullptr;   // 0x49b9cc K_HANG   — 0x320 mantle
  const std::byte* idle = nullptr;   // 0x49b9d0 K_IDLE   — 0x65
  const std::byte* jump = nullptr;   // 0x49b9d4 K_JUMP   — 0x2be
  const std::byte* land = nullptr;   // 0x49b9d8 K_LAND   — 0xc8
  const std::byte* lookD = nullptr;  // 0x49b9dc K_LOOKD  — 0x324 pitch>0
  const std::byte* lookU = nullptr;  // 0x49b9e0 K_LOOKU  — 0x324 pitch<0
  const std::byte* muzzF = nullptr;  // 0x49b9e4 K_MUZZF  — overlay
  const std::byte* rjmp = nullptr;   // 0x49b9e8 K_RJMP   — 0x2bf (+4 ofs)
  const std::byte* run = nullptr;    // 0x49b9ec K_RUN    — 0x258
  const std::byte* runFir = nullptr; // 0x49b9f0 K_RUNFIR — 0x259
  const std::byte* shot = nullptr;   // 0x49b9f4 K_SHOT   — 0x12c
  const std::byte* side = nullptr;   // 0x49b9f8 K_SIDE   — 0x1f4
  const std::byte* slide = nullptr;  // 0x49b9fc K_SLIDE  — 0x328 (SNI)
  const std::byte* slip = nullptr;   // 0x49ba00 K_SLIP   — 0x327 (SNI)
  const std::byte* spewP = nullptr;  // 0x49ba04 K_SPWEP  — 0x325 action
  const std::byte* still = nullptr;  // 0x49ba08 K_STILL  — 0x64/0x384
  const std::byte* surf = nullptr;   // 0x49ba0c K_SURF   — 0xc9 (SNI)
  const std::byte* surfJ = nullptr;  // 0x49ba10 K_SURFJ  — 0x321 (SNI)
  const std::byte* takeOf = nullptr; // 0x49ba14 K_TAKEOF — 0x3e8
  const std::byte* trn45 = nullptr;  // 0x49ba18 K_TRN45  — 0x190
};

// Per-frame inputs the machine reads from outside TraversalRuntime
// (the timing block + the live jump-held input the draw callback
// reads directly, plus the current arena scalar for the look bounds).
struct PlayerAnimEnvironment {
  int frameStep = 1;              // 0x49b6e8
  float smoothed = 1.0f;          // 0x49b6f0
  float deltaSec = 1.0f / 30.0f;  // 0x49b6f4
  float arenaScalar = 0.0f;       // current arena +0x462
  std::uint32_t jumpHeld = 0;     // 0x4ce768 — the 0x2bc land gate
};

// Sprite-record accessors over the payload+4 table image
// ({count, ofs[N], frames}; offsets relative to the count field).
std::uint32_t playerAnimRecCount(const std::byte* rec);
const std::byte* playerAnimRecFrame(const std::byte* rec, int index);

// FUN_0046445c — bind the 29 tables from the loaded TRAVSPRT.BNI and
// LEVEL<n>S.SNI images (rt.level.travsprtBytes / rt.level.sniBytes).
// Leaves any unresolvable table null. Called once per level load.
void playerAnimBindTables(TraversalRuntime& rt);

// FUN_004375f4 — wrap-aware frame-crossing test. dir>0: forward
// interval (prev,curr] (+wrap); dir<=0: reverse [curr,prev) (+wrap).
bool playerAnimFrameCrossed(int trig, int dir, int prev, int curr);

// FUN_00461954 — one traversal-frame animation tick. Applies the
// original gates, runs the locoState dispatch (frame select +
// side-effects), latches animPrev and produces the frame pair on
// rt.animMainFrame/rt.animOverlayFrame (+ jitter offsets + the
// visibility gate). Called from the world-tick body where the
// original's draw callback ran.
void playerAnimTick(TraversalRuntime& rt,
                    const PlayerAnimEnvironment& env);

// Phase 16B — FUN_00431300's player-entry registration: the
// projection half the display-list entry performs before the
// FUN_00461954 callback is queued (OBSERVED, BUILD_A disasm
// 0x4313ad..0x4314b4):
//
//   FUN_0046b4f8(0x540b80, 0x540bfc, &c14) — transform cs.pos by the
//     M1 projection-folded matrix into the {x',y',z',sx,sy,clip}
//     record, invoking the installed projector. The clip flags
//     classify x'/y' against +-z'; z' < 0.05 (f64 0x498e34) forces
//     sx=sy=0 with flag 0x10.
//   if (0x540da0 == 0): c4c = rint(sx), c50 = rint(sy) — the blit
//     anchor ints (FRNDINT + FISTP). da0 skips ONLY this write.
//   always (when registered): a second FUN_0046b4f8 of the point
//     pos+(0,0,1) yields sy2; 0x540dbc = rint(sy2 - c50) — the
//     pixels-per-unit scale the scope HUD consumes.
//
//   The projector itself is the installable function pointer
//   0x49bbe8 (FUN_0046ae60(mode)); BUILD_A traversal uses mode 0
//   (FUN_0046ad20): sx = ((x'+z')/z')*299.95 + 0.05,
//   sy = ((y'+z')/z')*180.4 + 0.05 — the alternates (sniper/alt
//   viewports, modes 1..4) install from paths the runtime does not
//   yet implement, so this port projects mode 0 only.
//
//   Registration itself is gated the same way the callback
//   registration is (c9c==0 || ca0==0) && (e6c==0 || !(e70&0x20)) —
//   under the gate the writes simply do not happen and the stale
//   values persist. This mirrors playerAnimTick's own head gate.
void playerAnimRegistration(TraversalRuntime& rt);

// Trace helper — maps a selected frame pointer back to
// {table index 0..28, frame index} for deterministic traces.
// Returns false when the pointer is not inside a bound table frame
// (null tables, stale pointers).
bool playerAnimFrameIdentity(const TraversalRuntime& rt,
                             const std::byte* frame,
                             int* tableIndex, int* frameIndex);

// Table display names, in slot order (for traces/tests).
const char* playerAnimTableName(int tableIndex);

} // namespace mdk

#endif // MDK_CORE_PLAYER_ANIMATION_H
