// traversal_runtime.cpp — Phase 5G: native traversal runtime host.
// See traversal_runtime.h for the original-ownership map and the
// evidence-level notes. Everything below is OBSERVED from MDK95.EXE
// disassembly unless marked NATIVE (port infrastructure), BOUNDED
// (the eager stream model), or SEAM (deferred, counted not emulated).

#include "core/traversal_runtime.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <span>

#include "core/data_root.h"
#include "core/frontend_machines.h"

namespace mdk {
namespace {

constexpr std::size_t kMaxDataFileBytes = 512u * 1024u * 1024u;

float asFloat(std::uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

std::uint32_t asToken(const void* p) {
  return static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(p));
}

// The per-axis slab test FUN_00435178 / FUN_00434b44 emit for the
// segment prev->cur against [lo,hi] — OBSERVED structure
// (0x43521d..0x4352a0 / 0x434bca..0x434c3a):
//   inside [lo,hi]           -> pass
//   (cur<lo) != (prev<lo)    -> pass   (crossed the low edge)
//   (cur<hi) != (prev<hi)    -> pass   (crossed the high edge)
//   otherwise                -> fail
// Note this is NOT symmetric: an overshoot (cur outside, prev
// inside) passes, as does an exact-edge landing.
bool segAxis(float lo, float hi, float prev, float cur) {
  if (lo <= cur && cur <= hi) return true;
  if ((cur < lo) != (prev < lo)) return true;
  if ((cur < hi) != (prev < hi)) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Lazy model cache — FUN_004286c8's deferred geometry table.
// ---------------------------------------------------------------------------

const RuntimeModel* traversalModelFor(int idx, void* ctx) {
  TraversalLevel& lv = *static_cast<TraversalLevel*>(ctx);
  if (idx < 0 || static_cast<std::size_t>(idx) >= lv.models.size())
    return nullptr;
  if (lv.models[idx]) return &*lv.models[idx];
  if (lv.modelTried[idx]) return nullptr;
  lv.modelTried[idx] = true;
  auto span = enemyModelData(
      lv.enemies, idx, std::span<const std::byte>(lv.cmiBytes), lv.cmi,
      &lv.mto, std::span<const std::byte>(lv.mtoBytes));
  if (!span) {
    ++lv.modelsFailed;
    return nullptr;
  }
  auto model = parseGeometryRecord(
      reinterpret_cast<const std::uint8_t*>(span->data()),
      reinterpret_cast<const std::uint8_t*>(span->data() + span->size()));
  if (!model) {
    ++lv.modelsFailed;
    return nullptr;
  }
  lv.models[idx] = std::move(model);
  ++lv.modelsResolved;
  return &*lv.models[idx];
}

// Locate the MTO directory entry + block for an arena name (the
// original searches table 1 by 8-byte name — FUN_00432404's lookup).
const MtoBlock* findMtoBlock(const TraversalLevel& lv,
                           const std::string& name) {
  for (std::size_t i = 0; i < lv.mto.entries.size(); ++i)
    if (lv.mto.entries[i].name() == name) return &lv.mto.blocks[i];
  return nullptr;
}

// CMI table-3 two-string lookup (FUN_00458550): an arena "has a
// script object" when table 3 holds a record whose name matches the
// arena name. The VM stays deferred — this only feeds the seam count.
bool cmiTable3Has(const CmiDirectory& cmi, const std::string& name) {
  if (cmi.tables.size() < 4) return false;
  for (const auto& rec : cmi.tables[3].records)
    if (rec.name() == name) return true;
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Arena / level teardown
// ---------------------------------------------------------------------------

TraversalArena::~TraversalArena() { surfaceRecordsDestroy(surface); }

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

const char* traversalLoadErrorName(TraversalLoadError e) {
  switch (e) {
  case TraversalLoadError::kOk: return "ok";
  case TraversalLoadError::kDtiRead: return "dti-read";
  case TraversalLoadError::kCmiRead: return "cmi-read";
  case TraversalLoadError::kMtoRead: return "mto-read";
  case TraversalLoadError::kDtiParse: return "dti-parse";
  case TraversalLoadError::kCmiParse: return "cmi-parse";
  case TraversalLoadError::kMtoParse: return "mto-parse";
  case TraversalLoadError::kArenaNotFound: return "arena-not-found";
  case TraversalLoadError::kCollisionBlobMissing:
    return "collision-blob-missing";
  case TraversalLoadError::kCollisionBlobParse:
    return "collision-blob-parse";
  case TraversalLoadError::kConnectUnmatched: return "connect-unmatched";
  case TraversalLoadError::kConnectMismatch: return "connect-mismatch";
  case TraversalLoadError::kBadSpawnArena: return "bad-spawn-arena";
  case TraversalLoadError::kBadStart: return "bad-start";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// FUN_00419ee0 — arena collision blob from the same-named MTO block.
// OBSERVED chain: table-1 lookup by name -> block at +0x08 file offset;
// buf = block+4; the collision counted-array table sits at buf+buf[0x0c]
// (MtoBlock::fieldAt0x0C, relative to fileOffset+4); FUN_00419ee0 walks
// countA*10(+pad2) -> countB*44 -> countC*36 -> countD*12 -> tail and
// patches the BSP nodes' +0x1c/+0x20 blob-end-relative offsets.
// ---------------------------------------------------------------------------

TraversalLoadError traversalArenaLoadGeometry(TraversalRuntime& rt,
                                              TraversalArena& arena,
                                              std::string* detail) {
  if (arena.geometryLoaded) return TraversalLoadError::kOk;
  const MtoBlock* block = findMtoBlock(rt.level, arena.name);
  if (!block) {
    if (detail) *detail = "no MTO block named " + arena.name;
    return TraversalLoadError::kCollisionBlobMissing;
  }
  const std::uint64_t geom = block->fileOffset + 4 + block->fieldAt0x0C;
  const std::uint64_t blockEnd = block->fileOffset + block->blockLength;
  if (geom + 4 > blockEnd ||
      geom + 4 > rt.level.mtoBytes.size()) {
    if (detail) *detail = "region-C offset out of block for " + arena.name;
    return TraversalLoadError::kCollisionBlobParse;
  }
  const std::uint8_t* blob = reinterpret_cast<const std::uint8_t*>(
      rt.level.mtoBytes.data() + geom);
  const std::size_t avail = static_cast<std::size_t>(
      std::min<std::uint64_t>(blockEnd, rt.level.mtoBytes.size()) - geom);
  std::uint32_t counts[4] = {};
  if (!collisionBlobParse(blob, avail, &arena.dyn.col, counts)) {
    if (detail) *detail = "collision blob parse failed for " + arena.name;
    return TraversalLoadError::kCollisionBlobParse;
  }
  // deepFloorZ (+0x44e) is provably zero for every arena: the
  // 0x466-stride record array is memset(0) at creation inside
  // FUN_00433d40 (0x434147 call 0x47d20a, edx=0), the per-record
  // init loop only writes +0x00 name/+0x34/+0x38/+0x3c/+0x40/
  // +0x44/+0x5c..+0x64/+0x462, and a full code-section sweep finds
  // no store to +0x44e (the only overlapping +0x44c/+0x450 dword
  // writes are FUN_00413c20/FUN_00413dd8 on a different record
  // type — name at +4, dispatch block at +0x444). All five +0x44e
  // readers take it as the arena's abyss reference: the player
  // failsafe (0x4673f3, -50) and object respawn checks through
  // obj+0x60 (0x4583ab/0x45bdd6/0x45fd55, -200/-150). Leaving the
  // CollisionArena default reproduces the observed flat -50 catch.

  // The arena's surface state aliases the live poly table (the
  // original's +0x28/+0x10 fields on the surface block).
  arena.surface.polys = const_cast<CollisionPoly*>(arena.dyn.col.polys);
  arena.surface.polyCount = static_cast<std::int32_t>(counts[2]);
  arena.geometryLoaded = true;
  return TraversalLoadError::kOk;
}

// ---------------------------------------------------------------------------
// Attach/detach — FUN_00432d9c / FUN_00432980 / FUN_00432bf8
// ---------------------------------------------------------------------------

static void traversalEnsureLoaded(TraversalRuntime& rt,
                                  TraversalArena& arena) {
  if (!arena.geometryLoaded) {
    std::string detail;
    traversalArenaLoadGeometry(rt, arena, &detail);
  }
  // FUN_00432d9c's tail: the +0x44 bit-2 spawn-once gate. The original
  // calls FUN_00456808 only when the bit is clear, then sets it.
  if (!arena.objectsSpawned && arena.rec) {
    spawnArenaObjects(arena.dyn, *arena.rec, traversalModelFor,
                      &rt.level);
    arena.objectsSpawned = true;
  }
}

void traversalAttachSideEffects(TraversalRuntime& rt,
                              TraversalArena& arena) {
  traversalEnsureLoaded(rt, arena);
  rt.partnerActive = true;
  rt.cs.carrierValid = 1;
}

// FUN_00432980 tail — object migration from stale peer arenas:
// for each type-6 peer of `a`, objects listed there whose +0x60
// home is `a` are transferred (pendingArena path). Dormant in the
// bounded runtime — no script moves objects across lists — but
// the mechanism is the original's.
void traversalMigrateInto(TraversalRuntime& rt, TraversalArena& a) {
  if (!a.rec) return;
  for (const DtiSubRecord& r : a.rec->subRecords) {
    if (r.type != 6) continue;
    const int peerIdx = static_cast<int>(r.fields[0]);
    if (peerIdx < 0 ||
        static_cast<std::size_t>(peerIdx) >= rt.arenas.size())
      continue;
    TraversalArena& peer = *rt.arenas[peerIdx];
    if (&peer == rt.cur || &peer == &a) continue;
    for (auto it = peer.dyn.storage.begin();
         it != peer.dyn.storage.end();) {
      DynamicObject& o = **it;
      auto next = std::next(it);
      if (o.col.named && (o.col.flags14a & 0x10) &&
          o.arena == &a.dyn) {
        o.pendingArena = &a.dyn;
        peer.dyn.transfer(o, a.dyn);
        ++rt.seams.objectMigrations;
      }
      it = next;
    }
  }
}

void traversalAttachPartner(TraversalRuntime& rt, TraversalArena& a) {
  if (&a == rt.cur) {
    traversalAttachSideEffects(rt, a);
    return;
  }
  if (&a != rt.partner) {
    rt.partner = &a;
    rt.cs.carrier = &a.dyn.col;
    traversalEnsureLoaded(rt, a);
    traversalMigrateInto(rt, a);
  }
  rt.partnerActive = true;
  rt.cs.carrierValid = 1;
}

void traversalPrefetchPartner(TraversalRuntime& rt, TraversalArena& a) {
  // FUN_00432980 via the type-3 path: partner slot + stream request
  // (eager geometry), then 0x540ca8 = 0 — no spawn-once.
  if (&a == rt.partner) return;
  rt.partner = &a;
  rt.cs.carrier = &a.dyn.col;
  if (!a.geometryLoaded) {
    std::string detail;
    traversalArenaLoadGeometry(rt, a, &detail);
  }
  rt.partnerActive = false;
  rt.cs.carrierValid = 0;
}

void traversalDetachPartner(TraversalRuntime& rt) {
  // FUN_00432bf8: when the carrier is busy and the master-load slot
  // is the partner, that load is cancelled first.
  if (rt.cs.carrierBusy && rt.loadArena == rt.partner)
    rt.loadArena = nullptr;
  rt.partner = nullptr;
  rt.cs.carrier = nullptr;
  rt.partnerActive = false;
  rt.cs.carrierValid = 0;
}

// ---------------------------------------------------------------------------
// FUN_00435178 — portal scan on the current arena's type-6 records.
// fields[0] = partner arena index (rewritten by connect pairing),
// fields[1] = side code, fields[2..7] = box {x0,y0,z0,x1,y1,z1}.
// Position source ebx = 0x540bfc, previous = ecx = 0x540c08.
// ---------------------------------------------------------------------------

TraversalArena* traversalPortalTest(TraversalRuntime& rt) {
  TraversalArena* cur = rt.cur;
  if (!cur || !cur->rec) return nullptr;
  const float* p = rt.cs.pos;
  const float* q = rt.cs.entryPos;
  constexpr float kSlabMargin = -5.0f;  // 0x497778 — z-slab low margin
  constexpr float kZPlane = -0.5f;      // 0x497780 — z-portal plane
  for (const DtiSubRecord& r : cur->rec->subRecords) {
    if (r.type != 6) continue;
    const std::uint32_t side = r.fields[1];
    const float x0 = r.fieldAsFloat(2), y0 = r.fieldAsFloat(3);
    const float z0 = r.fieldAsFloat(4), x1 = r.fieldAsFloat(5);
    const float y1 = r.fieldAsFloat(6), z1 = r.fieldAsFloat(7);
    bool pass = false;
    switch (side) {
    case 0: // crossed x0 going -x
      pass = segAxis(z0 + kSlabMargin, z1, q[2], p[2]) &&
             segAxis(y0, y1, q[1], p[1]) &&
             p[0] < x0 && q[0] >= x0;
      break;
    case 1: // crossed x0 going +x
      pass = segAxis(z0 + kSlabMargin, z1, q[2], p[2]) &&
             segAxis(y0, y1, q[1], p[1]) &&
             p[0] > x0 && q[0] <= x0;
      break;
    case 2: // crossed y0 going -y
      pass = segAxis(z0 + kSlabMargin, z1, q[2], p[2]) &&
             segAxis(x0, x1, q[0], p[0]) &&
             p[1] < y0 && q[1] >= y0;
      break;
    case 3: // crossed y0 going +y
      pass = segAxis(z0 + kSlabMargin, z1, q[2], p[2]) &&
             segAxis(x0, x1, q[0], p[0]) &&
             p[1] > y0 && q[1] <= y0;
      break;
    case 4: // crossed the z0-0.5 plane going down
      pass = segAxis(x0, x1, q[0], p[0]) &&
             segAxis(y0, y1, q[1], p[1]) &&
             p[2] < z0 + kZPlane && q[2] >= z0 + kZPlane;
      break;
    case 7: // crossed the z0-0.5 plane going up
      pass = segAxis(x0, x1, q[0], p[0]) &&
             segAxis(y0, y1, q[1], p[1]) &&
             p[2] > z0 + kZPlane && q[2] <= z0 + kZPlane;
      break;
    case 5: // diagonal half-plane — OBSERVED: pass when cross > 0
      if (segAxis(x0, x1, q[0], p[0]) &&
          segAxis(y0, y1, q[1], p[1]) &&
          segAxis(z0 + kSlabMargin, z1, q[2], p[2])) {
        const float cross =
            (p[1] - y0) * (x1 - x0) - (y1 - y0) * (p[0] - x0);
        pass = cross > 0.0f;
      }
      break;
    case 6:
    default:
      // OBSERVED: every side code not matched above falls through to
      // the side-6 block — the same slabs plus cross < 0.
      if (segAxis(x0, x1, q[0], p[0]) &&
          segAxis(y0, y1, q[1], p[1]) &&
          segAxis(z0 + kSlabMargin, z1, q[2], p[2])) {
        const float cross =
            (p[1] - y0) * (x1 - x0) - (y1 - y0) * (p[0] - x0);
        pass = cross < 0.0f;
      }
      break;
    }
    if (!pass) continue;
    const int dst = static_cast<int>(r.fields[0]);
    if (dst < 0 || static_cast<std::size_t>(dst) >= rt.arenas.size())
      return nullptr;
    return rt.arenas[dst].get();
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// FUN_00434b44 — type-1/3 trigger scan on the current arena. The test
// uses x/y only (0x434bb8..0x434d29) with the same segment-overlap
// pattern; fields[0] is the arena index (-1 = detach).
// ---------------------------------------------------------------------------

void traversalTriggerScan(TraversalRuntime& rt) {
  TraversalArena* cur = rt.cur;
  if (!cur || !cur->rec) return;
  const float* p = rt.cs.pos;
  const float* q = rt.cs.entryPos;
  for (const DtiSubRecord& r : cur->rec->subRecords) {
    if (r.type != 1 && r.type != 3) continue;
    const float x0 = r.fieldAsFloat(2), y0 = r.fieldAsFloat(3);
    const float x1 = r.fieldAsFloat(5), y1 = r.fieldAsFloat(6);
    if (!segAxis(x0, x1, q[0], p[0]) ||
        !segAxis(y0, y1, q[1], p[1]))
      continue;
    const int f0 = static_cast<int>(r.fields[0]);
    if (r.type == 1) {
      ++rt.seams.type1Triggers;
      if (f0 < 0) {
        traversalDetachPartner(rt);
      } else if (static_cast<std::size_t>(f0) < rt.arenas.size()) {
        traversalAttachPartner(rt, *rt.arenas[f0]);
      }
    } else {
      ++rt.seams.type3Prefetches;
      if (f0 < 0) {
        traversalDetachPartner(rt);
      } else if (static_cast<std::size_t>(f0) < rt.arenas.size()) {
        traversalPrefetchPartner(rt, *rt.arenas[f0]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// FUN_00412d04 — type-7 volume activation seam (tr_alcmd-driven).
// fields[0] is the script-facing hotspot id; the box is fields[2..7].
// The record is created only when a script command supplies
// kind/rate/mask — dormant otherwise (no load-time instantiation).
// ---------------------------------------------------------------------------

bool traversalVolumeActivate(TraversalArena& arena, int id, int kind,
                             float rate, std::uint32_t mask,
                             bool noFalloff) {
  if (!arena.rec) return false;
  for (const DtiSubRecord& r : arena.rec->subRecords) {
    if (r.type != 7 || static_cast<int>(r.fields[0]) != id) continue;
    float box[6];
    for (int i = 0; i < 6; ++i) box[i] = r.fieldAsFloat(2 + i);
    SurfaceRecord* rec =
        surfaceVolumeCreate(arena.surface, kind, box, rate, mask);
    if (!rec) return false;
    rec->name = static_cast<std::uint32_t>(id);
    rec->f10 = noFalloff ? 1u : 0u;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// FUN_00434e54 — connect pairing. Type-6 records carry a level-global
// connect id in fields[0]; each id appears in exactly two arenas with
// side codes (fields[1]) forming an even/odd+1 pair and identical
// boxes. The loader rewrites fields[0] to the OTHER arena's index —
// the portal test then indexes the arena table directly
// (imul fields[0],0x466). Errors are load-fatal in the original.
// ---------------------------------------------------------------------------

TraversalLoadError traversalConnectPairing(
    std::vector<DtiArenaRecord>& arenas, std::string* detail) {
  // Collect every type-6 record's original connect id first — the
  // rewrite below destroys the key.
  struct Conn {
    DtiSubRecord* rec;
    std::size_t arena;
    std::uint32_t id;
  };
  std::vector<Conn> conns;
  for (std::size_t i = 0; i < arenas.size(); ++i)
    for (auto& r : arenas[i].subRecords)
      // OBSERVED gate: only ids > 999 enter the pairing pass; a
      // type-6 record with fields[0] <= 999 keeps its value as a
      // direct arena index.
      if (r.type == 6 && r.fields[0] > 999)
        conns.push_back({&r, i, r.fields[0]});

  std::vector<bool> done(conns.size(), false);
  for (std::size_t a = 0; a < conns.size(); ++a) {
    if (done[a]) continue;
    std::size_t b = conns.size();
    for (std::size_t j = a + 1; j < conns.size(); ++j)
      if (!done[j] && conns[j].id == conns[a].id &&
          conns[j].arena != conns[a].arena) {
        b = j;
        break;
      }
    if (b == conns.size()) {
      if (detail)
        *detail = "unmatched connect id " + std::to_string(conns[a].id) +
                  " in " + arenas[conns[a].arena].name();
      return TraversalLoadError::kConnectUnmatched;
    }
    // OBSERVED rule: side codes form an even/odd+1 pair (XOR 1) and
    // the boxes are identical. Violations -> "Mismatched connect
    // type" in the original.
    const bool sideOk =
        (conns[a].rec->fields[1] ^ conns[b].rec->fields[1]) == 1;
    bool boxOk = true;
    for (int k = 0; k < 6; ++k)
      boxOk &= conns[a].rec->fields[2 + k] == conns[b].rec->fields[2 + k];
    if (!sideOk || !boxOk) {
      if (detail)
        *detail = "mismatched connect id " + std::to_string(conns[a].id) +
                  " between " + arenas[conns[a].arena].name() + " and " +
                  arenas[conns[b].arena].name();
      return TraversalLoadError::kConnectMismatch;
    }
    conns[a].rec->fields[0] = static_cast<std::uint32_t>(conns[b].arena);
    conns[b].rec->fields[0] = static_cast<std::uint32_t>(conns[a].arena);
    done[a] = done[b] = true;
  }
  return TraversalLoadError::kOk;
}

// ---------------------------------------------------------------------------
// Level load — FUN_00433d40 + the FUN_004346e8 spawn path.
// ---------------------------------------------------------------------------

TraversalLoadError traversalRuntimeLoad(const DataRoot& root,
                                        const std::string& dtiPath,
                                        const std::string& cmiPath,
                                        const std::string& mtoPath,
                                        TraversalRuntime& rt,
                                        std::string* detail) {
  auto fail = [&](TraversalLoadError e, const std::string& msg) {
    if (detail) *detail = msg;
    return e;
  };
  auto dti = root.readFile(dtiPath, kMaxDataFileBytes, detail);
  if (!dti) return fail(TraversalLoadError::kDtiRead,
                        detail ? *detail : "cannot read " + dtiPath);
  auto cmi = root.readFile(cmiPath, kMaxDataFileBytes, detail);
  if (!cmi) return fail(TraversalLoadError::kCmiRead,
                        detail ? *detail : "cannot read " + cmiPath);
  auto mto = root.readFile(mtoPath, kMaxDataFileBytes, detail);
  if (!mto) return fail(TraversalLoadError::kMtoRead,
                        detail ? *detail : "cannot read " + mtoPath);

  rt.level.dtiBytes = std::move(*dti);
  rt.level.cmiBytes = std::move(*cmi);
  rt.level.mtoBytes = std::move(*mto);

  rt.level.dti = inspectDtiStructure(rt.level.dtiBytes);
  if (rt.level.dti.status != DtiStructureStatus::kOk)
    return fail(TraversalLoadError::kDtiParse, rt.level.dti.detail);
  rt.level.cmi = inspectCmiDirectory(rt.level.cmiBytes);
  if (rt.level.cmi.status != CmiDirectoryStatus::kOk)
    return fail(TraversalLoadError::kCmiParse, rt.level.cmi.detail);
  rt.level.mto = inspectMtoDirectory(rt.level.mtoBytes);
  if (rt.level.mto.status != MtoDirectoryStatus::kOk)
    return fail(TraversalLoadError::kMtoParse, rt.level.mto.detail);

  rt.level.enemies = buildEnemyTable(rt.level.cmi);
  rt.level.models.resize(rt.level.enemies.entries.size());
  rt.level.modelTried.assign(rt.level.enemies.entries.size(), false);

  // The loader's arena work records: verbatim 36-byte sub-record
  // table + name fixups + connect pairing.
  rt.level.work = rt.level.dti.arenas;
  for (auto& rec : rt.level.work) {
    auto missing = resolveArenaRecordNames(rec, rt.level.enemies);
    for (std::size_t idx : missing)
      rt.level.unresolvedNames.push_back(rec.name() + ":" +
                                         std::to_string(idx));
  }
  if (auto e = traversalConnectPairing(rt.level.work, detail);
      e != TraversalLoadError::kOk)
    return e;

  rt.arenas.clear();
  rt.arenas.reserve(rt.level.work.size());
  for (std::size_t i = 0; i < rt.level.work.size(); ++i) {
    auto a = std::make_unique<TraversalArena>();
    a->name = rt.level.work[i].name();
    a->index = static_cast<int>(i);
    a->rec = &rt.level.work[i];
    a->dyn.name = a->name;
    a->scalar = rt.level.work[i].scalar();
    a->hasScriptObject = cmiTable3Has(rt.level.cmi, a->name);
    rt.arenas.push_back(std::move(a));
  }

  // OBSERVED spawn (FUN_004346e8 tail / loader): s0 word 0 = arena
  // index (min-clamped to the arena count), words 1..3 = position
  // floats, word 4 = yaw degrees.
  const auto& p = rt.level.dti.params;
  int spawnArena = static_cast<int>(p[0]);
  if (spawnArena < 0)
    return fail(TraversalLoadError::kBadSpawnArena,
                "s0 arena index negative");
  if (rt.arenas.empty())
    return fail(TraversalLoadError::kBadSpawnArena, "no arenas");
  spawnArena = std::min(spawnArena,
                        static_cast<int>(rt.arenas.size()) - 1);
  const float pos[3] = {asFloat(p[1]), asFloat(p[2]), asFloat(p[3])};
  const float yawDeg = asFloat(p[4]);

  // Query gates (FUN_00433c4c): 0x540c68/0x540c6c/0x540c70 = 1.
  rt.cs.arenaValid = 1;
  rt.cs.queryEnabled = 1;
  rt.cs.objectDataLoaded = 1;
  rt.cs.contactHook = surfaceContactHook;
  rt.cs.surfaceContextMask = 8; // the player sweep channel

  rt.cur = rt.arenas[spawnArena].get();
  rt.cs.arena = &rt.cur->dyn.col;
  rt.cs.surface = &rt.cur->surface;
  for (int i = 0; i < 3; ++i) {
    rt.cs.pos[i] = pos[i];
    rt.cs.entryPos[i] = pos[i];
  }
  rt.vert.posX = pos[0];
  rt.vert.posY = pos[1];
  rt.vert.posZ = pos[2];
  rt.motion.yawDeg = yawDeg;
  // 0x540c30..0x540c44 — the per-query player AABB (the loader's
  // init writes it; the frame then refreshes it each tail).
  rt.cs.playerBox[0] = pos[0] - 1.25f;
  rt.cs.playerBox[1] = pos[1] - 1.25f;
  rt.cs.playerBox[2] = pos[2];
  rt.cs.playerBox[3] = pos[0] + 1.25f;
  rt.cs.playerBox[4] = pos[1] + 1.25f;
  rt.cs.playerBox[5] = pos[2] + 4.25f;

  // Initial-arena stream + spawn-once (the loader's warm attach).
  traversalEnsureLoaded(rt, *rt.cur);
  return TraversalLoadError::kOk;
}

// NATIVE DIAGNOSTIC OVERRIDE — not original behavior.
TraversalLoadError traversalRuntimeDiagnosticStart(
    TraversalRuntime& rt, int arenaIndex, const float pos[3],
    float yawDeg, std::string* detail) {
  if (arenaIndex < 0 ||
      static_cast<std::size_t>(arenaIndex) >= rt.arenas.size()) {
    if (detail)
      *detail = "arena index " + std::to_string(arenaIndex) +
                " out of range";
    return TraversalLoadError::kBadStart;
  }
  traversalDetachPartner(rt);
  rt.cur = rt.arenas[arenaIndex].get();
  rt.cs.arena = &rt.cur->dyn.col;
  rt.cs.surface = &rt.cur->surface;
  for (int i = 0; i < 3; ++i) {
    rt.cs.pos[i] = pos[i];
    rt.cs.entryPos[i] = pos[i];
  }
  rt.vert.posX = pos[0];
  rt.vert.posY = pos[1];
  rt.vert.posZ = pos[2];
  rt.motion.yawDeg = yawDeg;
  rt.cs.playerBox[0] = pos[0] - 1.25f;
  rt.cs.playerBox[1] = pos[1] - 1.25f;
  rt.cs.playerBox[2] = pos[2];
  rt.cs.playerBox[3] = pos[0] + 1.25f;
  rt.cs.playerBox[4] = pos[1] + 1.25f;
  rt.cs.playerBox[5] = pos[2] + 4.25f;
  traversalEnsureLoaded(rt, *rt.cur);
  return TraversalLoadError::kOk;
}

// ---------------------------------------------------------------------------
// One traversal frame — FUN_00436100's traversal-active section.
// ---------------------------------------------------------------------------

TraversalFrameResult stepTraversalRuntime(
    TraversalRuntime& rt, const RawGameplayInput& raw,
    const GameplayInputBindings& bindings,
    const FrontendTimingState& timing) {
  TraversalFrameResult out;
  out.frame = rt.frameCounter;
  TraversalArena* cur = rt.cur;
  const float dt = timing.deltaSec;

  // OBSERVED gate: the whole traversal-active section runs only when
  // mode byte 0x541492 == 3 — this runtime models traversal mode only.
  // Head: FUN_0041b654 (0x540e9c gate) + FUN_00431cf4 — stream/render
  // seams; BOUNDED model keeps gates drained.
  rt.seams.streamStageCalls += 2;

  // FUN_00402388 — input consume sits BEFORE the dispatch in the
  // original (0x4361d2). It produces the merged control for frame
  // N+1; the dispatch below consumes the N-1 merge — one-frame
  // latency preserved structurally (Phase 5A).
  GameplayInputEnvironment inputEnv;
  inputEnv.moveBoostGate = rt.vert.jumpSustain;
  inputEnv.smoothedDelta = timing.smoothed;
  inputEnv.frameStep = timing.frameStep;
  const GameplayInputFrame nextFrame =
      consumeGameplayInput(raw, bindings, inputEnv, rt.inputState);

  // FUN_00437e80 (frontend) + FUN_0042534 stream-drain — seams.
  // ======================= player dispatch (FUN_00463608) =========
  PlayerMotionEnvironment motionEnv;
  motionEnv.smoothed = timing.smoothed;
  motionEnv.masterGate = rt.masterMoveGate;
  motionEnv.groundContact = rt.vert.contactObj != 0;
  motionEnv.lowFriction =
      rt.vert.contactObj != 0 && rt.lastContactPoly != nullptr &&
      (rt.lastContactPoly->flags & 4) != 0;
  motionEnv.moveBlocked =
      rt.vert.moveBlocker0 != 0 && rt.vert.moveBlockerFlag != 0;
  // Conveyor contribution — FUN_00412ef0 on the standing surface.
  float conv[3] = {0, 0, 0};
  if (rt.lastContactPoly)
    surfaceConveyorDelta(cur->surface, rt.lastContactPoly, dt, conv);
  motionEnv.conveyorX = conv[0];
  motionEnv.conveyorY = conv[1];
  motionEnv.conveyorZ = conv[2];

  PlayerMotionOutput mo =
      integratePlayerMotion(rt.prevFrame, motionEnv, rt.motion);

  // Horizontal collision — FUN_004630d4(disp, scale 0.75).
  const float preX = rt.cs.pos[0], preY = rt.cs.pos[1],
              preZ = rt.cs.pos[2];
  const CollisionPoly* hContact = collisionApply(
      rt.cs, mo.dispX, mo.dispY, mo.dispZ, 0.75f, nullptr, nullptr);
  const bool positionChanged =
      rt.cs.pos[0] != preX || rt.cs.pos[1] != preY ||
      rt.cs.pos[2] != preZ;
  playerMotionPostStep(motionEnv, positionChanged, rt.motion, mo);
  // 0x540e4c — every apply's EAX is stored (0 clears).
  rt.vert.contactObj = asToken(hContact);
  rt.lastContactPoly = hContact;

  // SEAM: FUN_0046603c slide helper — the slope-assist vector the
  // vertical integrator would consume. Deferred; counted.
  ++rt.seams.slideHelperCalls;

  // --------------------------- vertical --------------------------
  PlayerVerticalEnvironment vertEnv;
  vertEnv.smoothed = timing.smoothed;
  vertEnv.deltaSeconds = dt;
  vertEnv.frameStep = timing.frameStep;
  vertEnv.jumpHeld = rt.prevFrame.jump != 0;
  vertEnv.moveConsumed = mo.moveConsumed;
  vertEnv.locoState = rt.locoState;
  vertEnv.eventWordType = rt.eventType;
  vertEnv.vertEnable = true; // 0x540c6c — set at traversal init
  vertEnv.slideMode = rt.slideChannel != 0;
  vertEnv.sharedGateE6C = rt.cs.excludeObj != nullptr;
  vertEnv.flagE72bit1 = (rt.flagE72 & 2) != 0;
  vertEnv.carrierObj = rt.cs.carrier != nullptr;
  vertEnv.carrierCheckGate = rt.cs.carrierBusy != 0;
  // FUN_00412e94(player,1,&pos,&vec) — real type-7 volume query.
  float vec[3] = {0, 0, rt.vert.vertVel};
  const bool inRibbon =
      surfaceVolumeQuery(cur->surface, 1, rt.cs.pos, dt, vec) != 0;
  vertEnv.insideRibbonVolume = inRibbon;
  vertEnv.ribbonVelZ = inRibbon ? &vec[2] : nullptr;
  if (vertEnv.carrierObj && !vertEnv.carrierCheckGate &&
      rt.partner && rt.partnerActive) {
    float cvec[3] = {0, 0, rt.vert.vertVel};
    vertEnv.carrierInsideRibbonVolume =
        surfaceVolumeQuery(rt.partner->surface, 1, rt.cs.pos, dt,
                           cvec) != 0;
  }
  vertEnv.slideVec = nullptr; // FUN_0046603c's leftover — deferred
  vertEnv.deepFloorZ = cur->dyn.col.deepFloorZ;
  rt.vert.posX = rt.cs.pos[0];
  rt.vert.posY = rt.cs.pos[1];
  rt.vert.posZ = rt.cs.pos[2];
  PlayerVerticalFrame vf =
      integratePlayerVertical(vertEnv, rt.motion, rt.vert);
  float appliedZ = 0.0f;
  if (vf.collisionIssued) {
    const CollisionNode* node = nullptr;
    const CollisionPoly* vContact = collisionApply(
        rt.cs, 0.0f, 0.0f, vf.dispZ, 0.5f, nullptr, &node);
    appliedZ = rt.cs.pos[2] - rt.vert.posZ;
    rt.vert.contactObj = asToken(vContact);
    if (vContact) {
      rt.lastContactPoly = vContact;
      rt.vert.contactNormal[0] = node->nx;
      rt.vert.contactNormal[1] = node->ny;
      rt.vert.contactNormal[2] = node->nz;
    }
    VerticalCollisionResult vres;
    vres.contactObj = rt.vert.contactObj;
    vres.posX = rt.cs.pos[0];
    vres.posY = rt.cs.pos[1];
    vres.posZ = rt.cs.pos[2];
    if (vContact) {
      vres.normalX = node->nx;
      vres.normalY = node->ny;
      vres.normalZ = node->nz;
    }
    vres.hasFloor = (rt.cs.contactFlags & 2) != 0;
    vres.floorZ = rt.cs.floorZ;
    vres.blocker0 = asToken(rt.cs.floorObj);
    vres.blocker1 = rt.cs.floorElemMask;
    vres.blocker0Flag80 =
        rt.cs.floorObj && (rt.cs.floorObj->flags14a & 0x80);
    applyPlayerVerticalCollision(vertEnv, rt.motion, rt.vert, vres, vf);
  }
  playerVerticalPostStep(vertEnv, rt.vert);
  rt.vert.posX = rt.cs.pos[0];
  rt.vert.posY = rt.cs.pos[1];
  rt.vert.posZ = rt.cs.pos[2];
  if (vf.eventMag != 0) rt.locoState = vf.eventMag;
  rt.eventType = vf.eventType != 0 ? vf.eventType : mo.eventType;
  rt.eventMag = vf.eventMag != 0 ? vf.eventMag : mo.eventMag;
  if (vf.deepFloorReset) ++rt.seams.deepFloorFallbacks;
  if (mo.forwardIntent) ++rt.seams.mantleCalls;

  // Commit the N+1 merge — the latency hand-off.
  rt.prevFrame = nextFrame;

  // ================= traversal-active section =====================
  // 0x540c30..0x540c44 — the per-query player AABB (the original
  // rebuilds it here when mode 0x541492 == 3).
  rt.cs.playerBox[0] = rt.cs.pos[0] - 1.25f;
  rt.cs.playerBox[1] = rt.cs.pos[1] - 1.25f;
  rt.cs.playerBox[2] = rt.cs.pos[2];
  rt.cs.playerBox[3] = rt.cs.pos[0] + 1.25f;
  rt.cs.playerBox[4] = rt.cs.pos[1] + 1.25f;
  rt.cs.playerBox[5] = rt.cs.pos[2] + 4.25f;

  // FUN_00432f84 runs unconditionally (0x43627a); the 0x540c74 gate
  // lives inside it. Counted as a call, not a fire.
  ++rt.seams.objectPrepass;
  ++rt.seams.profilerHooks; // FUN_0042fecc rdtsc probe (0x43627f)

  // Object updates — FUN_004572ac subset (transform/ride/latch only;
  // behavior scripts stay a seam) then FUN_0045cf18 transfers.
  if (rt.cs.arenaValid) {
    TraversalArena* updateArenas[2] = {cur, nullptr};
    int updateCount = 1;
    if (rt.partnerActive && rt.partner) {
      updateArenas[1] = rt.partner;
      updateCount = 2;
    }
    for (int ai = 0; ai < updateCount; ++ai) {
      DynamicArena& da = updateArenas[ai]->dyn;
      for (auto& up : da.storage) {
        DynamicObject& o = *up;
        if (o.col.flags14a & 0x20) {
          updateMoverCollision(o, rt.cs, &rt.motion.yawDeg);
        } else {
          applyRideDisplacement(o, rt.cs, &rt.motion.yawDeg);
          latchObjectPrevState(o);
        }
      }
      // FUN_0045cf18 — pending-arena transfers (the splice mutates
      // the list, so collect first).
      std::vector<DynamicObject*> pending;
      for (auto& up : da.storage)
        if (up->pendingArena && up->pendingArena != &da)
          pending.push_back(up.get());
      for (DynamicObject* o : pending) {
        da.transfer(*o, *o->pendingArena);
        ++rt.seams.objectMigrations;
      }
    }
  }

  // FUN_004388d8 script-object calls — counted, not emulated.
  // OBSERVED gates: current on +0x220; partner on ca8 && ca4+0x220.
  if (cur->hasScriptObject) ++rt.seams.scriptObjectCalls;
  if (rt.partnerActive && rt.partner && rt.partner->hasScriptObject)
    ++rt.seams.scriptObjectCalls;
  ++rt.seams.profilerHooks; // FUN_0042fecc rdtsc probe (0x43632d)

  // 0x540ebc pending view snap — OBSERVED consumer is a teleport
  // apply (FUN_0043490c: pos/prevPos <- pendingView[0..2],
  // 0x540c2c <- pendingView[3], c48 <- snap arena). The writer is
  // script-side; nothing in the bounded runtime sets it — counted.
  if (rt.pendingViewSnap != 0 && rt.pendingViewSnap != -1) {
    ++rt.seams.pendingViewSnaps;
    rt.pendingViewSnap = 0;
  }

  // 0x540cdc teleport block — script-driven in the original.
  if (rt.teleportFlag != 0) ++rt.seams.teleportCalls;

  // FUN_00435178 portal test -> current-arena swap. OBSERVED
  // (0x436374..0x4363be): slideChannel <- -15 when >0; ca4 <- old
  // c48; ca8 <- 1; c48 <- dest; FUN_00432d9c(dest) — whose own
  // dest!=ca4 branch then leaves ca4 = dest = new current (the old
  // arena stays resident via the LRU slots, not the partner slot).
  bool swapped = false;
  int portalCand = -1;
  if (TraversalArena* dest = traversalPortalTest(rt)) {
    if (rt.slideChannel > 0) rt.slideChannel = -15; // OBSERVED
    rt.cur = dest;                              // 0x540c48 <- dest
    cur = dest;
    rt.partner = dest;                          // net ca4 <- dest
    rt.partnerActive = true;                    // 0x540ca8 = 1
    rt.cs.carrier = &dest->dyn.col;
    rt.cs.carrierValid = 1;
    portalCand = dest->index;
    rt.cs.arena = &cur->dyn.col;
    rt.cs.surface = &cur->surface;
    traversalAttachSideEffects(rt, *cur);       // FUN_00432d9c tail
    traversalMigrateInto(rt, *cur);             // FUN_00432980 tail
    ++rt.seams.portalsCrossed;
    swapped = true;
  }

  // FUN_00434b44 — trigger scan on the (possibly new) current arena.
  traversalTriggerScan(rt);

  // FUN_004301e0 — OBSERVED structure:
  //   flagC9c==0 && flag49b740!=0 → call FUN_00431100 + commit only.
  //   flagC9c!=0                  → flagBec=0 → shared tail.
  //   else  viewScalar!=scalar && flagBec==0 → blend 0.85/0.15 → tail;
  //         otherwise flagBec=0 → tail.
  //   tail: z-delta clamp +/-0.5 → [0x49b718] = old*0.97 + dz*0.03,
  //         then 0x540c08 = pos — the prev-pos commit runs EVERY
  //         frame on all paths (0x430272 / 0x4309ed movsd x3).
  if (rt.flagC9c == 0 && rt.flag49b740 != 0) {
    ++rt.seams.pendingViewSnaps; // FUN_00431100 — view-snap seam
  } else {
    if (rt.flagC9c != 0) {
      rt.flagBec = 0;
    } else if (rt.viewScalar != cur->scalar && rt.flagBec == 0) {
      rt.viewScalar = rt.viewScalar * 0.85f + cur->scalar * 0.15f;
    } else {
      rt.flagBec = 0;
    }
    float dz = rt.cs.pos[2] - rt.cs.entryPos[2];
    if (dz > 0.5f) dz = 0.5f;
    else if (dz < -0.5f) dz = -0.5f;
    rt.viewRoll = rt.viewRoll * 0.97f + dz * 0.03f; // 0x49b718
  }
  for (int i = 0; i < 3; ++i) rt.cs.entryPos[i] = rt.cs.pos[i];
  // bankIdle -> 0x5414b4; FUN_0046ae60(0x5414bc != 0) — OBSERVED.
  rt.bankIdle = (rt.motion.bank + rt.bankAux) == 0.0f;
  ++rt.seams.timersCalls;

  // Primary/secondary select — OBSERVED (0x436405..0x43641a +
  // 0x436b73): stream-busy (0x540d3c) -> prim=c48 only; otherwise
  // prim/sec = viewOnPartner ? (ca4,c48) : (c48,ca4), sec being the
  // RAW ca4 slot (not gated on ca8).
  TraversalArena* prim;
  TraversalArena* sec;
  if (rt.cs.carrierBusy) {
    prim = cur;
    sec = nullptr;
  } else if (rt.viewOnPartner) {
    prim = rt.partner;
    sec = cur;
  } else {
    prim = cur;
    sec = rt.partner;
  }
  rt.fieldCc8 = 0;
  ++rt.seams.profilerHooks; // FUN_0042fecc rdtsc probe (0x436422)
  if (rt.masterMoveGate) ++rt.seams.scriptedMoveCalls; // FUN_0040e19c
  if (sec) {
    ++rt.seams.arenaEventListCalls; // FUN_00404cd4(+0x5c list: empty)
    surfaceRecordUpdate(sec->surface, dt); // FUN_004134a0
  }
  ++rt.seams.arenaEventListCalls;
  surfaceRecordUpdate(prim->surface, dt);

  // 0x540eb0 event-timer — OBSERVED (0x43645d..0x436491): when the
  // timer is positive it decrements by the frame quantum and the
  // 0x540eb4 object is validated (named + live liveness fields);
  // invalid -> edi=0, timer=0. The +0x2a2/+0x08 liveness words are
  // UNMAPPED in the port — the named check carries the gate.
  if (rt.eventTimer > 0.0f) {
    rt.eventTimer -= dt;
    if (!(rt.eventTimerObj && rt.eventTimerObj->col.named))
      rt.eventTimer = 0.0f;
  }
  rt.fieldE14 = 0;
  // OBSERVED (0x43649e..0x4364d9): the extra tick pair runs only
  // when flag541548 != 0 — FUN_0042b20c(flagC9c && phase>1) then
  // FUN_00436d60(-1, prim, sec); FUN_00436d60(1, prim, sec) always.
  if (rt.flag541548) {
    ++rt.seams.extraWorldTickCalls; // FUN_0042b20c + 36d60(-1)
  }
  ++rt.seams.worldTickCalls;        // FUN_00436d60(1, prim, sec)

  // FUN_0040b4dc(0) — pending surface-op re-arms, current then
  // partner (when the partner is active).
  surfaceApplyPending(cur->surface, 0);
  if (rt.partnerActive && rt.partner)
    surfaceApplyPending(rt.partner->surface, 0);

  // FUN_00435eec — the frame-tail floor probe. The vertical path
  // consumed LAST frame's probe; this refreshes for frame N+1.
  collisionFloorProbe(rt.cs);

  // Post tail (0x43650f..0x436573): FUN_0042ff9c then the six
  // FUN_0042fef4 bitmask-callback walk sites over 0x49b708 —
  // stream/list bookkeeping, all deferred seams.
  rt.seams.postTailCalls += 7;

  ++rt.frameCounter;

  // ------------------------- result ------------------------------
  out.curArenaIndex = cur->index;
  out.partnerArenaIndex = rt.partner ? rt.partner->index : -1;
  for (int i = 0; i < 3; ++i) {
    out.pos[i] = rt.cs.pos[i];
    out.posPrev[i] = rt.cs.entryPos[i];
    out.contactNormal[i] = rt.vert.contactNormal[i];
  }
  for (int i = 0; i < 6; ++i) out.playerBox[i] = rt.cs.playerBox[i];
  out.yawDeg = rt.motion.yawDeg;
  out.bankDeg = rt.motion.bank;
  out.moveVel = rt.motion.moveVel;
  out.strafeVel = rt.motion.strafeVel;
  out.turnVel = rt.motion.turnVel;
  out.vertVel = rt.vert.vertVel;
  out.grounded = (rt.vert.contactFlags & 1) != 0;
  out.rideActive = rt.cs.rideActive != 0;
  out.positionChanged = positionChanged;
  out.collisionIssued = vf.collisionIssued;
  out.appliedDispZ = appliedZ;
  out.contactObj = rt.vert.contactObj;
  out.contactPoly = rt.lastContactPoly;
  out.locoState = rt.locoState;
  out.eventType = rt.eventType;
  out.eventMag = rt.eventMag;
  out.slideChannel = rt.slideChannel;
  out.viewOnPartner = rt.viewOnPartner;
  out.partnerActive = rt.partnerActive;
  out.currentArenaSwapped = swapped;
  out.portalCandidate = portalCand;
  out.eventTimer = rt.eventTimer;
  out.viewScalar = rt.viewScalar;
  out.seams = rt.seams;
  return out;
}

} // namespace mdk
