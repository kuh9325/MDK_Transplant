// Phase 19B.2A — Mode-5 indexed triangle raster backend (host half
// of the OBSERVED 0ca00 -> 0c860 -> {412970, 415260, ...} chain).
//
// The core (StreamScene) emits authoritative kRibbonTri events:
//   f[0..8] = three VIEW-SPACE verts {x,y,z} (pre-project, pre-clip)
//   aux     = packed per-vert clip flags (f0 | f1<<8 | f2<<16)
//   tag     = the raw pen scalar (always <= -1029 on real ribbons)
// The host owns: sel-0 projection, the 0ca00 clipper, the 0c860
// material dispatch, and the scanline fillers — all writing palette
// INDICES into the 600x360 IndexedFramebuffer. Phase 19B.2B1 added
// the model-submit entries: projectVert (the 46b4f8 vertex fill the
// submitter runs per element vertex) and drawTri (the 0ca00 + 0c860
// contract for verts that arrive already projected and flagged).
#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "core/framebuffer.h"

namespace mdkbridge {

// OBSERVED 0ca00 vertex record — 6 dwords {x,y,z,sx,sy,flags}.
// flags: bit0(0x01) y'>z' top, bit1(0x02) y'<-z' bottom,
//        bit2(0x04) x'>z' right, bit3(0x08) x'<-z' left,
//        bit4(0x10) z'<0.05 near — the FUN_0046b4f8 packing.
struct StreamTriVert {
  float x = 0, y = 0, z = 0;   // view-space
  float sx = 0, sy = 0;        // projected (sel-0 fill)
  std::uint32_t flags = 0;
};

// FUN_0040c860 dispatch census ids — OBSERVED branch arms:
//   pen >= 0            -> material table -> 46daac (unported)
//   pen in [-989,-1]    -> 415260 flat fill, byte = -pen&0xff
//   pen in [-1010,-990] -> 47a770 (unported)
//   pen in [-1023,-1011]-> 415260 flat fill, byte = -pen&0xff
//   pen in [-1027,-1024]-> 412970, lut = base + (-1024-pen)*256
//   pen == -1028        -> 46e940 (unported)
//   pen <= -1029        -> 412970, lut = base + (-1029-pen)*256
enum class StreamTriBranch : int {
  kLut1029 = 0,
  kLut1024,
  kFlat,
  kFx47a770,
  kFx46e940,
  kMaterial,
  kCount
};
const char* streamTriBranchName(StreamTriBranch b);

struct StreamRibbonDiag {
  int commands = 0;      // kRibbonTri events consumed
  int rasterized = 0;    // triangles that reached a filler
  int zeroPixels = 0;    //   filler calls that committed 0 pixels
  int clipped = 0;       // events that entered the 0ca00 clip path
  int clipDropped = 0;   //   clip-path events producing no fan tri
                         //   (incl. residual-flag rejects)
  int unsupported = 0;   // dispatches hitting an unported branch
  int lutMisses = 0;     // LUT branch hit with no palette bound
  std::uint64_t pixels = 0;                 // indexed writes committed
  std::map<int, int> branch;                // per-branch tri census
};

// Owns the mode-5 LUT (384 rows x 256 indices — 6 shade subtables x
// 64 material rows) and the raster chain. bindPalette() rebuilds the
// octree + LUT exactly as the mode-5 init does (FUN_00406b80 octree
// over the composed stream palette, FUN_00406d84 per row).
class StreamRibbonRaster {
public:
  // palette = the scene's composed 768B base palette
  // (StreamScene::palette() — native 0x4ed758). nullptr clears.
  void bindPalette(const std::uint8_t* palette768);
  bool lutReady() const { return lutReady_; }

  // One kRibbonTri event: project -> 0ca00 (reject/clip/fan) ->
  // 0c860 dispatch -> filler, writing into fb in emission order.
  void draw(mdk::IndexedFramebuffer& fb, int pen, const float v9[9],
            std::uint32_t packedFlags, StreamRibbonDiag& diag) const;

  // 19B.2B1 — FUN_0046b4f8 sel-0 vertex fill for the model
  // submitter: v' = in x M (row-major 3x4, f64 intermediates rounded
  // to f32 — the compose6aeb0/point6afe4 convention), the clip-flag
  // pack {bit0 y'>z', bit1 y'<-z', bit2 x'>z', bit3 x'<-z', bit4
  // z'<0.05}, then the sel-0 projected fill. The fill runs even on
  // near-flagged verts; z'==0 leaves sx/sy at 0.
  static void projectVert(const float m[12], const float in[3],
                          StreamTriVert& v);

  // The OBSERVED 0c860 compare chain — the dispatch branch for a raw
  // pen scalar (single source for the submitter's material census).
  static StreamTriBranch branchForPen(int pen);

  // 19B.2B1 — the 0ca00 + 0c860 entry for a tri whose verts arrive
  // already transformed, projected and flagged (the native ca00
  // contract — no re-projection). Trivial-reject on ANDed flags,
  // direct dispatch on or==0, else the five-pass clip + fan.
  void drawTri(mdk::IndexedFramebuffer& fb, int pen,
               const StreamTriVert v[3], StreamRibbonDiag& diag) const;

private:
  // The 0x28-byte native octree node: center rgb + level, the leaf's
  // palette index at +0x04, and 8 child slots (pool indices;
  // -1 = null). The freelist threads through child[0].
  struct OctNode {
    std::uint8_t r = 0, g = 0, b = 0;
    std::uint8_t level = 0;
    std::int32_t index = -1;
    std::int32_t child[8];
    OctNode() { for (auto& c : child) c = -1; }
  };
  int octAlloc();
  void octInsert(const std::uint8_t rgb[3], std::int32_t index);
  std::int32_t octSearch(const std::uint8_t rgb[3]) const;
  void lutRow(std::uint8_t* dst, const std::uint8_t tint[3],
              int shade) const;

  // 0c860 dispatch — verts arrive pre-sorted input; sorts by sy
  // ascending (OBSERVED fcomp order), fans the pen dispatch.
  void dispatch(mdk::IndexedFramebuffer& fb, int pen,
                const StreamTriVert v[3],
                StreamRibbonDiag& diag) const;
  void fillLut(mdk::IndexedFramebuffer& fb, const std::uint8_t* lutRow,
               const StreamTriVert v[3], StreamRibbonDiag& diag) const;
  void fillFlat(mdk::IndexedFramebuffer& fb, std::uint8_t penByte,
                const StreamTriVert v[3], StreamRibbonDiag& diag) const;

  OctNode root_{};                          // level-0 root (in-struct)
  std::array<OctNode, 1352> pool_{};        // the 0xd320-byte arena
  std::int32_t freeHead_ = -1;              // freelist via child[0]
  std::uint8_t pal_[256][4]{};              // the in-struct pal copy
  std::array<std::uint8_t, 6 * 64 * 256> lut_{};
  bool lutReady_ = false;
};

} // namespace mdkbridge
