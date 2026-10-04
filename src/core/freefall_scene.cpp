// freefall_scene.cpp — Phase 16C: FALL3D freefall presentation scene.
// See freefall_scene.h for the original-ownership map and evidence
// labels.

#include "core/freefall_scene.h"

#include <cstring>

#include "core/data_root.h"
#include "core/object_animation.h"

namespace mdk {

// OBSERVED BUILD_A .data pair — the name block at 0x49a67c (23
// 8-char fields) and the flag bytes at 0x49a664, populated by
// FUN_0040ef28's model-roster loop. bit7 = named-element record
// (FUN_00428400's flag arg), low 7 bits = the 0x4edcc0 model slot
// (FUN_00454794's name->slot result).
const std::array<FreefallModelRecord, 23> kFreefallModelTable = {{
    {"KURT", 0x81},     {"MISSILE", 0x83},   {"CHUTE", 0x84},
    {"BONES", 0x85},    {"SW_BONES", 0x06},  {"SW_GATT", 0x07},
    {"SW_HGREN", 0x08}, {"SW_HBOMB", 0x09},  {"SW_HOME", 0x0a},
    {"SW_LGREN", 0x0b}, {"SW_SGREN", 0x0c},  {"SW_SHOT", 0x0d},
    {"SW_H01", 0x0e},   {"SW_H25", 0x0f},    {"SW_H50", 0x10},
    {"SW_H100", 0x11},  {"SW_DUMMY", 0x92},  {"SW_H150", 0x93},
    {"SW_THUMP", 0x94}, {"SW_TWIST", 0x95},  {"SW_INTER", 0x96},
    {"SW_KEY", 0x17},   {"EXPLODE", 0x18},
}};

namespace {

const FreefallModelRecord* rosterBySlot(int slot) {
  for (const FreefallModelRecord& e : kFreefallModelTable) {
    if ((e.flag & 0x7f) == slot) return &e;
  }
  return nullptr;
}

const FreefallModelRecord* rosterByName(const char* name) {
  for (const FreefallModelRecord& e : kFreefallModelTable) {
    if (std::strncmp(e.name, name, 8) == 0) return &e;
  }
  return nullptr;
}

std::span<const std::uint8_t> bniPayload(const FreefallScene& s,
                                         const BniRecord& r) {
  const auto* base =
      reinterpret_cast<const std::uint8_t*>(s.bniBytes.data());
  return {base + r.payloadFileOffset, base + r.payloadEnd};
}

// Lazily parse one roster model. The FALL3D records drop the parser's
// leading flag word (the original passes it as the EDX arg), so the
// record is re-headed with the roster flag for the shared
// parseGeometryRecord.
const RuntimeModel* protoForSlot(FreefallScene& s, int slot) {
  if (auto it = s.protos.find(slot); it != s.protos.end()) {
    return &it->second;
  }
  if (s.protoFailed.count(slot)) return nullptr;
  const FreefallModelRecord* rec = rosterBySlot(slot);
  const BniRecord* r =
      rec ? findBniRecord(s.bni, rec->name) : nullptr;
  if (!r) {
    s.protoFailed.insert(slot);
    return nullptr;
  }
  const std::span<const std::uint8_t> payload = bniPayload(s, *r);
  const std::uint32_t flag = (rec->flag & 0x80) ? 1u : 0u;
  std::vector<std::uint8_t> headed(4 + payload.size());
  const std::uint8_t fl[4] = {
      static_cast<std::uint8_t>(flag), 0, 0, 0};
  std::memcpy(headed.data(), fl, 4);
  std::memcpy(headed.data() + 4, payload.data(), payload.size());
  std::optional<RuntimeModel> m = parseGeometryRecord(
      headed.data(), headed.data() + headed.size());
  if (!m) {
    s.protoFailed.insert(slot);
    return nullptr;
  }
  return &s.protos.emplace(slot, std::move(*m)).first->second;
}

// FUN_0042eb3c — the spawn-time anchor scan over the model's +0x20
// point table: the first call stores the table's minimum-x and
// maximum-x entries as anchors +0x24/+0x28 (a later call can add a
// third anchor by proximity, but the missile spawn calls it once —
// 0x411657 — so FX objects carry two). The table is implemented as
// the pooled element verts: the record format's only {count, vec3
// array} point table (HYPOTHESIS — see header note).
void scanTrailAnchors(const RuntimeModel& proto,
                      FreefallScene::Twin::Trail* tr) {
  tr->anchors = 0;
  float xmin = 0.0f, xmax = 0.0f;
  const float* lo = nullptr;
  const float* hi = nullptr;
  for (const auto& verts : proto.elemVerts) {
    for (std::size_t i = 0; i + 2 < verts.size(); i += 3) {
      const float* v = &verts[i];
      if (lo == nullptr || v[0] < xmin) { xmin = v[0]; lo = v; }
      if (hi == nullptr || v[0] > xmax) { xmax = v[0]; hi = v; }
    }
  }
  if (lo == nullptr) return;
  tr->anchorPts[0][0] = lo[0];
  tr->anchorPts[0][1] = lo[1];
  tr->anchorPts[0][2] = lo[2];
  tr->anchorPts[1][0] = hi[0];
  tr->anchorPts[1][1] = hi[1];
  tr->anchorPts[1][2] = hi[2];
  tr->anchors = 2;
}

void bindTwin(FreefallScene& s, int slot, const FreefallObject& o,
              FreefallScene::Twin* t) {
  const RuntimeModel* proto = protoForSlot(s, slot);
  if (!proto) {
    t->bound = false;
    return;
  }
  t->bound = true;
  t->modelSlot = slot;
  t->type = o.type;
  t->pickupRec = o.pickupRec;
  t->animTag = -1;   // forces the anim rebind below
  t->trail = FreefallScene::Twin::Trail{};
  if (o.fx != 0) scanTrailAnchors(*proto, &t->trail);

  DynamicObject& d = t->obj;
  d.model = deepCopyModel(*proto);
  d.model.rebind();
  d.elemSet = d.model.elementSet();
  d.col.model = d.elemSet.elems ? &d.elemSet : nullptr;
  d.col.elements = &d.elemSet;
  d.enemyIndex = 0;
  d.col.flags148 = o.flags;
  d.animRec = nullptr;
  d.animSoundName.clear();
  d.animSoundMark = 0;
  d.animImpulse[0] = d.animImpulse[1] = d.animImpulse[2] = 0.0f;
}

// The PEN_<n> name fallback: the name's digit run is the palette
// index (the documented name->index correlation — every OBSERVED MTI
// index record named PEN_n/GREYn carries n at +0x0c; here the record
// is absent so the digits are the only evidence). "NONE" decodes as
// 256 in the corpus — kept out of the u8 palette range on purpose
// (the original's flat-0xff path treats it as a no-draw; the caller
// sees paletteIndex 256 and can skip).
int penIndexFromName(const std::string& name) {
  if (name == "NONE") return 256;
  std::size_t pos = name.find_first_of("0123456789");
  if (pos == std::string::npos) return -1;
  int v = 0;
  for (; pos < name.size() && name[pos] >= '0' && name[pos] <= '9';
       ++pos) {
    v = v * 10 + (name[pos] - '0');
    if (v > 4096) return -1;
  }
  return v;
}

} // namespace

int freefallObjectModelSlot(const FreefallRuntime& rt,
                            const FreefallObject& o) {
  switch (o.model) {
    case kFfModelKurt: return 1;
    case kFfModelMissile: return 3;
    case kFfModelChute: return 4;
    case kFfModelBones: return 5;
    case kFfModelBang: return 24;      // EXPLODE record
    case kFfModelRadar: return -1;     // kind-1 sprite seam, no record
    default: break;
  }
  if (o.model >= kFfModelPickup) {
    const int recIdx = o.model - kFfModelPickup;
    if (recIdx >= 0 &&
        recIdx < static_cast<int>(rt.pickups.size())) {
      const FreefallModelRecord* e =
          rosterByName(rt.pickups[std::size_t(recIdx)].name);
      return e ? (e->flag & 0x7f) : -1;
    }
  }
  return -1;
}

const char* freefallAnimRecordName(int animTag) {
  switch (animTag) {
    case kFfAnimKurt: return "KURTANIM";
    case kFfAnimKurtHit: return "KURT_HIT";
    case kFfAnimBones: return "BONESANM";
    default: return nullptr;
  }
}

const char* freefallSceneErrorName(FreefallSceneError e) {
  switch (e) {
    case FreefallSceneError::kOk: return "ok";
    case FreefallSceneError::kReadBni: return "read FALL3D.BNI";
    case FreefallSceneError::kParseBni: return "parse FALL3D.BNI";
    case FreefallSceneError::kReadMti: return "read FALL3D_n.MTI";
    case FreefallSceneError::kParseMti: return "parse FALL3D_n.MTI";
    case FreefallSceneError::kPaletteMissing: return "FALLP_n palette";
  }
  return "?";
}

FreefallSceneError freefallSceneLoad(const DataRoot& root, int course,
                                     FreefallScene* out,
                                     std::string* detail) {
  *out = FreefallScene{};
  out->course = course;
  std::string err;

  auto bni = root.readFile("FALL3D/FALL3D.BNI", 1 << 28, &err);
  if (!bni) {
    if (detail) *detail = "FALL3D/FALL3D.BNI: " + err;
    return FreefallSceneError::kReadBni;
  }
  out->bniBytes = std::move(*bni);
  out->bni = inspectBniDirectory(
      std::span<const std::byte>(out->bniBytes));
  if (out->bni.status != BniDirectoryStatus::kOk) {
    if (detail) {
      *detail = "FALL3D.BNI directory: " +
                std::string(bniDirectoryStatusName(out->bni.status));
    }
    return FreefallSceneError::kParseBni;
  }

  char mtiName[32];
  std::snprintf(mtiName, sizeof mtiName, "FALL3D/FALL3D_%d.MTI",
                course + 1);
  auto mti = root.readFile(mtiName, 1 << 28, &err);
  if (!mti) {
    if (detail) *detail = std::string(mtiName) + ": " + err;
    return FreefallSceneError::kReadMti;
  }
  out->mtiBytes = std::move(*mti);
  out->mti = inspectMtiDirectory(
      std::span<const std::byte>(out->mtiBytes));
  if (out->mti.status != MtiDirectoryStatus::kOk) {
    if (detail) {
      *detail = std::string(mtiName) + " directory: " +
                std::string(mtiDirectoryStatusName(out->mti.status));
    }
    return FreefallSceneError::kParseMti;
  }

  // FALLP_<course+1> — the palette bound at 0x4edc28.
  out->paletteOk = false;
  char palName[16];
  std::snprintf(palName, sizeof palName, "FALLP%d", course + 1);
  if (const BniRecord* r = findBniRecord(out->bni, palName)) {
    if (r->payloadSize() >= 768) {
      const auto* base =
          reinterpret_cast<const std::uint8_t*>(out->bniBytes.data());
      std::memcpy(out->palette.data(),
                  base + r->payloadFileOffset, 768);
      out->paletteOk = true;
    }
  }

  // LEVEL%d — the minecrawler surface image the backdrop pass
  // (FUN_00412530) scroll-samples before the object walk. The MTI
  // payload record is {u16 w, u16 h, u8 pixels[w*h]} — the same
  // envelope arenaRenderMaterialDecode parses.
  out->backdropOk = false;
  {
    char lvl[16];
    std::snprintf(lvl, sizeof lvl, "LEVEL%d", course + 1);
    for (const MtiEntry& e : out->mti.entries) {
      if (e.isIndexRecord() || e.name() != lvl) continue;
      ArenaRenderMaterial am;
      if (arenaRenderMaterialDecode(
              std::span<const std::byte>(out->mtiBytes),
              e.payloadFileOffset(), e.fieldAt0x08, e.fieldAt0x0C,
              e.fieldAt0x10, e.nameField, &am) &&
          !am.pixels.empty()) {
        out->backdropPixels = am.pixels;
        out->backdropW = static_cast<int>(am.width);
        out->backdropH = static_cast<int>(am.height);
        out->backdropOk = true;
      }
      break;
    }
  }

  // FALLPU_<course+1> — 12-byte {name[8], u32} records terminated by
  // a NUL first name byte (OBSERVED FUN_0040ef28 count loop; same
  // parse as mdk-inspect's inspectReadFallpu).
  {
    char puName[16];
    std::snprintf(puName, sizeof puName, "FALLPU_%d", course + 1);
    if (const BniRecord* r = findBniRecord(out->bni, puName)) {
      const auto* base =
          reinterpret_cast<const std::uint8_t*>(out->bniBytes.data());
      const std::uint8_t* p = base + r->payloadFileOffset;
      const std::uint8_t* end = base + r->payloadEnd;
      for (; p + 12 <= end; p += 12) {
        if (p[0] == 0) break;
        FreefallPickupRec pr{};
        for (int k = 0; k < 8; ++k) pr.name[k] = static_cast<char>(p[k]);
        pr.name[8] = '\0';
        out->pickups.push_back(pr);
      }
    }
  }

  // Anim record spans — bound at 0x4edae0/4/8 in the init.
  for (int tag = 1; tag <= 3; ++tag) {
    const char* nm = freefallAnimRecordName(tag);
    const BniRecord* r = nm ? findBniRecord(out->bni, nm) : nullptr;
    if (r) {
      const auto* base =
          reinterpret_cast<const std::uint8_t*>(out->bniBytes.data());
      out->anims[std::size_t(tag)] = {
          base + r->payloadFileOffset, base + r->payloadEnd};
    }
  }
  return FreefallSceneError::kOk;
}

void freefallSceneStep(FreefallScene& s, const FreefallRuntime& rt,
                       float dtSec) {
  std::array<bool, 399> seen{};
  for (int i = rt.listHead; i >= 0; i = rt.pool[i].next) {
    if (i >= 399) break;
    const FreefallObject& o = rt.pool[std::size_t(i)];
    const int slot = freefallObjectModelSlot(rt, o);
    if (slot < 0) continue;   // model==0 / radar / unresolved pickup
    seen[std::size_t(i)] = true;
    FreefallScene::Twin& t = s.twins[std::size_t(i)];
    if (!t.bound || t.modelSlot != slot || t.type != o.type ||
        t.pickupRec != o.pickupRec) {
      bindTwin(s, slot, o, &t);
      if (!t.bound) continue;
    }
    DynamicObject& d = t.obj;
    // FUN_0046b2f8 basis — pitch 0, roll +0x13c, yaw +0x4c,
    // scale +0x58, origin = pos. Degrees, scale baked.
    d.pos[0] = o.px;
    d.pos[1] = o.py;
    d.pos[2] = o.pz;
    d.pitchDeg = 0.0f;
    d.bankDeg = o.roll;
    d.yawDeg = o.yaw;
    d.col.scale = o.scale;
    if (o.type == 1) {
      // Missiles: the +0xac velocity-tracking basis written by
      // FUN_0041139c in missileTick is the authoritative orientation
      // (the kind-2 draw consumes it verbatim via the work matrix);
      // only the scale is baked on top.
      for (int k = 0; k < 9; ++k) {
        d.col.xform[k] = o.basis[k] * o.scale;
      }
      d.col.origin[0] = o.px;
      d.col.origin[1] = o.py;
      d.col.origin[2] = o.pz;
      // FUN_0042ecc4 — the trail feed: the model-space anchors are
      // transformed by the (unscaled) object basis into the next
      // ring slot; the closing point mirrors anchor 0, so a
      // two-anchor slot reads {lo, hi, lo}.
      FreefallScene::Twin::Trail& tr = t.trail;
      if (o.fx != 0 && tr.anchors > 0) {
        float* dst = tr.pts[tr.cursor][0];
        const float pos[3] = {o.px, o.py, o.pz};
        for (int a = 0; a < tr.anchors; ++a) {
          const float* p = tr.anchorPts[a];
          for (int r = 0; r < 3; ++r) {
            dst[a * 3 + r] =
                o.basis[r * 3] * p[0] + o.basis[r * 3 + 1] * p[1] +
                o.basis[r * 3 + 2] * p[2] + pos[r];
          }
        }
        for (int c = 0; c < 3; ++c) dst[tr.anchors * 3 + c] = dst[c];
        tr.cursor = (tr.cursor + 1) % FreefallScene::Twin::kTrailCap;
        if (tr.count < FreefallScene::Twin::kTrailCap) ++tr.count;
      }
    } else {
      buildObjectMatrix(0.0f, o.roll, o.yaw, o.scale, d.pos,
                        d.col.xform, d.col.origin);
    }
    // Anim-handle switch (KURT -> KURT_HIT, restore, bones): rebind
    // the record span and mirror the accumulator family — the spawn/
    // hit sites write acc=-1/frame=-1 as the driver's start state.
    // Between switches the twin's own accumulator is authoritative —
    // objectAnimTickDt advances it by rate*animRate*dtSec, identical
    // to the runtime's frameUnits increment for the rate-1.0 records.
    //
    // Ordering on the switch frame — the runtime's fields are the
    // POST-frame values and the increment may belong to either record:
    //   * o.animAcc == -1 && o.animFrame == -1 exactly: a
    //     write-AFTER-step site (the missile hit writes the player
    //     inside missileTick, after the player's own driver step ran
    //     on the OLD handle). The original applied one step on the
    //     old record this frame, so the twin ticks the old binding
    //     first, then rebinds without ticking.
    //   * otherwise (spawn -1/0, the restore gate, bones): the write
    //     precedes the tick's +=frameUnits (write-BEFORE-step) — the
    //     increment belongs to the NEW record. Bind with the pre-step
    //     accumulator (acc - rate*animRate*dtSec) so the tick lands
    //     exactly on the runtime's post-frame value/frame.
    const bool switched = (t.animTag != o.animHandle);
    const bool writeAfterStep =
        switched && o.animAcc == -1.0f && o.animFrame == -1;
    if (writeAfterStep && d.animRec != nullptr &&
        t.animTag >= 0 && t.animTag <= 3) {
      objectAnimTickDt(d, s.anims[std::size_t(t.animTag)].limit,
                       dtSec);
    }
    if (switched) {
      t.animTag = o.animHandle;
      const FreefallScene::AnimSpan& sp =
          (o.animHandle >= 0 && o.animHandle <= 3)
              ? s.anims[std::size_t(o.animHandle)]
              : FreefallScene::AnimSpan{};
      d.animRec = sp.rec;
      d.animAcc = o.animAcc;
      d.animFrame = o.animFrame;
      d.animLatch = o.animSentinel;
      if (!writeAfterStep && sp.rec != nullptr) {
        const ObjectAnimView av{sp.rec, sp.limit};
        d.animAcc -= av.rate() * o.animRate * dtSec;
      }
      d.animSoundName.clear();
      d.animSoundMark = 0;
    }
    d.animRate = o.animRate;
    d.col.flags148 = o.flags;      // bit3 = loop enable (0x148&8)
    if (d.animRec != nullptr && !writeAfterStep) {
      const FreefallScene::AnimSpan& sp =
          s.anims[std::size_t(t.animTag)];
      objectAnimTickDt(d, sp.limit, dtSec);
    }
  }
  // Objects absent from the active list (freed, or model cleared)
  // unbind — the render walk emits nothing for them.
  for (std::size_t i = 0; i < s.twins.size(); ++i) {
    if (!seen[i] && s.twins[i].bound) s.twins[i].bound = false;
  }
}

const FreefallScene::Twin* freefallSceneTwin(const FreefallScene& s,
                                             int poolIdx) {
  if (poolIdx < 0 || poolIdx >= static_cast<int>(s.twins.size())) {
    return nullptr;
  }
  const FreefallScene::Twin& t = s.twins[std::size_t(poolIdx)];
  return t.bound ? &t : nullptr;
}

const FreefallScene::Twin::Trail* freefallSceneTrail(
    const FreefallScene& s, int poolIdx) {
  const FreefallScene::Twin* t = freefallSceneTwin(s, poolIdx);
  if (t == nullptr || t->trail.anchors == 0) return nullptr;
  return &t->trail;
}

const RuntimeModel* freefallSceneChuteModel(FreefallScene& s) {
  return protoForSlot(s, 4);   // CHUTE — lazily bound like the others
}

const FreefallMaterial* freefallSceneMaterial(FreefallScene& s,
                                              const std::string& name) {
  if (auto it = s.materials.find(name); it != s.materials.end()) {
    return it->second.valid ? &it->second : nullptr;
  }
  if (s.materialMiss.count(name)) return nullptr;
  for (const MtiEntry& e : s.mti.entries) {
    if (e.name() != name) continue;
    FreefallMaterial m;
    m.name = name;
    if (e.isIndexRecord()) {
      // Index record — +0x0c is the palette index.
      m.paletteIndex = static_cast<int>(e.fieldAt0x0C);
      m.valid = true;
    } else {
      ArenaRenderMaterial am;
      if (!arenaRenderMaterialDecode(
              std::span<const std::byte>(s.mtiBytes),
              e.payloadFileOffset(), e.fieldAt0x08, e.fieldAt0x0C,
              e.fieldAt0x10, e.nameField, &am) ||
          am.pixels.empty()) {
        s.materialMiss.insert(name);
        return nullptr;
      }
      m.pixels = am.pixels;
      m.width = static_cast<int>(am.width);
      m.height = static_cast<int>(am.height);
      m.frameCount = am.frameCount > 0 ? am.frameCount : 1;
      m.valid = true;
    }
    auto [it, _] = s.materials.emplace(name, std::move(m));
    return &it->second;
  }
  // Name-table entry absent from the bank — the PEN_<n> digit
  // convention resolves it to a flat palette pen.
  const int pen = penIndexFromName(name);
  if (pen >= 0) {
    FreefallMaterial m;
    m.name = name;
    m.paletteIndex = pen;
    m.valid = true;
    return &s.materials.emplace(name, std::move(m)).first->second;
  }
  s.materialMiss.insert(name);
  return nullptr;
}

} // namespace mdk
