// save_full_write.cpp — Phase 14D: the original-compatible full-save
// writer (FUN_00427ed4 + FUN_00426e98 + FUN_00426a0c + FUN_00427970 +
// FUN_00426440), emitting the stream applyFullSaveToTraversal
// consumes. See save_full_write.h for the route and token evidence.
//
// NATIVE PORT DECISIONS:
//   - Packet images are built explicitly — the native struct layouts
//     differ from the original records, so there is no memcpy source.
//   - Render-rebuilt/scratch regions (+0x64..0xab object bounds and
//     parent matrix, +0x2c..0xbb BULL flight scratch, the DAMP frame-
//     stat trackers) emit 0 — the original writes live garbage there
//     and no consumer reads it back (the loader either skips the range
//     or treats it as scratch).
//   - The loader-cleared fields are emitted as 0 except +0x140
//     (carrierBusy — a live native field the original memcpys; the
//     loader discards it) — parity with the original's staging.
//   - CMI-string fields are re-emitted by scanning the loaded image
//     for the operand bytes ({u8 len}{chars}{NUL}); the stored offset
//     is the char position (operand+1). A string with no image match
//     emits -1 + a warning (the original's field was a bytecode
//     pointer — a runtime-only string has no saved representation).
//   - PLAY regions with no typed mirror (the +0xeb tail and the
//     UNKNOWN-marked spans) carry rt.savePlayBlock — the verbatim
//     last-loaded block, the closest available source for globals
//     the port does not model.

#include "core/save_full_write.h"

#include <bit>
#include <cstring>
#include <unordered_map>

#include "core/dynamic_objects.h"
#include "core/player_fire.h"
#include "core/progression_runtime.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

constexpr std::uint32_t kArenaStride = 0x466;

// LE writers into a fixed packet image.
struct Img {
  std::vector<std::byte> b;
  explicit Img(std::size_t n) : b(n, std::byte(0)) {}

  void raw(std::uint32_t off, const void* p, std::size_t n) {
    std::memcpy(b.data() + off, p, n);
  }
  void u8(std::uint32_t off, std::uint8_t v) { b[off] = std::byte(v); }
  void u16(std::uint32_t off, std::uint16_t v) {
    b[off] = std::byte(v & 0xff);
    b[off + 1] = std::byte((v >> 8) & 0xff);
  }
  void u32(std::uint32_t off, std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
      b[off + k] = std::byte((v >> (8 * k)) & 0xff);
  }
  void i32(std::uint32_t off, std::int32_t v) {
    u32(off, static_cast<std::uint32_t>(v));
  }
  void f32(std::uint32_t off, float v) {
    u32(off, std::bit_cast<std::uint32_t>(v));
  }
  void i16(std::uint32_t off, std::int16_t v) {
    u16(off, static_cast<std::uint16_t>(v));
  }
};

// The writer-side token table — FUN_004262b0/FUN_00426738's inverse
// conversions plus the sequential +0x7c id stamp.
struct Toks {
  const TraversalRuntime& rt;
  FullWriteReport* rep;
  const std::byte* cmiImg = nullptr;  // file + 4 (the loader's base)
  std::size_t cmiImgSize = 0;
  // object address -> save id. Keyed by address; CollisionObject
  // pointers hash to the same slot because col is the first member.
  std::unordered_map<const void*, std::int32_t> ids;

  // Non-fatal degradation (documented -1 sentinel cases).
  void warn(const char* what, const void* p) {
    if (!rep) return;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s: unrepresentable ref %p", what, p);
    rep->warnings.emplace_back(buf);
  }

  // Gameplay-authoritative ref that cannot be represented — the
  // write must fail rather than silently serialize a null token.
  // Emission continues so one call reports every bad ref.
  std::vector<std::string> errs;
  void failRef(const char* what, const void* p) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s: unrepresentable ref %p", what, p);
    errs.emplace_back(buf);
  }

  // Arena record ptr -> position*0x466 (-1 = NULL). The loader
  // resolves off/0x466 against the arena vector, so the token is the
  // slot index — arena.index is the DTI table id, not the slot.
  std::int32_t arenaTok(const TraversalArena* a) {
    if (a == nullptr) return -1;
    for (std::size_t i = 0; i < rt.arenas.size(); ++i)
      if (rt.arenas[i].get() == a)
        return static_cast<std::int32_t>(i) *
               static_cast<std::int32_t>(kArenaStride);
    failRef("arena", a);
    return -1;
  }
  std::int32_t arenaTok(const DynamicArena* d) {
    if (d == nullptr) return -1;
    if (d->owner == nullptr) { failRef("arena", d); return -1; }
    return arenaTok(d->owner);
  }

  // CMI image ptr -> offset (-1 = NULL or out-of-image).
  std::int32_t cmiTok(const void* p) {
    if (p == nullptr) return -1;
    const auto* q = static_cast<const std::byte*>(p);
    if (q < cmiImg || static_cast<std::size_t>(q - cmiImg) >= cmiImgSize) {
      failRef("cmi", p);
      return -1;
    }
    return static_cast<std::int32_t>(q - cmiImg);
  }

  // CMI string operand -> the char-position offset (-1 = none).
  // The bytecode operand is {u8 len}{chars}{NUL} (len includes the
  // terminator); the original stores the chars pointer, so the saved
  // offset is match + 1.
  std::int32_t cmiStrTok(const std::string& s) {
    if (s.empty()) return -1;
    if (s.size() + 1 > 0xff) {
      warn("cmi-str", nullptr);
      return -1;
    }
    const auto len = static_cast<std::uint8_t>(s.size() + 1);
    const std::size_t n = s.size() + 1;  // len byte + chars + NUL span
    for (std::size_t i = 0; i + n <= cmiImgSize; ++i) {
      if (std::uint8_t(cmiImg[i]) != len) continue;
      if (std::memcmp(cmiImg + i + 1, s.data(), s.size()) != 0) continue;
      if (cmiImg[i + 1 + s.size()] != std::byte(0)) continue;
      return static_cast<std::int32_t>(i + 1);  // the char position
    }
    // Fallback: a bare NUL-terminated string (non-operand storage —
    // the saved token is the char position either way).
    for (std::size_t i = 0; i + s.size() + 1 <= cmiImgSize; ++i) {
      if (std::memcmp(cmiImg + i, s.data(), s.size()) != 0) continue;
      if (cmiImg[i + s.size()] != std::byte(0)) continue;
      return static_cast<std::int32_t>(i);
    }
    // Not CMI-resident (runtime literals like "DUMMY", model names).
    // The original saved ptr-cmiBase — a wild offset that resolved
    // back to its own static string in the same address space; the
    // port has no equivalent, so -1 with a warning (DEVIATION).
    warn("cmi-str", nullptr);
    return -1;
  }

  // Object ptr -> its stamped +0x7c id (0 = NULL; a non-null pointer
  // to a record outside the serialized set fails the write).
  std::int32_t objTok(const DynamicObject* o) {
    if (o == nullptr) return 0;
    auto it = ids.find(o);
    if (it == ids.end()) {
      failRef("object", o);
      return 0;
    }
    return it->second;
  }
  // CollisionObject refs (cs.floorObj &co.) — col is the first member,
  // so the address IS the object's.
  std::int32_t objTok(const CollisionObject* c) {
    return objTok(reinterpret_cast<const DynamicObject*>(c));
  }
};

// ---------------------------------------------------------------------------
// The shared 814B object record (ALIE payloads and each AREN's
// embedded +0x118 record — one original layout, two native views).
// `arenaScript` overrides the +0x108/+0x230/+0x22c/+0x234.. script
// fields for the embedded form, which the port keeps in
// TraversalArena::script rather than on eventLatch.
// ---------------------------------------------------------------------------

void emitObjectRecord(Img& img, const DynamicObject& o, Toks& tk,
                      const TraversalScriptState* arenaScript) {
  // +0x00 — the +0x68 list link: runtime-owned, emits 0.
  img.u16(0x04, o.enemyIndex);
  img.u8(0x06, o.col.named ? 1 : 0);
  img.u8(0x07, o.col.field07);
  img.i32(0x08, o.health);
  // +0x0c — the model deep-copy: cleared on both sides (OBSERVED).
  for (int i = 0; i < 3; ++i) {
    img.f32(0x10 + 4 * i, o.pos[i]);
    img.f32(0x1c + 4 * i, o.field1c[i]);
  }
  img.f32(0x28, o.field28);
  img.f32(0x2c, o.field2c);
  img.f32(0x30, o.field30);
  img.f32(0x34, o.field34);
  img.f32(0x38, o.field38);
  img.f32(0x3c, o.field3c);
  img.f32(0x40, o.field40);
  img.f32(0x44, o.field44);
  img.f32(0x48, o.field48);
  img.f32(0x4c, o.yawDeg);
  img.f32(0x50, o.prevYawDeg);
  img.f32(0x54, o.pitchDeg);
  img.f32(0x58, o.col.scale);
  img.f32(0x5c, o.zBias);
  img.i32(0x60, tk.arenaTok(o.arena));
  // +0x64..+0x78 screen bounds / +0x80..+0xa8 parent 3x4 — per-frame
  // render rebuild, no gameplay state: emit 0.
  img.i32(0x7c, tk.objTok(&o));      // the record's own save id
  // +0xac..+0xdb — the collision 3x4 {basis row, origin}.
  for (int i = 0; i < 3; ++i) {
    img.f32(0xac + 0x10 * i + 0, o.col.xform[i * 3 + 0]);
    img.f32(0xac + 0x10 * i + 4, o.col.xform[i * 3 + 1]);
    img.f32(0xac + 0x10 * i + 8, o.col.xform[i * 3 + 2]);
    img.f32(0xac + 0x10 * i + 12, o.col.origin[i]);
  }
  img.f32(0xdc, o.animAcc);
  img.f32(0xe0, o.animRate);
  img.i16(0xe4, o.animFrame);
  img.i16(0xe6, o.fieldE6);
  img.f32(0xe8, o.fieldE8);
  img.i32(0xec, tk.cmiTok(o.fieldEC));   // path record
  img.f32(0xf0, o.fieldF0);
  for (int i = 0; i < 3; ++i) img.f32(0xf4 + 4 * i, o.fieldF4[i]);
  img.f32(0x100, o.field100);
  img.f32(0x104, o.field104);
  img.i32(0x108, tk.cmiTok(o.field108));
  img.i32(0x10c, tk.cmiTok(o.field10c));
  img.i32(0x110, tk.cmiTok(o.field110));
  img.i32(0x114, tk.cmiTok(o.animRec));
  img.i16(0x118, o.animLatch);
  img.u8(0x11a, o.field11a);
  img.u8(0x11b, o.field11b);
  img.u8(0x11c, static_cast<std::uint8_t>(o.behaviorByte));
  img.u8(0x11e, o.field11e);
  img.u8(0x11f, o.field11f);
  for (int i = 0; i < 3; ++i) {
    img.f32(0x120 + 4 * i, o.field120[i]);
    img.f32(0x12c + 4 * i, o.field12c[i]);
  }
  img.i32(0x138, tk.objTok(o.field138));
  img.f32(0x13c, o.bankDeg);
  img.i32(0x140, tk.cmiStrTok(o.animSoundName));
  img.i16(0x144, o.animSoundMark);
  img.u16(0x146, o.spawnId);
  // +0x148..+0x14c — the flag dword; flags148's high byte IS +0x149.
  img.u8(0x148, static_cast<std::uint8_t>(o.col.flags148 & 0xff));
  img.u8(0x149, o.col.flags149);
  img.u8(0x14a, o.col.flags14a);
  img.u8(0x14b, o.col.flags14b);
  img.u8(0x14c, o.col.flags14c);
  img.i32(0x150, o.field150);   // raw CMI-offset markers (load keeps
  img.i32(0x154, o.field154);   //   the saved token verbatim)
  // +0x158 — cleared on both sides.
  img.i32(0x15c, tk.cmiStrTok(o.field15c));
  // +0x160..+0x17f — the eight element pointers, cleared both sides.
  for (int i = 0; i < 3; ++i) {
    img.f32(0x180 + 4 * i, o.prevPos[i]);
    img.f32(0x18c + 4 * i, o.field18c[i]);
  }
  for (int i = 0; i < 6; ++i) img.f32(0x198 + 4 * i, o.col.aabb[i]);
  for (int i = 0; i < 8; ++i)
    for (int k = 0; k < 3; ++k)
      img.f32(0x1b0 + (i * 3 + k) * 4, o.worldRef[i][k]);
  for (int i = 0; i < 3; ++i) img.f32(0x210 + 4 * i, o.field210[i]);
  img.u8(0x21c, o.field21c);
  img.u8(0x21d, o.field21d);
  img.u8(0x21e, o.field21e);
  img.u8(0x21f, o.flag21f);
  img.i32(0x220, o.field220);
  img.f32(0x224, o.field224);
  img.f32(0x228, o.field228);
  img.f32(0x22c, o.field22c);
  img.i32(0x230, tk.cmiTok(o.field230));
  // +0x234..+0x273 — locals[0..3], then the structured ctx fields
  // (locals[4..15] alias +0x244..+0x273 in the original; the flag/
  // stack/marker views are authoritative — var-mode-2 writes are
  // bounded to locals[0..3]).
  for (int i = 0; i < 4; ++i) img.f32(0x234 + 4 * i, o.scriptLocals[i]);
  img.u32(0x244, o.scriptFlagsLocal);
  img.i32(0x248, o.scriptCallDepth);
  for (int i = 0; i < 4; ++i) {
    img.i32(0x24c + 4 * i, tk.cmiTok(o.scriptRetPc[i]));
    img.i32(0x25c + 4 * i, tk.cmiTok(o.scriptSavedPc[i]));
  }
  for (int i = 0; i < 5; ++i) img.u16(0x26c + 2 * i, o.scriptMark[i]);
  img.u8(0x276, o.field276);
  img.u8(0x277, o.field277);
  img.i32(0x278, tk.objTok(o.field278));
  for (int i = 0; i < 6; ++i) img.f32(0x27c + 4 * i, o.clampBox[i]);
  for (int i = 0; i < 3; ++i) img.f32(0x294 + 4 * i, o.animImpulse[i]);
  img.u8(0x2a0, o.field2a0);
  img.u8(0x2a1, o.field2a1);
  img.u16(0x2a2, static_cast<std::uint16_t>(o.healthMirror2a2));
  img.f32(0x2a4, o.field2a4);
  img.f32(0x2a8, o.field2a8);
  img.f32(0x2ac, o.field2ac);
  // +0x2b0/+0x2b4 — the has-contact sentinels (nonzero -> statics).
  img.u32(0x2b0, o.field2b0 ? 1u : 0u);
  img.u32(0x2b4, o.field2b4 ? 1u : 0u);
  img.i32(0x2b8, tk.objTok(o.field2b8));
  img.i32(0x2bc, tk.arenaTok(o.pendingArena));
  img.f32(0x2c0, o.field2c0);
  img.f32(0x2c4, o.field2c4);
  img.u32(0x2c8, o.col.elemMaskB);
  img.u32(0x2cc, o.col.elemMaskLatch);   // OBSERVED (op 0x20): unmask latch
  img.u8(0x2d0, o.field2d0);
  img.u8(0x2d1, o.field2d1);
  for (int i = 0; i < 6; ++i) img.f32(0x2d2 + 4 * i, o.field2d2[i]);
  // +0x2ea..+0x301 — no proven consumer.

  // +0x302..+0x32d — the aliased union. The original memcpy's the
  // live lanes, then FUN_00426738 rewrites pointer slots in OBSERVED
  // order: mover child +0x312 (14a&0x20) -> connector fields
  // (14a&0x10) -> homing prefix +0x302 (149&0x20). The port emits the
  // verbatim lane views first, then overlays each flag-owned view in
  // the same order — one flag combination, one byte image, and the
  // loader derives every alias view back out of it.
  const bool isConn = (o.col.flags14a & 0x10) != 0;
  const bool isElem = (o.col.flags149 & 0x20) != 0;
  const bool isMover = (o.col.flags14a & 0x20) != 0;
  const bool rawMtx = (o.col.flags148 & 0x40) != 0;
  if (rawMtx) {
    for (int i = 0; i < 9; ++i) img.f32(0x302 + 4 * i, o.rawMatrix[i]);
  } else {
    img.u32(0x302, o.field302);
    img.u32(0x306, o.field306);
    img.u32(0x30a, o.field30a);
    if (isElem) {
      // The 0xc6 element pools own +0x30e..+0x32d for this owner.
      for (int i = 0; i < 8; ++i) {
        const std::int16_t hp =
            i < static_cast<int>(o.elemHp.size()) ? o.elemHp[i] : 0;
        const std::int16_t th =
            i < static_cast<int>(o.elemThresh.size()) ? o.elemThresh[i]
                                                    : 0;
        img.i16(0x30e + 2 * i, hp);
        img.i16(0x31e + 2 * i, th);
      }
    } else {
      img.i32(0x30e, o.field30e);
      // For a connector, +0x312's low u16 carries connState/Hi; the
      // high u16 keeps the verbatim scriptFlagsChild bits.
      img.u32(0x312,
              isConn ? (o.scriptFlagsChild & 0xffff0000u) |
                           static_cast<std::uint32_t>(o.connState) |
                           (static_cast<std::uint32_t>(o.connStateHi) << 8)
                     : o.scriptFlagsChild);
      // +0x316..+0x322 — lanes with no dedicated view; rawMatrix[5..8]
      // hold the loaded bits verbatim.
      img.f32(0x316, o.rawMatrix[5]);
      img.f32(0x31a, o.rawMatrix[6]);
      img.f32(0x31e, o.rawMatrix[7]);
      img.f32(0x322, o.rawMatrix[8]);
    }
  }
  // +0x326/+0x32a — the collision masks (alias elemThresh[4..7] for
  // element owners; the mask view is authoritative in the port).
  img.u32(0x326, o.connMaskLock);
  img.u32(0x32a, o.connMaskHC);
  if (isMover) img.i32(0x312, tk.objTok(o.moverChild));
  if (isConn) {
    img.i32(0x302, tk.arenaTok(o.connDest));
    img.i32(0x306, tk.cmiTok(o.animRecNear));
    img.i32(0x30a, tk.cmiTok(o.animRecFar));
    img.f32(0x30e, o.connRadius);
    img.i32(0x316, tk.cmiStrTok(o.connSound316));
    img.i32(0x31a, tk.cmiStrTok(o.connSound31a));
    img.i32(0x31e, tk.cmiStrTok(o.connSound31e));
    img.i32(0x322, tk.cmiStrTok(o.connSound322));
  }
  if (isElem) {
    img.i32(0x302, tk.cmiStrTok(o.homingPrefix));
    img.i32(0x306, o.homingDigitOfs);
  }

  if (arenaScript != nullptr) {
    // The embedded-record ctx overrides (TraversalScriptState carries
    // image OFFSETS, not pointers; 0 = null -> -1).
    const TraversalScriptState& s = *arenaScript;
    img.i32(0x108, s.pcImageOff == 0
                       ? -1
                       : static_cast<std::int32_t>(s.pcImageOff));
    img.i32(0x230, s.waitResumeImageOff == 0
                       ? -1
                       : static_cast<std::int32_t>(s.waitResumeImageOff));
    img.f32(0x22c, s.waitSeconds);
    img.u8(0x21d, s.eventByte);
    img.u8(0x21e, s.running);
    img.u8(0x11a, s.var11a);
    for (int i = 0; i < 4; ++i) img.f32(0x234 + 4 * i, s.locals[i]);
    img.u32(0x244, s.flagsLocal);
    img.i32(0x248, s.callDepth);
    for (int i = 0; i < 4; ++i) {
      img.i32(0x24c + 4 * i,
              s.retPc[i] == 0 ? -1 : static_cast<std::int32_t>(s.retPc[i]));
      img.i32(0x25c + 4 * i,
              s.savedPc[i] == 0
                  ? -1
                  : static_cast<std::int32_t>(s.savedPc[i]));
      img.u16(0x26c + 2 * i, s.marker[i]);
    }
    img.f32(0x30e, s.field30e);     // ctx field30e overrides the union
    img.u32(0x312, s.flagsChild);   // ctx flag group 5
  }
}

// ---------------------------------------------------------------------------
// MORE (52B) — FUN_00426a0c's staged ladder. +0x00 is the level
// identity (.CMI image length = file size - 4); +0x0c writes
// 0x5414a8 verbatim (the reader never consumes it — OBSERVED quirk:
// both a4 and a8 take the +0x08 value on load).
// ---------------------------------------------------------------------------
void emitMore(Img& img, const TraversalRuntime& rt, Toks& tk) {
  img.u32(0x00, static_cast<std::uint32_t>(tk.cmiImgSize));
  img.f32(0x04, rt.fadeTimer5414a0);
  img.f32(0x08, rt.fadeTimer5414a4);
  img.f32(0x0c, rt.fadeTimer5414a8);
  img.u32(0x10, rt.flag5414bc ? 1u : 0u);
  img.i32(0x14, rt.field5414d8);
  img.i32(0x18, rt.field541518);
  img.u8(0x1c, static_cast<std::uint8_t>(rt.g541534));
  img.i32(0x20, tk.objTok(rt.fieldB85c));   // the +0x07==1 view anchor
  img.i32(0x24, rt.flag49b740);
  img.f32(0x28, rt.overheadAux744);
  img.f32(0x2c, rt.overheadAux748);
  img.f32(0x30, rt.camera.overheadHeight);
}

// ---------------------------------------------------------------------------
// PLAY (239B) — the 0x541554 block. rt.savePlayBlock is the verbatim
// last-loaded copy; typed fields are stamped over it so the live
// authoritative values always win. The +0xc4..+0xca select/cadence
// cluster is emitted (the original memcpys the live globals) even
// though the loader resets it post-apply — byte fidelity.
// ---------------------------------------------------------------------------
void emitPlay(Img& img, const TraversalRuntime& rt,
              const ProgressionSession& sess) {
  img.raw(0, rt.savePlayBlock.data(), rt.savePlayBlock.size());
  img.i32(0x00, rt.fieldHealth);
  img.i32(0x04, rt.invHudTimer);
  for (int i = 0; i < 5; ++i) {
    const InventoryRecord& rec = rt.inventory[i];
    const std::uint32_t b = 0x08 + i * 0x24;
    img.i32(b + 0x00, rec.id);
    img.i32(b + 0x04, rec.charges);
    img.f32(b + 0x08, rec.animX);
    img.f32(b + 0x0c, rec.animY);
    img.f32(b + 0x10, rec.animVel);
    img.f32(b + 0x14, rec.animAux);
    img.i32(b + 0x18, rec.slotX);
    img.i32(b + 0x1c, rec.slotY);
    img.i32(b + 0x20, rec.aux);
  }
  img.i32(0xbc, rt.inventoryCount);
  img.i32(0xc0, rt.inventorySel);
  img.u16(0xc2, static_cast<std::uint16_t>(rt.invSelAux));
  img.u8(0xc4, static_cast<std::uint8_t>(rt.wpnSel0));
  img.u8(0xc5, static_cast<std::uint8_t>(rt.wpnSel1));
  img.u8(0xc6, static_cast<std::uint8_t>(rt.burstIndex));
  img.f32(0xc7, rt.fireCadence);
  for (int i = 0; i < 6; ++i) img.i32(0xcb + 4 * i, rt.ammo[i]);
  img.i32(0xe3, sess.deathCount);
  img.i32(0xe7, rt.field54163b);
}

// ---------------------------------------------------------------------------
// DAMP (724B) — the 0x540bfc block. Field-for-field inverse of
// applyDamp; the loader-cleared/scratch offsets emit the original
// writer's staged value (0) except +0x140 (live carrierBusy — the
// original memcpys it and the loader discards it).
// ---------------------------------------------------------------------------
void emitDamp(Img& img, const TraversalRuntime& rt, Toks& tk) {
  for (int i = 0; i < 3; ++i) {
    img.f32(0x00 + 4 * i, rt.cs.pos[i]);
    img.f32(0x0c + 4 * i, rt.cs.entryPos[i]);
  }
  // +0x18..+0x2f — stab-query scratch (no persisted native field).
  img.f32(0x30, rt.motion.yawDeg);
  for (int i = 0; i < 6; ++i) img.f32(0x34 + 4 * i, rt.cs.playerBox[i]);
  img.i32(0x4c, tk.arenaTok(rt.cur));
  // +0x50..+0x57 — render projection ints (rebuilt per frame).
  img.u8(0x58, rt.cs.contactFlags);
  img.f32(0x5c, rt.cs.floorZ);
  img.f32(0x60, rt.cs.floorOffset);
  img.i32(0x64, tk.objTok(rt.cs.floorObj));
  img.u32(0x68, rt.cs.floorElemMask);
  img.i32(0x6c, rt.cs.arenaValid);
  img.i32(0x70, rt.vertEnable ? 1 : 0);
  img.i32(0x74, rt.cs.queryEnabled);
  img.i32(0x78, rt.fieldC74);
  img.f32(0x7c, rt.vert.vertVel);
  img.i32(0x80, rt.vert.vertSkip);
  img.u32(0x84, rt.vert.jumpSustain);
  img.f32(0x88, rt.motion.airCharge);
  img.i32(0x8c, rt.vert.jumpActive);
  img.i32(0x90, rt.vert.jumpHoldCharge);
  img.i32(0x94, rt.vert.jumpLatch);
  img.i32(0x98, rt.motion.turnLock);
  img.i32(0x9c, rt.vert.jumpAux);
  img.i32(0xa0, rt.flagC9c);
  img.i32(0xa4, rt.transitionPhase);
  img.i32(0xa8, tk.arenaTok(rt.partner));
  img.i32(0xac, rt.partnerActive ? 1 : 0);
  img.i32(0xb0, rt.locoState);
  img.i32(0xb4, rt.animPrev);
  img.i32(0xb8, rt.animFrame);
  img.f32(0xbc, rt.animPhase);
  img.i32(0xc0, rt.eventPriority);
  img.i32(0xc4, rt.motion.moveDirLatch);
  // +0xc8 — the move-consumed flag, rewritten pre-read each frame.
  img.i32(0xcc, rt.fieldCc8);
  img.f32(0xd0, rt.scopeChanC);
  img.f32(0xd4, rt.scopeChanD0);
  img.f32(0xd8, rt.scopeChanD4);
  img.i32(0xdc, tk.objTok(rt.focusObj));
  img.f32(0xe0, rt.focusDist);
  img.i32(0xe4, rt.frameCounter);
  img.f32(0xe8, rt.camera.shakeMag);
  // +0xec..+0xfb — frame-stat trackers (no native fields).
  img.f32(0xfc, rt.camera.shakeX);
  img.f32(0x100, rt.camera.shakeY);
  img.f32(0x104, rt.fieldD00);
  // +0x108 — no xref.
  img.i32(0x10c, rt.reticleAux);
  img.i32(0x110, rt.fieldD0c);
  // +0x114/+0x118/+0x120/+0x124 — the original writer scrubs these
  // too (FUN_00426a0c stores 0 into the staged image).
  img.f32(0x11c, rt.ambientFades[0]);
  img.f32(0x128, rt.ambientFades[1]);
  img.f32(0x12c, rt.ambientFades[2]);
  img.i32(0x130, rt.fieldD2c);
  // 0x540d30 — the in-flight load arena. The loader clears the port
  // field post-attach, but the original's live global still held the
  // load target at save time — for a settled traversal state that IS
  // the current arena (OBSERVED: real saves carry cur's token here).
  img.i32(0x134,
          tk.arenaTok(rt.loadArena != nullptr ? rt.loadArena : rt.cur));
  img.i32(0x138, rt.scopeHudOffset);
  img.u32(0x13c, rt.inputState.setTurboLatch);
  img.i32(0x140, rt.cs.carrierBusy);   // live value; loader-cleared
  // +0x144/+0x16c — pending-list gates the loader clears; no fields.
  img.f32(0x14c, rt.motion.moveVel);
  img.f32(0x150, rt.motion.strafeVel);
  img.f32(0x154, rt.motion.turnVel);
  img.f32(0x158, rt.motion.zoomChannel);
  img.f32(0x15c, rt.look.lookPitchOffset);
  img.f32(0x160, rt.vert.landingAccum);
  img.i32(0x164, tk.arenaTok(rt.lruA));
  img.i32(0x168, tk.arenaTok(rt.lruB));
  // 0x540d6c — resolved-then-cleared arena ref; same live-value
  // convention as +0x134 (real saves carry cur's token).
  img.i32(0x170, tk.arenaTok(rt.cur));
  for (int i = 0; i < 6; ++i) img.i32(0x174 + 4 * i, rt.ambientChan[i]);
  for (int i = 0; i < 8; ++i) img.f32(0x18c + 4 * i, rt.scriptGVars[i]);
  img.u32(0x19c, rt.scriptGFlags);   // aliases scriptGVars[4]
  img.i32(0x1a0, rt.masterMoveGate ? 1 : 0);
  img.i32(0x1a4, rt.fieldDa0);
  img.i32(0x1a8, rt.fieldDa4);
  // +0x1ac/+0x1b4 — frame-stat trackers.
  img.i32(0x1b0, rt.fieldDac);
  img.f32(0x1b8, rt.camera.pullback);
  img.f32(0x1bc, rt.camera.eyeHeight);
  img.i32(0x1c0, rt.scopeScale);
  img.i32(0x1c4, tk.objTok(rt.cs.rideObj));
  img.u32(0x1c8, rt.cs.rideElemMask);
  img.i32(0x1cc, rt.cs.rideActive);
  // +0x1d0 — the cached arena ptr (attach-rebuilt; emit 0).
  // +0x1d4..+0x20f — the writer-scrubbed weapon-5 block (16 dwords).
  img.f32(0x214, rt.fieldE10);
  img.i32(0x218, rt.fieldE14);
  for (int i = 0; i < 3; ++i) img.f32(0x21c + 4 * i, rt.weapon5Aim[i]);
  img.i32(0x228, rt.slideChannel);
  img.i32(0x22c, rt.vert.bounceFlag);
  for (int i = 0; i < 6; ++i) img.f32(0x230 + 4 * i, rt.slideState[i]);
  img.f32(0x248, rt.animE44);
  img.f32(0x24c, rt.animE48);
  // +0x250 — the has-contact token (nonzero -> the static sentinel).
  img.u32(0x250, rt.vert.contactObj != 0 ? 1u : 0u);
  // +0x254 — the BSP-node token has no native field (emit 0).
  img.i32(0x258, rt.cmdFlag54);
  img.i32(0x25c, rt.cmdDetonate58);
  img.i32(0x260, tk.objTok(rt.cmdObj5c));
  img.i32(0x264, tk.objTok(rt.cmdObj60));
  img.f32(0x268, rt.mountYaw);
  img.i32(0x26c, tk.objTok(rt.cs.lastObjContact));
  img.i32(0x270, tk.objTok(rt.cs.excludeObj));
  img.u32(0x274, rt.mountClass);
  img.i32(0x278, rt.scopeAnimLatch);
  img.i32(0x27c, rt.punchTime);
  img.i32(0x280, rt.punchHitTime);
  img.i32(0x284, rt.shotSerial);
  img.i32(0x288, rt.shotHitCount);
  img.i32(0x28c, rt.fieldE88);
  // +0x290 — no xref.
  img.i32(0x294, rt.killTally);
  img.f32(0x298, rt.scopeBlend94);
  img.f32(0x29c, rt.scopeBlend98);
  img.i32(0x2a0, rt.fieldE9c);
  img.i32(0x2a4, rt.bombs);
  img.f32(0x2a8, rt.bombRecharge);
  // +0x2ac — the deferred-free countdown (writer+loader both clear).
  img.u32(0x2b0, rt.flagEac);
  img.f32(0x2b4, rt.eventTimer);
  img.i32(0x2b8, tk.objTok(rt.eventTimerObj));
  img.i32(0x2bc, rt.fieldEb8);
  // +0x2c0 — the pending-view arena gate: armed iff pendingViewSnap.
  img.i32(0x2c0, rt.pendingViewSnap != 0 ? tk.arenaTok(rt.cur) : -1);
  for (int i = 0; i < 4; ++i) img.f32(0x2c4 + 4 * i, rt.pendingView[i]);
}

// ---------------------------------------------------------------------------
// CAME (200B registry form — the +0xc8 scale tail is a reader-side
// tolerance; the writer emits the 200-byte block like real saves).
// ---------------------------------------------------------------------------
void emitCame(Img& img, const TraversalRuntime& rt) {
  const PlayerCameraState& c = rt.camera;
  const PlayerCameraPose& q = c.pose;
  for (int i = 0; i < 3; ++i) {
    img.f32(0x00 + 4 * i, q.pos[i]);
    img.f32(0x0c + 4 * i, q.back[i]);
    img.f32(0x18 + 4 * i, q.up[i]);
  }
  img.f32(0x24, rt.motion.bank);
  img.f32(0x28, rt.view.viewYawDeg);
  img.f32(0x2c, rt.viewScalar);
  img.f32(0x30, c.zoom);
  img.f32(0x34, c.heightOffset);
  img.f32(0x38, rt.bankAux);
  img.f32(0x3c, q.modeZoom);
  img.i32(0x40, q.viewW);
  img.i32(0x44, q.viewH);
  img.i32(0x48, q.viewCX);
  img.i32(0x4c, q.viewCY);
  img.i32(0x50, q.viewOX);
  img.i32(0x54, q.viewOY);
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 4; ++k) {
      img.f32(0x58 + r * 16 + k * 4, q.view[r][k]);
      img.f32(0x88 + r * 16 + k * 4, q.basis[r][k]);
    }
  }
  img.f32(0xb8, rt.view.viewPitchDeg);
  img.f32(0xbc, q.sinPitch);
  img.f32(0xc0, q.cosPitch);
  img.i32(0xc4, rt.flagBec);
}

// ---------------------------------------------------------------------------
// AREN (1126B) — name, counts, the +0x44/+0x48/+0x58 block, the
// +0x6c..+0x114 surface block, the embedded +0x118 object record
// (eventLatch + the arena script ctx view), +0x462 scalar.
// ---------------------------------------------------------------------------
void emitAren(Img& img, const TraversalRuntime& rt, const TraversalArena& a,
              Toks& tk) {
  std::size_t n = a.name.size() < 12 ? a.name.size() : 12;
  img.raw(0x00, a.name.data(), n);
  img.i32(0x0c, static_cast<std::int32_t>(a.dyn.storage.size()));
  std::uint32_t fans = 0;
  for (const SurfaceRecord* f = a.surface.records; f; f = f->next) ++fans;
  img.u32(0x10, fans);
  // +0x14..+0x43 — live ptr/geometry fields the loader skips.
  // +0x44 — the flag dword; bit2 IS the spawn-once gate (the port's
  // objectsSpawned mirror is authoritative for that bit).
  img.u32(0x44, (a.flags44 & ~4u) | (a.objectsSpawned ? 4u : 0u));
  for (int i = 0; i < 4; ++i) img.f32(0x48 + 4 * i, a.objVars48[i]);
  img.u32(0x58, a.flags58);
  // +0x5c..+0x6b — object list ptrs (loader skips).
  const SurfaceObjectState& so = a.surface;
  for (int i = 0; i < 16; ++i) {
    img.u8(0x6c + i, so.config[i]);
    img.u8(0x7c + i, so.handlerMask[i]);
    img.u32(0x8c + 4 * i, so.handlerOff[i]);
    img.i32(0xcc + 4 * i, so.counters[i]);
  }
  img.u32(0x10c, so.opMaskA);
  img.u32(0x110, so.opMaskB);
  img.u32(0x114, so.marks);
  // +0x118..+0x445 — the embedded pseudo-object record (0x32e).
  Img rec(0x32e);
  emitObjectRecord(rec, a.eventLatch, tk, &a.script);
  // AREN+0x148 == the embedded record's +0x30 — the op-0x61 script
  // byte aliases field30's low byte.
  rec.u8(0x30, a.objFlag148);
  img.raw(0x118, rec.b.data(), rec.b.size());
  // +0x446..+0x461 — fan list ptrs + stream state (loader skips).
  img.f32(0x462, a.scalar);
  (void)rt;
}

// ---------------------------------------------------------------------------
// FAND (72B) — one SurfaceRecord; +0x04 the owner arena's offset
// token, +0x08 the raw CMI name offset (both stored verbatim by the
// port post-load).
// ---------------------------------------------------------------------------
void emitFand(Img& img, const SurfaceRecord& f,
              const TraversalArena* owner, Toks& tk) {
  // +0x00 — the list link (runtime-owned).
  // +0x04 — the owner arena's token. The record hangs on `owner`'s
  // list, so its position is authoritative (the stored `f.owner`
  // index is the restored mirror and goes stale on runtime-created
  // records).
  img.i32(0x04, tk.arenaTok(owner));
  img.u32(0x08, f.name);   // raw CMI offset (kept verbatim by design)
  img.u8(0x0c, f.surfType);
  img.u32(0x10, f.f10);
  img.i32(0x14, f.kind);
  img.f32(0x18, f.rate);
  img.u32(0x1c, f.queryMask);
  for (int i = 0; i < 6; ++i) img.f32(0x20 + 4 * i, f.v[i]);
  img.f32(0x38, f.uvAcc[0]);
  img.f32(0x3c, f.uvAcc[1]);
  img.f32(0x40, f.target);
  img.f32(0x44, f.ramp);
}

// ---------------------------------------------------------------------------
// BULL (252B) — one PlayerShot slot. +0x18 arena token, +0xd8 home
// object id, +0xdc the home-element gate boolean (the original saves
// the raw pointer; the reader only tests nonzero), +0xe0 the element
// index verbatim, +0xd4 the fly-callback enum. +0x2c..+0xbb and +0xec
// are flight scratch (rebuilt; emit 0), +0x1c the class record (0 —
// the loader rebinds it from the shot type).
// ---------------------------------------------------------------------------
void emitBull(Img& img, const PlayerShot& s, Toks& tk) {
  img.i32(0x00, s.state);
  img.f32(0x04, s.yawDeg);
  img.f32(0x08, s.pitchDeg);
  img.f32(0x0c, s.spinDeg);
  img.i32(0x10, s.lifetime);
  img.i32(0x14, s.dyingTimer);
  img.i32(0x18, tk.arenaTok(s.arena));
  // +0x1c — the class record, rebound from type on load (emit 0).
  for (int i = 0; i < 3; ++i) img.f32(0x20 + 4 * i, s.pos[i]);
  // +0x2c..+0xbb — flight scratch (tick rederives; emit 0).
  img.f32(0xbc, s.tailLen);
  for (int i = 0; i < 3; ++i) img.f32(0xc0 + 4 * i, s.tail[i]);
  img.f32(0xcc, s.fieldCc);
  img.i32(0xd0, s.type);
  // FUN_00461724's inverse: callback addr -> enum {grenade 1, lobbed
  // 2, ribbon 3, else 0}.
  int flyEnum = 0;
  if (s.flyKind == kShotFlyGrenade) flyEnum = 1;
  else if (s.flyKind == kShotFlyLobbed) flyEnum = 2;
  else if (s.flyKind == kShotFlyRibbon) flyEnum = 3;
  img.i32(0xd4, flyEnum);
  img.i32(0xd8, tk.objTok(s.homeObj));
  img.u32(0xdc, s.homeElem != nullptr ? 1u : 0u);
  // +0xe0 — homeElemIdx verbatim; while ribbon-bound (+0xf8&1) the
  // dword aliases the path-record pointer (a dead slot under the
  // loader's gated rebind — emit the int view).
  img.i32(0xe0, s.homeElemIdx);
  img.f32(0xe4, (s.flags & 1) ? s.ribbonT : s.speedH);
  img.f32(0xe8, s.yawAccum);
  // +0xec — flight scratch.
  img.f32(0xf0, s.speedV);
  img.i32(0xf4, s.remnantIdx);
  img.u32(0xf8, s.flags);
}

} // namespace

std::vector<std::byte> saveGameWriteFull(const TraversalRuntime& rt,
                                       const ProgressionSession& sess,
                                       const SaveWriteFullInput& in,
                                       FullWriteReport* rep,
                                       std::string* detail) {
  auto fail = [&](const char* msg) -> std::vector<std::byte> {
    if (detail) *detail = msg;
    return {};
  };
  FullWriteReport local;
  if (rep == nullptr) rep = &local;
  if (rt.level.cmiBytes.size() < 4)
    return fail("no loaded CMI image (MORE identity cannot be written)");
  if (rt.arenas.empty()) return fail("no arenas to serialize");
  if (rt.cur == nullptr) return fail("no current arena");

  Toks tk{rt, rep, rt.level.cmiBytes.data() + 4,
          rt.level.cmiBytes.size() - 4, {}, {}};

  // --- Pass 1: stamp the sequential +0x7c ids (FUN_00426a0c order:
  // per arena the embedded +0x118 record first, then the +0x68 list
  // in order; 1-based, no gaps).
  std::int32_t nextId = 1;
  for (const auto& a : rt.arenas) {
    tk.ids[&a->eventLatch] = nextId++;
    for (const auto& o : a->dyn.storage) tk.ids[o.get()] = nextId++;
  }
  rep->saveIds = nextId - 1;

  // --- Pass 2: the packet stream (FUN_00426a0c's write order).
  std::vector<std::byte> stream;
  auto put32 = [&](std::uint32_t v) {
    std::byte b[4];
    for (int k = 0; k < 4; ++k) b[k] = std::byte((v >> (8 * k)) & 0xff);
    stream.insert(stream.end(), b, b + 4);
  };
  auto putPacket = [&](std::uint32_t tag, const void* p, std::size_t n) {
    put32(tag);
    put32(static_cast<std::uint32_t>(n));
    const auto* b = static_cast<const std::byte*>(p);
    stream.insert(stream.end(), b, b + n);
  };
  auto emit = [&](std::uint32_t tag, const Img& img) {
    putPacket(tag, img.b.data(), img.b.size());
  };

  const std::uint16_t seed = in.seed;
  putPacket(saveTag('S','A','V','E'), &seed, 2);

  // THMB — the capture is a host seam; zero-fill or the caller's blob.
  Img thmb(3648);
  if (!in.thumbnail.empty())
    thmb.raw(0, in.thumbnail.data(),
             in.thumbnail.size() < 3648 ? in.thumbnail.size() : 3648);
  emit(saveTag('T','H','M','B'), thmb);

  // GAME — FUN_00426e98's full form: mode field is the literal 0x3eb
  // (mode+1000), health verbatim (the <0x65 -> 100 floor is the
  // header-only branch only).
  Img game(24);
  game.i32(0x00, 0x3eb);
  game.i32(0x04, sess.levelId);
  game.i32(0x08, 0);               // uninit stack dword (load-neutral)
  game.i32(0x0c, rt.fieldHealth);
  game.i32(0x10, sess.deathCount);
  game.i32(0x14, sess.field54163b);
  emit(saveTag('G','A','M','E'), game);

  Img more(52);
  emitMore(more, rt, tk);
  emit(saveTag('M','O','R','E'), more);

  Img play(239);
  emitPlay(play, rt, sess);
  emit(saveTag('P','L','A','Y'), play);

  Img damp(724);
  emitDamp(damp, rt, tk);
  emit(saveTag('D','A','M','P'), damp);

  Img came(200);
  emitCame(came, rt);
  emit(saveTag('C','A','M','E'), came);

  for (const auto& a : rt.arenas) {
    Img aren(1126);
    emitAren(aren, rt, *a, tk);
    emit(saveTag('A','R','E','N'), aren);
    ++rep->arenasWritten;
    for (const auto& o : a->dyn.storage) {
      Img alie(0x32e);
      emitObjectRecord(alie, *o, tk, nullptr);
      emit(saveTag('A','L','I','E'), alie);
      ++rep->objectsWritten;
    }
    for (const SurfaceRecord* f = a->surface.records; f; f = f->next) {
      Img fand(72);
      emitFand(fand, *f, a.get(), tk);
      emit(saveTag('F','A','N','D'), fand);
      ++rep->fansWritten;
    }
  }

  for (const PlayerShot& s : rt.shots) {
    Img bull(252);
    emitBull(bull, s, tk);
    emit(saveTag('B','U','L','L'), bull);
  }

  putPacket(saveTag('S','E','N','D'), nullptr, 0);

  // A gameplay-authoritative ref that survived to a sentinel would
  // silently corrupt the restored world — refuse the whole write
  // (emission already collected every miss for the report).
  if (!tk.errs.empty()) {
    if (detail) {
      *detail = "unrepresentable state:";
      for (const auto& e : tk.errs) {
        *detail += ' ';
        *detail += e;
      }
    }
    return {};
  }
  return saveGameEnvelope(std::move(stream), seed);
}

} // namespace mdk
