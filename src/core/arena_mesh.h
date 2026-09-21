// Phase 7 (G1) — rasterizer-replacement arena mesh bundle.
//
// Platform-neutral flattening of ArenaRenderData into the data a
// presentation frontend actually draws: palette-expanded material
// texels in the original's masked address space, the effective
// 256-entry arena palette, and the painter-ordered triangle soup.
// This module performs no rasterization and owns nothing that the
// caller did not already own (file-backed views still alias the
// level buffers — see ArenaRenderData).
//
// The build is split into the camera-independent texture stage
// (arenaMeshTexturesBuild — palette expansion + atlas packing, run
// once per arena load) and the per-camera emission stage
// (arenaMeshTrisEmit — the ordered triangle soup, run whenever the
// BSP submission order is re-evaluated). arenaMeshBuild composes
// both for one-shot use.
//
// ORIGINAL ENGINE OBSERVATIONS (MDK95.EXE BUILD_A — instruction
// level):
//
//   Texel addressing (FUN_0046daac family): the mapper fetches
//   pixels[(v & vMask) | (u & uMask)] where the interpolated u/v
//   arrive pitch-shifted — equivalent to the linear index
//     idx = ((vInt & bucketMask) << shift) | (uInt & uMask)
//   with pitch = uMask+1 = 2^shift and bucketH = bucketMask+1
//   (OBSERVED record fields — see arena_render.h). In shipped data
//   every material width is a power of two, so pitch == width and
//   idx addresses the compact w*h payload row-major. A LEVEL3..8
//   sweep of every textured polygon found ZERO masked indices
//   beyond the payload extent — the bucket tail (idx >= w*h,
//   which would read following file bytes in the original) is
//   never reached by real content (CORROBORATED sweep result).
//
//   Displayed palette composition (OBSERVED call chain):
//     FUN_0040163c  startup — SYS_PAL record (192 bytes = entries
//                   [0,64)) staged at 0x540820, entry 0 forced
//                   black; then FUN_00413b20 uploads all 256.
//     FUN_004346e8  traversal init — copies the DTI s3 tail
//                   (entries [64,256)) over staging entries
//                   [64,256), then FUN_00413b20 re-uploads 256.
//     FUN_004321dc  per-arena (region-B load) — stages region B's
//                   count*3 bytes at staging entry 64 AND writes
//                   the BGRX runtime array [64, 64+count) via
//                   FUN_0046d490(0x40, count, regionB); the dirty
//                   range flushes to the DAC at present.
//   Effective arena palette therefore =
//     [0,64)          SYS_PAL head (entry 0 black)
//     [64,64+count)   arena region-B triplets (count = DTI s3
//                     paletteCount — 112 in LEVEL3, 64 in LEVEL6)
//     [64+count,256)  DTI s3 tail
//
//   Flat classes (FUN_0040c860, OBSERVED): pen polys write palette
//   index (-material)&0xff; unresolved/NULL materials write 0xff.
//   The fx dispatch classes (-1010..-990, -1028, -1027..-1024 and
//   <=-1029) are renderer-internal effects with UNKNOWN semantics —
//   this module preserves them as tagged flat placeholders so a
//   frontend can show deterministic stand-ins without treating
//   them as normal textures.
//
#ifndef MDK_CORE_ARENA_MESH_H
#define MDK_CORE_ARENA_MESH_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/arena_render.h"

namespace mdk {

// ---------------------------------------------------------------------------
// Effective arena palette — see header comment for the proven
// three-source composition. `sysPalHead` is exactly the 192-byte
// SYS_PAL record; `levelPal` exactly the 768-byte DTI s3 RGB table;
// `regionB` the arena's region-B triplets (any multiple of 3, the
// original copies paletteCount*3 of them). Entry 0 is forced black.
// `paletteCount` is the DTI s3 count (the original's region-B copy
// bound); it is clamped to what region B actually supplies and to
// the [64,256) window. Missing SYS_PAL (empty span) leaves the head
// black — the caller decides whether that is acceptable.
// ---------------------------------------------------------------------------
bool arenaPaletteCompose(std::span<const std::uint8_t> sysPalHead,
                         std::span<const std::uint8_t> levelPal,
                         std::span<const std::uint8_t> regionB,
                         std::uint32_t paletteCount,
                         std::uint8_t outRgb[768]);

// ---------------------------------------------------------------------------
// One material's payload expanded to RGBA inside its masked address
// space: rgba is pitch*bucketH*4 bytes, texel (x,y) =
// palette[payload[y*pitch + x]] when that linear index is inside the
// payload, else the entry-0 color (the bucket tail that shipped
// content never samples). pitch = uMask+1, bucketH =
// (vMask>>shift)+1 — both powers of two, so the frontend reproduces
// the original wrap with plain mod().
// ---------------------------------------------------------------------------
struct ArenaMeshTexture {
  std::string name;
  std::uint32_t pitch = 0;    // addressable width  (px) = uMask+1
  std::uint32_t bucketH = 0;  // addressable height (px)
  std::uint32_t atlasX = 0;   // shelf-packed position in the atlas
  std::uint32_t atlasY = 0;
  std::vector<std::uint8_t> rgba;  // pitch*bucketH*4, palette-expanded
};

// Flat-color slot space shared by pen/unresolved/effect polys. A
// tri's flatSlot indexes a one-row lookup strip the frontend stores
// next to the atlas: slots [0,256) are palette indices; slots
// [256,260) are the documented placeholder colors for the effect
// dispatch classes (frontend-chosen, NOT original colors).
inline constexpr std::uint32_t kArenaLutPaletteSlots = 256;
inline constexpr std::uint32_t kArenaLutFx770 = 256;
inline constexpr std::uint32_t kArenaLutFxE94 = 257;
inline constexpr std::uint32_t kArenaLutFx12970 = 258;
inline constexpr std::uint32_t kArenaLutSlots = 260;

// Camera-independent texture stage. `textures` holds one expanded
// image per resolved material that a poly actually references —
// texOfSlot maps materialOfName slots to it (-1 = unreferenced or
// undecodable). atlasW/atlasH describe the shelf-packed texture
// atlas; the flat LUT strip sits at (lutX, lutY), one row tall.
struct ArenaMeshTextures {
  std::vector<ArenaMeshTexture> textures;
  std::vector<std::int32_t> texOfSlot;   // per resolved slot
  std::uint32_t atlasW = 0, atlasH = 0;
  std::uint32_t lutX = 0, lutY = 0;
};

bool arenaMeshTexturesBuild(const ArenaRenderData& rd,
                            std::span<const std::uint8_t> palette768,
                            ArenaMeshTextures* out);

struct ArenaMeshTri {
  std::uint32_t poly = 0;      // source poly-table index
  float pos[3][3] = {};        // MDK-space positions, poly order
  float uv[3][2] = {};         // texel-space UVs (textured only)
  std::int32_t tex = -1;       // >= 0: textures[] index; -1: flat
  std::uint32_t flatSlot = 0;  // flat classes: LUT slot (above)
  ArenaMatClass cls = ArenaMatClass::kUnresolved;
  std::uint8_t flags = 0;      // poly +0x20 (skip already applied)
  std::uint8_t aux22 = 0;      // poly +0x22 (edge-overlay flags)
};

// Per-camera emission: one tri per `order` entry, verbatim sequence
// (index order IS the painter's submission contract — the caller
// must pass the arenaRenderOrder(..., false) result). `texs` carries
// the slot->texture map from arenaMeshTexturesBuild.
bool arenaMeshTrisEmit(const ArenaRenderData& rd,
                       const ArenaMeshTextures& texs,
                       std::span<const std::uint32_t> order,
                       std::vector<ArenaMeshTri>* outTris);

// ---------------------------------------------------------------------------
// One-shot bundle for tests and simple consumers.
// ---------------------------------------------------------------------------
struct ArenaMeshBundle {
  ArenaMeshTextures texs;
  std::vector<ArenaMeshTri> tris;          // painter's back-to-front
  std::array<std::uint8_t, 768> palette{}; // composed

  // Diagnostics (the mdk-inspect digests, mirrored):
  std::uint64_t geomDigest = 0;   // FNV-1a over verts + poly records
  std::uint64_t orderDigest = 0;  // FNV-1a over the submitted order
  std::uint32_t submitted = 0;    // tris.size()
  std::uint32_t clsCount[6] = {}; // textured/unresolved/pen/fx* —
                                  // census over ALL polys (incl. ones
                                  // outside the submitted set), same
                                  // convention as mdk-inspect
};

bool arenaMeshBuild(const ArenaRenderData& rd,
                    std::span<const std::uint32_t> order,
                    std::span<const std::uint8_t> palette768,
                    ArenaMeshBundle* out);

// FNV-1a over the submitted poly indices — the same fold
// mdk-inspect --arena-render prints, for cross-checking.
std::uint64_t arenaOrderDigest(std::span<const std::uint32_t> order);

}  // namespace mdk

#endif  // MDK_CORE_ARENA_MESH_H
