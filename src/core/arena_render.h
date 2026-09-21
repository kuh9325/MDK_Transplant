// Phase 6A (G1-RE) — platform-neutral arena render-data boundary.
//
// ORIGINAL ENGINE OBSERVATIONS (instruction-level, MDK95.EXE BUILD_A —
// see docs/reverse-engineering/ for the evidence chain):
//
//   Arena drawing enters at FUN_00431300 (draw_arena), reached from
//   the frame driver FUN_00436d60 via FUN_00436ea8 (current + partner
//   arena). It transforms the arena's vertex table (region-C array-4,
//   shared with collision) through the camera M1 matrix into a scratch
//   {x',y',z',sx,sy,clipFlags} record, inserts the frame's dynamic draw
//   entries into per-node lists, binds the render globals via
//   FUN_0040a688 and walks the BSP with FUN_00409a6c.
//
//   The walk (OBSERVED — the order flag DAT_00499f8c is initialized
//   to 1 in DGROUP (image offset 0x9838c) and has exactly one code
//   reference, the reader at 0x409a9d, so the flag!=0 branch is the
//   live path and is taken on every frame):
//     dist = node.plane . camPos + node.d
//     dist > 0  : recurse childFar (+0x10, the negative-halfspace
//                 subtree — VERIFIED against subtree geometry), flush
//                 the +0x28 entry list, submit the +0x14 poly span,
//                 flush the +0x24 list, tail-descend childNear (+0x12,
//                 the positive-halfspace subtree)
//     dist <= 0 : the symmetric mirror (childNear first, +0x18 span,
//                 childFar last)
//   i.e. the far-side subtree is submitted FIRST (back-to-front in
//   BSP terms — a painter's-algorithm walk). The rasterizer performs
//   unconditional indexed-byte stores with no depth test (OBSERVED
//   in both installed span-drawer tables) — correct because under
//   painter's order the LAST write wins and nearer geometry is always
//   submitted later. The dead flag==0 variant is the child-swapped
//   front-to-back order; it is never reachable in this build. Dynamic
//   draw entries follow the same convention through a 4096-entry
//   depth-keyed deferred list (DAT_00541500 = 1 from init): entries
//   are sorted descending depth and drained far-first. Submission
//   order is the visibility contract and is reproduced verbatim by
//   arenaRenderOrder().
//
//   Poly submission (FUN_00409860) iterates the node's {lo16 count,
//   hi16 firstIdx} span, skips polys with +0x20 bit4 (0x10), derives
//   DAT_005414b8 = (+0x20 bit0 && DAT_005414b4) where DAT_005414b4 is
//   the per-frame unbanked-view flag ((bank + auxBank) == 0.0,
//   computed in FUN_00436100), draws the +0x22-bit7 edge overlay,
//   and calls the clipper FUN_0040ca00 with the three transformed
//   verts, the poly's UV pairs, and the signed material index.
//
//   The raster dispatcher (FUN_0040c860) sorts verts by screen Y and
//   selects on the s16 material index:
//     >= 0 : arena material table entry -> textured mapper
//            (FUN_0046daac) when record +0x24 (pixel ptr) != 0, else
//            an error path then flat fill 0xff
//     <  0 : negative dispatch — [-1023,-1] flat palette pen with
//            color byte = (-index) & 0xff, except [-1010,-990] ->
//            FUN_0047a770(index+1000); [-1027,-1024] and <= -1029 ->
//            FUN_00412970(table 0x540b20 + scaled offset); -1028 ->
//            FUN_0046e940. (VERIFIED on LEVEL3 HMO_1 data: -37, -152
//            etc. map to PEN_n palette pens.)
//
//   Materials (FUN_0041a1e0) build 0x34-byte records from MTI payloads:
//     +0x00 shift = ceil(log2 width) clamped < 12
//     +0x04 width   +0x08 height   +0x0c class/flag word
//     +0x10 uMask = (1<<shift)-1
//     +0x14 vMask = (width==height ? uMask : bucketMask(height))<<shift
//     +0x18 ~uMask  +0x1c/+0x20 raw record fields  +0x24 pixel data
//     +0x28 name[8]
//   Extended payloads (class & 0x30000) carry a u16 frame count at
//   payload +0 and hold frameCount*width*height texel bytes after the
//   8-byte header (OBSERVED: EXPLODE = 26 frames of 128x128).
//
//   Material resolution (FUN_0041a694 "matlkup") matches each region-C
//   material name against bank A (the shared LEVELnS.MTI) FIRST, then
//   bank B (the arena's embedded .MAT); a miss logs "Texture %s not in
//   material list" ONCE at lookup time and stores the fallback pointer
//   DAT_0054b73c — which the bank loaders set to NULL. At draw time a
//   NULL table entry (or a record with +0x24 pixel ptr == 0) takes the
//   flat fill with pen 0xff (OBSERVED, FUN_0040c860). The per-poly
//   index is therefore a name-table index, not an MTI record index.
//
//   The palette (region B, 0x150 bytes OBSERVED) is RGB triplets
//   expanded by FUN_0046d490 into 4-byte entries.
//
// This module is DATA ONLY: it decodes proven layouts and reproduces
// the submission order. It performs no rasterization and owns nothing
// that the caller did not already own.
//
#ifndef MDK_CORE_ARENA_RENDER_H
#define MDK_CORE_ARENA_RENDER_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/collision_query.h"
#include "core/mto_directory.h"

namespace mdk {

// ---------------------------------------------------------------------------
// Decoded render fields of the shared 0x24-byte region-C poly record.
// CollisionPoly already models the same bytes (v[3] @+0, flags/surface
// @+0x20/+0x23); this view adds the render-only fields the collision
// path does not name. OBSERVED offsets.
// ---------------------------------------------------------------------------
struct ArenaRenderPoly {
  std::uint16_t v[3];       // +0x00 vertex indices into the vert array
  std::int16_t material;    // +0x06 — >=0: material-name index;
                            //       <0: pen/effect dispatch (below)
  float uv[3][2];           // +0x08/+0x10/+0x18 — per-vertex {u,v}
  std::uint8_t flags;       // +0x20 — bit4 (0x10) render skip;
                            //         bit0 (0x01) selects the |4 half
                            //         of the installed span-drawer
                            //         table when the view is unbanked
                            //         (DAT_005414b8 = bit0 &&
                            //         DAT_005414b4; OBSERVED mechanism,
                            //         visual role PARTIAL)
  std::uint8_t aux21;       // +0x21 — UNKNOWN (preserved raw)
  std::uint8_t aux22;       // +0x22 — bit7 (0x80) enables the clipped
                            //         edge-line overlay; bits
                            //         0x10/0x20/0x40 select edges
                            //         v1->v0 / v2->v1 / v2->v0 drawn
                            //         by FUN_0040da54 (flat pen line
                            //         or LUT-remap line for matIdx
                            //         <= -1024). OBSERVED; in real
                            //         data only remap-effect polys
                            //         (-1027..-1024) carry bit7.
  std::uint8_t surface;     // +0x23 — collision surface index + 1
};

// Extract the render view of one region-C poly record.
ArenaRenderPoly arenaRenderPolyDecode(const CollisionPoly& poly);

// +0x20 flag bits (OBSERVED in FUN_00409860):
inline constexpr std::uint8_t kArenaPolyAltSpan = 0x01;  // |4 drawer
                                                        // when unbanked
inline constexpr std::uint8_t kArenaPolySkip    = 0x10;  // render skip
// +0x22 edge-overlay bits (OBSERVED in FUN_00409860 ->
// FUN_0040da54): bit7 enables the overlay, the 0x70 mask selects
// which clipped edges are drawn as screen-space lines.
inline constexpr std::uint8_t kArenaEdgeV10 = 0x10;  // edge v1->v0
inline constexpr std::uint8_t kArenaEdgeV21 = 0x20;  // edge v2->v1
inline constexpr std::uint8_t kArenaEdgeV20 = 0x40;  // edge v2->v0
inline constexpr std::uint8_t kArenaEdgeOverlay = 0x80;

// ---------------------------------------------------------------------------
// Decoded material — the platform-neutral view of the original's
// 0x34-byte record (FUN_0041a1e0). Pixels alias the caller's source
// data (the .MTO/.MTI file buffer must outlive the view).
// ---------------------------------------------------------------------------
struct ArenaRenderMaterial {
  std::string name;           // +0x28 — 8-byte field, NUL-trimmed
  std::uint32_t shift = 0;    // +0x00 — ceil(log2 width), clamped <12
  std::uint32_t width = 0;    // +0x04
  std::uint32_t height = 0;   // +0x08
  std::uint32_t flags = 0;    // +0x0c — class word; extended payloads
                              //       carry frameCount << 16
  std::uint32_t uMask = 0;    // +0x10 — (1<<shift)-1
  std::uint32_t vMask = 0;    // +0x14 — height bucket mask << shift
  std::uint32_t invUMask = 0; // +0x18 — ~uMask
  std::uint32_t param0c = 0;  // +0x1c — raw MTI record +0x0c
  std::uint32_t param10 = 0;  // +0x20 — raw MTI record +0x10
  std::uint16_t frameCount = 0;          // extended header u16 @+0
  std::span<const std::uint8_t> pixels;  // +0x24 — frameCount*w*h bytes
  bool isIndexRecord = false;            // class word == 0xffffffff
};

// Decode one MTI record + its payload (from the file bytes that
// produced the directory). Returns false when the payload header or
// pixel extent escapes the file. `imgBase`/`fileOffset` adapt the
// embedded .MAT case (payload offsets are img-relative, not file-
// relative) — pass the byte offset of the embedded name field.
bool arenaRenderMaterialDecode(std::span<const std::byte> fileBytes,
                               std::uint64_t payloadOffset,
                               std::uint32_t classWord,
                               std::uint32_t raw0c,
                               std::uint32_t raw10,
                               std::span<const std::byte> nameField,
                               ArenaRenderMaterial* out);

// ---------------------------------------------------------------------------
// The per-poly material dispatch class — the FUN_0040c860 selection.
// For a negative index, penIndex() yields the flat palette byte.
// ---------------------------------------------------------------------------
enum class ArenaMatClass {
  kTextured,    // >= 0 with a resolved material (pixels != null)
  kUnresolved,  // >= 0 unresolved / null material record (flat 0xff)
  kPen,         // -1023..-1 except -1010..-990 -> flat (-idx)&0xff
  kEffect770,   // -1010..-990 -> FUN_0047a770 (UNKNOWN semantics)
  kEffectE94,   // -1028 -> FUN_0046e940 (UNKNOWN semantics)
  kEffect12970, // -1027..-1024 or <= -1029 -> FUN_00412970 (UNKNOWN)
};

ArenaMatClass arenaMatClassFor(std::int16_t material);
// For kPen: the palette index (-material) & 0xff.
std::uint8_t arenaPenIndex(std::int16_t material);

// ---------------------------------------------------------------------------
// Per-arena render data — the immutable, decoded bundle a frontend
// consumes. All file-backed views (pixels, palette, geometry) alias
// the level buffers; the struct copies only decoded records.
// ---------------------------------------------------------------------------
struct ArenaRenderData {
  // Geometry (shared with collision — aliases the region-C blob).
  const float* verts = nullptr;              // f32 triples
  const CollisionNode* nodes = nullptr;      // 0x2c records
  std::vector<ArenaRenderPoly> polys;        // decoded render polys
  std::uint32_t vertCount = 0;
  std::uint32_t nodeCount = 0;

  // Material names (region-C array-1, char[10] fields). The poly's
  // nonnegative material index selects one of these.
  std::vector<std::string> materialNames;

  // Material banks in the original's search order: A = shared level
  // bank (LEVELnS.MTI) searched FIRST, B = the arena's embedded .MAT.
  std::vector<ArenaRenderMaterial> bankA;
  std::vector<ArenaRenderMaterial> bankB;

  // matlkup result per name slot: 0..bankA.size()-1 index bank A,
  // bankA.size().. index bank B, or -1 = the original's NULL fallback
  // (draws as flat pen 0xff — OBSERVED FUN_0040c860 @0x40c9da).
  std::vector<int> materialOfName;

  // Palette — region-B RGB triplets (3 bytes per entry, OBSERVED
  // 0x150 bytes in BUILD_A). Aliases the source file.
  std::span<const std::uint8_t> paletteRgb;

  // Resolved material for a poly: for material >= 0 the resolved bank
  // record (or nullptr for the original's fallback/unresolved); for
  // material < 0 always nullptr (pen/effect — see arenaMatClassFor).
  const ArenaRenderMaterial* materialFor(std::size_t polyIdx) const;

  // The composed dispatch class for a poly: arenaMatClassFor for
  // negative indices; for nonnegative indices kTextured iff the
  // resolved record carries pixels (the original takes its flat-fill
  // 0xff error path otherwise -> kUnresolved).
  ArenaMatClass polyMaterialClass(std::size_t polyIdx) const;
};

// Build the render data for one MTO overlay block (an arena). The
// collision blob must already be parsed by collisionBlobParse (the
// render view aliases the same records). `fileBytes` is the whole
// .MTO file; `sharedBank` is the LEVELnS.MTI directory (bank A) — an
// empty span disables bank A. Returns false on parse failures that
// would make the render data unusable; individual material misses are
// represented in materialOfName as -1 (the original's fallback path).
bool arenaRenderDataBuild(std::span<const std::byte> fileBytes,
                          const MtoBlock& block,
                          const CollisionArena& arena,
                          std::uint32_t nodeCount, std::uint32_t polyCount,
                          std::uint32_t vertCount,
                          std::span<const std::byte> sharedBankFile,
                          ArenaRenderData* out);

// ---------------------------------------------------------------------------
// FUN_00409a6c — BSP submission order. Produces the ordered list of
// poly-table indices the original submits for `camPos`, honoring the
// render-skip bit (poly +0x20 bit4) exactly as FUN_00409860 does. The
// +0x24/+0x28 dynamic-entry flushes are out of scope (they interleave
// the dynamic objects; the static poly order is what a frontend
// consumes). The live order is painter's back-to-front (far-side
// subtree first) — DAT_00499f8c is initialized to 1 and never
// written, OBSERVED. `frontToBack` selects the dead flag==0 variant
// (camera-side subtree first) — never taken in this build; it exists
// only to document the binary's alternate path. Iterative equivalent
// of the original's recursion+tail loop.
// ---------------------------------------------------------------------------
void arenaRenderOrder(const CollisionArena& arena,
                      const float camPos[3], bool frontToBack,
                      std::vector<std::uint32_t>* outOrder);

} // namespace mdk

#endif // MDK_CORE_ARENA_RENDER_H
