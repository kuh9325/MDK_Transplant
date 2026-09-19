// Phase 5F — surface contact effects. See player_surface.h for the
// evidence map; every algorithm below mirrors the recovered disassembly.

#include "core/player_surface.h"

#include <cmath>
#include <cstring>

namespace mdk {
namespace {

// The record is the same bytes as the parsed CollisionPoly (+0x20 flags,
// +0x23 surface). The flag ops write the +0x20 low byte in place — the
// ops are all < 0x100 so they only touch byte +0x20 of the u16 `flags`.
inline std::uint16_t& polyFlags(CollisionPoly& p) { return p.flags; }

// Normalized box test used by both record consumers: closed on all six
// bounds (OBSERVED — FCOMP+JC/JBE in FUN_00412f84 and the opcode-0xe0
// scan). `zPad` is the volume query's +5.0 top margin (0 elsewhere).
inline bool inBox(const float pos[3], const float* v, double zPad) {
  return pos[0] >= v[0] && pos[0] <= v[3] && pos[1] >= v[1] &&
         pos[1] <= v[4] && pos[2] >= v[2] &&
         static_cast<double>(pos[2]) <=
             static_cast<double>(v[5]) + zPad;
}

// tr_alcmd opcode-0xe0 airborne path — vertVel -= dt * 128.0
// (0x44f7d6: FLD f4; FMUL double 128.0; FSUBR c78).
constexpr double kSlamRate = 128.0;

// The shape-6 volume wobble — two persistent globals in the original
// (DAT_0049a744 accumulator, DAT_0049a748 direction). Shared across all
// records in a frame exactly like the globals.
float g_wobbleAcc = 0.0f;   // DAT_0049a744
int g_wobbleDir = 0;        // DAT_0049a748

// FUN_00412f84's per-shape falloff on the normalized height t in [0,1].
float shapeFalloff(const SurfaceRecord& r, int kind, float t,
                   float posZ) {
  switch (kind) {
    case 1:  return 1.0f - t * t;                    // 0x413120
    case 2:  return 1.0f - t * t * t;                // 0x413131
    case 3:  return (1.0f - t) * (1.0f - t);         // 0x413145
    case 4:  return (1.0f - t) * (1.0f - t) * (1.0f - t);  // 0x41315a
    case 5:  return 1.0f - t;                        // 0x413172
    case 6:                                          // 0x413181
      if (static_cast<double>(r.v[5]) - static_cast<double>(posZ) >= 5.0)
        return 1.0f;
      return 1.0f - static_cast<float>(
                        (static_cast<double>(r.v[5]) - 5.0 -
                         static_cast<double>(posZ)) *
                        0.2);
    default: return 1.0f;                            // 0x41301d (kind 0/7+)
  }
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_0040a704 — polygon flag op for a 1-based surface id
// ---------------------------------------------------------------------------
void surfacePolyOp(CollisionPoly* polys, std::int32_t count,
                   std::uint32_t surfId, int op) {
  if (!polys || surfId == 0 || surfId > 0xff) return;
  for (std::int32_t i = 0; i < count; ++i) {
    CollisionPoly& p = polys[i];
    if (p.surface != surfId) continue;
    switch (op) {
      case kSurfOpSet30:   polyFlags(p) |= 0x30; break;
      case kSurfOpClear30: polyFlags(p) &= 0xcf; break;
      case kSurfOpSet10:   polyFlags(p) |= 0x10; break;
      case kSurfOpClear10: polyFlags(p) &= 0xef; break;
      case kSurfOpSet20:   polyFlags(p) |= 0x20; break;
      case kSurfOpClear20: polyFlags(p) &= 0xdf; break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------------------
// Record creation — FUN_00413380 (surface) / FUN_00412e10 (volume)
// ---------------------------------------------------------------------------
SurfaceRecord* surfaceRecordCreate(SurfaceObjectState& ctx,
                                   std::uint32_t surfType,
                                   const float dir[3], float rate) {
  auto* r = new SurfaceRecord();
  r->kind = -1;                      // +0x14 = -1 (surface-bound)
  r->surfType = static_cast<std::uint8_t>(surfType & 0xff);
  // FUN_00413380 normalizes the 3D direction into v[0..2] and derives the
  // planar/UV direction into v[3..4] (the UV-scroll pair) plus the
  // normalization factor into v[5].
  const double len = std::sqrt(static_cast<double>(dir[0]) * dir[0] +
                               static_cast<double>(dir[1]) * dir[1] +
                               static_cast<double>(dir[2]) * dir[2]);
  const float inv = (len != 0.0) ? static_cast<float>(1.0 / len) : 0.0f;
  r->v[0] = dir[0] * inv;
  r->v[1] = dir[1] * inv;
  r->v[2] = dir[2] * inv;
  r->v[5] = inv;                     // +0x34 — the stored norm factor
  // v[3]/v[4] (the +0x2c/+0x30 planar dir) are render-side; left zero.
  r->rate = rate;
  r->target = rate;
  r->ramp = 0.0f;
  r->next = ctx.records;
  ctx.records = r;
  return r;
}

SurfaceRecord* surfaceVolumeCreate(SurfaceObjectState& ctx, int shape,
                                   const float box[6], float rate,
                                   std::uint32_t queryMask) {
  auto* r = new SurfaceRecord();
  r->kind = shape;                   // +0x14 = 1..6 (volume shape)
  for (int i = 0; i < 6; ++i) r->v[i] = box[i];
  r->rate = rate;
  r->target = rate;
  r->queryMask = queryMask;
  r->next = ctx.records;
  ctx.records = r;
  return r;
}

void surfaceRecordsDestroy(SurfaceObjectState& ctx) {
  SurfaceRecord* r = ctx.records;
  while (r) {
    SurfaceRecord* n = r->next;
    delete r;
    r = n;
  }
  ctx.records = nullptr;
}

// ---------------------------------------------------------------------------
// FUN_0040b5d0 — the contact dispatcher
// ---------------------------------------------------------------------------
std::uint8_t surfaceDispatch(SurfaceObjectState& ctx,
                             std::int32_t secondary,
                             std::uint8_t contextMask,
                             CollisionPoly* poly, std::int32_t eventCode,
                             const float vecA[3], const float posB[3],
                             const float contactPos[3],
                             SurfaceFxState& fx, SurfaceScriptFn script,
                             void* user) {
  if (!poly) return 0;
  // +0x23 nonzero gate; the index is (+0x20 >> 24) - 1 — the same byte.
  const std::uint8_t surf = poly->surface;
  if (surf == 0) return 0;
  const int idx = surf - 1;
  if (idx >= kSurfaceSlots) return 0;

  std::uint8_t result = 0;
  std::int32_t secondaryLocal = secondary;
  int sideEffect = 0;
  const std::uint8_t cfg = ctx.config[idx];
  if (cfg & contextMask) {
    if (cfg & 0x80) {
      ctx.marks |= (1u << surf);
      surfacePolyOp(ctx.polys, ctx.polyCount, surf, kSurfOpClear10);
    }
    if (cfg & 0x40) {
      sideEffect = 1;
      if (secondaryLocal == 0) secondaryLocal = 1;
    }
    if (cfg & 0x20) result |= 0x2;
  }
  if (ctx.handlerOff[idx] == 0) return result;
  const std::uint8_t invokeMask = ctx.handlerMask[idx];
  if ((invokeMask & contextMask) == 0 && !sideEffect) return result;
  for (int i = 0; i < 3; ++i) {
    fx.delta[i] = posB[i] - contactPos[i];
    fx.vec[i] = vecA[i];
  }
  ctx.counters[idx] += secondaryLocal;
  result |= 0x1;
  if (script) script(ctx, ctx.handlerOff[idx], eventCode, result, fx, user);
  return result;
}

// ---------------------------------------------------------------------------
// FUN_0040b4dc — pending/persistent surface ops
// ---------------------------------------------------------------------------
void surfaceApplyPending(SurfaceObjectState& ctx, int mode) {
  if (mode == 0) {
    if (!ctx.marks) return;
    for (int i = 0; i < 32; ++i) {
      const std::uint32_t bit = 1u << i;
      if (ctx.marks & bit) {
        surfacePolyOp(ctx.polys, ctx.polyCount,
                      static_cast<std::uint32_t>(i), kSurfOpSet10);
        ctx.marks &= ~bit;
      }
    }
    return;
  }
  for (std::int32_t i = 0; i < ctx.polyCount; ++i) {
    CollisionPoly& p = ctx.polys[i];
    if (polyFlags(p) & 0x2) polyFlags(p) |= 0x30;
  }
  for (int i = 0; i < 32; ++i) {
    const std::uint32_t bit = 1u << i;
    int op;
    if (ctx.opMaskB & bit) op = kSurfOpSet10;       // bit in +0x110 -> op2
    else if (ctx.opMaskA & bit) op = kSurfOpSet20;  // bit in +0x10c -> op4
    else continue;
    surfacePolyOp(ctx.polys, ctx.polyCount, static_cast<std::uint32_t>(i),
                  op);
  }
}

// ---------------------------------------------------------------------------
// FUN_00412ef0 — conveyor displacement accumulate
// ---------------------------------------------------------------------------
void surfaceConveyorDelta(const SurfaceObjectState& ctx,
                          const CollisionPoly* poly, float dt,
                          float out[3]) {
  if (!poly) return;
  const std::uint8_t surf = poly->surface;
  for (const SurfaceRecord* r = ctx.records; r; r = r->next) {
    if (r->kind != -1) continue;
    if (r->surfType != surf) continue;
    out[0] += (r->v[0] * r->rate) * dt;
    out[1] += (r->v[1] * r->rate) * dt;
    out[2] += (r->v[2] * r->rate) * dt;
  }
}

// ---------------------------------------------------------------------------
// FUN_00412e94 + FUN_00412f84 — volume/ribbon query
// ---------------------------------------------------------------------------
namespace {

int volumeQueryOne(const SurfaceRecord& rec, std::uint32_t mask,
                   const float pos[3], float dt, float outVec[3]) {
  if (!(rec.queryMask & mask)) return 0;
  if (!inBox(pos, rec.v, 5.0)) return 0;
  float t;
  if (rec.f10 != 0) {
    t = 1.0f;
  } else {
    t = (pos[2] - rec.v[2]) / (rec.v[5] - rec.v[2]);
    t = shapeFalloff(rec, rec.kind, t, pos[2]);
  }
  float target = rec.rate * t;
  if (rec.kind == 6) {
    // The shape-6 bob: a persistent ±0.1-step accumulator clamped to
    // [-2, +2] that only applies within 2.0 of the volume top.
    if (static_cast<double>(rec.v[5]) - static_cast<double>(pos[2]) < 2.0) {
      if (g_wobbleDir == 0) {
        g_wobbleAcc = static_cast<float>(static_cast<double>(g_wobbleAcc) +
                                         -0.1);
        if (g_wobbleAcc < -2.0f) g_wobbleDir = 1;
      } else {
        g_wobbleAcc = static_cast<float>(static_cast<double>(g_wobbleAcc) +
                                         0.1);
        if (g_wobbleAcc > 2.0f) g_wobbleDir = 0;
      }
      target += g_wobbleAcc;
    }
  }
  // The z-ease: above target -> halve the gap; below -> approach by
  // target*dt, clamped at target.
  if (outVec[2] > target) {
    outVec[2] = static_cast<float>(
        (static_cast<double>(outVec[2]) + static_cast<double>(target)) *
        0.5);
  } else if (outVec[2] < target) {
    outVec[2] += target * dt;
    if (outVec[2] > target) outVec[2] = target;
  }
  return 1;
}

} // namespace

int surfaceVolumeQuery(const SurfaceObjectState& ctx, std::uint32_t mask,
                       const float pos[3], float dt, float outVec[3]) {
  int hits = 0;
  if (!ctx.records) return 0;
  for (const SurfaceRecord* r = ctx.records; r; r = r->next) {
    if (r->kind == -1) continue;
    hits |= volumeQueryOne(*r, mask, pos, dt, outVec);
  }
  return hits;
}

// ---------------------------------------------------------------------------
// FUN_004134a0 — per-frame record update (locomotion subset)
// ---------------------------------------------------------------------------
void surfaceRecordUpdate(SurfaceObjectState& ctx, float dt) {
  for (SurfaceRecord* r = ctx.records; r; r = r->next) {
    if (r->rate != r->target) {
      float nr = r->ramp * dt + r->rate;
      if ((nr - r->target) * (r->rate - r->target) <= 0.0f) {
        r->ramp = 0.0f;
        nr = r->target;
      }
      r->rate = nr;
    }
    if (r->kind == -1) {
      // The UV-scroll half: advance the accumulators and wrap into
      // (-512, 512]. The poly +0x8..0x1c UV writes are render-side and
      // not modelled (the native CollisionPoly leaves them unparsed).
      const float dU = r->v[3] * r->rate * dt;
      const float dV = r->v[4] * r->rate * dt;
      r->uvAcc[0] += dU;
      r->uvAcc[1] += dV;
      if (r->uvAcc[0] > 512.0f) r->uvAcc[0] -= 512.0f;
      else if (r->uvAcc[0] < -512.0f) r->uvAcc[0] += 512.0f;
      if (r->uvAcc[1] > 512.0f) r->uvAcc[1] -= 512.0f;
      else if (r->uvAcc[1] < -512.0f) r->uvAcc[1] += 512.0f;
    }
    // kind != -1: the original feeds rand() into a wobble — render-side,
    // not part of the locomotion model.
  }
}

// ---------------------------------------------------------------------------
// tr_alcmd opcode 0xe0 — the type-9 slide-zone trigger
// ---------------------------------------------------------------------------
SlideZoneResult slideZoneTrigger(const DtiSubRecord* recs,
                                 std::size_t count, std::uint8_t flag,
                                 const float pos[3], bool slideMode,
                                 bool hasContactNormal, float yawDeg,
                                 float speed, float dt) {
  SlideZoneResult r;
  if (flag == 0) {
    r.clearedSlide = true;          // e24 = 0, cbc = 0
    return r;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const DtiSubRecord& rec = recs[i];
    if (rec.type != 9) continue;
    const float box[6] = {rec.fieldAsFloat(3), rec.fieldAsFloat(4),
                          rec.fieldAsFloat(5), rec.fieldAsFloat(6),
                          rec.fieldAsFloat(7), rec.fieldAsFloat(8)};
    if (!inBox(pos, box, 0.0)) continue;
    r.inside = true;
    r.setBounceFlag = true;         // DAT_00540e28 = 1
    if (slideMode || hasContactNormal) {
      // The grounded/sliding redirect: enter slide, c2c = yaw, then a
      // FUN_00437f98(yaw)->{sin,cos} impulse scaled by speed feeds the
      // FUN_00465e64 slide channels. The slide-mode write itself is the
      // locomotion boundary (Phase 5F does not implement slide).
      r.slideRedirect = true;
      r.yawDeg = yawDeg;
      const double rad = static_cast<double>(yawDeg) *
                         0.01745329251994328;   // pi/180 (0x497924)
      r.impulseX = static_cast<float>(std::sin(rad)) * speed;
      r.impulseZ = static_cast<float>(std::cos(rad)) * speed;
    } else {
      r.downSlam = true;
      r.downSlamDelta = static_cast<float>(-(static_cast<double>(dt) *
                                             kSlamRate));
    }
    return r;
  }
  return r;
}

// ---------------------------------------------------------------------------
// The 0x4635e0 sweep-callback adapter
// ---------------------------------------------------------------------------
void surfaceContactHook(CollisionState& cs, const CollisionNode* /*node*/,
                        const CollisionPoly* poly) {
  SurfaceObjectState* ctx = cs.surface;
  if (!ctx || !poly) return;
  SurfaceFxState fx;
  // The callback pushes (EBX=0 secondary, ECX=8 context) then the stack
  // args {eventCode=-0xb, vecA=0x4a20c0, posB=0x540bfc, cPos=0x540c08}.
  const std::uint8_t result = surfaceDispatch(
      *ctx, 0, cs.surfaceContextMask, const_cast<CollisionPoly*>(poly),
      -0xb, cs.sweepContact, cs.pos, cs.entryPos, fx, ctx->scriptFn,
      ctx->scriptUser);
  for (int i = 0; i < 3; ++i) {
    cs.surfDelta[i] = fx.delta[i];
    cs.surfVec[i] = fx.vec[i];
  }
  cs.surfaceResult = result;
}

} // namespace mdk
