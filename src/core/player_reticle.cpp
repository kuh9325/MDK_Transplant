// Phase 5L — the mounted reticle / bomb-sight (see player_reticle.h
// for the full evidence map). FUN_00463608 mount-scan + class entries
// and the FUN_004691c4 per-frame update — a SEPARATE path from the
// sniper mode (they share only the channel globals + FUN_00467a00).

#include "core/player_reticle.h"

#include <string>

#include "core/collision_query.h"
#include "core/dynamic_objects.h"
#include "core/motion_channels.h"
#include "core/player_camera.h"
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
        // 0x469385 — the ballistic spawn: pos.z -= 5.0, the aim
        // unproject into M2, projectile spawn + setup — counted seam.
        rt.bombs -= 1;
        ++rt.seams.reticleSpawnCalls;
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
