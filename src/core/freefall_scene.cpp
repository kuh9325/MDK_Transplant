// freefall_scene.cpp — Phase 16C: FALL3D freefall presentation scene.
// See freefall_scene.h for the original-ownership map and evidence
// labels.

#include "core/freefall_scene.h"

#include <cmath>
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
// FUN_00411f48 — the RADAR wedge is built in code, not loaded: one
// element named "RADAR", 25 verts (apex + 4 hex rings, regenerated
// every radarTick) and 46 tris seeded from 46 dwords at 0x40edd0 —
// {u8 v0,v1,v2, u8 penByte} -> i16 pen -(penByte)-0x405. All
// remaining record fields zero (the original's FUN_0047d20a fill).
RuntimeModel makeRadarModel() {
  static constexpr std::uint8_t kFaces[46][4] = {
      {0, 2, 1, 0},  {0, 3, 2, 0},  {0, 4, 3, 0},  {0, 5, 4, 0},
      {0, 6, 5, 0},  {0, 1, 6, 0},
      {1, 8, 7, 1},  {1, 2, 8, 1},  {2, 9, 8, 1},  {2, 3, 9, 1},
      {3, 10, 9, 1}, {3, 4, 10, 1}, {4, 11, 10, 1},{4, 5, 11, 1},
      {5, 12, 11, 1},{5, 6, 12, 1}, {6, 7, 12, 1}, {6, 1, 7, 1},
      {7, 14, 13, 2}, {7, 8, 14, 2}, {8, 15, 14, 2}, {8, 9, 15, 2},
      {9, 16, 15, 2}, {9, 10, 16, 2},{10, 17, 16, 2},{10, 11, 17, 2},
      {11, 18, 17, 2},{11, 12, 18, 2},{12, 13, 18, 2},{12, 7, 13, 2},
      {13, 20, 19, 3},{13, 14, 20, 3},{14, 21, 20, 3},{14, 15, 21, 3},
      {15, 22, 21, 3},{15, 16, 22, 3},{16, 23, 22, 3},{16, 17, 23, 3},
      {17, 24, 23, 3},{17, 18, 24, 3},{18, 19, 24, 3},{18, 13, 19, 3},
      {24, 19, 21, 4},{21, 22, 24, 4},{23, 24, 22, 4},{19, 20, 21, 4},
  };
  RuntimeModel m;
  m.flag = 1;   // named-element form (the original's element is named)
  RuntimeModel::NameRec nr{};
  const char* nm = "RADAR";
  std::memcpy(nr.name.data(), nm, 6);
  m.names.push_back(nr);
  m.materials.push_back(nullptr);
  m.elems.resize(1);
  m.elemNames.resize(1);
  m.elemField2.resize(1);
  std::memcpy(m.elemNames[0].data(), nm, 6);
  m.elemVerts.resize(1);
  m.elemTris.resize(1);
  m.elemVerts[0].assign(25 * 3, 0.0f);
  auto& tris = m.elemTris[0];
  tris.resize(46 * 0x24, 0);
  for (int t = 0; t < 46; ++t) {
    std::uint8_t* rec = tris.data() + t * 0x24;
    for (int k = 0; k < 3; ++k) {
      const std::uint16_t v = kFaces[t][k];
      std::memcpy(rec + k * 2, &v, 2);
    }
    const std::int16_t pen =
        static_cast<std::int16_t>(-kFaces[t][3] - 0x405);
    std::memcpy(rec + 6, &pen, 2);
  }
  m.elems[0].triCount = 46;
  m.rebind();
  return m;
}

const RuntimeModel* protoForSlot(FreefallScene& s, int slot) {
  if (auto it = s.protos.find(slot); it != s.protos.end()) {
    return &it->second;
  }
  if (s.protoFailed.count(slot)) return nullptr;
  if (slot == 2) {   // radar wedge — FUN_00411f48's code-built model
    return &s.protos.emplace(slot, makeRadarModel()).first->second;
  }
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

// FUN_0042eb3c — the spawn-time anchor scan (OBSERVED): the missile
// spawn calls it once (0x411657), storing the minimum-x and maximum-x
// entries of the model's +0x20 element table's FIRST element vertex
// list as anchors +0x24/+0x28 (a second call could add a third anchor
// by proximity — mode-2 missiles never make it, so FX objects carry
// two). MISSILE has exactly one element; the scan binds elemVerts[0]
// explicitly either way.
void scanTrailAnchors(const RuntimeModel& proto,
                      FreefallScene::Twin::Trail* tr) {
  tr->anchors = 0;
  if (proto.elemVerts.empty()) return;
  const auto& verts = proto.elemVerts[0];
  float xmin = 0.0f, xmax = 0.0f;
  const float* lo = nullptr;
  const float* hi = nullptr;
  for (std::size_t i = 0; i + 2 < verts.size(); i += 3) {
    const float* v = &verts[i];
    if (lo == nullptr || v[0] < xmin) { xmin = v[0]; lo = v; }
    if (hi == nullptr || v[0] > xmax) { xmax = v[0]; hi = v; }
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

// FUN_00411aac tail (vert half) — apex v0 = model origin, then 4 hex
// rings (0x49a70c trig: cos at +0, sin at +4 — sin is 0.866 not
// 0.8660254) whose model-space centers converge on the beam point
// T = (tx-pos.x, ty-pos.y, tz) by rate s = 0x494ec8 (double 0.5,
// read as float): rings 1-3 = s(1+s+..+s^i-1)·T, ring 4 resets to
// exactly (T, L). Radius L = 10*tz/aux2 (0x494eb8 in the rise/sink
// arm; the wander arm's flat 10.0 is the same value at tz == aux2).
void radarWedgeVerts(const FreefallObject& o,
                     std::vector<float>& verts) {
  if (verts.size() < 25 * 3) return;
  static constexpr float kTrig[6][2] = {
      {0.5f, 0.8659999966621399f},  {1.0f, 0.0f},
      {0.5f, -0.8659999966621399f}, {-0.5f, -0.8659999966621399f},
      {-1.0f, 0.0f},                {-0.5f, 0.8659999966621399f},
  };
  constexpr float kVertScale = 0.5f;   // 0x494ec8
  const float tx = o.tx - o.px;
  const float ty = o.ty - o.py;
  const float tz = o.tz;
  const float l0 = (o.aux2 != 0.0f) ? 10.0f * o.tz / o.aux2 : 10.0f;
  verts[0] = verts[1] = verts[2] = 0.0f;
  float cx = 0.0f, cy = 0.0f, cz = 0.0f, L = 0.0f;
  float* w = verts.data() + 3;
  for (int ring = 1; ring <= 4; ++ring) {
    if (ring == 4) {
      cx = tx; cy = ty; cz = tz; L = l0;
    } else {
      cx = (cx + tx) * kVertScale;
      cy = (cy + ty) * kVertScale;
      cz = (cz + tz) * kVertScale;
      L = (L + l0) * kVertScale;
    }
    for (int k = 0; k < 6; ++k) {
      w[0] = cx + L * kTrig[k][0];
      w[1] = cy + L * kTrig[k][1];
      w[2] = cz;
      w += 3;
    }
  }
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
    case kFfModelRadar: return 2;      // 0x4eddd0 wedge — built in code
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
  // The working copy the POD seam wedge splices into (the original
  // mutates the loaded image at 0x4edc24 in place; we keep the
  // pristine span for diagnostics and write into this buffer).
  if (out->backdropOk) {
    out->backdropWork.assign(out->backdropPixels.begin(),
                             out->backdropPixels.end());
    out->backdropFrame.assign(600 * 360, 0);
  }

  // POD%d — the 64x1024 seam strip bound at 0x4edc2c.
  {
    char pod[16];
    std::snprintf(pod, sizeof pod, "POD%d", course + 1);
    for (const MtiEntry& e : out->mti.entries) {
      if (e.isIndexRecord() || e.name() != pod) continue;
      ArenaRenderMaterial am;
      if (arenaRenderMaterialDecode(
              std::span<const std::byte>(out->mtiBytes),
              e.payloadFileOffset(), e.fieldAt0x08, e.fieldAt0x0C,
              e.fieldAt0x10, e.nameField, &am) &&
          !am.pixels.empty()) {
        out->podPixels = am.pixels;
        out->podW = static_cast<int>(am.width);
        out->podH = static_cast<int>(am.height);
      }
      break;
    }
  }

  // L%d_C000{1..8} — the pod-column chunk sprites (0x4edb3c[8]).
  out->chunkCount = 0;
  for (int i = 0; i < 8; ++i) {
    char cn[24];
    std::snprintf(cn, sizeof cn, "L%d_C000%d", course + 1, i + 1);
    for (const MtiEntry& e : out->mti.entries) {
      if (e.isIndexRecord() || e.name() != cn) continue;
      ArenaRenderMaterial am;
      if (arenaRenderMaterialDecode(
              std::span<const std::byte>(out->mtiBytes),
              e.payloadFileOffset(), e.fieldAt0x08, e.fieldAt0x0C,
              e.fieldAt0x10, e.nameField, &am) &&
          !am.pixels.empty()) {
        out->chunks[std::size_t(i)].w = static_cast<int>(am.width);
        out->chunks[std::size_t(i)].h = static_cast<int>(am.height);
        out->chunks[std::size_t(i)].px = am.pixels;
        out->chunkCount = i + 1;
      }
      break;
    }
  }

  // ZOOM%4.4d — the 16 span tables (0x4edbb0[16]).
  out->zoomCount = 0;
  out->zoomOk.fill(false);
  for (int z = 0; z < 16; ++z) {
    char zn[16];
    std::snprintf(zn, sizeof zn, "ZOOM%4.4d", z);
    const BniRecord* r = findBniRecord(out->bni, zn);
    auto& rows = out->zoomRows[std::size_t(z)];
    rows.clear();
    if (!r) continue;
    const auto* base =
        reinterpret_cast<const std::uint8_t*>(out->bniBytes.data());
    const std::span<const std::uint8_t> rec(
        base + r->payloadFileOffset,
        base + r->payloadEnd);
    if (!freefallZoomParseTable(rec, &rows)) continue;
    out->zoomOk[std::size_t(z)] = true;
    ++out->zoomCount;
  }

  // FLARE4 / PICK — BNI sprite records {u16 w, u16 h, u8 px[w*h]}.
  {
    auto bniSprite = [&out](const char* nm,
                            FreefallScene::BackdropSprite* sp) {
      const BniRecord* r = findBniRecord(out->bni, nm);
      if (!r) return;
      const auto* base =
          reinterpret_cast<const std::uint8_t*>(out->bniBytes.data());
      const std::uint8_t* p = base + r->payloadFileOffset;
      const std::uint8_t* end = base + r->payloadEnd;
      if (p + 4 > end) return;
      const int w = p[0] | (p[1] << 8);
      const int h = p[2] | (p[3] << 8);
      if (w <= 0 || h <= 0 || p + 4 + std::size_t(w) * h > end) return;
      sp->w = w;
      sp->h = h;
      sp->px = {p + 4, std::size_t(w) * h};
    };
    bniSprite("FLARE4", &out->flare4);
    bniSprite("PICK", &out->pick);
  }

  // The generated LUT (0x540b20 block): 6 banks x 64 rows x 256 —
  // FUN_00406d84 per row: dst[c] = nearestPal((pal[c]*(256-level) +
  // key*level) >> 8). Bank levels {90,85,80,60,40,15} (OBSERVED
  // 0x42b8c0 init loop). The 64 row colors come from the 0x49b57c
  // keyframe ramp — 8 segments of 8 rows lerping between the
  // OBSERVED .rodata keyframe table (see FREEFALL_BACKDROP.md §2).
  out->lutOk = false;
  if (out->paletteOk) {
    struct Keyframe { std::uint8_t r, g, b, steps; };
    static constexpr Keyframe kRamp[9] = {
        {0x5a, 0xce, 0xde, 0},   // cyan-white anchor
        {0x21, 0x7b, 0x8c, 8},   // steel blue
        {0x08, 0x31, 0x7b, 8},   // deep blue
        {0x84, 0xa5, 0xc6, 8},   // light steel
        {0x8c, 0x7b, 0x84, 8},   // mauve
        {0xe7, 0xc6, 0xd6, 8},   // pale rose
        {0x84, 0xa5, 0xc6, 8},   // light steel
        {0x08, 0x31, 0x7b, 8},   // deep blue
        {0x5a, 0xce, 0xde, 8},   // cyan-white
    };
    static constexpr int kBankLevel[6] = {90, 85, 80, 60, 40, 15};
    // Row colors: segment k lerps kRamp[k] -> kRamp[k+1] over 8 rows.
    int row = 0;
    for (int kseg = 0; kseg < 8 && row < 64; ++kseg) {
      const Keyframe& a = kRamp[kseg];
      const Keyframe& b = kRamp[kseg + 1];
      for (int st = 0; st < b.steps && row < 64; ++st, ++row) {
        // 0x42b89e region: color = a + (b-a)*st/steps (u8 round).
        const int t = st;   // lerp numerator, /b.steps
        for (int ch = 0; ch < 3; ++ch) {
          const int ca = ch == 0 ? a.r : ch == 1 ? a.g : a.b;
          const int cb = ch == 0 ? b.r : ch == 1 ? b.g : b.b;
          out->keyColors[std::size_t(row)][ch] =
              static_cast<std::uint8_t>(
                  (ca * (b.steps - t) + cb * t) / b.steps);
        }
      }
    }
    while (row < 64) {   // pad with the last keyframe color
      out->keyColors[std::size_t(row)] =
          {kRamp[8].r, kRamp[8].g, kRamp[8].b};
      ++row;
    }
    out->lut.assign(384 * 256, 0);
    // nearestPal — exact squared-RGB search over the 256-entry
    // palette (the original's helper does the same metric).
    auto nearest = [&out](int r, int g, int b) -> std::uint8_t {
      int best = 0;
      int bestD = 0x7fffffff;
      for (int i = 0; i < 256; ++i) {
        const int dr = r - out->palette[i * 3];
        const int dg = g - out->palette[i * 3 + 1];
        const int db = b - out->palette[i * 3 + 2];
        const int d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
      }
      return static_cast<std::uint8_t>(best);
    };
    for (int bank = 0; bank < 6; ++bank) {
      const int L = kBankLevel[bank];
      for (int rw = 0; rw < 64; ++rw) {
        std::uint8_t* dst =
            out->lut.data() + (std::size_t(bank) * 64 + rw) * 256;
        const auto& key = out->keyColors[std::size_t(rw)];
        for (int c = 0; c < 256; ++c) {
          const int pr = out->palette[c * 3];
          const int pg = out->palette[c * 3 + 1];
          const int pb = out->palette[c * 3 + 2];
          dst[c] = nearest((pr * (256 - L) + key[0] * L) >> 8,
                           (pg * (256 - L) + key[1] * L) >> 8,
                           (pb * (256 - L) + key[2] * L) >> 8);
        }
      }
    }
    out->lutOk = true;
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

int freefallTrailSectionPen(int count, int section) {
  if (count <= 1 || section < 1 || section >= count) return -1054;
  // OBSERVED 0x42f096: pen = 0xfffffbfb - ((0x26 + section-1) - count)
  return section >= count - 8 ? count - section - 1066 : -1054;
}

bool freefallZoomParseTable(
    std::span<const std::uint8_t> rec,
    std::vector<FreefallScene::BackdropSpanRow>* rows) {
  rows->clear();
  const std::uint8_t* p = rec.data() + 4;   // the u32 size-4 prefix
  const std::uint8_t* const end =
      rec.data() + rec.size();
  if (rec.size() < 4) return false;
  bool ok = true;
  for (int row = 0; row < 180 && p < end; ++row) {
    FreefallScene::BackdropSpanRow sr;
    auto rd32 = [&p, end]() -> std::uint32_t {
      if (p + 4 > end) return 0;
      std::uint32_t v;
      std::memcpy(&v, p, 4);
      p += 4;
      return v;
    };
    sr.a = rd32();
    if (sr.a > 150 || p + sr.a * 4 > end) { ok = false; break; }
    sr.sa.assign(p, p + sr.a * 4);
    p += sr.a * 4;
    sr.b = rd32();
    sr.c = rd32();
    // Every real row covers EXACTLY 150 quads / 600 px — a short row
    // is just as malformed as an overflowing one (verified: all 180
    // rows x 16 tables in FALL3D.BNI sum exactly 150).
    if (sr.a + sr.b + sr.c != 150 || p + sr.c * 4 > end) {
      ok = false;
      break;
    }
    sr.sc.assign(p, p + sr.c * 4);
    p += sr.c * 4;
    rows->push_back(std::move(sr));
  }
  // The table is only valid when all 180 rows parsed and the record
  // was consumed to its last byte — a desynced walk must fail, not
  // ride through as a black cycle.
  if (!ok || rows->size() != 180 || p != end) {
    rows->clear();
    return false;
  }
  return true;
}

void freefallSceneBackdropStep(FreefallScene& s,
                               float camX, float camY, float camZ,
                               float dtSec) {
  auto& dg = s.backdropDiag;
  dg = FreefallScene::BackdropDiag{};
  // A rejected/absent ZOOM table is a hard miss — the original binds
  // all 16 unconditionally, so anything less than a full set must not
  // ride through as a valid all-black frame.
  if (!s.backdropOk || s.zoomCount != 16 || !s.lutOk ||
      s.backdropFrame.size() < 600 * 360) {
    return;
  }

  // Scroll integration — OBSERVED driver writes (0x41043d region):
  //   0x4edc00 scrollPos += 0x49b6f4 (1/30) per frame  -> +1.0/s
  //   0x4edb5c chunkScroll += 0x49b6f0 (0.5) per frame -> +15.0/s,
  //             wraps mod 8
  //   0x4edbf0 zoomCounter += 1 per call (per rendered frame), &15
  // The per-frame constants are the original's 30 fps cadence — the
  // port dt-scales so higher refresh rates scroll at wall-clock rate.
  const float frames = dtSec * 30.0f;
  s.backdropScrollPos += frames * (1.0f / 30.0f);
  s.backdropChunkScroll += frames * 0.5f;
  if (s.backdropChunkScroll >= 8.0f) {
    s.backdropChunkScroll = std::fmod(s.backdropChunkScroll, 8.0f);
  }
  s.backdropZoomCounter = (s.backdropZoomCounter + 1) & 15;

  // FUN_00412530 fp locals (double — the x87 math is 80-bit; double
  // matches the OBSERVED constants bit-for-bit well enough that the
  // 16.16 truncations below land identically for our value ranges).
  const double p = static_cast<double>(camZ) * 0.0001893939;
  const double uCenter = 0.36 * static_cast<double>(camX) + 512.0;
  // v3c = 824 + scrollPos*(1/33)*(-624) - 0.36*camY
  const double scrollTerm =
      static_cast<double>(s.backdropScrollPos) * (1.0 / 33.0) * -624.0;
  const double v3c = 824.0 + scrollTerm - 0.36 * static_cast<double>(camY);
  const double uStart = uCenter - 300.0 * p;      // 0x412955 path
  const double vStart = v3c - 180.0 * p;          // 0x412740 path
  // scrollRow = trunc(v3c + 0.36*camY - (chunkH*80>>8)) — the camY
  // terms cancel; (108*80)>>8 = 33.
  const int scrollRow = static_cast<int>(824.0 + scrollTerm - 33.0);
  dg.p = static_cast<float>(p);
  dg.uStart = static_cast<float>(uStart);
  dg.vStart = static_cast<float>(vStart);
  dg.scrollRow = scrollRow;
  dg.zoomTable = s.backdropZoomCounter;

  // POD -> LEVEL seam wedge (OBSERVED 0x4125f7-0x4126xx):
  // delta = max(24, prevScrollRow - scrollRow); per row, count =
  // row>=24 ? 32 : round(row*2/3 + 16); LEVEL[row][512-count..511]
  // = POD[row][32-count..31]. Anchored at the NEW scrollRow.
  if (!s.backdropWork.empty() &&
      s.podPixels.size() >= std::size_t(64) * 1024) {
    int delta = s.backdropScrollRow < 0
                    ? 24
                    : s.backdropScrollRow - scrollRow;
    if (delta < 24) delta = 24;
    if (delta > 512) delta = 512;   // sanity bound (orig writes in-place)
    std::uint8_t* lvl = s.backdropWork.data();
    const std::uint8_t* pod = s.podPixels.data();
    for (int row = 0; row < delta; ++row) {
      const int dstRow = scrollRow + row;
      if (dstRow < 0 || dstRow >= s.backdropH) break;
      const int count = row >= 24
                            ? 32
                            : static_cast<int>(row * (2.0 / 3.0) + 16.0 + 0.5);
      std::uint8_t* dst = lvl + dstRow * s.backdropW + 512;
      const std::uint8_t* src = pod + dstRow * s.podW + 32;
      for (int k = 0; k < count; ++k) {
        dst[-count + k] = src[-count + k];
      }
    }
  }
  s.backdropScrollRow = scrollRow;

  // FUN_0046d780 — the span-table textured sampler. Arg struct as
  // decoded: {uFrac0, vFrac0, uFracStep, vFracStep, uIntStep,
  // vIntStepRows, texelPtr, dstPtr, spanRec, lutBase}. All in 16.16
  // fixed point; fistp truncates (toward zero) so cast-to-int64.
  const int64_t uQ = static_cast<int64_t>(uStart * 65536.0);
  const int64_t vQ = static_cast<int64_t>(vStart * 65536.0);
  const int64_t pQ = static_cast<int64_t>(p * 65536.0);
  const std::uint32_t uFrac0 = static_cast<std::uint32_t>(uQ) << 16;
  const std::uint32_t vFrac0 = static_cast<std::uint32_t>(vQ) << 16;
  const std::uint32_t pFrac = static_cast<std::uint32_t>(pQ) << 16;
  const int64_t pInt = pQ >> 16;
  int64_t texelRow = (uQ >> 16) + (vQ >> 16) * 1024;
  std::uint32_t vFrac = vFrac0;
  std::uint8_t* out = s.backdropFrame.data();
  const std::uint8_t* lvl = s.backdropWork.empty()
                              ? s.backdropPixels.data()
                              : s.backdropWork.data();
  const std::uint8_t* lut = s.lut.data();
  constexpr int kShadeRow = 12;   // 0x4edbf4 = 0xc00 (write 0x410311) rel 0x4edc34

  const auto& rows = s.zoomRows[std::size_t(dg.zoomTable) %
                                s.zoomRows.size()];
  const std::size_t nRows = rows.size();
  int y = 0;
  for (std::size_t r = 0; r < nRows && y < 360; ++r) {
    const FreefallScene::BackdropSpanRow& sr = rows[r];
    for (int rep = 0; rep < 2 && y < 360; ++rep, ++y) {
      int64_t texel = texelRow;
      std::uint32_t uFrac = uFrac0;
      std::uint8_t* dstRow = out + y * 600;
      int px = 0;
      const auto step = [&]() {
        const std::uint32_t nf = uFrac + pFrac;
        texel += pInt + (nf < uFrac ? 1 : 0);
        uFrac = nf;
      };
      // Phase A — shaded span.
      const std::uint8_t* sa = sr.sa.data();
      for (std::uint32_t i = 0; i < sr.a; ++i) {
        for (int k = 0; k < 4 && px < 600; ++k) {
          const std::uint8_t t = lvl[std::size_t(texel) & 0xFFFFF];
          const int shade = sa[i * 4 + k];
          dstRow[px++] = lut[std::size_t(kShadeRow + shade) * 256 + t];
          step();
        }
      }
      // Phase B — raw span.
      for (std::uint32_t i = 0; i < sr.b * 4 && px < 600; ++i) {
        dstRow[px++] = lvl[std::size_t(texel) & 0xFFFFF];
        step();
      }
      // Phase C — shaded span.
      const std::uint8_t* sc = sr.sc.data();
      for (std::uint32_t i = 0; i < sr.c; ++i) {
        for (int k = 0; k < 4 && px < 600; ++k) {
          const std::uint8_t t = lvl[std::size_t(texel) & 0xFFFFF];
          const int shade = sc[i * 4 + k];
          dstRow[px++] = lut[std::size_t(kShadeRow + shade) * 256 + t];
          step();
        }
      }
      for (; px < 600; ++px) dstRow[px] = 0;   // underrun pad
    }
    // Row advance: vFrac += frac(p); texelRow += vInt*1024 + carry.
    const std::uint32_t nf = vFrac + pFrac;
    texelRow += pInt * 1024 + (nf < vFrac ? 1024 : 0);
    vFrac = nf;
  }

  // Chunk sprite — the L%d_C000%d pod column drawn through
  // FUN_00403a40 -> 0x46d680 (transparent scaled blit, pen-0 skips):
  //   x = int(-0.36*camX/p + 300),  y = int(0.36*camY/p + 180)
  //   scale = int(192/p)   dstW = (srcW*scale)>>8   dstH likewise
  //   src texel = nearest (fixed-point step (srcW<<16)/dstW)
  // centered at (x,y), clipped to the 600x360 viewport.
  dg.chunkFrame = -1;
  if (s.chunkCount > 0 && p > 1e-9) {
    const int ci = static_cast<int>(s.backdropChunkScroll) & 7;
    const auto& ch = s.chunks[std::size_t(ci) % s.chunks.size()];
    if (ch.px.size() >= std::size_t(ch.w) * ch.h) {
      const int scale = static_cast<int>(192.0 / p);   // trunc
      if (scale > 0) {
        const int dstW = (ch.w * scale) >> 8;
        const int dstH = (ch.h * scale) >> 8;
        if (dstW > 0 && dstH > 0) {
          const int cx = static_cast<int>(-0.36 * camX / p + 300.0);
          const int cy = static_cast<int>(0.36 * camY / p + 180.0);
          const int x0 = cx - dstW / 2;
          const int y0 = cy - dstH / 2;
          dg.chunkFrame = ci;
          dg.chunkX = static_cast<float>(cx);
          dg.chunkY = static_cast<float>(cy);
          dg.chunkW = static_cast<float>(dstW);
          dg.chunkH = static_cast<float>(dstH);
          const std::int64_t sxStep =
              (std::int64_t(ch.w) << 16) / dstW;
          const std::int64_t syStep =
              (std::int64_t(ch.h) << 16) / dstH;
          for (int dy = 0; dy < dstH; ++dy) {
            const int yy = y0 + dy;
            if (yy < 0 || yy >= 360) continue;
            const int srcY = static_cast<int>(
                (std::int64_t(dy) * syStep) >> 16);
            if (srcY < 0 || srcY >= ch.h) continue;
            for (int dx = 0; dx < dstW; ++dx) {
              const int xx = x0 + dx;
              if (xx < 0 || xx >= 600) continue;
              const int srcX = static_cast<int>(
                  (std::int64_t(dx) * sxStep) >> 16);
              if (srcX < 0 || srcX >= ch.w) continue;
              const std::uint8_t v = ch.px[srcY * ch.w + srcX];
              if (v != 0) out[yy * 600 + xx] = v;
            }
          }
        }
      }
    }
  }
}

FreefallBackdropStatus freefallSceneBackdropStatus(
    const FreefallScene& s) {
  FreefallBackdropStatus st;
  st.palette = s.paletteOk;
  st.level = s.backdropOk;
  st.pod = !s.podPixels.empty();
  st.lut = s.lutOk;
  st.chunks = s.chunkCount;
  st.zoom = s.zoomCount;
  for (int z = 0; z < 16; ++z) {
    if (s.zoomOk[std::size_t(z)]) st.zoomMask |= 1 << z;
  }
  st.flare4 = !s.flare4.px.empty();
  st.pick = !s.pick.px.empty();
  st.ready = st.level && st.pod && st.lut && st.zoom == 16;
  return st;
}

void freefallSceneTrailComposite(FreefallScene& s,
                               float camX, float camY, float camZ) {
  if (!s.lutOk || s.lut.size() < 384 * 256 ||
      s.backdropFrame.size() < 600 * 360) {
    return;
  }
  // The mode-2 folded view — same constants get_freefall_snapshot
  // builds the Godot camera from. Original view record (0x46b4f8
  // input): right = +X, down = -Y, +viewdir = -Z_mdk (the raw M2
  // row2); scaleX/Y folded into x'/y' rows.
  constexpr float kZoom = 2.4f;          // 0x540b58 boot value
  constexpr float kScaleX = 1.0f / (kZoom * 0.5f);
  constexpr float kScaleY = 1.0f / (kZoom * 0.3f);
  constexpr float kXDiv = 600.0f * 0.4999f;
  constexpr float kYDiv = 360.0f * 0.5011f;
  constexpr float kBias = 0.05f;
  constexpr float kNear = 0.05f;
  struct Pt { float x, y; bool on; };
  auto project = [&](float px, float py, float pz) -> Pt {
    const float zp = camZ - pz;
    if (zp <= kNear) return {0.0f, 0.0f, false};
    const float xp = kScaleX * (px - camX);
    const float yp = kScaleY * (camY - py);
    return {static_cast<float>((xp + zp) / zp * kXDiv + kBias),
            static_cast<float>((yp + zp) / zp * kYDiv + kBias), true};
  };

  constexpr float kHeadRamp[6] =
      {1.0f, 1.25f, 1.2f, 1.1f, 1.05f, 1.0f};   // 0x49b634
  constexpr int cap = FreefallScene::Twin::kTrailCap;
  std::uint8_t* fb = s.backdropFrame.data();
  const std::uint8_t* lut = s.lut.data();

  // One triangle of the screen-space quad, dst = lut[row*256+dst].
  auto fillTri = [&](const Pt& a, const Pt& b, const Pt& c, int row) {
    const std::uint8_t* lr = lut + std::size_t(row) * 256;
    const int minX = std::max(0, static_cast<int>(
        std::floor(std::min({a.x, b.x, c.x}))));
    const int maxX = std::min(599, static_cast<int>(
        std::ceil(std::max({a.x, b.x, c.x}))));
    const int minY = std::max(0, static_cast<int>(
        std::floor(std::min({a.y, b.y, c.y}))));
    const int maxY = std::min(359, static_cast<int>(
        std::ceil(std::max({a.y, b.y, c.y}))));
    if (minX > maxX || minY > maxY) return;
    const auto edge = [](const Pt& u, const Pt& v, float x, float y) {
      return (x - u.x) * (v.y - u.y) - (y - u.y) * (v.x - u.x);
    };
    const float area = edge(a, b, c.x, c.y);
    if (std::fabs(area) < 1e-9f) {
      // Degenerate (line) quad — draw the edge segments so thin
      // tails still mark, like the original's cap/edge coverage.
      auto plot = [&](float x0, float y0, float x1, float y1) {
        const int steps = std::max(1, static_cast<int>(std::ceil(
            std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)) * 2)));
        for (int k = 0; k <= steps; ++k) {
          const int xx = static_cast<int>(x0 + (x1 - x0) * k / steps);
          const int yy = static_cast<int>(y0 + (y1 - y0) * k / steps);
          if (xx >= 0 && xx < 600 && yy >= 0 && yy < 360) {
            std::uint8_t* d = fb + yy * 600 + xx;
            *d = lr[*d];
          }
        }
      };
      plot(a.x, a.y, b.x, b.y);
      plot(b.x, b.y, c.x, c.y);
      return;
    }
    const float s = area > 0 ? 1.0f : -1.0f;
    for (int yy = minY; yy <= maxY; ++yy) {
      for (int xx = minX; xx <= maxX; ++xx) {
        const float fx = xx + 0.5f;
        const float fy = yy + 0.5f;
        if (edge(a, b, fx, fy) * s < 0.0f ||
            edge(b, c, fx, fy) * s < 0.0f ||
            edge(c, a, fx, fy) * s < 0.0f) {
          continue;
        }
        std::uint8_t* d = fb + yy * 600 + xx;
        *d = lr[*d];
      }
    }
  };

  for (const FreefallScene::Twin& t : s.twins) {
    const FreefallScene::Twin::Trail& tr = t.trail;
    if (!t.bound || tr.count < 2 || tr.anchors < 2) continue;
    // Walked oldest->newest from the +0x20 read cursor; per-slot the
    // drawn edge tapers toward the centroid with t = headRamp[age]
    // for the newest six then the linear decay (OBSERVED
    // FUN_0042ee74's factor walk).
    Pt prevL{0, 0, false}, prevR{0, 0, false};
    for (int sw = 0; sw < tr.count; ++sw) {
      const int k = (tr.read + sw) & (cap - 1);
      const int age = tr.count - 1 - sw;
      const float tt =
          age < 6 ? kHeadRamp[age]
                  : std::max(0.0f, 1.0f - float(age - 6) /
                                          float(cap - 6));
      const float* l = tr.pts[k][0];
      const float* r = tr.pts[k][1];
      const float cx = (l[0] + r[0]) * 0.5f;
      const float cy = (l[1] + r[1]) * 0.5f;
      const float cz = (l[2] + r[2]) * 0.5f;
      const Pt pl = project(cx + (l[0] - cx) * tt,
                            cy + (l[1] - cy) * tt,
                            cz + (l[2] - cz) * tt);
      const Pt pr = project(cx + (r[0] - cx) * tt,
                            cy + (r[1] - cy) * tt,
                            cz + (r[2] - cz) * tt);
      if (sw > 0 && prevL.on && prevR.on && pl.on && pr.on) {
        // Section quad {prevL, prevR, pr, pl}; the pen is per
        // SECTION (walk index sw pairing slots sw-1, sw).
        const int pen = freefallTrailSectionPen(tr.count, sw);
        if (pen < -1028) {
          const int row = -1029 - pen;   // -1054->25, -1058..-1065->29..36
          if (row >= 0 && std::size_t(row) * 256 + 255 < s.lut.size()) {
            fillTri(prevL, prevR, pr, row);
            fillTri(prevL, pr, pl, row);
          }
        }
        // pen == -1028 (the textured tail cap) and pens
        // -1027..-1024 / -1023..-1011 (flat rows / palette) do not
        // appear in mode-2 trails: the observed walk emits only
        // -1054 and -1058..-1065 — those map to LUT veil rows.
      }
      prevL = pl;
      prevR = pr;
    }
  }
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
      // two-anchor slot reads {lo, hi, lo}. The write cursor is
      // mod-32 (&0x1f); when the ring is already full the read
      // cursor (0x42ee60: +0x20 = (+0x20+1)&0x1f) follows, keeping
      // `read` at the oldest slot.
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
        tr.cursor = (tr.cursor + 1) & (FreefallScene::Twin::kTrailCap - 1);
        if (tr.count < FreefallScene::Twin::kTrailCap) {
          ++tr.count;
        } else {
          tr.read = (tr.read + 1) & (FreefallScene::Twin::kTrailCap - 1);
        }
      }
    } else {
      buildObjectMatrix(0.0f, o.roll, o.yaw, o.scale, d.pos,
                        d.col.xform, d.col.origin);
      // FUN_00411aac tail — the radar wedge's ring verts regenerate
      // every tick from the beam state (model-space, relative to the
      // origin the tick tail wrote into +0x10/+0x14).
      if (o.type == 3 && !d.model.elemVerts.empty()) {
        radarWedgeVerts(o, d.model.elemVerts[0]);
      }
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
