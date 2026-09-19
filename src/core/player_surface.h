// Phase 5F — surface contact effects: the collision-contact dispatcher
// (FUN_0040b5d0), the per-object surface tables it consumes, the flag-op
// helpers (FUN_0040a704 / FUN_0040b4dc), the surface/volume "fan" record
// list (+0x45e) with its conveyor (FUN_00412ef0) and updraft
// (FUN_00412e94 / FUN_00412f84) consumers, and the type-9 slide-zone
// trigger behind tr_alcmd opcode 0xe0.
//
// Evidence basis (all OBSERVED unless noted):
//   callback 0x4635e0  = a PUSH-registered sweep callback that invokes
//                        FUN_0040b5d0(EAX=ctx, EDX=poly, EBX=0, ECX=8,
//                                     stack{-0xb, vecA, posB, c08}).
//   FUN_0040b5d0       = per-contact surface dispatcher. Reads the poly's
//                        surface byte (+0x23) as a nonzero gate and the
//                        high byte of +0x20 as the 1-based surface id;
//                        indexes 16 per-object tables in the collision
//                        object; runs flag ops / counters; invokes a
//                        CMI-relative script handler via FUN_004546ac.
//   FUN_004546ac       = builds a synthetic 0x32e alien object and runs
//                        the tr_alcmd VM on (CMI base + handlerOff). NOT a
//                        native surface executor — the handler path is
//                        script-defined, so it is a seam here.
//   +0x45e records     = the "fan" record list. kind<0 = surface-bound
//                        conveyor/UV-scroll; kind 1..6 = volume updraft.
//   opcode 0xe0        = type-9 DTI volume scan -> DAT_00540e28 (bounce
//                        flag) + slide redirect / airborne down-slam.
//
// Boundary: this file implements the bounded mechanisms proven in the
// disassembly. It does NOT implement the tr_alcmd VM, AI, rendering,
// audio, or the slide locomotion channel — the script handler and the
// slide-mode write are exposed as hooks/out-params only.

#pragma once

#include <cstdint>

#include "core/collision_query.h"
#include "core/dti_structure.h"

namespace mdk {

// Sixteen surface slots — (poly+0x20 >> 24) - 1 indexes [0, 16).
inline constexpr int kSurfaceSlots = 16;

struct SurfaceObjectState;

// Script-handler fx globals (0x4a2438 delta / 0x4a2448 vec). Written on
// every handler invoke so the CMI script can read the contact geometry.
struct SurfaceFxState {
  float delta[3] = {};  // posB - contactPos
  float vec[3] = {};    // vecA (the sweep's working/contact position)
};

// The FUN_004546ac seam — a CMI-relative handler script. The native port
// does not run the tr_alcmd VM; this hook receives the invoke so a
// bounded adapter can service proven effects. `result` carries the
// dispatcher's accumulating result byte (bit0 = invoked, bit1 = cfg&0x20)
// and may be modified by the hook exactly like the VM's CL return.
using SurfaceScriptFn = void (*)(SurfaceObjectState& ctx,
                                 std::uint32_t handlerOff,
                                 std::int32_t eventCode,
                                 std::uint8_t& result,
                                 const SurfaceFxState& fx, void* user);

// ---------------------------------------------------------------------------
// Polygon flag operations — FUN_0040a704 (OBSERVED)
// ---------------------------------------------------------------------------
// poly+0x20 packs {surfaceId << 24 | flags}. The op codes toggle the
// 0x10/0x20/0x30 state bits on every poly whose high byte matches a
// 1-based surface id. Bit 0x20 is also the sweep's skip bit; bit 0x10 is
// the per-frame "armed" bit the 0x80 contact effect clears/re-arms.
enum SurfaceFlagOp : int {
  kSurfOpSet30 = 0,    // |= 0x30
  kSurfOpClear30 = 1,  // &= ~0x30
  kSurfOpSet10 = 2,    // |= 0x10
  kSurfOpClear10 = 3,  // &= ~0x10
  kSurfOpSet20 = 4,    // |= 0x20
  kSurfOpClear20 = 5,  // &= ~0x20
};

// FUN_0040a704 — apply `op` to every poly in [polys, polys+count) whose
// +0x20 high byte == surfId (1-based, must be in [1, 0xff]).
void surfacePolyOp(CollisionPoly* polys, std::int32_t count,
                   std::uint32_t surfId, int op);

// ---------------------------------------------------------------------------
// Per-object surface state — the collision object's +0x6c..+0x114 block
// ---------------------------------------------------------------------------
// These tables are written by tr_alcmd surface opcodes (OBSERVED):
//   opcode 0x62 {surfId, op}      -> polyOp + opMaskA/B updates
//   opcode 0x63 {mask,surfId,off} -> handlerMask + handlerOff
//   opcode 0xa8 {surfId, mask}    -> config (+ polyOp set-0x10 if 0x80)
//   counter setter {surfId,u16}   -> counters
// At load the block is zeroed (the arena record is memset).
struct SurfaceObjectState {
  std::uint8_t config[kSurfaceSlots] = {};       // +0x6c — context-enable
                                                 // low bits + fx
                                                 // 0x80/0x40/0x20
  std::uint8_t handlerMask[kSurfaceSlots] = {};  // +0x7c — invoke context
  std::uint32_t handlerOff[kSurfaceSlots] = {};  // +0x8c — CMI-relative
  std::int32_t counters[kSurfaceSlots] = {};     // +0xcc
  std::uint32_t opMaskA = 0;                     // +0x10c — persistent set
  std::uint32_t opMaskB = 0;                     // +0x110 — persistent set
  std::uint32_t marks = 0;                       // +0x114 — pending bits
                                                 // (bit i = surfId i)
  CollisionPoly* polys = nullptr;                // +0x28 — mutable poly
                                                 // table (flag ops write)
  std::int32_t polyCount = 0;                    // +0x10
  struct SurfaceRecord* records = nullptr;       // +0x45e — record list
  // The collision object's FUN_004546ac seam (the CMI-relative handler
  // scripts at +0x8c). Not part of the original record — a native port
  // convenience so the contact hook can route invokes.
  SurfaceScriptFn scriptFn = nullptr;
  void* scriptUser = nullptr;
};

// ---------------------------------------------------------------------------
// The +0x45e "fan" record — 0x48 bytes (OBSERVED)
// ---------------------------------------------------------------------------
// kind < 0 (-1): surface-bound record — scrolls matching-surface poly UVs
//   and contributes conveyor displacement via FUN_00412ef0.
//   v[0..2] = normalized 3D direction, v[3..4] = UV/planar dir,
//   v[5] = normalization factor, surfType = the +0x20>>24 surface id.
// kind 1..6: volume/ribbon record — an AABB (v[0..5] = minx..maxz) plus a
//   falloff shape; FUN_00412f84 eases an output vec.z toward rate*shape(t)
//   for each volume containing the query point.
struct SurfaceRecord {
  SurfaceRecord* next = nullptr;   // +0x00
  std::uint32_t owner = 0;         // +0x04
  std::uint32_t name = 0;          // +0x08 — script-facing key
  std::uint8_t surfType = 0;       // +0x0c — surface id (surface kind)
  std::uint32_t f10 = 0;           // +0x10 — volume: no-falloff flag
  std::int32_t kind = -1;          // +0x14 — -1 surface; 1..6 volume
  float rate = 0.0f;               // +0x18 — current rate
  std::uint32_t queryMask = 0;     // +0x1c — volume query mask (0 = all)
  float v[6] = {};                 // +0x20..0x37 — dir/uv | box (per kind)
  float uvAcc[2] = {};             // +0x38/0x3c — surface UV accumulators
  float target = 0.0f;             // +0x40 — ramp target rate
  float ramp = 0.0f;               // +0x44 — per-frame ramp delta
};

// Surface-record creation — FUN_00413380 (surface conveyor) and
// FUN_00412e10 (volume). Both push onto ctx.records and normalize the
// direction (surface) / copy the box (volume).
SurfaceRecord* surfaceRecordCreate(SurfaceObjectState& ctx,
                                   std::uint32_t surfType,
                                   const float dir[3], float rate);
SurfaceRecord* surfaceVolumeCreate(SurfaceObjectState& ctx, int shape,
                                   const float box[6], float rate,
                                   std::uint32_t queryMask);

// Free the intrusive record list (the original uses a fixed static pool;
// the native port heap-allocates so hosts must release it).
void surfaceRecordsDestroy(SurfaceObjectState& ctx);

// ---------------------------------------------------------------------------
// The contact dispatcher — FUN_0040b5d0 (OBSERVED)
// ---------------------------------------------------------------------------

// FUN_0040b5d0 — run the surface dispatch for one contact.
//   ctx        the collision object's surface state (EAX)
//   secondary  additive flag input (EBX; 0 from the sweep callback)
//   contextMask channel selector (ECX; 8 for the player sweep callback)
//   poly       the contacted poly (EDX)
//   eventCode  script event arg (stack [0x8]; -0xb from the callback)
//   vecA       sweep working/contact position (stack [0xc] = 0x4a20c0)
//   posB       player position (stack [0x10] = 0x540bfc)
//   contactPos entry/snapshot position (stack [0x14] = 0x540c08)
// Returns the result byte (bit0 = handler invoked, bit1 = cfg&0x20 fx).
std::uint8_t surfaceDispatch(SurfaceObjectState& ctx,
                             std::int32_t secondary,
                             std::uint8_t contextMask,
                             CollisionPoly* poly, std::int32_t eventCode,
                             const float vecA[3], const float posB[3],
                             const float contactPos[3],
                             SurfaceFxState& fx, SurfaceScriptFn script,
                             void* user);

// FUN_0040b4dc — apply the pending/persistent surface ops.
//   mode 0: for each bit set in marks -> polyOp(set-0x10); clears marks
//           (the per-frame re-arm pass).
//   mode 1: set 0x30 on polys flagged &2; then per bit in opMaskA|opMaskB
//           -> polyOp(op2 if in opMaskB else op4).
void surfaceApplyPending(SurfaceObjectState& ctx, int mode);

// ---------------------------------------------------------------------------
// Record consumers — FUN_00412ef0 / FUN_00412e94 / FUN_00412f84
// ---------------------------------------------------------------------------

// FUN_00412ef0 — accumulate the conveyor displacement for `poly` from all
// surface records whose surfType matches poly+0x20>>24: out += dir * rate
// * dt. `dt` is the frame step (DAT_0049b6f4 = 1/30 nominal).
void surfaceConveyorDelta(const SurfaceObjectState& ctx,
                          const CollisionPoly* poly, float dt,
                          float out[3]);

// FUN_00412e94 + FUN_00412f84 — volume/ribbon query. For each volume
// record (kind != -1) whose AABB contains pos and whose queryMask passes,
// ease outVec[2] toward rate * shapeFalloff(t). Returns the OR of hits
// (bit per record in the original; nonzero = inside some volume).
int surfaceVolumeQuery(const SurfaceObjectState& ctx, std::uint32_t mask,
                       const float pos[3], float dt, float outVec[3]);

// FUN_004134a0 — per-frame record update: ramp `rate` toward `target` by
// `ramp`, advance the surface records' UV accumulators and re-sync the
// matching polys' flags. (The UV-scroll poly writes are render-side and
// not modelled — only the rate ramp + flag sync are.)
void surfaceRecordUpdate(SurfaceObjectState& ctx, float dt);

// ---------------------------------------------------------------------------
// Type-9 slide-zone trigger — tr_alcmd opcode 0xe0 (OBSERVED)
// ---------------------------------------------------------------------------
// The opcode scans the arena's DTI sub-record table (+0x38/+0x3c) for
// type-9 records and, when the player position is inside a record's box,
// writes DAT_00540e28 = 1 (the bounce flag) and applies a redirect:
//   slideMode || hasContactNormal -> enter slide + yaw/speed impulse
//   else                          -> vertVel -= dt * 128.0 (down-slam)
// A zero flag arg clears slideMode instead. This is the slide/deflect
// mechanic — e28 marks "in zone" so the landing path suppresses the
// normal landing transition while the redirect applies.
struct SlideZoneResult {
  bool inside = false;       // a type-9 box contained pos
  bool setBounceFlag = false;// DAT_00540e28 = 1 written
  bool slideRedirect = false;// grounded/sliding path taken
  float impulseX = 0.0f;     // FUN_00465e64 slide-channel args
  float impulseZ = 0.0f;
  float yawDeg = 0.0f;       // the opcode's yaw arg (c2c)
  bool downSlam = false;     // airborne path: vertVel -= dt*128
  float downSlamDelta = 0.0f;// the applied vertVel change
  bool clearedSlide = false; // flag==0 path: e24/cbc cleared
};

// The opcode-0xe0 mechanics. `recs`/`count` = the arena's DTI sub-record
// table; `flag` = the opcode's enable arg (0 -> clear slide, no scan);
// `yawDeg`/`speed` = the opcode's redirect args; `dt` = frame step.
SlideZoneResult slideZoneTrigger(const DtiSubRecord* recs,
                                 std::size_t count, std::uint8_t flag,
                                 const float pos[3], bool slideMode,
                                 bool hasContactNormal, float yawDeg,
                                 float speed, float dt);

// ---------------------------------------------------------------------------
// The sweep-contact adapter — the 0x4635e0 callback equivalent
// ---------------------------------------------------------------------------
// Install as `CollisionState::contactHook` to give the swept-collision
// query the original's per-contact surface dispatch. Reads the surface
// block + channel from `cs.surface` / `cs.surfaceContextMask`, the sweep
// contact position staged in `cs.sweepContact`, the player position from
// `cs.pos`, and the entry snapshot from `cs.entryPos`; the event code is
// the callback's -0xb. The collision object's `scriptFn` (if set) receives
// each handler invoke — the FUN_004546ac seam.
void surfaceContactHook(CollisionState& cs, const CollisionNode* node,
                        const CollisionPoly* poly);

} // namespace mdk
