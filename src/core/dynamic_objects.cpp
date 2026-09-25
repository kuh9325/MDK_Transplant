#include "core/dynamic_objects.h"

#include <cmath>
#include <cstring>
#include <new>

namespace mdk {

namespace {

// 0x540ed0 — the global dynamic-object freelist. FUN_0045cf90 pushes
// memset records here (LIFO) and FUN_0045cffc pops before falling
// back to the inactive-arena scavenge. The port keeps freed records
// alive in this list instead of returning them to the heap: stale
// DynamicObject* held by other objects then stay dereferenceable —
// the same guarantee the original's fixed record pool provides. The
// original's 399-slot cap is a memory-budget mechanism; the port's
// list is bounded by the peak live-object count and needs none.
std::list<std::unique_ptr<DynamicObject>>& objectFreelist() {
  static std::list<std::unique_ptr<DynamicObject>> l;
  return l;
}

std::uint32_t rdU32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

float rdF32(const std::uint8_t* p) {
  float v;
  std::memcpy(&v, p, 4);
  return v;
}

std::string printable(const char* p, std::size_t n) {
  std::size_t len = 0;
  while (len < n && p[len]) ++len;
  return std::string(p, len);
}

// FUN_00459d54 — min/max over `count` f32 triples -> {min,max}.
void boundsFromPoints(const float* pts, std::size_t count,
                      float out[6]) {
  out[0] = out[1] = out[2] = 1e30f;
  out[3] = out[4] = out[5] = -1e30f;
  for (std::size_t i = 0; i < count; ++i) {
    for (int k = 0; k < 3; ++k) {
      const float v = pts[i * 3 + k];
      if (v < out[k]) out[k] = v;
      if (v > out[3 + k]) out[3 + k] = v;
    }
  }
}

} // namespace

// Row-major 3x3 (scale baked) + translation — the matrix contract
// shared with the collision query (world = M.local + origin).
void transformPoint(const float m[9], const float org[3],
                    const float in[3], float out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = in[0] * m[i * 3 + 0] + in[1] * m[i * 3 + 1] +
             in[2] * m[i * 3 + 2] + org[i];
  }
}

// ---------------------------------------------------------------------------
// RuntimeModel
// ---------------------------------------------------------------------------

void RuntimeModel::rebind() {
  const std::size_t n = elems.size();
  elemNames.resize(n);
  elemField2.resize(n);
  elemVerts.resize(n);
  elemTris.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    elems[i].verts = elemVerts[i].empty() ? nullptr : elemVerts[i].data();
    elems[i].tris = elemTris[i].empty() ? nullptr : elemTris[i].data();
  }
}

std::string RuntimeModel::elemName(std::size_t i) const {
  if (i >= elemNames.size()) return {};
  return printable(elemNames[i].data(), elemNames[i].size());
}

std::string RuntimeModel::modelName() const {
  if (names.empty()) return {};
  return printable(names[0].name.data(), names[0].name.size());
}

// ---------------------------------------------------------------------------
// FUN_00428400 — geometry record parse
// ---------------------------------------------------------------------------

std::optional<RuntimeModel> parseGeometryRecord(
    const std::uint8_t* rec, const std::uint8_t* end) {
  if (!rec || !end || end - rec < 8) return std::nullopt;
  const std::uint32_t flag = rdU32(rec);      // *(base) -> EDX arg
  const std::uint8_t* cur = rec + 4;          // stream starts at +4
  const std::uint32_t nameCount = rdU32(cur);
  cur += 4;
  if (nameCount > 0x10000 ||
      static_cast<std::uint64_t>(end - cur) <
          static_cast<std::uint64_t>(nameCount) * 16u) {
    return std::nullopt;
  }

  RuntimeModel m;
  m.flag = flag;
  m.names.reserve(nameCount);
  for (std::uint32_t i = 0; i < nameCount; ++i) {
    RuntimeModel::NameRec nr;
    std::memcpy(nr.name.data(), cur, 12);
    nr.tag = rdU32(cur + 12);
    m.names.push_back(nr);
    cur += 16;
  }

  std::uint32_t elemCount = 1;
  if (flag != 0) {
    if (end - cur < 4) return std::nullopt;
    elemCount = rdU32(cur);
    cur += 4;
    if (elemCount > 0x10000) return std::nullopt;
  }

  m.elems.resize(elemCount);
  m.elemNames.resize(elemCount);
  m.elemField2.resize(elemCount);
  m.elemVerts.resize(elemCount);
  m.elemTris.resize(elemCount);

  for (std::uint32_t e = 0; e < elemCount; ++e) {
    if (flag != 0) {
      if (end - cur < 24) return std::nullopt;
      std::memcpy(m.elemNames[e].data(), cur, 12);
      std::memcpy(m.elemField2[e].data(), cur + 12, 12);
      cur += 24;
    }
    if (end - cur < 4) return std::nullopt;
    const std::uint32_t vertCount = rdU32(cur);
    cur += 4;
    if (vertCount > 0x100000 ||
        static_cast<std::uint64_t>(end - cur) <
            static_cast<std::uint64_t>(vertCount) * 12u) {
      return std::nullopt;
    }
    m.elemVerts[e].resize(static_cast<std::size_t>(vertCount) * 3);
    std::memcpy(m.elemVerts[e].data(), cur,
                static_cast<std::size_t>(vertCount) * 12);
    // FUN_00459d54 — local AABB over the verts -> +0x2c.
    boundsFromPoints(m.elemVerts[e].data(), vertCount,
                     m.elems[e].localAabb);
    if (vertCount == 0) {
      std::memset(m.elems[e].localAabb, 0, sizeof(m.elems[e].localAabb));
    }
    cur += static_cast<std::uint64_t>(vertCount) * 12u;

    if (end - cur < 4) return std::nullopt;
    const std::uint32_t triCount = rdU32(cur);
    cur += 4;
    if (triCount > 0x100000 ||
        static_cast<std::uint64_t>(end - cur) <
            static_cast<std::uint64_t>(triCount) * 0x24u) {
      return std::nullopt;
    }
    m.elemTris[e].resize(static_cast<std::size_t>(triCount) * 0x24u);
    std::memcpy(m.elemTris[e].data(), cur,
                static_cast<std::size_t>(triCount) * 0x24u);
    m.elems[e].triCount = static_cast<std::int32_t>(triCount);
    cur += static_cast<std::uint64_t>(triCount) * 0x24u;

    if (flag != 0) {
      // The 0x18 trailer is skipped only on the named-element path.
      if (end - cur < 0x18) return std::nullopt;
      cur += 0x18;
    }
  }

  // +0x18 gap, then the reference-point tail.
  if (end - cur < 0x18 + 4) return std::nullopt;
  cur += 0x18;
  const std::uint32_t refCount = rdU32(cur);
  cur += 4;
  if (refCount > 8) return std::nullopt;   // original's error bound
  if (static_cast<std::uint64_t>(end - cur) <
      static_cast<std::uint64_t>(refCount) * 12u) {
    return std::nullopt;
  }
  m.refPointCount = refCount;
  for (std::uint32_t i = 0; i < refCount; ++i) {
    for (int k = 0; k < 3; ++k) m.refPoints[i][k] = rdF32(cur + i * 12 + k * 4);
  }
  cur += static_cast<std::uint64_t>(refCount) * 12u;

  // Tail: body/head element bookkeeping (names XG1_BODY / XG1_HEAD).
  m.bodyElemIndex = -1;   // 0xff in the original byte field
  m.headElemMask = 0;
  for (std::uint32_t e = 0; e < elemCount; ++e) {
    const std::string en = m.elemName(e);
    if (en == "XG1_BODY") m.bodyElemIndex = static_cast<int>(e);
    if (en == "XG1_HEAD") m.headElemMask |= (1u << e);
  }

  m.rebind();
  return m;
}

// ---------------------------------------------------------------------------
// FUN_00403720 — deep copy (fresh element array + vertex/tri storage)
// ---------------------------------------------------------------------------

RuntimeModel deepCopyModel(const RuntimeModel& src) {
  RuntimeModel m = src;   // vectors deep-copy; views still point at src
  m.rebind();             // re-point at this copy's storage
  return m;
}

// ---------------------------------------------------------------------------
// Enemy table (FUN_004286c8 product) + geometry resolution
// ---------------------------------------------------------------------------

EnemyTable buildEnemyTable(const CmiDirectory& cmi) {
  EnemyTable t;
  if (cmi.tables.size() < 2) return t;
  t.entries.reserve(cmi.tables[1].records.size());
  for (const auto& r : cmi.tables[1].records) {
    EnemyTable::Entry e;
    e.name = r.name();
    e.dataImageOff = r.value;
    e.unresolved = (r.value == 0);
    t.entries.push_back(std::move(e));
  }
  return t;
}

int EnemyTable::indexOf(const std::string& name) const {
  for (std::size_t i = 0; i < entries.size(); ++i)
    if (entries[i].name == name) return static_cast<int>(i);
  return -1;
}

std::optional<std::span<const std::uint8_t>> enemyModelData(
    const EnemyTable& enemies, int idx,
    std::span<const std::byte> cmiFile, const CmiDirectory& cmiDir,
    const MtoDirectory* mtoDir, std::span<const std::byte> mtoFile) {
  if (idx < 0 || static_cast<std::size_t>(idx) >= enemies.entries.size())
    return std::nullopt;
  const auto& e = enemies.entries[static_cast<std::size_t>(idx)];
  if (!e.unresolved) {
    // CMI data region — image-relative value -> file offset 4+value,
    // bounded by the directory's dataRegionEnd (record extends to the
    // region end at most; the parser bounds its own interior).
    const std::uint64_t off = kCmiImageBaseOffset + e.dataImageOff;
    const std::uint64_t end = cmiDir.dataRegionEnd;
    if (off >= end || end > cmiFile.size()) return std::nullopt;
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(cmiFile.data()) + off,
        static_cast<std::size_t>(end - off));
  }
  if (!mtoDir || mtoFile.empty()) return std::nullopt;
  // MTO deferred path — region-A array-B {name[8], u32 off} matched
  // by name; record base = regionAOffset (tA) + off.
  const auto* base =
      reinterpret_cast<const std::uint8_t*>(mtoFile.data());
  for (const auto& blk : mtoDir->blocks) {
    for (const auto& r : blk.regionAArrayB) {
      if (r.name() != e.name) continue;
      const std::uint64_t off = blk.regionAOffset + r.fieldAt0x08;
      if (off >= mtoFile.size() || blk.regionAEndOffset > mtoFile.size() ||
          off >= blk.regionAEndOffset) {
        return std::nullopt;
      }
      return std::span<const std::uint8_t>(
          base + off,
          static_cast<std::size_t>(blk.regionAEndOffset - off));
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// DynamicObject / DynamicArena
// ---------------------------------------------------------------------------

void DynamicObject::setPosition(float x, float y, float z) {
  pos[0] = x;
  pos[1] = y;
  pos[2] = z;
  col.baseZ = z;   // +0x18 == the ride-offset reference
}

void DynamicObject::syncCollisionView() {
  elemSet = model.elementSet();
  col.elements = &elemSet;
  col.model = (health != 0) ? &model : nullptr;
}

DynamicObject& DynamicArena::allocFront() {
  // FUN_0045cffc — the freelist pop comes first (a recycled record is
  // memset-clean from the free path); the original's fallback was a
  // scavenged inactive-arena record, the port's is a fresh heap
  // record — same observable state.
  auto& fl = objectFreelist();
  if (!fl.empty()) {
    storage.push_front(std::move(fl.front()));
    fl.pop_front();
  } else {
    storage.push_front(std::make_unique<DynamicObject>());
  }
  DynamicObject& o = *storage.front();
  o.col.next = col.objects;
  col.objects = &o.col;
  o.col.named = true;
  o.arena = this;
  return o;
}

DynamicObject& DynamicArena::allocBack() {
  auto& fl = objectFreelist();
  if (!fl.empty()) {
    storage.push_back(std::move(fl.front()));
    fl.pop_front();
  } else {
    storage.push_back(std::make_unique<DynamicObject>());
  }
  DynamicObject& o = *storage.back();
  o.col.next = nullptr;
  if (col.objects == nullptr) {
    col.objects = &o.col;
  } else {
    const CollisionObject* tail = col.objects;
    while (tail->next) tail = tail->next;
    const_cast<CollisionObject*>(tail)->next = &o.col;  // owned by storage
  }
  o.col.named = true;
  o.arena = this;
  return o;
}

void DynamicArena::detach(DynamicObject& obj) {
  CollisionObject* prev = nullptr;   // owned non-const below
  for (const CollisionObject* c = col.objects; c; c = c->next) {
    if (c == &obj.col) {
      if (prev) {
        prev->next = c->next;
      } else {
        col.objects = c->next;
      }
      break;
    }
    prev = const_cast<CollisionObject*>(c);   // owned by `storage`
  }
  for (auto it = storage.begin(); it != storage.end(); ++it) {
    if (it->get() == &obj) {
      // FUN_0045cf90 — the record is memset before the freelist push;
      // destroying + reconstructing in place is the port equivalent
      // (members with owned storage are released by the destructor).
      std::unique_ptr<DynamicObject> up = std::move(*it);
      storage.erase(it);
      obj.~DynamicObject();
      new (&obj) DynamicObject();
      objectFreelist().push_front(std::move(up));
      return;
    }
  }
  obj.arena = nullptr;
}

void DynamicArena::reapUnnamed() {
  // FUN_0045cf18 — walk +0x68 and free every corpse. The storage
  // walk pre-reads the next node (same pattern as the original's
  // next-link read and the update loop) so detach can't corrupt it.
  for (auto it = storage.begin(); it != storage.end();) {
    auto nextIt = std::next(it);
    if (!(*it)->col.named) detach(**it);
    it = nextIt;
  }
}

void DynamicArena::transfer(DynamicObject& obj, DynamicArena& dst) {
  // Unlink from this list (objects are owned non-const via `storage`).
  CollisionObject* prev = nullptr;
  for (const CollisionObject* c = col.objects; c; c = c->next) {
    if (c == &obj.col) {
      if (prev) {
        prev->next = c->next;
      } else {
        col.objects = c->next;
      }
      break;
    }
    prev = const_cast<CollisionObject*>(c);
  }
  // Splice the storage node across, then push-front onto dst.
  for (auto it = storage.begin(); it != storage.end(); ++it) {
    if (it->get() == &obj) {
      dst.storage.splice(dst.storage.begin(), storage, it);
      break;
    }
  }
  obj.col.next = dst.col.objects;
  dst.col.objects = &obj.col;
  // FUN_004574d0 — for a connector (+0x14a & 0x10) the destination
  // pointer flips to the arena it just left, so the door keeps working
  // in both directions after the player crosses.
  if (obj.col.flags14a & 0x10)
    obj.connDest = owner;                     // +0x302 <- old home arena
  obj.arena = &dst;
  obj.pendingArena = nullptr;
}

// ---------------------------------------------------------------------------
// FUN_0046b2f8 — Euler(degrees)+scale -> row-major 3x3 + origin
// ---------------------------------------------------------------------------

void buildObjectMatrix(float pitchDeg, float bankDeg, float yawDeg,
                       float scale, const float pos[3],
                       float outXform[9], float outOrigin[3]) {
  constexpr double kDegToRad = 0.01745329251994328;  // pi/180 (0x497924)
  const double a1 = pitchDeg * kDegToRad;
  const double a2 = bankDeg * kDegToRad;
  const double a3 = yawDeg * kDegToRad;
  const float s1 = static_cast<float>(std::sin(a1));
  const float c1 = static_cast<float>(std::cos(a1));
  const float s2 = static_cast<float>(std::sin(a2));
  const float c2 = static_cast<float>(std::cos(a2));
  const float s3 = static_cast<float>(std::sin(a3));
  const float c3 = static_cast<float>(std::cos(a3));
  outXform[0] = (c2 * c3) * scale;
  outXform[1] = (-s1 * s2 * c3 - c1 * s3) * scale;
  outXform[2] = (-c1 * s2 * c3 + s1 * s3) * scale;
  outXform[3] = (c2 * s3) * scale;
  outXform[4] = (-s1 * s2 * s3 + c1 * c3) * scale;
  outXform[5] = (-c1 * s2 * s3 - s1 * c3) * scale;
  outXform[6] = (s2) * scale;
  outXform[7] = (s1 * c2) * scale;
  outXform[8] = (c1 * c2) * scale;
  outOrigin[0] = pos[0];
  outOrigin[1] = pos[1];
  outOrigin[2] = pos[2];
}

// ---------------------------------------------------------------------------
// FUN_0045612c — transform + world-AABB rebuild (collision core)
// ---------------------------------------------------------------------------

DynamicObject::~DynamicObject() { surfaceRecordsDestroy(surface); }

void rebuildObjectTransform(DynamicObject& obj) {
  // OBSERVED seed quirk: min = {old minZ x3}, max = {old maxZ x3}.
  const float seedLo = obj.col.aabb[2];
  const float seedHi = obj.col.aabb[5];
  obj.col.aabb[0] = obj.col.aabb[1] = obj.col.aabb[2] = seedLo;
  obj.col.aabb[3] = obj.col.aabb[4] = obj.col.aabb[5] = seedHi;

  if (obj.col.flags148 & 0x40) {
    // Raw scripted matrix — +0x302..0x322 * scale, tz = z + zBias.
    for (int i = 0; i < 9; ++i)
      obj.col.xform[i] = obj.rawMatrix[i] * obj.col.scale;
    obj.col.origin[0] = obj.pos[0];
    obj.col.origin[1] = obj.pos[1];
    obj.col.origin[2] = obj.pos[2] + obj.zBias;
  } else {
    buildObjectMatrix(obj.pitchDeg, obj.bankDeg, obj.yawDeg,
                      obj.col.scale, obj.pos, obj.col.xform,
                      obj.col.origin);
  }

  const std::size_t n = obj.model.elems.size();
  for (std::size_t e = 0; e < n; ++e) {
    if ((1u << (e & 31)) & obj.col.elemMaskB) continue;   // +0x2c8 skip
    CollisionElement& el = obj.model.elems[e];
    // FUN_00459e40 — 8 corners of the local AABB -> world AABB (+0x44).
    float worldMin[3] = {1e30f, 1e30f, 1e30f};
    float worldMax[3] = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
      const float corner[3] = {
          (c & 1) ? el.localAabb[3] : el.localAabb[0],
          (c & 2) ? el.localAabb[4] : el.localAabb[1],
          (c & 4) ? el.localAabb[5] : el.localAabb[2]};
      float w[3];
      transformPoint(obj.col.xform, obj.col.origin, corner, w);
      for (int k = 0; k < 3; ++k) {
        if (w[k] < worldMin[k]) worldMin[k] = w[k];
        if (w[k] > worldMax[k]) worldMax[k] = w[k];
      }
    }
    el.aabb[0] = worldMin[0];
    el.aabb[1] = worldMin[1];
    el.aabb[2] = worldMin[2];
    el.aabb[3] = worldMax[0];
    el.aabb[4] = worldMax[1];
    el.aabb[5] = worldMax[2];
    // Union into the object AABB (+0x198).
    for (int k = 0; k < 3; ++k) {
      if (el.aabb[k] < obj.col.aabb[k]) obj.col.aabb[k] = el.aabb[k];
      if (el.aabb[3 + k] > obj.col.aabb[3 + k])
        obj.col.aabb[3 + k] = el.aabb[3 + k];
    }
  }

  // +0x1b0..+0x1cb — world-space refpoints (subtype 0x4a reads them;
  // the anim driver keeps the LOCAL slots in model.refPoints, so the
  // world copy is refreshed with the same matrix/origin).
  for (std::uint32_t i = 0; i < obj.model.refPointCount && i < 8; ++i)
    transformPoint(obj.col.xform, obj.col.origin, obj.model.refPoints[i],
                   obj.worldRef[i]);
}

// ---------------------------------------------------------------------------
// FUN_004566f0 — init (collision subset)
// ---------------------------------------------------------------------------

// FUN_004566f0's default block + identity matrix — the part that runs
// BEFORE the table-2 "%s$%s" init script (which may then override
// +0x58 scale / +0x08 health / etc.). rebuildObjectTransform is NOT
// called here: FUN_004566f0 runs it once AFTER the init script, so the
// script's +0x58/+0x5c take effect. Callers that don't run a script
// call initObjectCollision (defaults + view + rebuild) instead.
void initObjectDefaults(DynamicObject& obj) {
  // OBSERVED (MDK95.EXE FUN_004566f0): +0x08=10, +0x38=50, +0x3c=10,
  // +0x40=15, +0x44=64, +0x48=32, +0x58=1.0, +0xe0=30, +0xd4/+0xe8/
  // +0x2c0=1.0, +0x2c4=1000, +0xc0=+0xd4, +0xac=+0xc0.
  obj.health = 10;                       // +0x08
  obj.field38 = 50.0f;                   // +0x38
  obj.field3c = 10.0f;                   // +0x3c
  obj.field40 = 15.0f;                   // +0x40
  obj.field44 = 64.0f;                   // +0x44
  obj.field48 = 32.0f;                   // +0x48
  obj.col.scale = 1.0f;                  // +0x58
  obj.animRate = 30.0f;              // +0xe0
  obj.fieldE8 = 1.0f;                    // +0xe8
  obj.field2c0 = 1.0f;                   // +0x2c0
  obj.field2c4 = 1000.0f;                // +0x2c4
  const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::memcpy(obj.col.xform, ident, sizeof(ident));  // +0xac..+0xd4
  obj.col.origin[0] = obj.pos[0];
  obj.col.origin[1] = obj.pos[1];
  obj.col.origin[2] = obj.pos[2];
}

void initObjectCollision(DynamicObject& obj) {
  initObjectDefaults(obj);
  obj.syncCollisionView();
  rebuildObjectTransform(obj);
}

// ---------------------------------------------------------------------------
// FUN_00433d40 record rewrite + FUN_00456808 spawn
// ---------------------------------------------------------------------------

std::vector<std::size_t> resolveArenaRecordNames(
    DtiArenaRecord& rec, const EnemyTable& enemies) {
  std::vector<std::size_t> failed;
  for (std::size_t i = 0; i < rec.subRecords.size(); ++i) {
    auto& sr = rec.subRecords[i];
    if (sr.type != 2 && sr.type != 4) continue;
    const int idx = enemies.indexOf(sr.name18());
    if (idx < 0) {
      failed.push_back(i);
      continue;
    }
    if (sr.type == 2) {
      sr.fields[0] = (sr.fields[0] & 0xffffu) |
                     (static_cast<std::uint32_t>(idx) << 16);
    } else {
      sr.fields[0] = static_cast<std::uint32_t>(idx);
    }
  }
  return failed;
}

namespace {

// Shared tail of the type-2/type-4 spawn paths: dedup, alloc, model
// deep-copy, position/prevPos, init, +0x108 script binding.
DynamicObject* spawnRecord(DynamicArena& arena, int modelIndex,
                           std::uint16_t spawnId, const float pos[3],
                           DynamicModelSource modelFor, void* ctx,
                           bool dedupBySpawnId,
                           DynamicObjectScriptSource scriptFor,
                           void* scriptCtx) {
  for (const auto& up : arena.storage) {
    const DynamicObject& o = *up;
    if (!o.col.named) continue;
    if (static_cast<int>(o.enemyIndex) != modelIndex) continue;
    if (dedupBySpawnId && o.spawnId != spawnId) continue;
    if (o.pos[0] == pos[0] && o.pos[1] == pos[1] && o.pos[2] == pos[2])
      return nullptr;   // dedup hit — original skips the spawn
  }
  const RuntimeModel* src = modelFor ? modelFor(modelIndex, ctx) : nullptr;
  if (!src) return nullptr;
  DynamicObject& o = arena.allocFront();
  o.model = deepCopyModel(*src);        // FUN_00403720
  o.enemyIndex = static_cast<std::uint16_t>(modelIndex);   // +0x04
  o.spawnId = spawnId;                                     // +0x146
  o.setPosition(pos[0], pos[1], pos[2]);                   // +0x10..0x18
  o.prevPos[0] = pos[0];                                   // +0x180..
  o.prevPos[1] = pos[1];
  o.prevPos[2] = pos[2];
  o.prevYawDeg = o.yawDeg;
  initObjectCollision(o);               // FUN_004566f0 subset
  // FUN_00456808 — the table-0 "%s$%s_%d" record binds +0x108, the
  // persistent per-object script (the tr_alcmd object tick's gate).
  if (scriptFor) {
    const std::string mn = o.model.modelName();
    o.field108 = scriptFor(arena.name.c_str(), mn.c_str(), spawnId,
                           scriptCtx);
  }
  return &o;
}

} // namespace

int spawnArenaObjects(DynamicArena& arena, const DtiArenaRecord& rec,
                      DynamicModelSource modelFor, void* ctx,
                      std::vector<DynamicObject*>* spawned,
                      DynamicObjectScriptSource scriptFor,
                      void* scriptCtx) {
  int count = 0;
  for (const auto& sr : rec.subRecords) {
    const float pos[3] = {sr.fieldAsFloat(2), sr.fieldAsFloat(3),
                          sr.fieldAsFloat(4)};
    if (sr.type == 2) {
      const int enemyIdx =
          static_cast<int>((sr.fields[0] >> 16) & 0xffffu);
      const std::uint16_t spawnId =
          static_cast<std::uint16_t>(sr.fields[0] & 0xffffu);
      DynamicObject* o =
          spawnRecord(arena, enemyIdx, spawnId, pos, modelFor, ctx, true,
                      scriptFor, scriptCtx);
      if (o) {
        o->behaviorByte = 7;            // +0x11c
        ++count;
        if (spawned) spawned->push_back(o);
      }
    } else if (sr.type == 4) {
      const int modelIdx = static_cast<int>(sr.fields[0] & 0xffffu);
      DynamicObject* o =
          spawnRecord(arena, modelIdx, 0, pos, modelFor, ctx, false,
                      scriptFor, scriptCtx);
      if (o) {
        o->health = 1;                  // +0x08 = 1 (overrides init's 10)
        o->col.flags148 |= 0x08a0;      // +0x148 dword |= 0x2008a0:
        o->col.flags149 |= 0x08;        //   byte +0x148 = 0xa0
        o->col.flags14a |= 0x20;        //   byte +0x149 = 0x08
                                        //   byte +0x14a = 0x20 (mover)
        o->syncCollisionView();
        if (o->model.modelName() == "SW_DUMMY") {
          for (std::size_t e = 0; e < o->model.elems.size(); ++e) {
            if (o->model.elemName(e) == "SW_DUMMY")
              o->col.elemMaskB |= (1u << (e & 31));   // +0x2c8
          }
        }
        ++count;
        if (spawned) spawned->push_back(o);
      }
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// FUN_004572ac tail — ride displacement + prev-state latch
// ---------------------------------------------------------------------------

void applyRideDisplacement(const DynamicObject& obj, CollisionState& cs,
                           float* playerYawDeg) {
  if (&obj.col != cs.rideObj) return;
  cs.pos[0] += obj.pos[0] - obj.prevPos[0];
  cs.pos[1] += obj.pos[1] - obj.prevPos[1];
  cs.pos[2] += obj.pos[2] - obj.prevPos[2];
  if (playerYawDeg) *playerYawDeg += obj.yawDeg - obj.prevYawDeg;
}

void latchObjectPrevState(DynamicObject& obj) {
  // FUN_004572ac tail (0x45737b): +0x18c velocity = pos - prevPos
  // (the 1.0/DAT_0049b6f0 scale is x1.0), written BEFORE the latch.
  obj.field18c[0] = obj.pos[0] - obj.prevPos[0];
  obj.field18c[1] = obj.pos[1] - obj.prevPos[1];
  obj.field18c[2] = obj.pos[2] - obj.prevPos[2];
  obj.prevPos[0] = obj.pos[0];
  obj.prevPos[1] = obj.pos[1];
  obj.prevPos[2] = obj.pos[2];
  obj.prevYawDeg = obj.yawDeg;
}

void updateMoverCollision(DynamicObject& obj, CollisionState& cs,
                          float* playerYawDeg) {
  rebuildObjectTransform(obj);
  applyRideDisplacement(obj, cs, playerYawDeg);
  latchObjectPrevState(obj);
}

} // namespace mdk
