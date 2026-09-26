// Phase 5D — player collision query/apply path and floor probe.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md §"Phase 5D" for the full
// evidence chain):
//
//   FUN_004630d4(dx, dy, dz, scale, extVec, outAux) is the generic
//   swept-volume collision query/apply used by both the horizontal
//   movement path (scale 0.75, extVec NULL -> {0.6,0.6,2.5},
//   outAux NULL) and the vertical path (scale 0.5, extVec NULL ->
//   {0.4,0.4,2.5}, outAux -> &DAT_00540e50). Six stack arguments;
//   ECX/EDX carry no inputs. EAX returns the hit polygon-record
//   pointer (0 = no contact).
//
//     pos (0x540bfc/0x540c00/0x540c04) -> lifted start
//       (posZ + ext.z + margin; margin 0.5 horizontal / 0.01 vertical)
//     -> FUN_00407fc0 iterative sweep (flag=4: up to 4 slide steps
//        + 1 contact pass) against arena c48's {nodes, polys, verts}
//     -> retry once against carrier arena ca4 (flag=0) when the
//        primary reports no contact and the portal-transition gates
//        allow (ca8 && !d3c && !e6c)
//     -> swept-AABB object pass over the arena +0x68 list
//        (FUN_0045ce58 / FUN_0045c838, X/Y face clamp only)
//     -> final static re-sweep (flag=0) when an object resolved
//     -> pos += applied delta (always commits; never restores)
//
//   FUN_00435eec() is NOT inside the query: it is the per-frame floor
//   probe called once at the end of FUN_00436100's traversal frame,
//   after the movement dispatcher. It probes the object list with a
//   top->bottom segment (posZ+3.0 -> posZ-3.0) via FUN_004138d8 /
//   FUN_00413730 and writes c58 floorZ, c60/c64 blockers, c54 bit1.
//
//   The lower level is proven: recursive BSP traversal
//   (FUN_00408260) over 0x2c-byte nodes carrying plane+child pairs
//   and two polygon-set dwords, leaf polygon records (0x24-byte,
//   three u16 vertex indices into a float3 array), a projected
//   box-vs-triangle test (FUN_004089c0) and iterative plane slide
//   (FUN_004088cc / FUN_0040894c).
//
// NATIVE PORT DECISIONS:
//   - The original's globals (pos, c54/c58/c5c/c60/c64, c68/c70,
//     ca4/ca8/d3c, dc0/dc4/dc8, e6c, e68) live in CollisionState;
//     the sweep scratch block (0x4a2098..) lives in a per-query
//     SweepState on the stack.
//   - The 0x4635e0 -> FUN_0040b5d0 surface-effect dispatch becomes
//     an optional contact hook (bounce/conveyor are that system's
//     outputs, not this layer's).
//   - FUN_00461878 (dismount/reset) is likewise a hook; the state
//     field it gates (rideActive -> 0) is still written here.
//   - Struct layouts mirror the proven runtime records so a future
//     loader can populate them directly from the FUN_00419ee0 blob.
//
#ifndef MDK_CORE_COLLISION_QUERY_H
#define MDK_CORE_COLLISION_QUERY_H

#include <cstddef>
#include <cstdint>

namespace mdk {

// ---------------------------------------------------------------------------
// Runtime collision geometry (per-arena, built by FUN_00419ee0)
// ---------------------------------------------------------------------------

// BSP node — 0x2c bytes, the +0x2c table inside each arena record.
struct CollisionNode {
  float nx, ny, nz, d;   // +0x00 split plane
  std::int16_t childFar; // +0x10 — descend when startDist <= +margin
  std::int16_t childNear;// +0x12 — descend when startDist >= -margin
  // Polygon sets are {lo16 count, hi16 firstIdx} into the poly table.
  std::uint32_t polysPos;// +0x14 — consulted when startDist >= 0
  std::uint32_t polysNeg;// +0x18 — consulted when startDist < 0
  std::uint32_t aux1c;   // +0x1c — relocated pointer (unused by sweep)
  std::uint32_t aux20;   // +0x20 — relocated pointer (unused by sweep)
  std::uint32_t aux24;   // +0x24 — unused by the sweep
  std::uint32_t aux28;   // +0x28 — unused by the sweep
};
static_assert(sizeof(CollisionNode) == 0x2c);

// Leaf polygon record — 0x24 bytes, the +0x28 table.
struct CollisionPoly {
  std::uint16_t v[3];    // +0x00 vertex indices into the vert array
  std::uint8_t pad[26];  // +0x06..0x1f — not read by the collision path
  std::uint16_t flags;   // +0x20 — bit5 (0x20) = skip; bit2 (0x4) = low friction
  std::uint8_t pad22;    // +0x22
  std::uint8_t surface;  // +0x23 — surface-type index + 1 (0 = none)
};
static_assert(sizeof(CollisionPoly) == 0x24);

// Element record — the collision-used fields of the original's
// 0x5c-stride element entries (object +0x0c -> {+0x1c count, +0x20 array}).
struct CollisionElement {
  std::int32_t triCount;      // +0x10
  const float* verts;         // +0x14 — local-space f32 triples
  const std::uint8_t* tris;   // +0x18 — 0x24-stride records, u16 v[3] at +0
  float aabb[6];              // +0x44 {minx,miny,minz,maxx,maxy,maxz} — world
  float localAabb[6];         // +0x2c — local bounds (FUN_00459d54 output);
                              // consumed only by the transform rebuild
                              // (FUN_00459e40 8-corner transform), never
                              // by the query itself. Phase 5E addition.
};

struct CollisionElementSet { // object +0x0c record {+0x1c count, +0x20 elems}
  std::int32_t count;
  const CollisionElement* elems;
};

// Object node — the collision-used fields of the arena +0x68 list
// records (the original record is much larger; offsets noted per field).
struct CollisionObject {
  const CollisionObject* next;       // +0x00
  bool named;                        // +0x06 != 0
  std::uint8_t field07 = 0;          // +0x07 — view-anchor class; the
                                     // FUN_004572ac update loop latches
                                     // the last object whose byte is 1
                                     // into 0x49b85c (camera anchor)
  const void* model;                 // +0x08 != 0 (presence gate)
  const CollisionElementSet* elements;// +0x0c -> element set record
  float baseZ;                       // +0x18 — ride-offset reference
  std::uint16_t flags148;            // +0x148 — sweep skips &0x810, floor skips &0x10
  std::uint8_t flags149;             // +0x149 — bit0 = has standable geometry
  std::uint8_t flags14a;             // +0x14a — bit4 = elemMaskA enable; bit7 = mountable
  std::uint8_t flags14b;             // +0x14b — bit0 = camera
                                     // obstruction gate (FUN_00430bf8),
                                     // bit1 = mount-scan gate
                                     // (FUN_00463608), bit2 = reticle
                                     // disabled (FUN_004691c4)
  // +0x14c — per-frame contact byte (FUN_0045bac0 clears bits
  // 0x13 at the head of each object update: bit0 = wall/XY sweep hit,
  // bit1 = floor contact, bit4 = FUN_00459618 touch-scan hit). Bits
  // 2/3/5..7 survive the clear (steering-side flags).
  std::uint8_t flags14c = 0;
  float aabb[6];                     // +0x198
  float xform[9];                    // +0xac..0xd4 — row-major 3x3 (scale baked)
  float origin[3];                   // +0xb8, +0xc8, +0xd8
  float scale;                       // +0x58 — uniform scale
  std::uint32_t elemMaskB;           // +0x2c8 — excluded-element bitmask
  std::uint32_t elemMaskLatch;       // +0x2cc — unmask latch (op 0x20 skips set bits)
  std::uint32_t elemMaskA;           // +0x326 — extra mask (flags14a bit4 gate)
};

// The arena's collision-relevant fields (of the 0x466-byte record).
struct CollisionArena {
  const float* verts = nullptr;            // +0x24 — f32 triples
  const CollisionPoly* polys = nullptr;    // +0x28 — 0x24 records
  const CollisionNode* nodes = nullptr;    // +0x2c — 0x2c records
  const CollisionObject* objects = nullptr;// +0x68 — linked list
  // +0x44e — arena geometry AABB minZ, the "abyss" reference for
  // the failsafe family (player -50 at 0x4673f3; object kill plane
  // -200 at 0x45bdd6; death snap -150 at 0x4583ab/0x4583ce; 0x45fd55).
  // OBSERVED (FUN_004320d0): folded from the installed verts via
  // lea ecx,[eax+0x446] + register-indirect stores — which is why a
  // disp32 pattern sweep finds no +0x44e writer. Zero when the
  // record has no verts (FUN_004320d0 early-returns on +0x24 == 0).
  float deepFloorZ = 0.0f;
};

// ---------------------------------------------------------------------------
// Collision state — the globals the original carries across frames
// ---------------------------------------------------------------------------

// Hook replacing the 0x4635e0 -> FUN_0040b5d0 surface-effect dispatch.
// Invoked once per sweep iteration that produced a contact (same rule
// as the original callback). `node` is the hit BSP node (normal source).
using CollisionContactHook = void (*)(
    struct CollisionState& cs, const CollisionNode* node,
    const CollisionPoly* poly);

// Hook replacing FUN_00461878 (dismount/reset on the ride state).
using CollisionDismountHook = void (*)(struct CollisionState& cs);

// Phase 5F — the collision object's surface-effect block (player_surface.h).
struct SurfaceObjectState;

struct CollisionState {
  // 0x540bfc / 0x540c00 / 0x540c04 — player position. collisionApply
  // always commits pos += applied delta (no restore on failure).
  float pos[3] = {0.0f, 0.0f, 0.0f};
  // 0x540c54 — contact byte. bit1 = floor-probe valid (written here);
  // bit0 = grounded, owned by the vertical landing path (not here).
  std::uint8_t contactFlags = 0;
  // 0x540c58 / 0x540c5c — floor height + ridden-object relative offset.
  float floorZ = 0.0f;
  float floorOffset = 0.0f;
  // 0x540c60 / 0x540c64 — probe blockers: floor object + element mask.
  const CollisionObject* floorObj = nullptr;
  std::uint32_t floorElemMask = 0;
  // 0x540dc0 / 0x540dc4 / 0x540dc8 — "standing on object" mount state,
  // written by the vertical landing path; consumed here.
  const CollisionObject* rideObj = nullptr;
  std::uint32_t rideElemMask = 0;
  int rideActive = 0;
  // 0x540c68 / 0x540c70 / 0x4999d4 — query gates.
  int arenaValid = 0;
  int queryEnabled = 0;
  int objectDataLoaded = 0;
  // 0x540c48 / 0x540ca4 / 0x540ca8 / 0x540d3c — arena + carrier context.
  const CollisionArena* arena = nullptr;
  const CollisionArena* carrier = nullptr;
  int carrierValid = 0;
  int carrierBusy = 0;
  // 0x540e6c / 0x540e68 — excluded object + last object contact.
  const CollisionObject* excludeObj = nullptr;
  const CollisionObject* lastObjContact = nullptr;
  // 0x540c30..0x540c44 — the per-query player AABB (6 floats).
  float playerBox[6] = {0, 0, 0, 0, 0, 0};
  // --- Phase 5F — surface-contact dispatch inputs (the 0x4635e0 args) ---
  // 0x540c08 — the entry/snapshot position the dispatch's delta reads
  // (host-written on arena entry; the original sets it in the load/
  // transition path, NOT per contact).
  float entryPos[3] = {0.0f, 0.0f, 0.0f};
  // The sweep's working/contact position (s.hitPt), staged before each
  // contactHook invoke — the 0x4a20c0 vecA of the original callback.
  float sweepContact[3] = {0.0f, 0.0f, 0.0f};
  // 0x4a2438 / 0x4a2448 — the surface-handler fx outputs (delta/vec),
  // written by the dispatch when a handler invokes.
  float surfDelta[3] = {0.0f, 0.0f, 0.0f};
  float surfVec[3] = {0.0f, 0.0f, 0.0f};
  // The collision object's +0x6c..0x45e surface block + the dispatch
  // result byte + the caller's context channel (8 = player sweep).
  SurfaceObjectState* surface = nullptr;
  std::uint8_t surfaceContextMask = 0;
  std::uint8_t surfaceResult = 0;
  // Optional hooks (surface effects + dismount reset).
  CollisionContactHook contactHook = nullptr;
  CollisionDismountHook dismountHook = nullptr;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// FUN_004630d4 — swept collision query + apply.
//   dx/dy/dz    requested displacement (already time-scaled by caller)
//   scale       slide-budget factor (0.75 horizontal / 0.5 vertical)
//   extVec      box half-extents or nullptr for the originals'
//               {0.6,0.6,2.5} (dz==0) / {0.4,0.4,2.5} (dz!=0) defaults
//   outNode     optional; receives the hit BSP node (normal source)
// Returns the hit polygon-record pointer (0 = no contact) — the EAX
// token the original stores in 0x540e4c.
const CollisionPoly* collisionApply(CollisionState& cs, float dx,
                                    float dy, float dz, float scale,
                                    const float* extVec,
                                    const CollisionNode** outNode);

// FUN_00435eec — the per-frame floor/contact probe. Run once per
// frame after movement (the original calls it at the tail of the
// traversal frame), NOT inside collisionApply.
void collisionFloorProbe(CollisionState& cs);

// FUN_00419ee0 — parse a level-stream collision blob into arena
// tables. The blob is self-describing (used in place, zero-copy):
//   [u32 countA][countA x 10B][pad 2B if countA odd]
//   [u32 countB][countB x 44B]  BSP nodes (+0x1c/+0x20 = offsets
//                               relative to the blob end)
//   [u32 countC][countC x 36B]  poly records (u16 v[3] at +0)
//   [u32 countD][countD x 12B]  f32 vertex triples
//   [u32 tail]
// Returns false when the layout is inconsistent or out of bounds.
// On success `arena` points into `blob` (blob must outlive it) and
// `outCounts` receives {countA, nodes, polys, verts}.
bool collisionBlobParse(const std::uint8_t* blob, std::size_t size,
                        CollisionArena* arena,
                        std::uint32_t outCounts[4]);

// FUN_00407fc0 — the iterative sweep orchestrator, exposed for tests
// and the carrier re-sweep. `flag` = slide-iteration budget (the
// original passes 4 for the primary query, 0 for probes).
const CollisionPoly* collisionSweep(CollisionState& cs,
                                    const float* start, const float* target,
                                    int flag, const CollisionArena& arena,
                                    const float* ext, float scale,
                                    float* outPos,
                                    const CollisionNode** outNode);

// FUN_0045cd38 — segment-vs-inflated-AABB overlap prefilter.
//   a/b: segment endpoints; box6: {minx,miny,minz,maxx,maxy,maxz};
//   ext: per-axis box inflation. Returns 1 on overlap (non-strict
//   inequalities — touching counts).
int collisionSegAabbOverlap(const float* a, const float* b,
                            const float* box6, const float* ext);

// FUN_0045c838 — segment vs element AABB, 2.5D resolver.
//   outClamp: earliest face-crossing point; outAlt: face-clamped
//   target. Returns 0 = no interaction, 1 = face clamp, 2 = inside.
int collisionSegAabbResolve(const float* start, const float* target,
                            const float* box6, float* outClamp,
                            float* outAlt);

// FUN_00418c60 — BSP segment stab (the camera-obstruction grounding
// probe). Walks `arena`'s node tree from `from` to `to`; returns the
// containing node (0 = no non-skip polygon crossed the segment) and,
// on a hit, the plane-crossing point via outPos. The mode byte is
// always 0 on this entry (FUN_00418ce8's modes 1/2 are a different
// caller set — not used by the camera path).
const CollisionNode* collisionStab(const CollisionArena& arena,
                                   const float* from, const float* to,
                                   float* outPos);

// FUN_00418c60 + FUN_00418d6c pair — identical walk, additionally
// reports the containing polygon record (the original exposes it
// through the 0x54b6d4 global read by FUN_00418d6c; the projectile
// impact dispatch consumes it as the surfaceDispatch poly).
const CollisionNode* collisionStabFull(const CollisionArena& arena,
                                       const float* from, const float* to,
                                       float* outPos,
                                       const CollisionPoly** outPoly);

// FUN_00418ce8 — the mode-1 BSP stab (0x54b6f8=1, OBSERVED
// 0x418b0d..0x418c35): crossings on planes with |nz| < 0.5 are
// skipped outright, and containment scans a single poly set chosen
// by the nz sign (polysPos for nz>=+0.5, polysNeg for nz<=-0.5). The
// script yaw-offset floor probe (object op 0xec -> FUN_0045d71c) is
// the sole caller family — a vertical "is there floor here" query.
const CollisionNode* collisionStabMode1(const CollisionArena& arena,
                                        const float* from,
                                        const float* to);

// FUN_004138d8 — object-local segment probe. Transforms the
// world-space `start`->`end` segment into the object's local frame,
// scans every unmasked element triangle (segTri shortens `end` to the
// hit — nearest wins across the whole element set), and writes the
// world hit point back into `end` when any triangle was hit.
// `outElem`/`outTri` receive the last (nearest) hit indices; -1 on a
// miss. The shot path relies on the `end` writeback: successive
// probes re-clip the segment so the nearest object wins.
void collisionObjectProbe(const CollisionObject* obj, const float* start,
                          float* end, int* outElem, int* outTri);

} // namespace mdk

#endif // MDK_CORE_COLLISION_QUERY_H
