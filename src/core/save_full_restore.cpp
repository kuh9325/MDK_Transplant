// save_full_restore.cpp — Phase 14C: the FUN_00427218 full-save load.
//
// See the header for the packet order + reference model. All offsets
// below are OBSERVED from the original loader/writer pair
// (FUN_00427218 / FUN_00426a0c + FUN_00426f34/00426738) and verified
// against original/installed/SAVES/1.SAV and MDK.SAV.

#include "core/save_full_restore.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "core/dynamic_objects.h"
#include "core/enemy_runtime.h"
#include "core/progression_runtime.h"
#include "core/traversal_runtime.h"
#include "core/traversal_script.h"

namespace mdk {
namespace {

// ---------------------------------------------------------------------------
// Little-endian readers
// ---------------------------------------------------------------------------

std::uint8_t rd8(std::span<const std::byte> b, std::size_t off) {
  return static_cast<std::uint8_t>(b[off]);
}
std::int8_t rdi8(std::span<const std::byte> b, std::size_t off) {
  return static_cast<std::int8_t>(rd8(b, off));
}
std::uint16_t rd16(std::span<const std::byte> b, std::size_t off) {
  return static_cast<std::uint16_t>(rd8(b, off)) |
         static_cast<std::uint16_t>(rd8(b, off + 1) << 8);
}
std::int16_t rdi16(std::span<const std::byte> b, std::size_t off) {
  return static_cast<std::int16_t>(rd16(b, off));
}
std::uint32_t rd32(std::span<const std::byte> b, std::size_t off) {
  std::uint32_t v;
  std::memcpy(&v, b.data() + off, 4);
  return v;
}
std::int32_t rdi32(std::span<const std::byte> b, std::size_t off) {
  return static_cast<std::int32_t>(rd32(b, off));
}
float rdf32(std::span<const std::byte> b, std::size_t off) {
  float v;
  std::memcpy(&v, b.data() + off, 4);
  return v;
}

// ---------------------------------------------------------------------------
// Reference resolution — the load-side half of FUN_004262b0/004262c8
// (offset<->pointer) and FUN_004282dc (object id -> record).
// ---------------------------------------------------------------------------

constexpr std::uint32_t kArenaStride = 0x466;  // the original record size

struct Refs {
  TraversalRuntime* rt;
  const std::byte* cmi;
  std::size_t cmiSize;
  FullRestoreReport* rep;
  // save id -> record (FUN_004282dc's scan materialized): each arena's
  // embedded +0x118 record first, then its ALIE objects — ids are
  // 1-based and sequential across arenas in stream order.
  std::unordered_map<std::int32_t, DynamicObject*> byId;

  void warn(const char* what, std::int32_t v) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s: unresolved ref %d", what, v);
    rep->warnings.emplace_back(buf);
  }

  // FUN_004262c8 on the arena base (0x54c670): -1 -> null, else
  // base + off. Saved offsets are stride multiples by construction.
  TraversalArena* arenaOff(std::int32_t off) {
    if (off == -1) return nullptr;
    if (off < 0 || static_cast<std::uint32_t>(off) % kArenaStride != 0) {
      ++rep->arenaRefsFailed;
      warn("arena offset", off);
      return nullptr;
    }
    const std::size_t idx = static_cast<std::size_t>(off) / kArenaStride;
    if (idx >= rt->arenas.size()) {
      ++rep->arenaRefsFailed;
      warn("arena offset", off);
      return nullptr;
    }
    ++rep->arenaRefsResolved;
    return rt->arenas[idx].get();
  }

  // FUN_004262c8 on the CMI image base (0x54c6bc): -1 -> null, else
  // image + off. The port keeps raw pointers into level.cmiBytes —
  // the same storage the original's resolved pointer addressed.
  const void* cmiOff(std::int32_t off) {
    if (off == -1) return nullptr;
    if (off < 0 || static_cast<std::size_t>(off) >= cmiSize) {
      ++rep->cmiRefsFailed;
      warn("cmi offset", off);
      return nullptr;
    }
    ++rep->cmiRefsResolved;
    return cmi + off;
  }

  // The CMI-string view of cmiOff for the fields the port models as
  // std::string (animSoundName/field15c/connSound*/homingPrefix).
  std::string cmiName(std::int32_t off) {
    if (off == -1 || off < 0 || static_cast<std::size_t>(off) >= cmiSize)
      return {};
    const char* s = reinterpret_cast<const char*>(cmi + off);
    std::size_t n = 0;
    while (off + n < cmiSize && s[n] != '\0') ++n;
    return std::string(s, n);
  }

  // FUN_004282dc — saved +0x7c id -> live record. 0 -> null (the
  // writer leaves untouched ref fields at 0; the scan never matches
  // id 0 because stamping starts at 1).
  DynamicObject* objId(std::int32_t id) {
    if (id <= 0) return nullptr;
    auto it = byId.find(id);
    if (it == byId.end()) {
      ++rep->objectRefsFailed;
      warn("object id", id);
      return nullptr;
    }
    ++rep->objectRefsResolved;
    return it->second;
  }
};

// ---------------------------------------------------------------------------
// MORE (52B) — the staged store ladder. +0x00 is the level identity
// (the loaded .CMI byte length); +0x0c is written by the saver but
// never restored (OBSERVED quirk — both 0x5414a4 and 0x5414a8 take
// the +0x08 value).
// ---------------------------------------------------------------------------

void applyMore(std::span<const std::byte> p, TraversalRuntime& rt,
               Refs& refs, std::int32_t& anchorId) {
  rt.fadeTimer5414a0 = rdf32(p, 0x04);
  rt.fadeTimer5414a4 = rdf32(p, 0x08);
  rt.fadeTimer5414a8 = rdf32(p, 0x08);   // +0x0c never read (OBSERVED)
  rt.flag5414bc = rd32(p, 0x10) != 0;
  rt.field5414d8 = rdi32(p, 0x14);
  rt.field541518 = rdi32(p, 0x18);
  rt.g541534 = rdi8(p, 0x1c);
  anchorId = rdi32(p, 0x20);             // 0x49b85c object ref
  rt.flag49b740 = rdi32(p, 0x24);
  rt.overheadAux744 = rdf32(p, 0x28);
  rt.overheadAux748 = rdf32(p, 0x2c);
  rt.camera.overheadHeight = rdf32(p, 0x30);
}

// ---------------------------------------------------------------------------
// PLAY (239B) — verbatim into rt.savePlayBlock (the 0x541554 copy),
// then typed fields. OBSERVED ordering quirk: the block lands BEFORE
// FUN_004346e8, whose FUN_00433c4c reset rewrites 0x541618/19/1a/1b —
// the saved wpnSel0/1 + burstIndex + fireCadence are discarded; the
// loader's 0/0/3/0.0 stand. Health, ammo, deathCount and field54163b
// are not reset and restore normally.
// ---------------------------------------------------------------------------

void applyPlay(std::span<const std::byte> p, TraversalRuntime& rt,
               ProgressionSession& sess) {
  std::memcpy(rt.savePlayBlock.data(), p.data(),
              rt.savePlayBlock.size());
  rt.fieldHealth = rdi32(p, 0x00);
  sess.health = rt.fieldHealth;
  for (int i = 0; i < 6; ++i) {
    rt.ammo[i] = rdi32(p, 0xcb + 4 * i);
    sess.ammo[i] = rt.ammo[i];
  }
  sess.deathCount = rdi32(p, 0xe3);
  sess.field54163b = rdi32(p, 0xe7);
}

// ---------------------------------------------------------------------------
// CAME (200B, or 212B with the +0xc8..0xd3 scale tail) — the camera
// block at 0x540b28. FUN_004279ac accepts actual <= expected; a 200B
// packet leaves scaleX/Y/Z = 1.0f (OBSERVED default writes).
// ---------------------------------------------------------------------------

void applyCame(std::span<const std::byte> p, TraversalRuntime& rt) {
  PlayerCameraState& c = rt.camera;
  PlayerCameraPose& q = c.pose;
  for (int i = 0; i < 3; ++i) {
    q.pos[i] = rdf32(p, 0x00 + 4 * i);     // 0x540b28
    q.back[i] = rdf32(p, 0x0c + 4 * i);    // 0x540b34
    q.up[i] = rdf32(p, 0x18 + 4 * i);      // 0x540b40
  }
  rt.motion.bank = rdf32(p, 0x24);         // 0x540b4c
  rt.view.viewYawDeg = rdf32(p, 0x28);     // 0x540b50
  rt.viewScalar = rdf32(p, 0x2c);          // 0x540b54
  c.zoom = rdf32(p, 0x30);                 // 0x540b58
  c.heightOffset = rdf32(p, 0x34);         // 0x540b5c
  rt.bankAux = rdf32(p, 0x38);             // 0x540b60
  q.modeZoom = rdf32(p, 0x3c);             // 0x540b64
  q.viewW = rdi32(p, 0x40);                // 0x540b68..0x540b7c
  q.viewH = rdi32(p, 0x44);
  q.viewCX = rdi32(p, 0x48);
  q.viewCY = rdi32(p, 0x4c);
  q.viewOX = rdi32(p, 0x50);
  q.viewOY = rdi32(p, 0x54);
  for (int r = 0; r < 3; ++r) {            // M1 0x540b80 / M2 0x540bb0
    for (int k = 0; k < 4; ++k) {
      q.view[r][k] = rdf32(p, 0x58 + r * 16 + k * 4);
      q.basis[r][k] = rdf32(p, 0x88 + r * 16 + k * 4);
    }
  }
  rt.view.viewPitchDeg = rdf32(p, 0xb8);   // 0x540be0
  q.sinPitch = rdf32(p, 0xbc);             // 0x540be4
  q.cosPitch = rdf32(p, 0xc0);             // 0x540be8
  rt.flagBec = rdi32(p, 0xc4);             // 0x540bec
  if (p.size() >= 212) {
    q.scaleX = rdf32(p, 0xc8);             // 0x540bf0
    q.scaleY = rdf32(p, 0xcc);             // 0x540bf4
    q.scaleZ = rdf32(p, 0xd0);             // 0x540bf8
  } else {
    q.scaleX = q.scaleY = q.scaleZ = 1.0f; // 200B default (OBSERVED)
  }
}

// ---------------------------------------------------------------------------
// DAMP (724B) — the 0x540bfc..0x540ecf block. Pointer/object fields
// carry saved references; they are staged raw here and resolved in
// the fixup pass (mirroring the loader's store-then-fix order).
// ---------------------------------------------------------------------------

struct DampRefs {
  std::int32_t cur = -1, partner = -1, loadArena = -1;
  std::int32_t lruA = -1, lruB = -1, d6c = -1;
  std::int32_t floorObj = 0, focusObj = 0, rideObj = 0;
  std::int32_t cmdObj5c = 0, cmdObj60 = 0;
  std::int32_t lastObjContact = 0, excludeObj = 0, eventTimerObj = 0;
  std::int32_t pendingViewSnap = -1;
};

void applyDamp(std::span<const std::byte> p, TraversalRuntime& rt,
               DampRefs& r, FullRestoreReport& rep) {
  for (int i = 0; i < 3; ++i) {
    rt.cs.pos[i] = rdf32(p, 0x00 + 4 * i);
    rt.cs.entryPos[i] = rdf32(p, 0x0c + 4 * i);
  }
  rt.vert.posX = rt.cs.pos[0];             // vert mirror of cs.pos —
  rt.vert.posY = rt.cs.pos[1];             // one original vec, two
  rt.vert.posZ = rt.cs.pos[2];             // native views kept in sync
  // +0x18..+0x2f — no proven consumer (unmapped).
  rep.unmapped.push_back({"DAMP", 0x18, 0x18});
  rt.motion.yawDeg = rdf32(p, 0x30);
  for (int i = 0; i < 6; ++i)
    rt.cs.playerBox[i] = rdf32(p, 0x34 + 4 * i);
  r.cur = rdi32(p, 0x4c);                    // 0x540c48
  rep.unmapped.push_back({"DAMP", 0x50, 8}); // +0x50..+0x57
  rt.cs.contactFlags = rd8(p, 0x58);
  rt.vert.contactFlags = rt.cs.contactFlags;
  rt.cs.floorZ = rdf32(p, 0x5c);
  rt.vert.floorZ = rt.cs.floorZ;
  rt.cs.floorOffset = rdf32(p, 0x60);
  r.floorObj = rdi32(p, 0x64);               // 0x540c60
  rt.cs.floorElemMask = rd32(p, 0x68);
  rt.vert.blocker1 = rt.cs.floorElemMask;
  rt.cs.arenaValid = rdi32(p, 0x6c);
  rt.vertEnable = rdi32(p, 0x70) != 0;
  rt.cs.queryEnabled = rdi32(p, 0x74);
  rt.fieldC74 = rdi32(p, 0x78);
  rt.vert.vertVel = rdf32(p, 0x7c);
  rt.vert.vertSkip = rdi32(p, 0x80);
  rt.vert.jumpSustain = rd32(p, 0x84);
  rt.motion.airCharge = rdf32(p, 0x88);
  rt.vert.jumpActive = rdi32(p, 0x8c);
  rt.vert.jumpHoldCharge = rdi32(p, 0x90);
  rt.vert.jumpLatch = rdi32(p, 0x94);
  rt.motion.turnLock = rdi32(p, 0x98);
  rt.vert.jumpAux = rdi32(p, 0x9c);
  rt.flagC9c = rdi32(p, 0xa0);
  rt.transitionPhase = rdi32(p, 0xa4);
  r.partner = rdi32(p, 0xa8);                // 0x540ca4
  rt.partnerActive = rdi32(p, 0xac) != 0;
  rt.cs.carrierValid = rdi32(p, 0xac);     // 0x540ca8 — the partner-
                                         // active mirror (attach tail
                                         // rebuilds it regardless)
  rt.locoState = rdi32(p, 0xb0);
  rt.animPrev = rdi32(p, 0xb4);
  rt.animFrame = rdi32(p, 0xb8);
  rep.unmapped.push_back({"DAMP", 0xbc, 4}); // 0x540cb8
  rt.eventPriority = rdi32(p, 0xc0);
  rt.vert.eventIdle = rt.eventPriority;
  rt.motion.moveDirLatch = rdi32(p, 0xc4);
  rep.unmapped.push_back({"DAMP", 0xc8, 4}); // 0x540cc4
  rt.fieldCc8 = rdi32(p, 0xcc);
  rt.scopeChanC = rdf32(p, 0xd0);
  rt.scopeChanD0 = rdf32(p, 0xd4);
  rt.scopeChanD4 = rdf32(p, 0xd8);
  r.focusObj = rdi32(p, 0xdc);               // 0x540cd8
  rt.teleportFlag = rdi32(p, 0xe0);          // 0x540cdc / focusDist
  rt.focusDist = rdf32(p, 0xe0);
  for (int i = 0; i < 6; ++i)
    rt.teleportVec[i] = rdf32(p, 0xe4 + 4 * i);
  rt.camera.shakeX = rdf32(p, 0xf8);
  rt.camera.shakeY = rdf32(p, 0xfc);
  rep.unmapped.push_back({"DAMP", 0x100, 8}); // 0x540d00/0x540d04
  rt.reticleAux = rdi32(p, 0x108);
  rt.fieldD0c = rdi32(p, 0x10c);
  // +0x110/+0x114/+0x11c/+0x120 (0x540d10/14/1c/20) are zeroed by the
  // loader — not restored. +0x118/+0x124/+0x128 have no proven
  // consumer.
  rep.unmapped.push_back({"DAMP", 0x118, 4});
  rep.unmapped.push_back({"DAMP", 0x124, 8});
  rt.fieldD2c = rdi32(p, 0x12c);
  r.loadArena = rdi32(p, 0x130);             // 0x540d30
  rt.scopeHudOffset = rdi32(p, 0x134);
  rep.unmapped.push_back({"DAMP", 0x138, 4}); // 0x540d38
  // +0x13c/+0x140 (0x540d3c/0x540d40 pending lists) cleared post-fixup.
  rep.unmapped.push_back({"DAMP", 0x144, 4}); // 0x540d44
  rt.motion.moveVel = rdf32(p, 0x148);
  rt.motion.strafeVel = rdf32(p, 0x14c);
  rt.motion.turnVel = rdf32(p, 0x150);
  rt.motion.zoomChannel = rdf32(p, 0x154);
  rt.look.lookPitchOffset = rdf32(p, 0x158);
  rt.vert.landingAccum = rdf32(p, 0x15c);
  r.lruA = rdi32(p, 0x160);                  // 0x540d60
  r.lruB = rdi32(p, 0x164);                  // 0x540d64
  // +0x168 (0x540d68 pending list) cleared post-fixup.
  r.d6c = rdi32(p, 0x16c);                   // 0x540d6c — resolved then
                                           // cleared by the loader
  rep.unmapped.push_back({"DAMP", 0x170, 0x1c}); // 0x540d70..0x540d8b
  for (int i = 0; i < 8; ++i)
    rt.scriptGVars[i] = rdf32(p, 0x18c + 4 * i); // 0x540d88 (script
                                                 // operand group 0)
  rt.scriptGFlags = rd32(p, 0x19c);          // 0x540d98 flag group 0
                                             // (aliases scriptGVars[4])
  rt.masterMoveGate = rdi32(p, 0x1a0) != 0;
  rt.fieldDa0 = rdi32(p, 0x1a4);
  rt.fieldDa4 = rdi32(p, 0x1a8);
  rep.unmapped.push_back({"DAMP", 0x1ac, 4}); // 0x540da8
  rt.fieldDac = rdi32(p, 0x1b0);
  rep.unmapped.push_back({"DAMP", 0x1b4, 4}); // 0x540db0
  rt.camera.pullback = rdf32(p, 0x1b8);
  rt.camera.eyeHeight = rdf32(p, 0x1bc);
  rt.scopeScale = rdi32(p, 0x1c0);
  r.rideObj = rdi32(p, 0x1c4);               // 0x540dc0
  rt.cs.rideElemMask = rd32(p, 0x1c8);
  rt.vert.moveBlocker1 = rt.cs.rideElemMask;
  rt.cs.rideActive = rdi32(p, 0x1cc);
  rt.vert.moveBlockerFlag = rt.cs.rideActive;
  rep.unmapped.push_back({"DAMP", 0x1d0, 4}); // 0x540dcc
  // +0x1d4..+0x210 (0x540dd0..0x540e0c) — 16 dwords the loader zeroes
  // (weapon-5 charge/work globals — the saved values are discarded).
  rt.fieldE10 = rdf32(p, 0x214);
  rt.fieldE14 = rdi32(p, 0x218);
  for (int i = 0; i < 3; ++i)
    rt.weapon5Aim[i] = rdf32(p, 0x21c + 4 * i);
  rt.slideChannel = rdi32(p, 0x228);
  rt.vert.bounceFlag = rdi32(p, 0x22c);
  rep.unmapped.push_back({"DAMP", 0x230, 0x18}); // 0x540e2c..0x540e43
  rt.animE44 = rdf32(p, 0x248);
  rt.animE48 = rdf32(p, 0x24c);
  // +0x250/+0x254 (0x540e4c/0x540e50) — pointer tokens: nonzero is
  // rebound to the 0x49b3ac/0x49b3d0 statics. Saved values are stale
  // heap pointers (OBSERVED 0x04d43fe4/0x04d3a5dc in 1.SAV), not
  // object ids. The port's contactObj keeps the opaque-token
  // semantics; +0x254 is a BSP-node output the float-normal view
  // cannot represent — logged unmapped rather than faked.
  rt.vert.contactObj = (rd32(p, 0x250) != 0) ? 1u : 0u;
  rt.lastContactPoly = nullptr;              // ptr view has no saved data
  rep.unmapped.push_back({"DAMP", 0x254, 4}); // node token (no native
                                              // float-normal home)
  rt.cmdFlag54 = rdi32(p, 0x258);            // 0x540e54
  rt.cmdDetonate58 = rdi32(p, 0x25c);        // 0x540e58
  r.cmdObj5c = rdi32(p, 0x260);              // 0x540e5c
  r.cmdObj60 = rdi32(p, 0x264);              // 0x540e60
  rt.mountYaw = rdf32(p, 0x268);             // 0x540e64
  r.lastObjContact = rdi32(p, 0x26c);        // 0x540e68
  r.excludeObj = rdi32(p, 0x270);            // 0x540e6c
  rt.mountClass = rd32(p, 0x274);            // 0x540e70
  rt.scopeAnimLatch = rdi32(p, 0x278);       // 0x540e74
  rt.punchTime = rdi32(p, 0x27c);            // 0x540e78
  rt.punchHitTime = rdi32(p, 0x280);         // 0x540e7c
  rt.shotSerial = rdi32(p, 0x284);           // 0x540e80
  rt.shotHitCount = rdi32(p, 0x288);         // 0x540e84
  rep.unmapped.push_back({"DAMP", 0x28c, 8}); // 0x540e88/0x540e8c
  rt.killTally = rdi32(p, 0x294);            // 0x540e90
  rt.scopeBlend94 = rdf32(p, 0x298);         // 0x540e94
  rt.scopeBlend98 = rdf32(p, 0x29c);         // 0x540e98
  rep.unmapped.push_back({"DAMP", 0x2a0, 4}); // 0x540e9c
  rt.bombs = rdi32(p, 0x2a4);                // 0x540ea0
  rt.bombRecharge = rdf32(p, 0x2a8);         // 0x540ea4
  // +0x2ac (0x540ea8 deferred-free countdown) cleared by the loader.
  rep.unmapped.push_back({"DAMP", 0x2b0, 4}); // 0x540eac
  rt.eventTimer = rdf32(p, 0x2b4);           // 0x540eb0
  r.eventTimerObj = rdi32(p, 0x2b8);         // 0x540eb4
  rt.fieldEb8 = rdi32(p, 0x2bc);             // 0x540eb8
  r.pendingViewSnap = rdi32(p, 0x2c0);       // 0x540ebc
  for (int i = 0; i < 4; ++i)
    rt.pendingView[i] = rdf32(p, 0x2c4 + 4 * i);
}

// ---------------------------------------------------------------------------
// Object record (814B) — shared layout for ALIE records and each
// AREN's embedded +0x118 record. Saved refs stay raw; `emit` writes
// typed fields, ref fields are returned through `out` for the fixup
// pass. FUN_00426f34's per-record fixups run inline where the saved
// field is already the resolved form (CMI offsets) or cleared.
// ---------------------------------------------------------------------------

struct ObjRefs {
  std::int32_t saveId = 0;         // +0x7c — the writer-stamped id
  std::int32_t arena = -1;         // +0x60
  std::int32_t field138 = 0;       // +0x138
  std::int32_t field278 = 0;       // +0x278
  std::int32_t field2b8 = 0;       // +0x2b8
  std::int32_t pendingArena = -1;  // +0x2bc
  std::int32_t connDest = -1;      // +0x302 (connector view)
  std::int32_t moverChild = 0;     // +0x312 (mover view)
  bool hasField2b0 = false;        // +0x2b0 nonzero -> sentinel
  bool hasField2b4 = false;        // +0x2b4 nonzero -> sentinel
  // CMI-offset fields — resolved to image pointers / strings in the
  // fixup pass.
  std::int32_t fieldEC = -1;       // +0xec  path record
  std::int32_t field108 = -1;      // +0x108 script PC
  std::int32_t field10c = -1;      // +0x10c remote-call PC
  std::int32_t field110 = -1;      // +0x110 death-handoff PC
  std::int32_t animRec = -1;       // +0x114 active anim record
  std::int32_t field230 = -1;      // +0x230 resume/wait PC
  std::int32_t retPc[4] = {-1, -1, -1, -1};
  std::int32_t savedPc[4] = {-1, -1, -1, -1};
  std::int32_t animSoundName = -1; // +0x140 CMI string
  std::int32_t field150 = -1;      // +0x150 CMI-offset marker
  std::int32_t field154 = -1;      // +0x154 CMI-offset marker
  std::int32_t field15c = -1;      // +0x15c CMI string
  std::int32_t animRecNear = -1;   // +0x306 connector union
  std::int32_t animRecFar = -1;    // +0x30a connector union
  std::int32_t connSound316 = -1;  // +0x316/+0x31a/+0x31e/+0x322
  std::int32_t connSound31a = -1;
  std::int32_t connSound31e = -1;
  std::int32_t connSound322 = -1;
  std::int32_t homingPrefix = -1;  // +0x302 (homing view)
};

void applyObjectRecord(std::span<const std::byte> r, DynamicObject& o,
                       ObjRefs& out) {
  // +0x00 list link — runtime-owned, not in the packet view.
  o.enemyIndex = rd16(r, 0x04);
  o.col.named = rd8(r, 0x06) != 0;
  o.col.field07 = rd8(r, 0x07);
  o.health = rdi32(r, 0x08);
  // +0x0c — the model deep-copy: cleared on both sides (OBSERVED);
  // the port mirrors it with a null element set (no flags148&1
  // restored object has been observed, so the +0x0c-deref paths are
  // unreachable — same as the original).
  o.col.elements = nullptr;
  o.col.model = (o.health != 0) ? &o.model : nullptr;
  for (int i = 0; i < 3; ++i) {
    o.pos[i] = rdf32(r, 0x10 + 4 * i);
    o.field1c[i] = rdf32(r, 0x1c + 4 * i);
  }
  o.col.baseZ = o.pos[2];
  o.field28 = rdf32(r, 0x28);
  o.field2c = rdf32(r, 0x2c);
  o.field30 = rdf32(r, 0x30);
  o.field34 = rdf32(r, 0x34);
  o.field38 = rdf32(r, 0x38);
  o.field3c = rdf32(r, 0x3c);
  o.field40 = rdf32(r, 0x40);
  o.field44 = rdf32(r, 0x44);
  o.field48 = rdf32(r, 0x48);
  o.yawDeg = rdf32(r, 0x4c);
  o.prevYawDeg = rdf32(r, 0x50);
  o.pitchDeg = rdf32(r, 0x54);
  o.col.scale = rdf32(r, 0x58);
  o.zBias = rdf32(r, 0x5c);
  out.arena = rdi32(r, 0x60);
  // +0x64..+0x78 — screen-space bounds; +0x7c..+0xa8 — the parent-
  // composed 3x4 matrix (FUN_0046afe4 render tail). Both are rebuilt
  // per frame and hold no gameplay state; +0x7c is where the writer
  // stamps the save id (read into the id table by the driver, not
  // stored as a field). The object has no serialized surface block —
  // SurfaceObjectState is runtime scratch here (dispatch-written,
  // zeroed at load). Render-only region: not restored.
  out.saveId = rdi32(r, 0x7c);
  // +0xac..+0xdb — the collision 3x4: three 16-byte rows {basis[3],
  // origin} (OBSERVED: 90-deg yaw object restores [[0,-1,0],[1,0,0],
  // [0,0,1]] + {50,90,10}). Rebuilt per frame from the Euler fields;
  // restored verbatim anyway so pre-update sweeps see the saved pose.
  for (int i = 0; i < 3; ++i) {
    o.col.xform[i * 3 + 0] = rdf32(r, 0xac + 0x10 * i + 0);
    o.col.xform[i * 3 + 1] = rdf32(r, 0xac + 0x10 * i + 4);
    o.col.xform[i * 3 + 2] = rdf32(r, 0xac + 0x10 * i + 8);
    o.col.origin[i] = rdf32(r, 0xac + 0x10 * i + 12);
  }
  o.animAcc = rdf32(r, 0xdc);
  o.animRate = rdf32(r, 0xe0);
  o.animFrame = rdi16(r, 0xe4);
  o.fieldE6 = rdi16(r, 0xe6);
  o.fieldE8 = rdf32(r, 0xe8);
  out.fieldEC = rdi32(r, 0xec);       // path record — CMI offset
  o.fieldF0 = rdf32(r, 0xf0);
  for (int i = 0; i < 3; ++i)
    o.fieldF4[i] = rdf32(r, 0xf4 + 4 * i);
  o.field100 = rdf32(r, 0x100);
  o.field104 = rdf32(r, 0x104);
  out.field108 = rdi32(r, 0x108);     // script PC — CMI offset
  out.field10c = rdi32(r, 0x10c);     // remote-call PC — CMI offset
  out.field110 = rdi32(r, 0x110);     // death-handoff PC — CMI offset
  out.animRec = rdi32(r, 0x114);      // anim record — CMI offset
  o.animLatch = rdi16(r, 0x118);
  o.field11a = rd8(r, 0x11a);
  o.field11b = rd8(r, 0x11b);
  o.behaviorByte = rd8(r, 0x11c);
  o.field11e = rd8(r, 0x11e);
  o.field11f = rd8(r, 0x11f);
  for (int i = 0; i < 3; ++i) {
    o.field120[i] = rdf32(r, 0x120 + 4 * i);
    o.field12c[i] = rdf32(r, 0x12c + 4 * i);
  }
  out.field138 = rdi32(r, 0x138);
  o.bankDeg = rdf32(r, 0x13c);
  out.animSoundName = rdi32(r, 0x140);     // CMI str offset
  o.animSoundMark = rdi16(r, 0x144);
  o.spawnId = rd16(r, 0x146);
  o.col.flags148 = rd16(r, 0x148);   // u16 — +0x148/+0x149
  o.col.flags149 = rd8(r, 0x149);    // aliases the u16 high byte
  o.col.flags14a = rd8(r, 0x14a);
  o.col.flags14b = rd8(r, 0x14b);
  o.col.flags14c = rd8(r, 0x14c);
  // +0x14d..+0x14f — bytes with no named collision-field consumer.
  o.field150 = out.field150 = rdi32(r, 0x150);  // CMI-offset markers;
  o.field154 = out.field154 = rdi32(r, 0x154);  // fixup validates them
  o.field158 = nullptr;                    // +0x158 cleared (OBSERVED)
  out.field15c = rdi32(r, 0x15c);          // CMI str offset
  // +0x160..+0x17f — the eight element pointers, cleared on both
  // sides (the deep-copied model record is not persisted).
  for (int i = 0; i < 3; ++i) {
    o.prevPos[i] = rdf32(r, 0x180 + 4 * i);
    o.field18c[i] = rdf32(r, 0x18c + 4 * i);
  }
  for (int i = 0; i < 6; ++i)
    o.col.aabb[i] = rdf32(r, 0x198 + 4 * i);
  for (int i = 0; i < 8; ++i)
    for (int k = 0; k < 3; ++k)
      o.worldRef[i][k] = rdf32(r, 0x1b0 + (i * 3 + k) * 4);
  for (int i = 0; i < 3; ++i)
    o.field210[i] = rdf32(r, 0x210 + 4 * i);
  o.field21c = rd8(r, 0x21c);
  o.field21d = rd8(r, 0x21d);
  o.field21e = rd8(r, 0x21e);
  o.flag21f = rd8(r, 0x21f);
  o.field220 = rdi32(r, 0x220);
  o.field224 = rdf32(r, 0x224);
  o.field228 = rdf32(r, 0x228);
  o.field22c = rdf32(r, 0x22c);
  out.field230 = rdi32(r, 0x230);          // resume PC — CMI offset
  for (int i = 0; i < 16; ++i)
    o.scriptLocals[i] = rdf32(r, 0x234 + 4 * i);
  o.scriptFlagsLocal = rd32(r, 0x244);
  o.scriptCallDepth = rdi32(r, 0x248);
  for (int i = 0; i < 4; ++i) {
    out.retPc[i] = rdi32(r, 0x24c + 4 * i);
    out.savedPc[i] = rdi32(r, 0x25c + 4 * i);
  }
  for (int i = 0; i < 5; ++i)
    o.scriptMark[i] = rd16(r, 0x26c + 2 * i);
  o.field276 = rd8(r, 0x276);
  o.field277 = rd8(r, 0x277);
  out.field278 = rdi32(r, 0x278);
  for (int i = 0; i < 6; ++i)
    o.clampBox[i] = rdf32(r, 0x27c + 4 * i);
  for (int i = 0; i < 3; ++i)
    o.animImpulse[i] = rdf32(r, 0x294 + 4 * i);
  o.field2a0 = rd8(r, 0x2a0);
  o.field2a1 = rd8(r, 0x2a1);
  o.healthMirror2a2 = rd16(r, 0x2a2);
  o.field2a4 = rdf32(r, 0x2a4);
  o.field2a8 = rdf32(r, 0x2a8);
  o.field2ac = rdf32(r, 0x2ac);
  out.hasField2b0 = rd32(r, 0x2b0) != 0;
  out.hasField2b4 = rd32(r, 0x2b4) != 0;
  out.field2b8 = rdi32(r, 0x2b8);
  out.pendingArena = rdi32(r, 0x2bc);
  o.field2c0 = rdf32(r, 0x2c0);
  o.field2c4 = rdf32(r, 0x2c4);
  o.col.elemMaskB = rd32(r, 0x2c8);
  // +0x2cc — unmapped dword.
  o.field2d0 = rd8(r, 0x2d0);
  o.field2d1 = rd8(r, 0x2d1);
  for (int i = 0; i < 6; ++i)
    o.field2d2[i] = rdf32(r, 0x2d2 + 4 * i);
  // +0x2ea..+0x301 — no proven consumer (24B unmapped).
  // +0x302..+0x322 — the aliased union block. The port keeps each
  // view in its own field; all are fed from the same saved dwords.
  for (int i = 0; i < 9; ++i)
    o.rawMatrix[i] = rdf32(r, 0x302 + 4 * i);
  o.field302 = rd32(r, 0x302);
  o.field306 = rd32(r, 0x306);
  o.field30a = rd32(r, 0x30a);
  o.field30e = rdi32(r, 0x30e);
  o.scriptFlagsChild = rd32(r, 0x312);
  out.connDest = rdi32(r, 0x302);
  out.moverChild = rdi32(r, 0x312);
  out.animRecNear = rdi32(r, 0x306);
  out.animRecFar = rdi32(r, 0x30a);
  o.connRadius = rdf32(r, 0x30e);
  o.connState = rd8(r, 0x312);
  o.connStateHi = rd8(r, 0x313);
  out.connSound316 = rdi32(r, 0x316);
  out.connSound31a = rdi32(r, 0x31a);
  out.connSound31e = rdi32(r, 0x31e);
  out.connSound322 = rdi32(r, 0x322);
  out.homingPrefix = rdi32(r, 0x302);
  o.homingDigitOfs = rdi32(r, 0x306);
  // The 0xc6 element-hp pools (i16 views of +0x30e..+0x32d).
  o.elemHp.assign(8, 0);
  o.elemThresh.assign(8, 0);
  for (int i = 0; i < 8; ++i) {
    o.elemHp[i] = rdi16(r, 0x30e + 2 * i);
    o.elemThresh[i] = rdi16(r, 0x31e + 2 * i);
  }
  o.col.elemMaskA = rd32(r, 0x326);
  o.connMaskLock = rd32(r, 0x326);
  o.connMaskHC = rd32(r, 0x32a);
  // +0x302 pointer view — the command-runner target is armed at
  // runtime (FUN_0046aa30 writes the player pos), never persisted.
  o.field302ptr = nullptr;
}

// The post-alloc fixup half — every saved reference in the record,
// resolved after the id table is complete (FUN_00426f34 + the BULL-
// style inline work).
void fixupObjectRecord(DynamicObject& o, const ObjRefs& s, Refs& refs) {
  TraversalArena* home = refs.arenaOff(s.arena);
  if (home) o.arena = &home->dyn;
  o.field138 = refs.objId(s.field138);
  o.field278 = refs.objId(s.field278);
  o.field2b8 = refs.objId(s.field2b8);
  o.pendingArena = s.pendingArena == -1 ? nullptr
                                      : &refs.arenaOff(s.pendingArena)->dyn;
  o.fieldEC = refs.cmiOff(s.fieldEC);
  o.field108 = refs.cmiOff(s.field108);
  o.field10c = refs.cmiOff(s.field10c);
  o.field110 = refs.cmiOff(s.field110);
  o.animRec = refs.cmiOff(s.animRec);
  o.field230 = refs.cmiOff(s.field230);
  for (int i = 0; i < 4; ++i) {
    o.scriptRetPc[i] = refs.cmiOff(s.retPc[i]);
    o.scriptSavedPc[i] = refs.cmiOff(s.savedPc[i]);
  }
  o.animSoundName = refs.cmiName(s.animSoundName);
  (void)refs.cmiOff(s.field150);   // int32-marker fields — validated
  (void)refs.cmiOff(s.field154);   // only (raw offset kept in-place)
  o.field15c = refs.cmiName(s.field15c);
  // +0x2b0/+0x2b4 — nonzero is rebound to the 0x49b3ac/0x49b3d0
  // statics (a generic "has contact" token); the port keeps the
  // sentinel semantics with its own statics.
  static const CollisionPoly kRestoredPoly{};
  static const CollisionNode kRestoredNode{};
  o.field2b0 = s.hasField2b0 ? &kRestoredPoly : nullptr;
  o.field2b4 = s.hasField2b4 ? &kRestoredNode : nullptr;
  // Union views — FUN_00426f34 resolves each under its owner flag:
  // +0x14a&0x10 connector (+0x302 dest arena, +0x306/+0x30a anim
  // records, +0x316..+0x322 sound strings), +0x14a&0x20 mover
  // (+0x312 child object), +0x149&0x20 homing (+0x302 name prefix).
  // Other owners (rawMatrix 0x148&0x40, speed lanes, command runner)
  // keep the raw u32 bits in field302/306/30a/30e.
  if (o.col.flags14a & 0x10) {
    o.connDest = refs.arenaOff(s.connDest);
    o.animRecNear = refs.cmiOff(s.animRecNear);
    o.animRecFar = refs.cmiOff(s.animRecFar);
    o.connSound316 = refs.cmiName(s.connSound316);
    o.connSound31a = refs.cmiName(s.connSound31a);
    o.connSound31e = refs.cmiName(s.connSound31e);
    o.connSound322 = refs.cmiName(s.connSound322);
  }
  if (o.col.flags14a & 0x20) o.moverChild = refs.objId(s.moverChild);
  if (o.col.flags149 & 0x20) o.homingPrefix = refs.cmiName(s.homingPrefix);
}

// ---------------------------------------------------------------------------
// AREN head — the runtime-view fields of the 0x466 record. Regions
// the loader skips (name/links/geometry +0x00..0x43, object list
// +0x5c..0x6b, fan list +0x446..0x461) keep their fresh-load values.
// ---------------------------------------------------------------------------

void applyArenaHead(std::span<const std::byte> p, TraversalArena& a,
                    Refs& refs) {
  a.flags44 = rd32(p, 0x44);
  a.objectsSpawned = (a.flags44 & 4) != 0;
  for (int i = 0; i < 4; ++i)
    a.objVars48[i] = rdf32(p, 0x48 + 4 * i);
  a.flags58 = rd32(p, 0x58);
  for (int i = 0; i < 16; ++i) {
    a.surface.config[i] = rd8(p, 0x6c + i);
    a.surface.handlerMask[i] = rd8(p, 0x7c + i);
    a.surface.handlerOff[i] = rd32(p, 0x8c + 4 * i);
    a.surface.counters[i] = rdi32(p, 0xcc + 4 * i);
  }
  a.surface.opMaskA = rd32(p, 0x10c);
  a.surface.opMaskB = rd32(p, 0x110);
  a.surface.marks = rd32(p, 0x114);
  a.scalar = rdf32(p, 0x462);
  (void)refs;
}

// The embedded +0x118 record: the object field map into eventLatch,
// plus the script-ctx subset into arena.script (the original has one
// 814B record; the port carries two views of it).
void applyEmbeddedRecord(std::span<const std::byte> rec,
                         TraversalArena& a, Refs& refs, ObjRefs& out) {
  (void)refs;
  applyObjectRecord(rec, a.eventLatch, out);
  a.eventLatch.arena = &a.dyn;
  TraversalScriptState& s = a.script;
  const std::int32_t pc = rdi32(rec, 0x108);
  s.pcImageOff = (pc == -1) ? 0u : static_cast<std::uint32_t>(pc);
  s.active = s.pcImageOff != 0;
  a.hasScriptObject = s.active;
  s.waitSeconds = rdf32(rec, 0x22c);
  const std::int32_t resume = rdi32(rec, 0x230);
  s.waitResumeImageOff =
      (resume == -1) ? 0u : static_cast<std::uint32_t>(resume);
  s.running = rd8(rec, 0x21e);
  s.eventByte = rd8(rec, 0x21d);
  s.var11a = rd8(rec, 0x11a);
  s.field30e = rdf32(rec, 0x30e);
  s.flagsLocal = rd32(rec, 0x244);
  s.flagsChild = rd32(rec, 0x312);
  for (int i = 0; i < 16; ++i)
    s.locals[i] = rdf32(rec, 0x234 + 4 * i);
  s.callDepth = rdi32(rec, 0x248);
  for (int i = 0; i < 4; ++i) {
    const std::int32_t rp = rdi32(rec, 0x24c + 4 * i);
    const std::int32_t sp = rdi32(rec, 0x25c + 4 * i);
    s.retPc[i] = static_cast<std::uint32_t>(rp);
    s.savedPc[i] = static_cast<std::uint32_t>(sp);
    s.marker[i] = rd16(rec, 0x26c + 2 * i);
  }
}

// ---------------------------------------------------------------------------
// FAND (72B) — one SurfaceRecord; +0x04 owner is an arena offset,
// +0x08 name a CMI offset (both resolved in the fixup pass — the
// original walks the arena's +0x45e list for them).
// ---------------------------------------------------------------------------

void applyFand(std::span<const std::byte> p, SurfaceRecord& f) {
  f.owner = rd32(p, 0x04);
  f.name = rd32(p, 0x08);
  f.surfType = rd8(p, 0x0c);
  f.f10 = rd32(p, 0x10);
  f.kind = rdi32(p, 0x14);
  f.rate = rdf32(p, 0x18);
  f.queryMask = rd32(p, 0x1c);
  for (int i = 0; i < 6; ++i) f.v[i] = rdf32(p, 0x20 + 4 * i);
  f.uvAcc[0] = rdf32(p, 0x38);
  f.uvAcc[1] = rdf32(p, 0x3c);
  f.target = rdf32(p, 0x40);
  f.ramp = rdf32(p, 0x44);
}

// ---------------------------------------------------------------------------
// BULL (252B) — one PlayerShot slot. The +0x2c..0xbb region is the
// saved flight scratch (no typed consumer — the tick rederives dir
// from yaw/pitch and rebuilds the tail); +0x18/+0xd8 are references
// resolved in the fixup pass, +0xd4 the fly-callback enum.
// ---------------------------------------------------------------------------

struct BullRefs {
  std::int32_t arena = -1;
  std::int32_t homeObj = 0;
  std::int32_t homeElemIdx = 0;
  std::int32_t flyEnum = 0;
  bool hadHomeElem = false;
};

void applyBull(std::span<const std::byte> p, PlayerShot& s, BullRefs& r,
               FullRestoreReport& rep) {
  s.state = rdi32(p, 0x00);
  s.yawDeg = rdf32(p, 0x04);
  s.pitchDeg = rdf32(p, 0x08);
  s.spinDeg = rdf32(p, 0x0c);
  s.lifetime = rdi32(p, 0x10);
  s.dyingTimer = rdi32(p, 0x14);
  r.arena = rdi32(p, 0x18);
  // +0x1c — the class-record pointer, rebound by FUN_00461798.
  for (int i = 0; i < 3; ++i) s.pos[i] = rdf32(p, 0x20 + 4 * i);
  rep.unmapped.push_back({"BULL", 0x2c, 0x90}); // flight scratch
  s.tailLen = rdf32(p, 0xbc);
  for (int i = 0; i < 3; ++i) s.tail[i] = rdf32(p, 0xc0 + 4 * i);
  s.fieldCc = rdf32(p, 0xcc);
  s.type = rdi32(p, 0xd0);
  r.flyEnum = rdi32(p, 0xd4);
  r.homeObj = rdi32(p, 0xd8);
  r.hadHomeElem = rdi32(p, 0xdc) != 0;
  r.homeElemIdx = rdi32(p, 0xe0);
  s.speedH = rdf32(p, 0xe4);
  s.yawAccum = rdf32(p, 0xe8);
  // +0xec — flight scratch (unmapped).
  s.speedV = rdf32(p, 0xf0);
  s.remnantIdx = rdi32(p, 0xf4);
  s.flags = rd32(p, 0xf8);
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_00427218
// ---------------------------------------------------------------------------

SaveError applyFullSaveToTraversal(const SaveGame& save,
                                   const DataRoot& root,
                                   ProgressionSession& sess,
                                   TraversalRuntime& rt,
                                   FullRestoreReport* report,
                                   std::string* detail) {
  FullRestoreReport local;
  FullRestoreReport& rep = report ? *report : local;
  auto fail = [&](SaveError e, const char* msg) {
    if (detail) *detail = msg;
    return e;
  };

  if (!save.game.full())
    return fail(SaveError::kMissingGame,
                "save is header-only (modeField < 1000)");
  rep.modeField = save.game.modeField;
  rep.mode = save.game.mode();
  rep.levelId = save.game.levelId;

  // FUN_004278c0's GAME field stores (they precede the full-save call).
  sess.levelId = save.game.levelId;
  sess.health = save.game.health;
  sess.deathCount = save.game.deathCount;
  sess.field54163b = save.game.field54163b;
  sess.mode = save.game.mode();

  // Packet presence — the loader reads these unconditionally.
  const SavePacketView* more = save.find(saveTag('M', 'O', 'R', 'E'));
  const SavePacketView* play = save.find(saveTag('P', 'L', 'A', 'Y'));
  const SavePacketView* damp = save.find(saveTag('D', 'A', 'M', 'P'));
  const SavePacketView* came = save.find(saveTag('C', 'A', 'M', 'E'));
  if (!more || more->payload.size() < 52)
    return fail(SaveError::kTruncatedPacket, "MORE missing/short");
  if (!play || play->payload.size() < 239)
    return fail(SaveError::kTruncatedPacket, "PLAY missing/short");
  if (!damp || damp->payload.size() < 724)
    return fail(SaveError::kTruncatedPacket, "DAMP missing/short");
  if (!came || came->payload.size() < 200)
    return fail(SaveError::kTruncatedPacket, "CAME missing/short");

  // --- FUN_0041b7b4 + FUN_004346e8(2): the fresh level load with
  // the spawn/activation block suppressed (param bit1 — OBSERVED).
  const int dir = progressionLevelDir(sess.levelId);
  if (dir < 0)
    return fail(SaveError::kBadLevelId,
                "level id outside the 0x4999e8 table");
  char base[64];
  std::snprintf(base, sizeof base, "TRAVERSE/LEVEL%d/LEVEL%d", dir, dir);
  const auto le = traversalRuntimeLoad(
      root, std::string(base) + ".DTI", std::string(base) + ".CMI",
      std::string(base) + "O.MTO", rt, detail,
      kTraversalLoadSuppressSpawn);
  if (le != TraversalLoadError::kOk)
    return fail(SaveError::kReadFail,
                detail ? detail->c_str() : "level load failed");

  // --- MORE[0] identity: the loaded .CMI blob length (0x54c680).
  // The original's blob view starts at file +4 (the length prefix is
  // not part of the image — the same +4 base the script VM uses);
  // saved CMI offsets are relative to that base (OBSERVED: 1.SAV
  // carries 0x107901 = LEVEL7.CMI size - 4).
  const std::size_t cmiImageSize =
      rt.level.cmiBytes.size() >= 4 ? rt.level.cmiBytes.size() - 4 : 0;
  rep.identityOk = rd32(more->payload, 0) == cmiImageSize;
  if (!rep.identityOk)
    return fail(SaveError::kPacketSize,
                "MORE level-length does not match the loaded .CMI");

  Refs refs{&rt, rt.level.cmiBytes.data() + 4, cmiImageSize,
            &rep, {}};

  // --- MORE/PLAY globals (post-load — the loader's store ladder).
  std::int32_t anchorId = 0;
  applyMore(more->payload, rt, refs, anchorId);
  applyPlay(play->payload, rt, sess);

  // --- DAMP + CAME.
  DampRefs dr;
  applyDamp(damp->payload, rt, dr, rep);
  applyCame(came->payload, rt);
  // A 212B CAME packet covers the +0xc8 scale tail; 200B leaves the
  // 1.0f defaults (handled inside applyCame).

  // --- Per-arena AREN + ALIE + FAND (stream order; the original
  // iterates the arena array and consumes the packet chain). The id
  // table is keyed by each record's stamped +0x7c save id — the
  // FUN_004282dc scan's materialization.
  std::vector<std::pair<DynamicObject*, ObjRefs>> objFixups;
  std::vector<std::pair<SurfaceRecord*, TraversalArena*>> fanFixups;
  std::size_t arenaIdx = 0;
  TraversalArena* curArena = nullptr;
  for (const SavePacketView& pkt : save.packets) {
    if (pkt.tag == saveTag('A', 'R', 'E', 'N')) {
      ++rep.arenPackets;
      if (arenaIdx >= rt.arenas.size()) {
        rep.warnings.emplace_back("AREN beyond runtime arena count");
        curArena = nullptr;
        continue;
      }
      curArena = rt.arenas[arenaIdx].get();
      ++arenaIdx;
      const char* nameSrc =
          reinterpret_cast<const char*>(pkt.payload.data());
      std::size_t nameLen = 0;
      while (nameLen < 9 && nameSrc[nameLen] != '\0') ++nameLen;
      const std::string savedName(nameSrc, nameLen);
      if (savedName != curArena->name) {
        ++rep.arenNameMismatch;
        rep.warnings.emplace_back("AREN name " + savedName +
                                  " != runtime " + curArena->name);
      }
      applyArenaHead(pkt.payload, *curArena, refs);
      // The embedded +0x118 record rides inside AREN; its stamped
      // +0x7c id precedes the objects' in the global sequence.
      ObjRefs er;
      applyEmbeddedRecord(pkt.payload.subspan(0x118, 0x32e),
                          *curArena, refs, er);
      if (er.saveId > 0) refs.byId[er.saveId] = &curArena->eventLatch;
      else rep.warnings.emplace_back("embedded record has no save id");
      objFixups.emplace_back(&curArena->eventLatch, er);
      ++rep.arenApplied;
    } else if (pkt.tag == saveTag('A', 'L', 'I', 'E')) {
      if (!curArena) {
        rep.warnings.emplace_back("ALIE without a preceding AREN");
        continue;
      }
      DynamicObject& o = curArena->dyn.allocFront();
      ObjRefs r;
      applyObjectRecord(pkt.payload, o, r);
      if (r.saveId > 0) refs.byId[r.saveId] = &o;
      else rep.warnings.emplace_back("ALIE record has no save id");
      objFixups.emplace_back(&o, r);
      ++rep.objectsAllocated;
      if (r.field108 != -1) ++rep.scriptedObjects;
    } else if (pkt.tag == saveTag('F', 'A', 'N', 'D')) {
      if (!curArena) {
        rep.warnings.emplace_back("FAND without a preceding AREN");
        continue;
      }
      // FUN_00412cb8 — push-front onto the arena's +0x45e list (the
      // original draws from the 0x48-record freelist; the port's
      // records are individually owned, freed by ~TraversalArena).
      auto* rec = new SurfaceRecord();
      applyFand(pkt.payload, *rec);
      rec->next = curArena->surface.records;
      curArena->surface.records = rec;
      fanFixups.emplace_back(rec, curArena);
      ++rep.fansAllocated;
    }
  }

  // --- BULL x3 -> the 0x540ed4 pool.
  std::vector<BullRefs> bullRefs;
  for (const SavePacketView& pkt : save.packets) {
    if (pkt.tag != saveTag('B', 'U', 'L', 'L')) continue;
    if (rep.shotSlots >= 3) {
      rep.warnings.emplace_back("BULL beyond the 3-slot pool");
      break;
    }
    PlayerShot& s = rt.shots[rep.shotSlots];
    BullRefs r;
    applyBull(pkt.payload, s, r, rep);
    bullRefs.push_back(r);
    ++rep.shotSlots;
    if (s.state != 0) ++rep.shotsActive;
  }

  // === The fixup tail (0x427594..0x4278b0) ===

  // Global refs — MORE/DAMP staged fields resolved in place.
  rt.fieldB85c = refs.objId(anchorId);
  rt.cur = refs.arenaOff(dr.cur);
  rep.curArenaIndex = rt.cur ? rt.cur->index : -1;
  rt.cs.floorObj = dr.floorObj ? &refs.objId(dr.floorObj)->col : nullptr;
  rt.vert.blocker0 = dr.floorObj ? 1u : 0u;
  rt.partner = refs.arenaOff(dr.partner);
  rep.partnerArenaIndex = rt.partner ? rt.partner->index : -1;
  rt.focusObj = refs.objId(dr.focusObj);
  rt.loadArena = refs.arenaOff(dr.loadArena);
  rep.loadArenaIndex = rt.loadArena ? rt.loadArena->index : -1;
  rt.lruA = refs.arenaOff(dr.lruA);
  rt.lruB = refs.arenaOff(dr.lruB);
  (void)refs.arenaOff(dr.d6c);        // 0x540d6c — resolved then
                                      // cleared by the loader
  DynamicObject* rideObj = refs.objId(dr.rideObj);
  rt.cs.rideObj = rideObj ? &rideObj->col : nullptr;
  rt.vert.moveBlocker0 = rideObj ? 1u : 0u;
  rt.cmdObj5c = refs.objId(dr.cmdObj5c);
  rt.cmdObj60 = refs.objId(dr.cmdObj60);
  DynamicObject* lastObj = refs.objId(dr.lastObjContact);
  rt.cs.lastObjContact = lastObj ? &lastObj->col : nullptr;
  DynamicObject* exclObj = refs.objId(dr.excludeObj);
  rt.cs.excludeObj = exclObj ? &exclObj->col : nullptr;
  rt.eventTimerObj = refs.objId(dr.eventTimerObj);
  // 0x540ebc — an arena-offset gate resolved in place (nonzero after
  // resolution arms the pending snap).
  rt.pendingViewSnap =
      (refs.arenaOff(dr.pendingViewSnap) != nullptr) ? 1 : 0;
  // The DAMP-side clears the loader performs:
  //   0x540d10/14/1c/20 (no native fields), 0x540dd0..0x540e0c
  //   (no native fields), 0x540ea8 (deferred-free countdown —
  //   no producer yet), pending lists 0x540d3c/40/68/6c.

  // Per-arena + per-object record fixups (FUN_00426f34 each).
  for (auto& [obj, r] : objFixups) fixupObjectRecord(*obj, r, refs);
  // The embedded pseudo-object binds +0x0c to the class-table head in
  // the original (arena+0x124 = 0x4edcc0 — OBSERVED at 0x42772f): the
  // port binds model 0's element view the same way FUN_0045a3b0 does
  // for index 0 (shared record semantics — no copy).
  for (auto& a : rt.arenas) {
    if (const RuntimeModel* m0 = traversalModelFor(0, &rt.level)) {
      a->eventLatch.elemSet = m0->elementSet();
      a->eventLatch.col.elements = &a->eventLatch.elemSet;
    }
  }

  // FAND owner (+0x04 -> arena index token) + name (+0x08 — kept as
  // the raw CMI offset: the port's u32 key field has no string view,
  // and the offset is the lossless native-equivalent token).
  for (auto& [f, owner] : fanFixups) {
    f->owner = refs.arenaOff(static_cast<std::int32_t>(f->owner))
                   ? static_cast<std::uint32_t>(owner->index)
                   : 0;
    (void)refs.cmiOff(static_cast<std::int32_t>(f->name)); // validate
  }

  // BULL rebinds — +0x18 arena, +0xd4 callback enum, +0xd8 object,
  // +0xdc element rebuild (gated on state+saved dc+resolved object,
  // and on the target having a bound element set — OBSERVED saves
  // carry state==0 everywhere, so the path is dormant). +0x1c is the
  // class record — FUN_00461798 rebinds it FROM TYPE via the static
  // name table, never from the saved value.
  static const char* const kShotClassNames[5] = {
      nullptr, "SW_HOME", "SW_SGREN", "SW_HGREN", "SW_LGREN"};
  for (std::size_t i = 0; i < bullRefs.size(); ++i) {
    PlayerShot& s = rt.shots[i];
    const BullRefs& r = bullRefs[i];
    s.arena = refs.arenaOff(r.arena);
    s.classIdx = (s.type >= 1 && s.type <= 4)
        ? rt.level.enemies.indexOf(kShotClassNames[s.type])
        : -1;                        // -1 = the 0x4edd48 default record
    switch (r.flyEnum) {
      case 1: s.flyKind = kShotFlyGrenade; break;
      case 2: s.flyKind = kShotFlyLobbed; break;
      case 3: s.flyKind = kShotFlyRibbon; break;
      default: s.flyKind = kShotFlyTracer; break;
    }
    DynamicObject* home = refs.objId(r.homeObj);
    s.homeObj = home;
    s.homeElemIdx = r.homeElemIdx;
    s.homeElem = nullptr;
    if (s.state != 0 && r.hadHomeElem && home != nullptr &&
        home->col.elements != nullptr &&
        r.homeElemIdx >= 0 &&
        r.homeElemIdx < home->col.elements->count) {
      s.homeElem = &home->col.elements->elems[r.homeElemIdx];
    }
  }

  // The post-fixup clears (pending lists + the load-arena slot).
  rt.loadArena = nullptr;            // consumed by the c34 call below
  rt.partnerActive = false;          // rebuilt by the attach calls
  rt.cs.carrier = rt.partner ? &rt.partner->dyn.col : nullptr;
  rt.cs.carrierBusy = 0;             // 0x540d3c pending head — cleared

  // === Attach tail: FUN_00432c34(d30) -> FUN_00432d9c(cur) ->
  // FUN_00432d9c(partner) -> FUN_004348d4 (audio seam — no-op). ===
  if (rep.loadArenaIndex >= 0 && rep.loadArenaIndex != rep.curArenaIndex &&
      rep.loadArenaIndex != rep.partnerArenaIndex) {
    // The saved stream-load arena gets the temporary-activation path;
    // observed saves carry d30 == cur, so this is normally covered by
    // the cur attach below.
    traversalEnsureLoaded(rt, *rt.arenas[rep.loadArenaIndex]);
    // FUN_00432c34 reaches FUN_004321dc — the temp-activated arena's
    // named objects get the +0x0c element-set rebind as well.
    for (auto& o : rt.arenas[rep.loadArenaIndex]->dyn.storage)
      if (o->col.named) objectArenaActivate(rt, *o);
  }
  if (rt.cur) {
    traversalMigrateInto(rt, *rt.cur);      // FUN_00432d9c pull-in
    traversalAttachSideEffects(rt, *rt.cur); // ensure + ca8 + carrier
    rt.cs.arena = &rt.cur->dyn.col;
    rt.cs.surface = &rt.cur->surface;
  } else {
    rep.warnings.emplace_back("no current arena restored (DAMP+0x4c)");
    return fail(SaveError::kPacketSize, "DAMP current arena missing");
  }
  if (rt.partner) {
    traversalAttachPartner(rt, *rt.partner);
  }
  rep.playerPos[0] = rt.cs.pos[0];
  rep.playerPos[1] = rt.cs.pos[1];
  rep.playerPos[2] = rt.cs.pos[2];
  rep.playerYawDeg = rt.motion.yawDeg;
  rep.health = rt.fieldHealth;
  return SaveError::kOk;
}

} // namespace mdk
