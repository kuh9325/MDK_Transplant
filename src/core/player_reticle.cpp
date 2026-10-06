// Phase 5L — the mounted reticle / bomb-sight (see player_reticle.h
// for the full evidence map). FUN_00463608 mount-scan + class entries
// and the FUN_004691c4 per-frame update — a SEPARATE path from the
// sniper mode (they share only the channel globals + FUN_00467a00).

#include "core/player_reticle.h"

#include <cmath>
#include <string>

#include "core/collision_query.h"
#include "core/dynamic_objects.h"
#include "core/enemy_runtime.h"
#include "core/motion_channels.h"
#include "core/player_camera.h"
#include "core/player_fire.h"
#include "core/player_look.h"
#include "core/player_sniper.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

// The mounted/candidate object: the original keeps a single e6c/e68
// object-record pointer; the native splits it into the CollisionObject
// view (cs.excludeObj / cs.lastObjContact) plus this DynamicObject
// upcast for the +0x4c yaw / +0x10 pos / +0x08 health / +0x14x flags
// the reticle and mount entries touch. col is the first member, so
// the cast is the standard pointer-interconvertible container_of.
DynamicObject* mountObjFrom(const CollisionObject* col) {
  return const_cast<DynamicObject*>(
      reinterpret_cast<const DynamicObject*>(col));
}

// Rebuild the +0x148 dword from the word + byte mirrors (the original
// addresses the whole dword; the native splits it into flags148 plus
// the flags149/flags14a/flags14b byte mirrors).
std::uint32_t objectFlagsDword(const CollisionObject& col) {
  return static_cast<std::uint32_t>(col.flags148) |
         (static_cast<std::uint32_t>(col.flags14a) << 16) |
         (static_cast<std::uint32_t>(col.flags14b) << 24);
}

// Write the +0x148 dword back, keeping the flags148 word and the
// flags149 byte-mirror coherent (flags149 IS flags148's high byte).
void objectFlagsSetDword(CollisionObject& col, std::uint32_t v) {
  col.flags148 = static_cast<std::uint16_t>(v & 0xffffu);
  col.flags149 = static_cast<std::uint8_t>((v >> 8) & 0xffu);
  col.flags14a = static_cast<std::uint8_t>((v >> 16) & 0xffu);
  col.flags14b = static_cast<std::uint8_t>((v >> 24) & 0xffu);
}

void copyObjPosToPlayer(TraversalRuntime& rt, const DynamicObject* o) {
  rt.cs.pos[0] = o->pos[0];
  rt.cs.pos[1] = o->pos[1];
  rt.cs.pos[2] = o->pos[2];
}

void clearChannels(TraversalRuntime& rt) {
  rt.motion.zoomChannel = 0.0f;   // d54
  rt.motion.turnVel = 0.0f;       // d50
  rt.motion.strafeVel = 0.0f;     // d4c
  rt.motion.moveVel = 0.0f;       // d48
}

// FUN_00463608 0x463c6d — the XD / XD2 class-1 entry (0x10039).
void class1Entry(TraversalRuntime& rt, DynamicObject* o) {
  rt.mountClass = 0x10039;
  o->col.flags14a |= 0x08;
  rt.mountYaw = o->yawDeg;
  rt.eventMag = 0;
  copyObjPosToPlayer(rt, o);
  rt.eventType = 0;
  clearChannels(rt);
}

// FUN_00463608 0x463d1c — the XSNOWB class-2 entry (0x20002).
void class2Entry(TraversalRuntime& rt, DynamicObject* o) {
  objectFlagsSetDword(o->col, objectFlagsDword(o->col) | 0x80800u);
  rt.mountClass = 0x20002;
  if (rt.cs.rideActive != 0) {
    sniperReset(rt);            // FUN_00461878(0) — the shared reset
    rt.cs.rideActive = 0;       // dc8 = 0
  }
  rt.mountYaw = o->yawDeg;
  objectFlagsSetDword(o->col, objectFlagsDword(o->col) & ~0x80100u);
  rt.eventMag = 0;
  rt.eventType = 0;
  clearChannels(rt);
}

// FUN_00463608 0x463dba — the X_STRIKE / XE class-4 entry (0x40031).
void class4Entry(TraversalRuntime& rt, DynamicObject* o) {
  rt.mountClass = 0x40031;
  rt.motion.yawDeg = o->yawDeg;         // c2c = obj yaw
  rt.eventMag = 0;
  rt.eventType = 0;
  rt.flag49b740 = 1;                    // b740 — overhead-cam gate
  rt.overheadAux76c = 0.0f;             // b76c/b748/b744 = 0
  rt.overheadAux748 = 0.0f;
  rt.overheadAux744 = 0.0f;
  rt.camera.overheadHeight = 50.0f;     // b74c = 50.0
  copyObjPosToPlayer(rt, o);
  rt.reticleAux = 0;                    // d08 = 0
  rt.motion.moveVel = 300.0f;           // d48 = 300 (reticle X)
  rt.motion.strafeVel = 180.0f;         // d4c = 180 (reticle Y)
  rt.motion.zoomChannel = 0.0f;         // d54 = 0
  rt.motion.turnVel = 0.0f;             // d50 = 0
  rt.bombRecharge = 1.0f;               // ea4 = 1.0
  rt.bombs = kReticleBombMax;           // ea0 = 10
}

} // namespace

void playerReticleMountScan(TraversalRuntime& rt) {
  // 0x463c00 — latch the ride contact into e68, then the mountable
  // candidate gate (named && +0x14b&2 && !mounted && !c74).
  if (rt.cs.rideObj != nullptr && rt.cs.rideActive != 0) {
    rt.cs.lastObjContact = rt.cs.rideObj;
  }
  const CollisionObject* cand = rt.cs.lastObjContact;
  if (cand == nullptr || !cand->named || (cand->flags14b & 0x02) == 0 ||
      rt.cs.excludeObj != nullptr || rt.fieldC74 != 0) {
    return;
  }
  rt.cs.excludeObj = cand;              // 0x463c57 — MOUNT
  DynamicObject* obj = mountObjFrom(cand);
  const std::string name =
      obj != nullptr ? obj->model.modelName() : std::string();

  // Class-name dispatch — FUN_0042fa50 exact compares, checked in
  // order XD, XD2, XSNOWB, X_STRIKE, XE (the "WB" name is never tested).
  if ((name == "XD" || name == "XD2") && rt.fieldC74 == 0 &&
      rt.motion.airCharge == 0.0f) {
    class1Entry(rt, obj);
    return;
  }
  if (name == "XSNOWB") {
    class2Entry(rt, obj);
    return;
  }
  if (name == "X_STRIKE" || name == "XE") {
    class4Entry(rt, obj);
    return;
  }
  // 0x463e6a — "Unrecognised controlalien": the scan logs only; e6c
  // stays latched with e70 = 0 so the mounted dispatch unmounts it.
  ++rt.seams.mountUnmountCalls;
}

void playerReticleDispatchMounted(TraversalRuntime& rt,
                                  const RawGameplayInput& raw,
                                  const GameplayInputBindings& bindings,
                                  const GameplayInputFrame& ctrl,
                                  float f0, float f4, int frameStep) {
  // 0x463a6a — the e70 dword's byte2 (0x540e72) selects the class.
  const int cls = static_cast<int>((rt.mountClass >> 16) & 0xff);
  if ((cls & 0x1) != 0) {
    ++rt.seams.mountUpdateCalls;    // FUN_00467ac4 — XD / XD2
    ++rt.seams.weaponSlotCalls;     // FUN_00469cd0
  } else if ((cls & 0x2) != 0) {
    ++rt.seams.mountUpdateCalls;    // FUN_00467ed0 — XSNOWB
    ++rt.seams.weaponSlotCalls;     // FUN_00469cd0
  } else if ((cls & 0x4) != 0) {
    playerReticleUpdate(rt, raw, bindings, ctrl, f0, f4, frameStep);
    ++rt.seams.weaponSlotCalls;     // FUN_00469cd0
  } else {
    // 0x463aac — "Unrecognised controlalien" + unmount (no 0x469cd0).
    ++rt.seams.mountUnmountCalls;
    DynamicObject* obj = mountObjFrom(rt.cs.excludeObj);
    if (obj != nullptr) obj->col.flags14b &= ~0x02u;
    rt.cs.excludeObj = nullptr;
  }
}

// ---------------------------------------------------------------------------
// FUN_004691c4's armed-fire tail (0x469385..0x469517) — the XBN_BOMB
// lob. OBSERVED (MDK95.EXE BUILD_A, instruction-level):
//   - 0x4693f8: 540ea0 -= 1 runs BEFORE the trace/spawn — the bomb is
//     consumed even when the class lookup fails.
//   - launch = playerPos (0x540bfc) with z += -5.0 (0x498c08).
//   - dir.xy = (d48-300)*M2row0 + (d4c-180)*M2row1 through the camera
//     basis rows 0x540bb0/b4 + 0x540bc0/c4 (0x498c10/0x498c18
//     offsets); dir.z = -600.0 / 0x540b58 (0x498c20 / zoom).
//   - FUN_0046153c(pos, dir, 1000.0, &hit, 0,0, 3, 0x30, 0): outPos is
//     the unclipped far end on a miss and the clipped point on a hit.
//   - hit: t = sqrt(2*(launch.z - hit.z)/mount+0x48); miss: t = 2.5
//     (0x40200000). vel.xy = (hit.xy - pos.xy)/t; vel.z = 0 — a pure
//     lob: horizontal launch velocity only, gravity does the drop.
//   - spawn = FUN_00454af8(arena 0x540c48, launch, spawnId 1,
//     classIdx("XBN_BOMB"), scriptOff 0, flag 0) — a scriptless object
//     driven entirely by the +0x30a = 0x81 command body.
//   - tail writes: +0x30e=900 (fuse), +0x58=1.0 (scale), +0x30a=0x81,
//     +0x44=0 (drag off), +0x148 dword |= 0x818a6, +0x28..0x30 = vel,
//     +0x4c = 0x540c2c (locomotion yaw), FUN_0045612c rebuild,
//     +0x15c = "DROP" (0x49bacc -> 0x4985f8), FUN_00402160 FX child
//     (&+0x158, slot, &+0x10, 0, 0x7fff, 1.0f, 50.0f).
// `mount` is the original's 0x540e6c — the object whose +0x48 gravity
// feeds the flight-time solve. On the scripted Level-3 ride there is
// no mount object; the FUN_004566f0 default +0x48 = 32.0 applies
// (every object inits to 32.0 and no class-record write to +0x48
// exists — DOCUMENTED at initObjectDefaults).
// ---------------------------------------------------------------------------
void playerReticleFireBomb(TraversalRuntime& rt,
                           const DynamicObject* mount) {
  rt.bombs -= 1;                                 // 0x4693f8
  ++rt.seams.reticleSpawnCalls;   // FUN_0046153c + spawn-family seam

  // 0x46939c..0x469407 — the reticle offset through the unscaled M2
  // basis rows plus the fixed downward reach -600/zoom.
  const float* pos = rt.cs.pos;
  const float retX = rt.motion.moveVel - 300.0f;   // 0x498c10
  const float retY = rt.motion.strafeVel - 180.0f; // 0x498c18
  const float dir[3] = {
      retX * rt.camera.pose.basis[0][0] + retY * rt.camera.pose.basis[0][1],
      retX * rt.camera.pose.basis[1][0] + retY * rt.camera.pose.basis[1][1],
      -600.0f / rt.camera.zoom};                   // 0x498c20 / 0x540b58

  // FUN_0046153c — end = pos + dir*1000, then the object/BSP clip.
  float hit[3] = {pos[0] + dir[0] * 1000.0f,
                  pos[1] + dir[1] * 1000.0f,
                  pos[2] + dir[2] * 1000.0f};
  const bool traced = weapon5RayProbe(rt, pos, hit);

  // 0x46941c..0x469432 / 0x4695e7 — the flight-time solve.
  const float g = mount != nullptr ? mount->field48 : 32.0f;
  float t = 2.5f;                                  // 0x40200000
  if (traced) {
    const float q = 2.0f * ((pos[2] - 5.0f) - hit[2]) / g;
    // PORT GUARD: a hit above the launch pushes q < 0 into fsqrt —
    // the original propagates the NaN into the velocity (dead bomb).
    // Keeping the miss time is observable-equivalent without
    // poisoning the port's collision sweep math.
    if (q > 0.0f) t = std::sqrt(q);
  }
  // 0x469435..0x469459 — horizontal velocity only; vel.z = 0 always.
  const float vel[3] = {(hit[0] - pos[0]) / t, (hit[1] - pos[1]) / t,
                        0.0f};

  // FUN_00454794 — class record lookup; a miss skips the spawn tail
  // (0x469461 jl) but the bomb stays consumed.
  ++rt.seams.classLookupCalls;
  const int idx = rt.level.enemies.indexOf("XBN_BOMB");
  if (idx < 0) return;
  const RuntimeModel* src = traversalModelFor(idx, &rt.level);
  if (src == nullptr || rt.cur == nullptr) return;  // port bound

  // FUN_00454af8 — generic spawn into the current arena (0x540c48):
  // pos + spawnId 1 + class idx + scriptOff 0.
  DynamicObject& o = rt.cur->dyn.allocFront();      // FUN_0045cffc
  o.scriptClass = "XBN_BOMB";
  o.scriptOff = 0;                  // +0x108 — scriptless object
  o.enemyIndex = static_cast<std::uint16_t>(idx);
  o.arena = &rt.cur->dyn;
  o.spawnId = 1;                    // +0x146
  o.setPosition(pos[0], pos[1], pos[2] - 5.0f);   // 0x498c08 launch
  o.prevPos[0] = pos[0]; o.prevPos[1] = pos[1];
  o.prevPos[2] = pos[2] - 5.0f;                     // +0x180..
  o.behaviorByte = 7;               // +0x11c
  o.model = deepCopyModel(*src);    // FUN_00403720
  initObjectCollision(o);           // FUN_004566f0 defaults + rebuild

  // 0x469481..0x469517 — the fire block's own field writes.
  o.field30e = 900;                 // +0x30e = 0x384 — 30 s fuse
  o.col.scale = 1.0f;               // +0x58
  o.field30a = 0x81;                // +0x30a — the bomb command body
  o.field44 = 0.0f;                 // +0x44 — drag off
  o.col.flags148 |= 0x18a6;         // +0x148 dword |= 0x818a6
  o.col.flags14a |= 0x08;
  o.field28 = vel[0];               // +0x28..+0x30 — launch velocity
  o.field2c = vel[1];
  o.field30 = vel[2];
  o.yawDeg = rt.motion.yawDeg;      // +0x4c = 0x540c2c
  o.prevYawDeg = rt.motion.yawDeg;
  rebuildObjectTransform(o);        // FUN_0045612c
  o.field15c = "DROP";              // +0x15c = PTR 0x49bacc -> "DROP"
  o.field158 = fxChildSpawn(rt, o); // FUN_00402160
}

void playerReticleUpdate(TraversalRuntime& rt,
                         const RawGameplayInput& raw,
                         const GameplayInputBindings& bindings,
                         const GameplayInputFrame& ctrl,
                         float f0, float f4, int frameStep) {
  DynamicObject* obj = mountObjFrom(rt.cs.excludeObj);
  if (obj == nullptr) return;

  // 0x4691cf — player yaw pinned to the mount; pos = obj pos; the
  // overhead settle decays 25/s and marks obj+0x148|=0x10 on expiry.
  rt.motion.yawDeg = obj->yawDeg;
  copyObjPosToPlayer(rt, obj);
  rt.camera.overheadHeight -= f4 * static_cast<float>(kReticleSettleRate);
  if (rt.camera.overheadHeight < 0.0f) {
    obj->col.flags148 |= 0x10;
    rt.camera.overheadHeight = 0.0f;
  }

  if ((obj->col.flags14b & 0x04) == 0) {
    // 0x469227 — semantic channels through accelChannel; DL tracks the
    // fired mask (bit2 = d50, bit3 = d54).
    int dl = 0;
    if (ctrl.yawThird != 0.0f) {
      accelChannel(rt.motion.turnVel, ctrl.yawThird, ctrl.yaw10, f0);
      dl |= 4;
    }
    if (ctrl.moveThird != 0.0f) {
      accelChannel(rt.motion.zoomChannel, ctrl.moveThird, ctrl.move10,
                   f0);
      dl |= 8;
    }
    // 0x46926d — raw mouse (CURRENT frame): only with no semantic
    // channel, a nonzero delta and mouseOn. The "/3" rate; no
    // mouseYReversed flip.
    if (dl == 0 && (raw.mouseDx != 0 || raw.mouseDy != 0) &&
        bindings.mouseOn) {
      const float mk = static_cast<float>(kReticleMouseK);
      rt.motion.moveVel +=
          static_cast<float>(raw.mouseDx) * mk;   // d48 += dx/3
      rt.motion.strafeVel +=
          static_cast<float>(raw.mouseDy) * mk;   // d4c += dy/3
      rt.motion.turnVel = 0.0f;
      rt.motion.zoomChannel = 0.0f;
    }
    // 0x4692cb — inactive channels decay.
    if ((dl & 4) == 0) linearDecay(rt.motion.turnVel, kReticleDecay, f0);
    if ((dl & 8) == 0)
      linearDecay(rt.motion.zoomChannel, kReticleDecay, f0);
    // 0x4692f3 — integrate + pixel clamp (the 600x360 third-person
    // frame, not the sniper viewport).
    rt.motion.moveVel += rt.motion.turnVel * f0;
    if (rt.motion.moveVel < kReticleXMin)
      rt.motion.moveVel = kReticleXMin;
    if (rt.motion.moveVel > kReticleXMax)
      rt.motion.moveVel = kReticleXMax;
    rt.motion.strafeVel += rt.motion.zoomChannel * f0;
    if (rt.motion.strafeVel < kReticleYMin)
      rt.motion.strafeVel = kReticleYMin;
    if (rt.motion.strafeVel > kReticleYMax)
      rt.motion.strafeVel = kReticleYMax;

    // 0x46935b — the semi-auto latch: fire==0 re-arms to 999; fire!=0
    // spawns only on the armed frame then drains by frameStep.
    if (ctrl.fire == 0) {
      rt.fieldD0c = kReticleLatchArmed;
    } else {
      if (rt.fieldD0c == kReticleLatchArmed && rt.bombs > 0) {
        // 0x469385 — the XBN_BOMB ballistic lob (the whole spawn tail,
        // not a counted seam).
        playerReticleFireBomb(rt, obj);
      }
      rt.fieldD0c -= frameStep;
    }
    // 0x469523 — recharge: one bomb per second up to 10.
    if (rt.bombs >= kReticleBombMax) {
      rt.bombRecharge = kReticleRecharge;
    } else {
      rt.bombRecharge -= f4;
      if (rt.bombRecharge <= 0.0f) {
        rt.bombs += 1;
        rt.bombRecharge = kReticleRecharge;
      }
    }
  } else {
    // 0x469611 — disabled: clear d58 (look pitch) + d54 (Y velocity)
    // and skip straight to the energy check.
    rt.look.lookPitchOffset = 0.0f;
    rt.motion.zoomChannel = 0.0f;
  }

  // 0x469567 — energy: any deficit off the 10000 sentinel drains the
  // player through FUN_00467a00; on empty health the mount dies.
  if (obj->health != kReticleEnergySentinel) {
    sniperDamageDrain(rt, kReticleEnergySentinel - obj->health);
    obj->health = kReticleEnergySentinel;
    if (rt.fieldHealth <= 0) {
      obj->health = 0;
      ++rt.seams.reticleDeathCalls;
    }
  }
}

} // namespace mdk
