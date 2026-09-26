// traversal_runtime.cpp — Phase 5G: native traversal runtime host.
// See traversal_runtime.h for the original-ownership map and the
// evidence-level notes. Everything below is OBSERVED from MDK95.EXE
// disassembly unless marked NATIVE (port infrastructure), BOUNDED
// (the eager stream model), or SEAM (deferred, counted not emulated).

#include "core/traversal_runtime.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

#include "core/bni_directory.h"
#include "core/data_root.h"
#include "core/enemy_runtime.h"
#include "core/frontend_machines.h"
#include "core/object_animation.h"
#include "core/object_path.h"
#include "core/player_fire.h"
#include "core/player_projectiles.h"
#include "core/player_reticle.h"
#include "core/player_sniper.h"

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

// FUN_00456808 — CMI table-0 "%s$%s_%u" per-object script lookup:
// the record value is the script's code offset within the image
// (file offset = value + 4; OBSERVED LEVEL3 HMO_1$XGS_9). Returns an
// image pointer for +0x108, or nullptr when the record is absent or
// out of bounds (bounds-checked per the safety policy).
const void* traversalObjectScriptFor(const char* arenaName,
                                     const char* modelName,
                                     std::uint16_t spawnId, void* ctx) {
  const TraversalLevel* lv = static_cast<const TraversalLevel*>(ctx);
  if (!lv || lv->cmi.tables.empty()) return nullptr;
  char key[96];
  std::snprintf(key, sizeof key, "%s$%s_%u", arenaName, modelName,
                static_cast<unsigned>(spawnId));
  for (const auto& rec : lv->cmi.tables[0].records) {
    if (rec.name() != key) continue;
    const std::uint64_t fo =
        4 + static_cast<std::uint64_t>(rec.value);
    if (fo >= lv->cmiBytes.size()) return nullptr;
    return lv->cmiBytes.data() + fo;
  }
  return nullptr;
}

} // namespace

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
  // deepFloorZ (+0x44e) — populated by collisionBlobParse from the
  // installed verts (see collision_query.cpp). OBSERVED: the original
  // runs FUN_004320d0 right after FUN_00419ee0 (call pairs at
  // 0x4323dd and 0x42911c), folding the record's +0x446..+0x45a AABB
  // from +0x24/+0x0c. +0x44e is the unaligned minZ; +0x128..+0x130
  // gets the AABB center (not yet mirrored). The earlier "provably
  // zero" conclusion was wrong — the stores go through
  // lea ecx,[eax+0x446] + [ecx+8]/[ecx+0x14], invisible to a
  // [reg+disp32] sweep. Consequence: deep arenas (GUNT_10 verts span
  // z -427..-321) get a real abyss reference instead of 0, so
  // script-spawned objects below the -200 plane survive as they do
  // in the original.

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

void traversalEnsureLoaded(TraversalRuntime& rt,
                           TraversalArena& arena) {
  if (!arena.geometryLoaded) {
    std::string detail;
    traversalArenaLoadGeometry(rt, arena, &detail);
  }
  // FUN_00432d9c's tail: the +0x44 bit-2 spawn-once gate. The original
  // calls FUN_00456808 only when the bit is clear, then sets it.
  if (!arena.objectsSpawned && arena.rec) {
    // The script source binds each spawn's +0x108 — the persistent
    // per-object VM PC (table-0 "%s$%s_%u" record, FUN_00456808).
    spawnArenaObjects(arena.dyn, *arena.rec, traversalModelFor,
                      &rt.level, nullptr, traversalObjectScriptFor,
                      &rt.level);
    arena.objectsSpawned = true;
  }
}

// FUN_0046a3d8 — inventory record delete. OBSERVED (0x46a3d8): each
// record above idx drops its HUD slot target by 0x30 (48px) and
// recomputes the slide-in velocity (slotX - animX) * 2.0f before the
// shift; then count-- and the selection fixup incl. the type-6 GATT
// skip.
void inventoryRemove(TraversalRuntime& rt, int idx) {
  for (int i = idx + 1; i < rt.inventoryCount; ++i) {
    InventoryRecord& s = rt.inventory[i];
    s.slotX -= 0x30;
    s.animVel = (s.slotX - s.animX) * 2.0f;
    rt.inventory[i - 1] = s;
  }
  if (rt.inventoryCount > 0) --rt.inventoryCount;
  if (rt.inventorySel == rt.inventoryCount && rt.inventoryCount != 0)
    --rt.inventorySel;
  if (rt.inventoryCount > 1 && rt.inventorySel >= 0 &&
      rt.inventorySel < 5 &&
      rt.inventory[rt.inventorySel].id == 6) {
    if (rt.inventorySel == 0) {
      rt.inventorySel = 1;
    } else {
      --rt.inventorySel;
    }
  }
}

void traversalAttachSideEffects(TraversalRuntime& rt,
                              TraversalArena& arena) {
  traversalEnsureLoaded(rt, arena);
  rt.partnerActive = true;
  rt.cs.carrierValid = 1;
}

// FUN_00432980 tail — connector pull-in from type-6 peer arenas
// (OBSERVED 0x432b01..0x432b79): for each type-6 record of `a`, the
// peer arena's +0x68 chain is scanned for a named connector
// (+0x06!=0, +0x14a&0x10) whose home +0x60 OR destination +0x302 is
// `a`; a match takes +0x2bc <- `a` and transfers via FUN_004574d0.
// Peers equal to c48 are skipped; a peer equal to ca4 is skipped
// only when ca8 is set (the port's call sites always run this with
// `a` freshly bound as partner, so peer==partner is covered by the
// peer==`a` skip).
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
          (o.arena == &a.dyn || o.connDest == &a)) {
        o.pendingArena = &a.dyn;
        peer.dyn.transfer(o, a.dyn);
        ++rt.seams.objectMigrations;
      }
      it = next;
    }
  }
  // FUN_004321dc tail (OBSERVED — called at the end of FUN_00432980,
  // the pull-in this function mirrors): walk the current arena's
  // +0x68 list then the partner's and run FUN_0045a3b0 on every named
  // object — the lazy +0x0c element-set rebind. Save-load restore and
  // FUN_0045a2d0 migration-outs leave +0x0c null; the frame's
  // collision probes (FUN_004138d8) deref it unguarded, so the bind
  // must complete before the next frame step.
  if (rt.cur)
    for (auto& o : rt.cur->dyn.storage)
      if (o->col.named) objectArenaActivate(rt, *o);
  if (rt.partner && rt.partner != rt.cur)
    for (auto& o : rt.partner->dyn.storage)
      if (o->col.named) objectArenaActivate(rt, *o);
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
// FUN_00457738 — arena-connector update, gated by col.flags14a & 0x10
// (the tr_alcmd 0x95 connector spawn). Runs first in the per-object
// dispatch (FUN_004572ac) before movement/animation.
//
// OBSERVED behavior (0x457738..0x457a5b):
//  * Self-migration: when the player's current arena == +0x302 (the
//    connector's destination) but the object is still homed elsewhere,
//    +0x2bc <- +0x302 so the door follows the player across.
//  * +0x312 state byte: low nibble = phase {8 closed, 2 opening,
//    1 open, 4 closing}, high nibble = sub-flags. An "active" anim
//    (+0x114 != 0 and +0x118 != 0xff00) blocks the open/close latch;
//    with +0x114 == 0 the latch is immediate.
//  * Proximity: player inside +0x30e -> opening (attach the far-side
//    arena via FUN_00432d9c); outside -> closing; closing latch ->
//    closed + detach partner.
//  * +0x2c8 element mask rebuilt from +0x326 (LOCK) / +0x32a (HC).
//  * +0x148 bit4 toggled on (state&0x10 && state&1) / off — sets the
//    sweep-skip bit 0x10 when a flagged connector is open.
//
// For XCORDOOR (this route) the table-2 init script CHMO_2$XCORDOOR
// binds +0x306/+0x30a to real anim records (16-frame open, 21-frame
// close) and sets +0x312 bit4 (collision toggle). So the door does NOT
// latch immediately — it animates over ~16 frames while +0x118 runs
// from 0xffff to the 0xff00 done latch, then opens. There are no HC*/
// LOCK elements, so the mask update is a no-op; the observable effect
// is the collision-toggle sweep-skip plus the partner attach/detach
// that supplies the corridor's carrier floor.
// ---------------------------------------------------------------------------
void traversalConnectorUpdate(DynamicObject& o, TraversalRuntime& rt) {
  TraversalArena* cur = rt.cur;                 // 0x540c48
  TraversalArena* home = o.arena ? o.arena->owner : nullptr;  // +0x60
  TraversalArena* dest = o.connDest;            // +0x302
  TraversalArena* partner = rt.partner;         // 0x540ca4
  const bool partnerActive = rt.partnerActive;  // 0x540ca8
  if (!dest) return;

  // Self-migration (0x457745..0x45775e): cur!=home && cur==dest ->
  // +0x2bc <- +0x302. The pending-arena transfer then flips +0x302 to
  // the old home (FUN_004574d0), making the connector bidirectional.
  if (cur != home && cur == dest)
    o.pendingArena = &dest->dyn;                // +0x2bc

  // Anim latch (0x457764..0x4577a1). +0x114==0 or +0x118==0xff00 means
  // "no active anim / complete". With the init script bound the door
  // animates: +0x118 stays -1 while running, 0xff00 when done.
  const bool animDone = o.animDone();
  if ((o.connState & 2) == 0) {
    if ((o.connState & 4) != 0 && animDone) {
      o.connState = static_cast<std::uint8_t>((o.connState & 0xf0) | 8);
      traversalDetachPartner(rt);               // FUN_00432d9c(0)
      // play +0x31e (seam — sound system not reconstructed)
    }
  } else if (animDone) {
    o.connState = static_cast<std::uint8_t>((o.connState & 0x70) | 1);
    // play +0x316 (seam)
  }

  // Proximity (0x4577a6..0x4579a5): squared distance player->object.
  const float dx = o.pos[0] - rt.cs.pos[0];
  const float dy = o.pos[1] - rt.cs.pos[1];
  const float dz = o.pos[2] - rt.cs.pos[2];
  const float dist2 = dx * dx + dy * dy + dz * dz;
  const float r2 = o.connRadius * o.connRadius;

  if (r2 <= dist2) {
    // Player outside radius -> closing (0x4579aa..0x457a1a). The
    // transition writes +0xdc=-1.0, +0xe4=0xffff, +0x118=0xffff,
    // +0xe0=30, clears +0x148 bit3 (loop) and binds +0x114=+0x30a.
    if ((o.connState & 0x2c) == 0) {
      o.animAcc = -1.0f;                  // +0xdc
      o.animFrame = -1;                  // +0xe4 = 0xffff
      o.animLatch = -1;                     // +0x118 = 0xffff
      o.animRate = 30.0f;                   // +0xe0
      o.col.flags148 &= ~0x8u;                  // +0x148 &= 0xf7
      o.animRec = o.animRecFar;               // +0x114 = +0x30a
      o.connState = static_cast<std::uint8_t>((o.connState & 0xf0) | 4);
      // play +0x322 (seam)
    }
  } else {
    // Player inside radius -> opening + partner attach
    // (0x4577f0..0x4579a5). Transition writes +0x114=+0x306, +0xdc=-1,
    // +0xe4=0xffff, +0x118=0xffff, clears +0x148 bit3, +0xe0=30.
    if ((o.connState & 0x43) == 0) {
      o.animRec = o.animRecNear;              // +0x114 = +0x306
      o.animAcc = -1.0f;                  // +0xdc
      o.animFrame = -1;                  // +0xe4 = 0xffff
      o.animLatch = -1;                     // +0x118 = 0xffff
      o.col.flags148 &= ~0x8u;                  // +0x148 &= 0xf7
      o.connState = static_cast<std::uint8_t>((o.connState & 0xf0) | 2);
      o.animRate = 30.0f;                   // +0xe0
      // FUN_00432d9c — attach whichever of {dest,home} is on the far
      // side of the door from the player and not already the partner.
      // Primary target is +0x302; the +0x60 home is the secondary when
      // the player has already crossed (cur==dest) or dest is resident.
      TraversalArena* tgt = nullptr;
      if (dest != cur) {
        if (!partnerActive || dest != partner) tgt = dest;
      }
      if (tgt == nullptr && cur != home &&
          (!partnerActive || partner != home))
        tgt = home;
      if (tgt) traversalAttachPartner(rt, *tgt);
      // play +0x31a (seam)
    }
  }

  // Element-mask update (0x457890..0x4578df): rebuild +0x2c8 from the
  // LOCK(+0x326)/HC(+0x32a) masks per the open/closed phase.
  if ((o.connState & 8) == 0) {
    // not closed: LOCK in, HC out
    o.col.elemMaskB |= o.connMaskLock;
    o.col.elemMaskB &= ~o.connMaskHC;
  } else {
    // closed: HC in; LOCK in unless the 0x40 variant + +0x313 bit0 say
    // otherwise
    o.col.elemMaskB |= o.connMaskHC;
    if ((o.connState & 0x40) == 0 || (o.connStateHi & 1) != 0)
      o.col.elemMaskB |= o.connMaskLock;
    else
      o.col.elemMaskB &= ~o.connMaskLock;
  }

  // Collision toggle (0x4578df..0x4578f1): when +0x312 bit4 is set the
  // door's solidity follows the open bit — +0x148 bit4 = sweep skip.
  if ((o.connState & 0x10) != 0) {
    if ((o.connState & 1) != 0) o.col.flags148 |= 0x10u;
    else o.col.flags148 &= ~0x10u;
  }
}

// ---------------------------------------------------------------------------
// FUN_004555bc — per-object animation advance. The full driver + the
// FUN_00455890 vertex/ref-point applier + the FUN_00455c48 rigid
// channel decode now live in object_animation.cpp (Phase 11A/G5 port —
// OBSERVED): +0xdc accumulator advance (rate * +0xe0 * 1/30), target
// clamp at +0x118, loop/clamp at frameCount-1, then
// FUN_00455890(obj, rec, FRNDINT(+0xdc) - +0xe4) applies each pending
// frame (delta keys are cumulative — intermediate frames must apply),
// latching +0x118 = 0xff00 on completion of a non-looping anim.
// ---------------------------------------------------------------------------
void traversalObjectAnimUpdate(DynamicObject& o,
                               const std::uint8_t* recLimit) {
  objectAnimTick(o, recLimit);
}

// ---------------------------------------------------------------------------
// FUN_00435178 — portal scan on an arena's type-6 records.
// fields[0] = partner arena index (rewritten by connect pairing),
// fields[1] = side code, fields[2..7] = box {x0,y0,z0,x1,y1,z1}.
// OBSERVED call sites (FUN_00436100 + FUN_004301e0 tail):
//   player:  arena=0x540c48, q=0x540c08 (prev), p=0x540bfc (pos)
//   camera:  arena=0x540c48, q=eye(0x540bfc+3z), p=0x540b28 (camPos)
// ---------------------------------------------------------------------------

TraversalArena* traversalPortalScanSegment(TraversalRuntime& rt,
                                           const TraversalArena& arena,
                                           const float from[3],
                                           const float to[3]) {
  if (!arena.rec) return nullptr;
  const float* p = to;
  const float* q = from;
  const TraversalArena* cur = &arena;
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

TraversalArena* traversalPortalTest(TraversalRuntime& rt) {
  if (!rt.cur) return nullptr;
  return traversalPortalScanSegment(rt, *rt.cur, rt.cs.entryPos,
                                    rt.cs.pos);
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

bool traversalVolumeActivate(TraversalArena& arena, int id,
                             const std::string& name, int kind,
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
    rec->nameText = name;      // +0x8 in the original — the lstr ptr
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
                                        std::string* detail,
                                        std::uint32_t flags) {
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

  // TRAVSPRT.BNI — the traversal-context anim bank (DAT_004a1e38
  // while traversing). The mover's SW_H150 records bind through
  // FUN_004039ec (payload+4) at context init (0x434510/0x434524).
  // Non-fatal: ports/tests without the bank keep null globals and
  // the SW_H150 name branch degrades to its null-record state.
  {
    const std::string dir = dtiPath.substr(0, dtiPath.find_last_of('/'));
    const std::string bniPath = dir.substr(
        0, dir.find_last_of('/') + 1) + "TRAVSPRT.BNI";
    auto bni = root.readFile(bniPath, kMaxDataFileBytes, detail);
    if (bni) {
      rt.level.travsprtBytes = std::move(*bni);
      const BniDirectory bd = inspectBniDirectory(
          std::span<const std::byte>(rt.level.travsprtBytes));
      if (bd.status == BniDirectoryStatus::kOk) {
        const auto payload4 = [&](const char* nm) -> const void* {
          const BniRecord* r = findBniRecord(bd, nm);
          if (!r) return nullptr;
          // FUN_004039ec — record payload + 4 (skips the head u32).
          return rt.level.travsprtBytes.data() + r->payloadFileOffset + 4;
        };
        rt.animH150I = payload4("H150_I");    // 0x54c6a4
        rt.animH150R = payload4("H150_R");    // 0x54c6b0
      }
    }
  }

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
    a->dyn.owner = a.get();
    a->scalar = rt.level.work[i].scalar();
    a->hasScriptObject = cmiTable3Has(rt.level.cmi, a->name);
    // FUN_00458550 bind — +0x220 = image-relative code offset (the
    // VM's persisted gate + entry PC; 0 when no record / codeOff==0).
    a->script.pcImageOff = cmiScriptCodeOffset(
        rt.level.cmi,
        std::span<const std::byte>(rt.level.cmiBytes.data(),
                                   rt.level.cmiBytes.size()),
        a->name);
    a->script.active = (a->script.pcImageOff != 0);
    // FUN_004546ac — surface-contact handler scripts route through
    // the VM; scriptUser carries the runtime for env construction.
    a->surface.scriptFn = traversalScriptSurfaceHandler;
    a->surface.scriptUser = &rt;
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

  // FUN_00433c4c's weapon/fire indicator reset (0x433d22..0x433d30 —
  // also written by the mode-6 briefing at 0x429d4b): 541618 = 0,
  // 541619 = 0, 54161a = 3, 54161b = 0. The ammo block 0x54161f..33
  // is NOT reset — grants/inventory persist across the transition.
  rt.wpnSel0 = 0;
  rt.wpnSel1 = 0;
  rt.burstIndex = 3;
  rt.fireCadence = 0.0f;

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
  // kTraversalLoadSuppressSpawn mirrors FUN_004346e8(param=2): the
  // save-load path leaves every arena's object list empty for the
  // ALIE records (unactivated arenas keep lazy spawn via +0x44 bit2).
  if ((flags & kTraversalLoadSuppressSpawn) == 0)
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

  // FUN_00436100 head — the scope-phase advance (ca0): one phase per
  // frame, committed (2 -> 3) then requested (1 -> 2). Runs after the
  // frontend input consume and before the dispatch, as in the original.
  sniperScopePhaseAdvance(rt);

  // FUN_00437e80 (frontend) + FUN_0042534 stream-drain — seams.
  // ======================= player dispatch (FUN_00463608) =========
  // OBSERVED dispatch head (0x463608): the pending-event slots
  // 0x54cb00/0x54cb08 are cleared every frame; the current event
  // priority 0x540cbc is reset while the dispatched state sits in
  // the transient set {300, 400, 500, 600, 601}.
  rt.eventType = 0;
  rt.eventMag = 0;
  if (rt.locoState == 300 || rt.locoState == 400 ||
      rt.locoState == 500 || rt.locoState == 600 ||
      rt.locoState == 601)
    rt.eventPriority = 0;

  // OBSERVED (0x463608): the dispatch picks ONE branch per frame —
  // mounted object (e6c && +0x14b&2) > sniper (c9c) > unscoped
  // (cac >= 800 scripted, else the FUN_00465228 normal path).
  const bool mounted = rt.cs.excludeObj != nullptr &&
                       (rt.cs.excludeObj->flags14b & 0x02) != 0;
  PlayerMotionOutput mo{};
  PlayerVerticalFrame vf{};
  float appliedZ = 0.0f;
  bool positionChanged = false;

  // The shared vertical environment — the sniper's gravity-only call
  // (FUN_00467180) and the normal/scripted jump+gravity (FUN_00466740)
  // both read it. vec/cvec hold the ribbon-query out-vectors and must
  // outlive the calls below (ribbonVelZ points into them).
  float vec[3] = {0, 0, rt.vert.vertVel};
  float cvec[3] = {0, 0, rt.vert.vertVel};
  const auto makeVertEnv = [&](bool moveConsumed) {
    PlayerVerticalEnvironment ve;
    ve.smoothed = timing.smoothed;
    ve.deltaSeconds = dt;
    ve.frameStep = timing.frameStep;
    ve.jumpHeld = rt.prevFrame.jump != 0;
    ve.moveConsumed = moveConsumed;
    ve.locoState = rt.locoState;
    ve.eventWordType = rt.eventType;
    ve.vertEnable = rt.vertEnable;   // 0x540c6c — set at traversal init
    ve.slideMode = rt.slideChannel != 0;
    ve.sharedGateE6C = rt.cs.excludeObj != nullptr;
    // 0x540e72 = byte2 of the e70 mount-class dword; bit1 silences the
    // hard-landing flash (set for class 2/XSNOWB, e70 = 0x20002).
    ve.flagE72bit1 = ((rt.mountClass >> 16) & 2) != 0;
    ve.carrierObj = rt.cs.carrier != nullptr;
    ve.carrierCheckGate = rt.cs.carrierBusy != 0;
    // FUN_00412e94(player,1,&pos,&vec) — real type-7 volume query.
    const bool inRibbon =
        surfaceVolumeQuery(cur->surface, 1, rt.cs.pos, dt, vec) != 0;
    ve.insideRibbonVolume = inRibbon;
    ve.ribbonVelZ = inRibbon ? &vec[2] : nullptr;
    if (ve.carrierObj && !ve.carrierCheckGate && rt.partner &&
        rt.partnerActive) {
      ve.carrierInsideRibbonVolume = surfaceVolumeQuery(
          rt.partner->surface, 1, rt.cs.pos, dt, cvec) != 0;
    }
    ve.slideVec = nullptr;   // FUN_0046603c's leftover — deferred
    ve.deepFloorZ = cur->dyn.col.deepFloorZ;
    return ve;
  };

  if (mounted) {
    // 0x463a6a — the mounted-class dispatch (byte2 of the e70 dword).
    // The per-class update is self-contained; no normal vertical/look.
    playerReticleDispatchMounted(rt, raw, bindings, rt.prevFrame,
                                 timing.smoothed, dt, timing.frameStep);
  } else if (rt.flagC9c != 0) {
    // 0x463ad7 — the sniper branch.
    if (rt.transitionPhase == 0) {
      // 0x463ae9 — ca0==0 (scope-in pending): the semantic channels
      // are cleared; the sniper core stays idle until ca0 != 0.
      rt.motion.moveVel = 0.0f;
      rt.motion.strafeVel = 0.0f;
      rt.motion.turnVel = 0.0f;
      rt.motion.zoomChannel = 0.0f;
    } else {
      // 0x463b06 — FUN_00464624 (gravity-only vertical + the lateral
      // sweep + aim + zoom), then the FUN_00469b98 weapon-select seam.
      // The gravity frame / applied Z / lateral move are surfaced so the
      // out-diagnostics match the normal branch's reporting.
      const PlayerVerticalEnvironment sEnv = makeVertEnv(false);
      sniperCoreUpdate(rt, raw, bindings, rt.prevFrame, sEnv,
                       timing.smoothed, &vf, &appliedZ, &positionChanged);
      playerWeaponSelect(rt, rt.prevFrame);   // FUN_00469b98
    }
  } else {
    // Unscoped + unmounted — the >=800 scripted branch or the
    // FUN_00465228 normal path.
    const bool scripted = rt.locoState >= 0x320;
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

    if (!scripted) {
      mo = integratePlayerMotion(rt.prevFrame, motionEnv, rt.motion);
      // Horizontal collision — FUN_004630d4(disp, scale 0.75).
      const float preX = rt.cs.pos[0], preY = rt.cs.pos[1],
                  preZ = rt.cs.pos[2];
      const CollisionPoly* hContact = collisionApply(
          rt.cs, mo.dispX, mo.dispY, mo.dispZ, 0.75f, nullptr, nullptr);
      positionChanged = rt.cs.pos[0] != preX || rt.cs.pos[1] != preY ||
                        rt.cs.pos[2] != preZ;
      playerMotionPostStep(motionEnv, positionChanged, rt.motion, mo);
      // 0x540e4c — every apply's EAX is stored (0 clears).
      rt.vert.contactObj = asToken(hContact);
      rt.lastContactPoly = hContact;
      // The motion post feeds the shared pending slots (cb00/cb08).
      if (mo.eventMag != 0) {
        rt.eventType = mo.eventType;
        rt.eventMag = mo.eventMag;
      }
    } else {
      // OBSERVED scripted-branch write (0x463705..): the semantic
      // channels d48/d4c/d50/d54 are cleared to e6c (= 0 unmounted).
      rt.motion.moveVel = 0.0f;
      rt.motion.strafeVel = 0.0f;
      rt.motion.turnVel = 0.0f;
      rt.motion.zoomChannel = 0.0f;
    }

    // SEAM: FUN_0046603c slide helper — runs on both branches.
    ++rt.seams.slideHelperCalls;

    // ------------------------- vertical --------------------------
    const PlayerVerticalEnvironment vertEnv =
        makeVertEnv(mo.moveConsumed);
    rt.vert.posX = rt.cs.pos[0];
    rt.vert.posY = rt.cs.pos[1];
    rt.vert.posZ = rt.cs.pos[2];
    vf = integratePlayerVertical(vertEnv, rt.motion, rt.vert);
    if (vf.collisionIssued) {
      const CollisionPoly* vContact = playerVerticalApplyCollision(
          vertEnv, rt.cs, rt.motion, rt.vert, vf, &appliedZ);
      if (vContact) rt.lastContactPoly = vContact;
    }
    playerVerticalPostStep(vertEnv, rt.vert);
    rt.vert.posX = rt.cs.pos[0];
    rt.vert.posY = rt.cs.pos[1];
    rt.vert.posZ = rt.cs.pos[2];
    // The vertical post overwrites the pending slots — OBSERVED
    // producer order inside FUN_00465228 (motion events, then
    // FUN_00466740's jump/landing events, then the look integrator).
    if (vf.eventMag != 0) {
      rt.eventType = vf.eventType;
      rt.eventMag = vf.eventMag;
    }
    if (vf.deepFloorReset) ++rt.seams.deepFloorFallbacks;
    if (mo.forwardIntent) ++rt.seams.mantleCalls;

    // FUN_00465c4c — the semantic look integrator; runs after the jump
    // machine on both dispatch branches (N-1 merged look controls).
    PlayerLookEnvironment lookEnv;
    lookEnv.deltaSeconds = dt;
    lookEnv.arenaScalar = cur->scalar;
    lookEnv.eventPriority = rt.eventPriority;
    lookEnv.locoState = rt.locoState;
    lookEnv.vertVelZero = rt.vert.vertVel == 0.0f;
    lookEnv.grounded = (rt.vert.contactFlags & 1) != 0;
    const PlayerLookFrame lk =
        integratePlayerLook(rt.prevFrame, lookEnv, rt.look);
    if (lk.eventPosted) {
      rt.eventType = kLookEventPri;
      rt.eventMag = kLookEventCode;
    }

    if (!scripted) {
      // FUN_00465228 tail — itemUse (ce774) seam, then the sniper
      // entry, then the normal-fire latch (a deferred seam).
      if (rt.prevFrame.itemUse != 0) ++rt.seams.itemUseCalls;
      if (rt.prevFrame.sniperPulse != 0 && rt.eventPriority < 8 &&
          rt.eventType < 8) {
        // eligibility: c6c == 0 -> free; else vertVel == +-0 &&
        // grounded && no dying-surface record under the contact.
        const bool eligible =
            !rt.vertEnable ||
            (rt.vert.vertVel == 0.0f &&
             (rt.vert.contactFlags & 1) != 0 &&
             !sniperDyingSurface(rt, rt.lastContactPoly));
        if (eligible) {
          rt.fieldC74 = 0;
          ++rt.seams.hudEventCalls;   // FUN_00469668(0)
          rt.flagC9c = 1;
          rt.transitionPhase = 0;
          rt.motion.moveVel = 0.0f;
          rt.motion.strafeVel = 0.0f;
          rt.motion.turnVel = 0.0f;
          rt.motion.zoomChannel = 0.0f;
          rt.eventMag = 0x323;
          rt.eventType = 8;
        }
      }
      // normal-fire latch (0x465717+) — FUN_00465228's tail: the
      // 0x540c74 punch gate + the 0x12c/0x259 fire anim event.
      playerFireLatch(rt, rt.prevFrame);
      ++rt.seams.weaponSlotCalls;   // FUN_00469cd0
      playerReticleMountScan(rt);   // the mount-scan + class entry
    } else {
      ++rt.seams.weaponSlotCalls;   // FUN_00469cd0
    }
  }

  // Commit the N+1 merge — the latency hand-off (FUN_00406f14 runs
  // at dispatch end in the original, before the tail below).
  rt.prevFrame = nextFrame;

  // Dispatcher tail (OBSERVED 0x4638xx): with no latched event and
  // no post this frame, the idle restore posts the idle state —
  // code 0x65 on the unmounted path (the 100 variant needs the
  // mount/mounted-idle path — deferred). Then the priority latch
  // adopts the frame's winning event.
  if (rt.eventPriority == 0 && rt.eventType == 0) {
    rt.eventType = 1;
    rt.eventMag = 0x65;
  }
  if (rt.eventPriority < rt.eventType) {
    rt.locoState = rt.eventMag;
    rt.eventPriority = rt.eventType;
  }

  // ================= traversal-active section =====================
  // 0x540c30..0x540c44 — the per-query player AABB (the original
  // rebuilds it here when mode 0x541492 == 3).
  rt.cs.playerBox[0] = rt.cs.pos[0] - 1.25f;
  rt.cs.playerBox[1] = rt.cs.pos[1] - 1.25f;
  rt.cs.playerBox[2] = rt.cs.pos[2];
  rt.cs.playerBox[3] = rt.cs.pos[0] + 1.25f;
  rt.cs.playerBox[4] = rt.cs.pos[1] + 1.25f;
  rt.cs.playerBox[5] = rt.cs.pos[2] + 4.25f;
  // Snapshot the standing-box extents before the in-frame collision
  // queries rewrite 0x540c30..44 with per-query probe AABBs.
  for (int i = 0; i < 6; ++i) {
    out.playerBodyBox[i] = rt.cs.playerBox[i];
  }

  // FUN_00432f84 runs unconditionally (0x43627a); the 0x540c74 gate
  // lives inside it. The punch target scan + hitscan boundary.
  ++rt.seams.objectPrepass;
  playerPunch(rt, timing.frameStep);
  ++rt.seams.profilerHooks; // FUN_0042fecc rdtsc probe (0x43627f)

  // Object updates — FUN_004572ac, per-object order (OBSERVED from raw
  // disasm): +0x07==1 view latch -> connector (0x14a&0x10) -> orbit
  // (0x14a&0x40) -> command runner (0x14b&0x40, +0x08 recheck) ->
  // pendingArena transfer (+0x2bc, nonzero skips) -> +0x108 script VM
  // -> +0x06 -> path follower (+0xec) -> FUN_004533d4 subtype -> +0x06
  // -> gravity -> collision -> +0x06 -> FUN_0045897c enemy dispatch
  // (0x149&0x10, replaces mover) / mover (0x14a&0x20) -> FUN_004555bc
  // anim -> +0x06 -> +0x18c vel-cache -> roll ride (0x148&0x40) ->
  // ridden-follow -> prev-state latch.
  if (rt.cs.arenaValid) {
    // Shared script env — built once per frame; the per-object tick
    // rebinds selfArena per object inside the call.
    TraversalScriptEnv objEnv;
    objEnv.image = std::span<const std::byte>(rt.level.cmiBytes.data(),
                                              rt.level.cmiBytes.size());
    objEnv.imageBase = 4;
    objEnv.playerPos = rt.cs.pos;
    objEnv.rt = &rt;
    objEnv.currentArena = cur;
    objEnv.modelFor = traversalModelFor;
    objEnv.modelCtx = &rt.level;
    objEnv.dt = dt;
    objEnv.slideChannel = rt.slideChannel;
    objEnv.slideMode = (rt.slideChannel != 0);
    objEnv.hasContactNormal = (rt.lastContactPoly != nullptr);
    objEnv.diagLog = &rt.scriptDiag;
    // Persistent script globals (0x541534 / 0x540d88 / 0x540d98) —
    // seeded from the runtime so writes survive past this frame.
    objEnv.g541534 = rt.g541534;
    objEnv.gFlags = rt.scriptGFlags;
    for (int i = 0; i < 8; ++i) objEnv.gVars[i] = rt.scriptGVars[i];

    // FUN_00436100 (OBSERVED 0x4362a8..0x4362e6): the object pass and
    // the FUN_0045cf18 corpse sweep run per arena, cur first, then the
    // partner — with the ca8/ca4 gate evaluated AT the partner call
    // site, so a partner attached during cur's pass takes its first
    // object pass this same frame.
    for (int ai = 0; ai < 2; ++ai) {
      TraversalArena* ua = cur;
      if (ai == 1) {
        if (!rt.partnerActive || rt.partner == nullptr) break;
        ua = rt.partner;
      }
      DynamicArena& da = ua->dyn;
      // FUN_004572ac — the linked-list walk reads the next link BEFORE
      // the body (0x4572c1), so a mid-frame transfer/teardown can't
      // corrupt iteration; the +0x06 head-scan skips dead objects.
      for (auto it = da.storage.begin(); it != da.storage.end();) {
        auto nextIt = std::next(it);
        DynamicObject& o = **it;
        it = nextIt;
        if (!o.col.named) continue;                  // +0x06 head-scan
        // 0x4572f4 — +0x07==1 view latch -> connector (0x14a&0x10) ->
        // orbit (0x14a&0x40).
        if (o.col.field07 == 1) rt.fieldB85c = &o;
        if (o.col.flags14a & 0x10)
          traversalConnectorUpdate(o, rt);           // FUN_00457738
        if (o.col.flags14a & 0x40)
          objectOrbit(o, rt.cs.pos, dt);             // FUN_00457ab8
        // 0x45747c — command runner branch (0x14b&0x40): runs, then
        // the +0x08 health check; alive continues at the transfer.
        if (o.col.flags14b & 0x40) {
          objectCommandRunner(rt, o, da, dt);        // FUN_0045ab44
          if (o.health == 0) continue;
        }
        // 0x457492 — pending-arena transfer (+0x2bc); nonzero return
        // skips the rest of this object's update.
        if (o.pendingArena != nullptr &&
            objectArenaTransfer(rt, o, da)) {
          ++rt.seams.objectMigrations;
          continue;
        }
        // 0x45733a — object script VM (+0x108 gate).
        if (o.field108 != nullptr) {
          TraversalScriptResult sr =
              traversalObjectScriptTick(objEnv, o);  // FUN_004388d8
          rt.scriptInsnTotal += sr.instructions;
        }
        if (!o.col.named) continue;                  // 0x45734a
        // Path follower (+0xec) -> subtype -> +0x06 -> gravity ->
        // collision -> +0x06.
        if (o.fieldEC != nullptr)
          objectPathFollow(o, rt.cs.pos,
                           rt.motion.yawDeg);        // FUN_00456d28
        objectSubtypeUpdate(rt, o, da, dt);          // FUN_004533d4
        if (!o.col.named) continue;                  // 0x45736b
        objectGravity(rt, o, da, dt);                // FUN_0045b9fc
        // The +0x14a&8 retry target is the live partner global in the
        // original (FUN_0045bac0 reads 0x540ca4 per object) — a
        // partner attached earlier in this same pass is visible here.
        TraversalArena* otherArena =
            (ai == 0)
                ? ((rt.partnerActive && rt.partner) ? rt.partner
                                                    : nullptr)
                : cur;
        objectCollide(rt, o, da, otherArena, dt);    // FUN_0045bac0
        if (!o.col.named) continue;                  // 0x457383
        // 0x4574a6 — enemy dispatch (+0x149&0x10) REPLACES the mover
        // branch and jumps straight to the anim step.
        if (o.col.flags149 & 0x10) {
          enemyCommandDispatch(rt, o, da, dt);       // FUN_0045897c
          if (!o.col.named) continue;                // 0x4574ad
        } else if (o.col.flags14a & 0x20) {
          objectMover(rt, o, da, dt, timing.frameStep); // FUN_004585c4
        }
        traversalObjectAnimUpdate(
            o, reinterpret_cast<const std::uint8_t*>(
                   rt.level.cmiBytes.data()) +
                   rt.level.cmiBytes.size());        // FUN_004555bc
        if (!o.col.named) continue;                  // 0x4573b1
        // Tail: roll ride (0x148&0x40) -> ridden-follow -> +0x18c
        // vel-cache + prev-state latch (inside latchObjectPrevState).
        if (o.col.flags148 & 0x40) objectRollRide(o);  // FUN_0045d578
        applyRideDisplacement(o, rt.cs, &rt.motion.yawDeg);
        latchObjectPrevState(o);
      }
      // FUN_004572ac epilogue — the 3-slot shot pool ticks at the tail
      // of EACH arena update (0x4572cd), so partner-active frames tick
      // it twice. OBSERVED quirk, preserved.
      playerShotPoolTick(rt, timing.frameStep, dt, timing.smoothed);
      // FUN_0045cf18 — the per-arena corpse sweep runs after the
      // object pass (OBSERVED 0x4362b2 cur / 0x4362e6 partner) and
      // before the script pass: teardown'd records leave the +0x68
      // list and their storage here, so the script-side spawn dedup
      // sees live objects only and a same-frame respawn reuses the
      // freed record (freelist pop in FUN_0045cffc/allocFront).
      da.reapUnnamed();
    }
    // Write the persistent script globals back (the original's
    // globals outlive the frame — the env is per-frame scratch).
    rt.g541534 = objEnv.g541534;
    rt.scriptGFlags = objEnv.gFlags;
    for (int i = 0; i < 8; ++i) rt.scriptGVars[i] = objEnv.gVars[i];
  }

  // FUN_004388d8 script-object calls — Phase 5H tr_alcmd VM.
  // OBSERVED gates/order: FUN_004388d8(c48+0x118) when c48+0x220
  // nonzero, then FUN_004388d8(ca4+0x118) when ca8 && ca4+0x220.
  // The VM reads the persisted PC each frame; checkpoint(0x01)
  // retargets it, wait(0x40) suspends to +0x230, stop(0x09) clears it.
  {
    TraversalScriptEnv env;
    env.image = std::span<const std::byte>(rt.level.cmiBytes.data(),
                                           rt.level.cmiBytes.size());
    env.imageBase = 4;
    env.playerPos = rt.cs.pos;
    env.rt = &rt;
    env.currentArena = cur;
    env.modelFor = traversalModelFor;     // FUN_004286c8 lazy models
    env.modelCtx = &rt.level;
    env.dt = dt;
    env.slideChannel = rt.slideChannel;
    env.slideMode = (rt.slideChannel != 0);
    env.hasContactNormal = (rt.lastContactPoly != nullptr);
    env.diagLog = &rt.scriptDiag;
    env.frameStep = timing.frameStep;
    env.g541534 = rt.g541534;
    env.gFlags = rt.scriptGFlags;
    for (int i = 0; i < 8; ++i) env.gVars[i] = rt.scriptGVars[i];

    TraversalArena* run[2] = {cur, nullptr};
    int nrun = 1;
    if (rt.partnerActive && rt.partner) { run[1] = rt.partner; nrun = 2; }
    for (int ai = 0; ai < nrun; ++ai) {
      TraversalArena* a = run[ai];
      if (a->script.pcImageOff == 0) continue;   // +0x220 gate
      env.selfArena = a;
      ++rt.seams.scriptObjectCalls;
      ++rt.scriptRuns;
      TraversalScriptResult sr = traversalScriptRun(env);
      rt.scriptInsnTotal += sr.instructions;
      if (env.slideClear) { rt.slideChannel = 0; rt.eventPriority = 0;
                            env.slideClear = false; }
      rt.slideChannel = env.slideChannel;
    }
    rt.g541534 = env.g541534;
    rt.scriptGFlags = env.gFlags;
    for (int i = 0; i < 8; ++i) rt.scriptGVars[i] = env.gVars[i];
    rt.scriptSpawned += env.seamsSpawned;
  }
  ++rt.seams.profilerHooks; // FUN_0042fecc rdtsc probe (0x43632d)

  // 0x540ebc pending view snap — OBSERVED consumer is a teleport
  // apply (FUN_0043490c: pos/prevPos <- pendingView[0..2],
  // 0x540c2c <- pendingView[3], c48 <- snap arena). The writer is
  // script-side; nothing in the bounded runtime sets it — counted.
  // The -1 mailbox value is NOT consumed here — it is the end-level
  // request checked at the FUN_00436100 tail (see below).
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

  // FUN_00461954's 0x324 handler (render pass, OBSERVED): once the
  // look offset has re-centred to exactly 0 the look state's event
  // priority is released — the NEXT dispatch's idle restore then
  // returns cac to the idle state. (The full animation machine is
  // deferred; this is the write the look exit depends on.)
  if (rt.locoState == kLookEventCode &&
      rt.look.lookPitchOffset == 0.0f)
    rt.eventPriority = 0;

  // FUN_004301e0 — OBSERVED structure:
  //   flagC9c==0 && flag49b740!=0 → call FUN_00431100 + commit only.
  //   flagC9c!=0                  → flagBec=0 → shared tail.
  //   else  viewScalar!=scalar && flagBec==0 → blend 0.85/0.15 → tail;
  //         otherwise flagBec=0 → tail.
  //   tail: z-delta clamp +/-0.5 → 0x49b718 EMA+sign-slew, the
  //         lookEff arena-relative clamp, viewYaw = 90 - yaw, the
  //         c84 pitch lift, effective pitch — then the Phase 5K
  //         camera block (position/basis/matrix pair, obstruction
  //         seam, view-config write) and the eye->camPos portal
  //         tail, then the prev-pos commit which runs EVERY frame
  //         on all paths (0x430272 / 0x4309ed movsd x3).
  bool overheadView = false;
  if (rt.flagC9c == 0 && rt.flag49b740 != 0) {
    // FUN_00431100 — overhead view block (scripted 0x40031 / cheat).
    ++rt.seams.overheadViewCalls;
    overheadView = true;
    PlayerCameraEnvironment ce;
    for (int i = 0; i < 3; ++i) ce.playerPos[i] = rt.cs.pos[i];
    ce.yawDeg = rt.motion.yawDeg;       // raw 0x540c2c — not viewYaw
    ce.altAspect = rt.flag5414bc;
    updatePlayerCameraOverhead(ce, rt.camera);
    // 0x4309ed — the overhead path commits prevPos after the call.
    for (int i = 0; i < 3; ++i) rt.cs.entryPos[i] = rt.cs.pos[i];
  } else {
    if (rt.flagC9c != 0) {
      rt.flagBec = 0;
    } else if (rt.viewScalar != cur->scalar && rt.flagBec == 0) {
      rt.viewScalar = rt.viewScalar * 0.85f + cur->scalar * 0.15f;
    } else {
      rt.flagBec = 0;
    }
    PlayerViewTailEnvironment vte;
    vte.smoothed = timing.smoothed;
    vte.deltaSeconds = dt;
    vte.arenaScalar = cur->scalar;
    vte.yawDeg = rt.motion.yawDeg;
    vte.lookOffset = rt.look.lookPitchOffset;
    vte.viewScalar = rt.viewScalar;
    vte.dz = rt.cs.pos[2] - rt.cs.entryPos[2];
    vte.airCharge = rt.motion.airCharge;
    // 0x430272 — prevPos commit sits INSIDE the tail, right after
    // the dz read and before the camera block (the FUN_00430bf8
    // seam, when ported, moves the player — the original commits
    // the pre-move position here).
    for (int i = 0; i < 3; ++i) rt.cs.entryPos[i] = rt.cs.pos[i];
    updatePlayerViewTail(vte, rt.view);

    // FUN_004301e0 camera block (0x43042b..0x4309dd) — pose, basis,
    // M1/M2 matrix pair, view-config write. The FUN_00430bf8
    // obstruction seam sits inside (post-basis, pre-commit) and may
    // move the camera AND the player when ported — env.playerPos is
    // the in/out channel for that contract.
    PlayerCameraEnvironment ce;
    for (int i = 0; i < 3; ++i) ce.playerPos[i] = rt.cs.pos[i];
    ce.viewYawDeg = rt.view.viewYawDeg;
    ce.effPitchDeg = rt.view.viewPitchDeg;
    ce.bankDeg = rt.motion.bank + rt.bankAux;   // b4c + b60
    ce.yawDeg = rt.motion.yawDeg;
    ce.altAspect = rt.flag5414bc;
    ce.sniperViewport = (rt.flagC9c != 0 && rt.transitionPhase != 0);
    ce.lookActive = (rt.look.lookPitchOffset != 0.0f);
    // Phase 5M — the FUN_00430bf8 collision context: cs carries the
    // player pos (0x540bfc), the arenas (c48/ca4), the carrier gate
    // (d3c) and the object-pass gate (c68); contactToken is the last
    // gameplay apply's 0x540e4c token (the obstruction's own applies
    // do not write it — the original stores it only in the
    // locomotion callers FUN_00467180/FUN_00467ed0).
    ce.collision = &rt.cs;
    ce.contactToken = rt.lastContactPoly;
    const PlayerCameraFrame cf = updatePlayerCamera(ce, rt.camera);
    if (cf.obstructionSeam) ++rt.seams.cameraObstructionCalls;
    // FUN_00430bf8 writes BOTH endpoints when it runs — env.playerPos
    // returns the post-obstruction 0x540bfc (already == rt.cs.pos).
    for (int i = 0; i < 3; ++i) rt.cs.pos[i] = ce.playerPos[i];

    // 0x43097f..0x4309ce — portal tail: b714 <- 0; eye = pos+3z;
    // FUN_00435178(cur, eye -> camPos); hit == ca4 → b714 = 1.
    rt.viewOnPartner = false;
    float eye[3] = {rt.cs.pos[0], rt.cs.pos[1],
                    static_cast<float>(rt.cs.pos[2] + 3.0)};
    if (TraversalArena* hit = traversalPortalScanSegment(
            rt, *cur, eye, rt.camera.pose.pos))
      if (hit == rt.partner) rt.viewOnPartner = true;
  }
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
  // 0x540d2c — frameStep-decayed countdown (0x431dd8..0x431deb):
  // decrements while positive, may go negative (no clamp).
  if (rt.fieldD2c > 0) rt.fieldD2c -= timing.frameStep;
  // 0x540e10 — the player-damage suppress window; decays by the FIXED
  // 1/30 tick (0x46404a: fldz; fcomp; else -= DAT_0049b6f4), not
  // frameStep and not the smoothed units.
  if (rt.fieldE10 > 0.0f) rt.fieldE10 -= 1.0f / 30.0f;
  rt.fieldE14 = 0;
  // OBSERVED (0x436491..0x4364dd): EAX = 0; when flag541548 == 0 the
  // single call is FUN_00436d60(0) — body only, no bracket. When
  // flag541548 != 0 the caller first runs FUN_0042b20c(c9c != 0 &&
  // ca0 > 1 ? 1 : 0), then FUN_00436d60(-1) and falls through to
  // FUN_00436d60(+1) — the shared body runs TWICE per frame, once
  // inside each call's save/nudge/restore bracket.
  // FUN_00436d60(arg): arg != 0 -> save the 212-byte camera block
  // (FUN_0042b060) -> FUN_0042b0c0(arg) -> shared body -> restore
  // (FUN_0042b090); arg == 0 -> body only. Each call's flag541548
  // tail rewrites the same nudgeTick via FUN_0042b248(arg) — no
  // observable delta. OBSERVED: 0x541548 is BSS and has NO writer in
  // BUILD_A (all 27 xrefs are reads) — the dual-call bracket is a
  // dead second-viewport path in this build.
  const auto run36d60Body = [&]() {
    ++rt.seams.animDriverCalls;
    // FUN_00436ea8 -> FUN_00431300 -> FUN_00461954: the
    // animation/state machine. The bounded subset handles the
    // sniper-lifecycle states (0x323 scope-in, 0x384 unscope) + the
    // cb0 first-frame latch; the rest of the machine is deferred.
    playerAnimAdvance(rt, timing.frameStep);
    // Scope gate A (0x436dd3): c9c != 0 && ca0 > 1 -> the shot-pool
    // render pass FUN_0045f030(0) + the charge probe FUN_00437aa8
    // (which writes the 0x540e14 live flag).
    if (rt.flagC9c != 0 && rt.transitionPhase > 1) {
      ++rt.seams.shotRenderCalls;   // FUN_0045f030(0)
      playerChargeProbe(rt);         // FUN_00437aa8
    }
    // FUN_00469f7c — the unconditional inventory/HUD icon updater.
    // Out of the fire scope; counted as a HUD seam.
    ++rt.seams.hudEventCalls;
    // Scope gate B (0x436e1c): c9c != 0 && ca0 > 1 -> FUN_0045f030(1)
    // + FUN_00436f08 (the shared d0c fire-cadence counter) +
    // FUN_00437660 (the cadence/burst/ammo machine).
    if (rt.flagC9c != 0 && rt.transitionPhase > 1) {
      ++rt.seams.shotRenderCalls;   // FUN_0045f030(1)
      if (rt.hudActive != 0 && rt.fieldD0c < 999) ++rt.fieldD0c;
      playerWeaponCadence(rt, dt);
    }
  };
  if (rt.flag541548) {
    ++rt.seams.extraWorldTickCalls; // FUN_0042b20c + the -1/+1 calls
    cameraNudgeApplyMode(
        rt.camera,
        (rt.flagC9c != 0 && rt.transitionPhase > 1) ? 1 : 0);
    ++rt.seams.worldTickCalls;    // 36d60(-1): save, nudge, body, restore
    PlayerCameraPose saved = rt.camera.pose;
    cameraNudge(-1, rt.camera);
    run36d60Body();
    rt.camera.pose = saved;
    ++rt.seams.worldTickCalls;    // 36d60(+1): save, nudge, body, restore
    saved = rt.camera.pose;
    cameraNudge(1, rt.camera);
    run36d60Body();
    rt.camera.pose = saved;
  } else {
    ++rt.seams.worldTickCalls;    // 36d60(0): body only
    run36d60Body();
  }

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

  // 0x540ebc == -1 — the FUN_00436100 end-level arm (0x436b0b ->
  // 0x436d30, OBSERVED): the mailbox is cleared, the 0x540c68
  // (arenaValid) and 0x540c74 (FUN_00432f84) gates drop, FUN_00469668
  // notifies, then FUN_0040dde0(pos) starts the victory sequence.
  // The bounded runtime surfaces that call as the endLevelRequest
  // edge — the session driver maps it to
  // progressionRequestTraversalEnd.
  if (rt.pendingViewSnap == -1) {
    rt.pendingViewSnap = 0;
    rt.cs.arenaValid = 0;
    rt.fieldC74 = 0;
    ++rt.seams.animEventCalls;    // FUN_00469668 notify
    rt.endLevelRequest = 1;
    ++rt.seams.endLevelRequests;
  }

  // FUN_0047c9e0 per-object transform refresh — OBSERVED (0x47cd01 /
  // 0x47cede): the render-collection pass calls FUN_0045612c once per
  // frame for every live object whose +0x148 dword lacks 0x201000 —
  // i.e. +0x14a&0x20 movers (which rebuild inside FUN_004585c4) and
  // +0x148&0x1000. Path/mover/steer movement during the tick therefore
  // reaches the collision xform + element world-AABBs only here, at
  // frame end — same-frame probes see last frame's refresh.
  {
    TraversalArena* colRun[2] = {cur, nullptr};
    int ncolRun = 1;
    if (rt.partnerActive && rt.partner) {
      colRun[1] = rt.partner;
      ncolRun = 2;
    }
    for (int ai = 0; ai < ncolRun; ++ai) {
      for (auto& up : colRun[ai]->dyn.storage) {
        DynamicObject& o = *up;
        if (!o.col.named) continue;                  // dead/teardown
        if ((o.col.flags148 & 0x1000) != 0 ||
            (o.col.flags14a & 0x20) != 0)            // +0x148 dword
          continue;                                 //  & 0x201000 skip
        rebuildObjectTransform(o);                  // FUN_0045612c
      }
    }
  }

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
  out.endLevelRequested = rt.endLevelRequest != 0;
  out.endingRequested = rt.endingRequest != 0;
  out.eventTimer = rt.eventTimer;
  out.viewScalar = rt.viewScalar;
  out.lookOffsetDeg = rt.look.lookPitchOffset;
  out.viewYawDeg = rt.view.viewYawDeg;
  out.viewPitchDeg = rt.view.viewPitchDeg;
  out.viewZDelta = rt.view.viewZDelta;
  out.viewPitchLift = rt.view.viewPitchLift;
  out.overheadViewActive = overheadView;
  out.camera = rt.camera.pose;
  out.seams = rt.seams;
  return out;
}

} // namespace mdk
