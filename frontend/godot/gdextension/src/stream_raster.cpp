// See stream_raster.h for the phase framing. Everything below is a
// host-side reproduction of the OBSERVED BUILD_A chain
//   FUN_0040ca00 (clip/fan) -> FUN_0040c860 (material dispatch) ->
//   FUN_00412970 (LUT remap filler) / FUN_00415260 (flat filler)
// with the mode-5 LUT build FUN_00406b80/FUN_00406d84 — all writes
// are palette indices into the 600x360 indexed surface.
#include "stream_raster.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace mdkbridge {
namespace {

// ------------------------------------------------------------------
// Magic-bias rounding — OBSERVED: the fillers fld the f32 constant
// 6755399441055744.0 (1.5 * 2^52) from 0x49a740/0x49a780, fadd each
// projected coordinate at f87 precision, fstp an f64 and read its
// low dword — i.e. round-to-nearest-even extracted as int32.
std::int32_t magicRound(float v) {
  const double d = static_cast<double>(v) + 6755399441055744.0;
  std::uint32_t lo;
  std::memcpy(&lo, &d, sizeof lo);  // little-endian low dword
  return static_cast<std::int32_t>(lo);
}

// FUN_0046b5f0 -> selector-0 fill (FUN_0046ad20), OBSERVED:
//   sx = f32(((x+z)/z)*299.95 + 0.05)
//   sy = f32(((y+z)/z)*180.40 + 0.05)
// z == 0 -> sx = sy = 0. The fill runs even on near-flagged verts;
// z != 0 is the only gate. 64-bit intermediates match the port's
// project6b4f8 convention (x87 f80 intermediates narrowed to f32).
void project(StreamTriVert& v) {
  if (v.z != 0.0f) {
    const double tx = (static_cast<double>(v.x) + v.z) / v.z;
    const double ty = (static_cast<double>(v.y) + v.z) / v.z;
    v.sx = static_cast<float>(tx * 299.95 + 0.05);
    v.sy = static_cast<float>(ty * 180.40 + 0.05);
  } else {
    v.sx = v.sy = 0.0f;
  }
}

// ------------------------------------------------------------------
// FUN_0040ca00 — ping-pong Sutherland-Hodgman clipper (OBSERVED).
// Passes run in fixed order, each gated on the CURRENT or-accum:
//   near 0x10 (z < 0.05), left 0x08 (x < -z), right 0x04 (x > z),
//   top 0x01 (y > z), bottom 0x02 (y < -z).
// Inside verts copy verbatim (flags carried); straddling edges emit
// one lerped vert — on-plane coord forced, flags reclassified (or
// forced 0 for the y planes) and reprojected when flag == 0.
// Post-pass gates: count < 3 rejects; the emit requires or == 0.

// Reclass for the near pass: {x<-z:8, x>z:4, y>z:1, y<-z:2, else
// 0 + reproject} — full recompute against the forced z=0.05 vert.
std::uint32_t reclassFull(StreamTriVert& v) {
  if (v.x < -v.z) return 8;
  if (v.x > v.z) return 4;
  if (v.y > v.z) return 1;
  if (v.y < -v.z) return 2;
  v.flags = 0;
  project(v);
  return 0;
}

// Reclass for the x-plane crossings (z is forced onto +-x): only
// the y bits are recomputed — {y>z:1, y<-z:2, else 0 + reproject}.
std::uint32_t reclassY(StreamTriVert& v) {
  if (v.y > v.z) return 1;
  if (v.y < -v.z) return 2;
  v.flags = 0;
  project(v);
  return 0;
}

// Edge crossing — a -> b, plane `bit`. The lerped vert lands exactly
// on the plane via the forced coordinate; the UV bank lerps with the
// same `t` (OBSERVED — the clipper's second parallel array carries
// the material uv pair per vert, f32-stored like the coords).
StreamTriVert clipCrossing(const StreamTriVert& a, const StreamTriVert& b,
                           std::uint32_t bit) {
  StreamTriVert n;
  double t;
  if (bit == 0x10) {
    // z = 0.05: t = (0.05 - a.z)/(b.z - a.z); z forced to 0.05.
    t = (0.05 - static_cast<double>(a.z)) /
        (static_cast<double>(b.z) - a.z);
    n.x = static_cast<float>(a.x + (static_cast<double>(b.x) - a.x) * t);
    n.y = static_cast<float>(a.y + (static_cast<double>(b.y) - a.y) * t);
    n.z = 0.05f;
    n.flags = reclassFull(n);
  } else if (bit == 8) {
    // x = -z: t = (-a.z - a.x)/(dz + dx); z forced to -x.
    t = (-static_cast<double>(a.z) - a.x) /
        ((static_cast<double>(b.z) - a.z) +
         (static_cast<double>(b.x) - a.x));
    n.x = static_cast<float>(a.x + (static_cast<double>(b.x) - a.x) * t);
    n.y = static_cast<float>(a.y + (static_cast<double>(b.y) - a.y) * t);
    n.z = -n.x;
    n.flags = reclassY(n);
  } else if (bit == 4) {
    // x = +z: t = (a.z - a.x)/(dx - dz); z forced to x.
    t = (static_cast<double>(a.z) - a.x) /
        ((static_cast<double>(b.x) - a.x) -
         (static_cast<double>(b.z) - a.z));
    n.x = static_cast<float>(a.x + (static_cast<double>(b.x) - a.x) * t);
    n.y = static_cast<float>(a.y + (static_cast<double>(b.y) - a.y) * t);
    n.z = n.x;
    n.flags = reclassY(n);
  } else if (bit == 1) {
    // y = +z: t = (a.z - a.y)/(dy - dz); z forced to y. flag = 0 —
    // a vert on y=z with z > 0 cannot violate the remaining planes.
    t = (static_cast<double>(a.z) - a.y) /
        ((static_cast<double>(b.y) - a.y) -
         (static_cast<double>(b.z) - a.z));
    n.x = static_cast<float>(a.x + (static_cast<double>(b.x) - a.x) * t);
    n.y = static_cast<float>(a.y + (static_cast<double>(b.y) - a.y) * t);
    n.z = n.y;
    n.flags = 0;
    project(n);
  } else {  // bit == 2
    // y = -z: t = (-a.z - a.y)/(dy + dz); z forced to -y; flag = 0.
    t = (-static_cast<double>(a.z) - a.y) /
        ((static_cast<double>(b.y) - a.y) +
         (static_cast<double>(b.z) - a.z));
    n.x = static_cast<float>(a.x + (static_cast<double>(b.x) - a.x) * t);
    n.y = static_cast<float>(a.y + (static_cast<double>(b.y) - a.y) * t);
    n.z = -n.y;
    n.flags = 0;
    project(n);
  }
  // The parallel 8-byte UV bank lerps with the same t.
  n.u = static_cast<float>(a.u + (static_cast<double>(b.u) - a.u) * t);
  n.v = static_cast<float>(a.v + (static_cast<double>(b.v) - a.v) * t);
  return n;
}

// One S-H pass over `in` (n verts) into `out`; returns the output
// count and accumulates emitted flags into orOut. OBSERVED order
// per edge index i: copy in[i] when inside, then test (in[i],
// in[(i+1)%n]) — the wrap edge is the same fold at i = n-1.
// The native banks hold 10 verts (the 0xf0-byte record arrays);
// writes past that are dropped — a convex clip of a triangle by five
// planes can not exceed 8 verts, so the cap is unreachable on data.
constexpr int kMaxClipV = 10;

int clipPass(const StreamTriVert* in, int n, StreamTriVert* out,
             std::uint32_t bit, std::uint32_t& orOut) {
  int m = 0;
  for (int i = 0; i < n; ++i) {
    const StreamTriVert& a = in[i];
    if (!(a.flags & bit)) {
      orOut |= a.flags;
      if (m < kMaxClipV) out[m++] = a;
    }
    const StreamTriVert& b = in[(i + 1) % n];
    if ((a.flags ^ b.flags) & bit && m < kMaxClipV) {
      out[m] = clipCrossing(a, b, bit);
      orOut |= out[m].flags;
      ++m;
    }
  }
  return m;
}

} // namespace

// ------------------------------------------------------------------
// Octree — FUN_00406b18/6b80/6bfc/6e24/6d38 (all OBSERVED asm).
// Node: {rgb[3], level, index, child[8]}; pool of 1352 nodes
// threaded by child[0]; root center (0x80,0x80,0x80), level 0.

int StreamRibbonRaster::octAlloc() {
  if (freeHead_ < 0) return -1;          // "No more colour nodes"
  const int i = freeHead_;
  freeHead_ = pool_[i].child[0];
  pool_[i] = OctNode{};                  // memset 0x28 + index = -1
  return i;
}

// FUN_00406bfc — descend to a level-7 leaf, allocating on the way;
// leaf rgb + index overwritten (last insert wins).
void StreamRibbonRaster::octInsert(const std::uint8_t rgb[3],
                                   std::int32_t index) {
  OctNode* node = &root_;
  while (node->level != 7) {
    std::uint32_t oct = 0;
    if (rgb[0] >= node->r) oct |= 1;
    if (rgb[1] >= node->g) oct |= 2;
    if (rgb[2] >= node->b) oct |= 4;
    std::int32_t ci = node->child[oct];
    if (ci < 0) {
      ci = octAlloc();
      if (ci < 0) return;               // pool exhausted — insert dropped
      node->child[oct] = ci;
      OctNode& child = pool_[ci];
      child.level = node->level + 1;
      const int step = 1 << (7 - child.level);
      child.r = static_cast<std::uint8_t>(node->r + ((oct & 1) ? step : -step));
      child.g = static_cast<std::uint8_t>(node->g + ((oct & 2) ? step : -step));
      child.b = static_cast<std::uint8_t>(node->b + ((oct & 4) ? step : -step));
    }
    node = &pool_[ci];
  }
  node->r = rgb[0];
  node->g = rgb[1];
  node->b = rgb[2];
  node->index = index;
}

// FUN_00406e24 — descend by octant; a missing child probes the
// siblings in ord[] ^ octant order (ord = 01 02 04 03 05 06 07 —
// the 8th slot is never reached: oct itself is already known null).
// Total miss -> "Illegal tree" log path -> -1 (port: byte 0xff).
std::int32_t StreamRibbonRaster::octSearch(const std::uint8_t rgb[3]) const {
  static constexpr std::uint8_t kOrd[7] = {1, 2, 4, 3, 5, 6, 7};
  const OctNode* node = &root_;
  for (;;) {
    if (node->level == 7) return node->index;
    std::uint32_t oct = 0;
    if (rgb[0] >= node->r) oct |= 1;
    if (rgb[1] >= node->g) oct |= 2;
    if (rgb[2] >= node->b) oct |= 4;
    std::int32_t ci = node->child[oct];
    if (ci >= 0) {
      node = &pool_[ci];
      continue;
    }
    bool found = false;
    for (int d = 0; d < 7; ++d) {
      const std::int32_t cand = node->child[kOrd[d] ^ oct];
      if (cand >= 0) {
        node = &pool_[cand];
        found = true;
        break;
      }
    }
    if (!found) return -1;
  }
}

// FUN_00406d84 — one 256-entry row: q[c] = (pal[c]*(256-shade) +
// tint[c]*shade) >> 8, then nearest palette index via the octree.
void StreamRibbonRaster::lutRow(std::uint8_t* dst,
                                const std::uint8_t tint[3],
                                int shade) const {
  const int inv = 256 - shade;
  for (int p = 0; p < 256; ++p) {
    std::uint8_t q[3];
    for (int c = 0; c < 3; ++c)
      q[c] = static_cast<std::uint8_t>(
          (pal_[p][c] * inv + tint[c] * shade) >> 8);
    dst[p] = static_cast<std::uint8_t>(octSearch(q));
  }
}

void StreamRibbonRaster::bindPalette(const std::uint8_t* palette768) {
  lutReady_ = false;
  lut_.fill(0);
  if (!palette768) return;

  // 6b18 — repack the freelist over the fixed pool.
  for (int i = 0; i < static_cast<int>(pool_.size()); ++i) {
    pool_[i] = OctNode{};
    pool_[i].child[0] = i + 1 < static_cast<int>(pool_.size()) ? i + 1 : -1;
  }
  freeHead_ = 0;
  // 6b80 — root node: memset, index -1, level 0, center 0x80^3.
  root_ = OctNode{};
  for (auto& c : root_.child) c = -1;
  root_.r = root_.g = root_.b = 0x80;
  // The palette copy — the native expands to 4B records; bytes
  // 0..2 carry the channels and byte 3 is unused by 6d84.
  for (int i = 0; i < 256; ++i) {
    pal_[i][0] = palette768[i * 3 + 0];
    pal_[i][1] = palette768[i * 3 + 1];
    pal_[i][2] = palette768[i * 3 + 2];
    pal_[i][3] = 0;
  }
  for (int i = 0; i < 256; ++i) octInsert(pal_[i], i);

  // The material ramp table at 0x49b57c — OBSERVED 4B records
  // {r,g,b,count}; consecutive stops lerp over `next.count` rows,
  // terminated by a zero-rgb stop (or the 64-row cap).
  static constexpr std::uint8_t kMatRamp[][4] = {
      {90, 206, 222, 0}, {33, 123, 140, 8}, {8, 49, 123, 8},
      {132, 165, 198, 8}, {140, 123, 132, 8}, {231, 198, 214, 8},
      {132, 165, 198, 8}, {8, 49, 123, 8}, {90, 206, 222, 8},
      {0, 0, 0, 0}};
  static constexpr int kShades[6] = {0x5a, 0x55, 0x50, 0x3c, 0x28, 0x0f};

  int row = 0;                            // [ebp-0x24] 0..63
  for (int seg = 0; row < 64; ++seg) {
    const std::uint8_t* cur = kMatRamp[seg];
    const std::uint8_t* next = kMatRamp[seg + 1];
    if (next[0] == 0 && next[1] == 0 && next[2] == 0) break;
    const int count = next[3];
    for (int k = 0; k < count && row < 64; ++k, ++row) {
      std::uint8_t tint[3];
      for (int c = 0; c < 3; ++c)
        tint[c] = static_cast<std::uint8_t>(
            (cur[c] * (count - k) + next[c] * k) / count);
      for (int s = 0; s < 6; ++s)
        lutRow(&lut_[(s * 64 + row) * 256], tint, kShades[s]);
    }
  }
  lutReady_ = true;
}

// ------------------------------------------------------------------
// FUN_00412970 — split-at-mid-Y 16.16 LUT-remap filler (OBSERVED):
//   iX/iY = magicRound(sx/sy); verts arrive sorted top/mid/bot by sy.
//   long edge top->bot walks the FULL height; the top half picks its
//   side by a SLOPE compare (longSlope vs shortSlope, once), the
//   bottom half by a POSITION compare per row. Exclusive right edge;
//   each written pixel is read back and remapped through lutRow.
//   No x clipping — the clipper keeps spans inside the surface.

void StreamRibbonRaster::fillLut(mdk::IndexedFramebuffer& fb,
                                 const std::uint8_t* lutRow,
                                 const StreamTriVert v[3],
                                 StreamRibbonDiag& diag) const {
  const std::uint64_t before = diag.pixels;
  const std::int32_t iTopX = magicRound(v[0].sx);
  const std::int32_t iTopY = magicRound(v[0].sy);
  const std::int32_t iMidX = magicRound(v[1].sx);
  const std::int32_t iMidY = magicRound(v[1].sy);
  const std::int32_t iBotX = magicRound(v[2].sx);
  const std::int32_t iBotY = magicRound(v[2].sy);
  const int totalH = iBotY - iTopY;
  if (totalH <= 0) { ++diag.zeroPixels; return; }

  // long edge top->bot over the whole height, bias +0.5 (bh = 0x80)
  const std::int32_t longSlope =
      static_cast<std::int32_t>(
          static_cast<std::uint32_t>(iBotX - iTopX) << 16) / totalH;
  std::int32_t longX =
      static_cast<std::int32_t>(
          (static_cast<std::uint32_t>(iTopX) << 16) | 0x8000u);
  const int midH = iMidY - iTopY;

  auto span = [&](int y, std::int32_t lx, std::int32_t rx) {
    // OBSERVED: logical >>16 on the 16.16 accumulators, exclusive
    // right edge; bounds are the surface (native has no x clamp —
    // the clipper guarantees [0,600)/[0,360) for real data).
    const int l = static_cast<int>(
        static_cast<std::uint32_t>(lx) >> 16);
    const int r = static_cast<int>(
        static_cast<std::uint32_t>(rx) >> 16);
    if (y < 0 || y >= fb.height() || l >= r) return;
    const int a = l < 0 ? 0 : l;
    const int b = r > fb.width() ? fb.width() : r;
    std::uint8_t* row = fb.pixels() + static_cast<std::size_t>(y) * fb.stride();
    for (int x = a; x < b; ++x) {
      row[x] = lutRow[row[x]];
      ++diag.pixels;
    }
  };

  // The native loop trusts the clipper to keep rows on-surface.
  // Clamp the row ranges so a garbage vertex can't spin the row
  // loop; hidden rows still advance the accumulators exactly as
  // the unclamped walk would (the 16.16 adds are linear).
  auto clampRows = [&](int top, int count, int& i0, int& i1) {
    const std::int64_t lo = -(std::int64_t)top;
    i0 = top < 0 ? (int)std::min<std::int64_t>(lo, count) : 0;
    const std::int64_t lim = (std::int64_t)fb.height() - top;
    i1 = (int)std::min<std::int64_t>(count, lim);
    if (i1 < i0) i1 = i0;
  };

  if (midH > 0) {
    // top half — rows [iTopY, iMidY); side fixed by slope compare
    const std::int32_t shortSlope =
        static_cast<std::int32_t>(
            static_cast<std::uint32_t>(iMidX - iTopX) << 16) / midH;
    std::int32_t shortX = longX;   // both start at iTopX<<16 + 0x8000
    const bool longLeft = longSlope < shortSlope;
    int i0, i1; clampRows(iTopY, midH, i0, i1);
    longX += (std::int32_t)((std::int64_t)i0 * longSlope);
    shortX += (std::int32_t)((std::int64_t)i0 * shortSlope);
    for (int i = i0; i < i1; ++i) {
      const int y = iTopY + i;
      if (longLeft) span(y, longX, shortX);
      else span(y, shortX, longX);
      longX += longSlope;
      shortX += shortSlope;
    }
    // rows below the surface still advance the long edge — it is
    // shared by the bottom half.
    longX += (std::int32_t)((std::int64_t)(midH - i1) * longSlope);
  }
  const int botH = iBotY - iMidY;
  if (botH > 0) {
    const std::int32_t shortSlope =
        static_cast<std::int32_t>(
            static_cast<std::uint32_t>(iBotX - iMidX) << 16) / botH;
    std::int32_t shortX = static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(iMidX) << 16) | 0x8000u);
    int i0, i1; clampRows(iMidY, botH, i0, i1);
    longX += (std::int32_t)((std::int64_t)i0 * longSlope);
    shortX += (std::int32_t)((std::int64_t)i0 * shortSlope);
    for (int i = i0; i < i1; ++i) {
      const int y = iMidY + i;
      // bottom half — per-row POSITION compare
      if (longX < shortX) span(y, longX, shortX);
      else span(y, shortX, longX);
      longX += longSlope;
      shortX += shortSlope;
    }
  }
  if (diag.pixels == before) ++diag.zeroPixels;
}

// FUN_00415260 — the same skeleton with an INCLUSIVE right edge
// (inc ecx -> len = r-l+1) and a constant pen byte; bias constant
// 0x49a780 is the identical 6755399441055744.0.
void StreamRibbonRaster::fillFlat(mdk::IndexedFramebuffer& fb,
                                  std::uint8_t penByte,
                                  const StreamTriVert v[3],
                                  StreamRibbonDiag& diag) const {
  const std::uint64_t before = diag.pixels;
  const std::int32_t iTopX = magicRound(v[0].sx);
  const std::int32_t iTopY = magicRound(v[0].sy);
  const std::int32_t iMidX = magicRound(v[1].sx);
  const std::int32_t iMidY = magicRound(v[1].sy);
  const std::int32_t iBotX = magicRound(v[2].sx);
  const std::int32_t iBotY = magicRound(v[2].sy);
  const int totalH = iBotY - iTopY;
  if (totalH <= 0) { ++diag.zeroPixels; return; }

  const std::int32_t longSlope =
      static_cast<std::int32_t>(
          static_cast<std::uint32_t>(iBotX - iTopX) << 16) / totalH;
  std::int32_t longX =
      static_cast<std::int32_t>(
          (static_cast<std::uint32_t>(iTopX) << 16) | 0x8000u);
  const int midH = iMidY - iTopY;

  auto span = [&](int y, std::int32_t lx, std::int32_t rx) {
    const int l = static_cast<int>(
        static_cast<std::uint32_t>(lx) >> 16);
    const int r = static_cast<int>(
        static_cast<std::uint32_t>(rx) >> 16);
    if (y < 0 || y >= fb.height() || l > r) return;
    const int a = l < 0 ? 0 : l;
    const int b = r >= fb.width() ? fb.width() - 1 : r;
    std::uint8_t* row = fb.pixels() + static_cast<std::size_t>(y) * fb.stride();
    for (int x = a; x <= b; ++x) {
      row[x] = penByte;
      ++diag.pixels;
    }
  };

  auto clampRows = [&](int top, int count, int& i0, int& i1) {
    const std::int64_t lo = -(std::int64_t)top;
    i0 = top < 0 ? (int)std::min<std::int64_t>(lo, count) : 0;
    const std::int64_t lim = (std::int64_t)fb.height() - top;
    i1 = (int)std::min<std::int64_t>(count, lim);
    if (i1 < i0) i1 = i0;
  };

  if (midH > 0) {
    const std::int32_t shortSlope =
        static_cast<std::int32_t>(
            static_cast<std::uint32_t>(iMidX - iTopX) << 16) / midH;
    std::int32_t shortX = longX;
    const bool longLeft = longSlope < shortSlope;
    int i0, i1; clampRows(iTopY, midH, i0, i1);
    longX += (std::int32_t)((std::int64_t)i0 * longSlope);
    shortX += (std::int32_t)((std::int64_t)i0 * shortSlope);
    for (int i = i0; i < i1; ++i) {
      const int y = iTopY + i;
      if (longLeft) span(y, longX, shortX);
      else span(y, shortX, longX);
      longX += longSlope;
      shortX += shortSlope;
    }
    longX += (std::int32_t)((std::int64_t)(midH - i1) * longSlope);
  }
  const int botH = iBotY - iMidY;
  if (botH > 0) {
    const std::int32_t shortSlope =
        static_cast<std::int32_t>(
            static_cast<std::uint32_t>(iBotX - iMidX) << 16) / botH;
    std::int32_t shortX = static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(iMidX) << 16) | 0x8000u);
    int i0, i1; clampRows(iMidY, botH, i0, i1);
    longX += (std::int32_t)((std::int64_t)i0 * longSlope);
    shortX += (std::int32_t)((std::int64_t)i0 * shortSlope);
    for (int i = i0; i < i1; ++i) {
      const int y = iMidY + i;
      if (longX < shortX) span(y, longX, shortX);
      else span(y, shortX, longX);
      longX += longSlope;
      shortX += shortSlope;
    }
  }
  if (diag.pixels == before) ++diag.zeroPixels;
}

namespace {

// ------------------------------------------------------------------
// 19B.2C — FUN_0046daac / FUN_0046e52c textured triangle mapping.
// OBSERVED at instruction level (tex_decomp.txt, tex_drawers.txt):
//
//   46daac (perspective): rounds endpoint sy to int, computes the
//   2D cross term (dxTB*dyTM - dxTM*totalH — ebf4 is the NEGATED TM
//   dx), early-outs on height<=0 / cross==0. A near-plane clamp
//   replaces any vert whose z < maxZ/64 with a 6-dword scratch copy
//   holding z=clamp — the UV bank is NOT reseated (the originals'
//   uvs keep flowing). On the no-clamp arm (maxZ <= minZ*64) an
//   affine gate tail-calls 46e52c when
//       bit_cast<float>(rec +0x1c) < minZ
//       && (maxZ - minZ) * bit_cast<float>(rec +0x20) < minZ
//   (the 0x540b58/0x540b64 zoom ratio is 1.0f in mode 5 — the port
//   holds the zoom constant fixed at the unzoomed value). Otherwise
//   the projective walk: per-vert q=1/z, uq=u/z, vq=v/z (f32), the
//   screen-space gradients with the cross term doubled by its own
//   sign (ebec += (ebec<1 ? -2*ebfc : +2*ebfc)), and a row walk that
//   emits the left-edge {q,uq,vq} into the span drawers.
//
//   Span drawers A0-A3 (0x47xxxx table, selected by flags&3):
//   the 32-px block pipeline — head (32-(px&31) px), full 32-blocks,
//   tail — with per-region boundary evals dividing the projected
//   delta by the region length through the 0x54dc30 reciprocal table
//   (R[0]=1, R[k]=1/k). Fixed point: V in 20.12 (1.5*2^40 bias), U in
//   16.16 (1.5*2^36); the seed u-int uses a LOGICAL shift while all
//   step int-parts use signed >>. OBSERVED cross-wire quirks: the
//   entry eval seeds the carry slots swapped (st1=V0, st2=U0 vs the
//   boundary evals' st1=U, st2=V — the first eval per row subtracts
//   across channels), and the U-channel delta drives the 20.12/row
//   step while the V-channel delta drives the 16.16/col step, even
//   though the seed puts V0 in the row acc and U0 in the col acc.
//   The mid-block eval adds the x32-prescaled gradients ONCE.
//   Each draw region primes (accs step,
//   texel fetch, addr advance) then writes delayed texels; the final
//   write-iter of a block drops the fetch+advance, and the tail exit
//   writes the last fetched texel TWICE — count+1 pixels per span.
//   Keyed variants (flags&1) skip the write when the texel index is
//   0; wrap variants (flags&2) re-mask the running texel offset by
//   uMask|vMask per pixel.
//
//   46e52c (affine): the tail-called setup — absolute cross term
//   (ebec += 2*ebfc; the original adds |ebec| after the 46daac
//   adjustment — port: 2*ebfc + |raw cross|), 16.16 UV fixed point,
//   per-pixel and per-row step pairs, a running texel anchor seeded
//   at the TOP VERT's masked texel that walks the LONG edge by the
//   row deltas — the left-edge texel is never re-evaluated, so the
//   long edge carries the anchor in both arms. Drawer A8-A11 covers
//   the whole half: arm A draws left-to-right when longX <= shortX,
//   arm B anchors on the RIGHT edge and writes right-to-left while
//   the texel walk still runs forward (the mirrored sampling quirk),
//   the final texel duplicates one pixel PAST the span's far side.
//   The row walk advances the anchor by the row step pair +
//   frac-acc carries and the edge accs by the 16.16 slopes; the
//   per-row pixel accs reload from the row accs.
//
// The fixed-point extractions use the same magic-bias trick as
// magicRound at different widths: +1.5*2^36 (16.16) and +1.5*2^40
// (20.12), f64-store low dword = round-to-nearest-even.
std::int32_t fix1616(double v) {
  const double d = v + 103079215104.0;    // 1.5 * 2^36
  std::uint32_t lo; std::memcpy(&lo, &d, sizeof lo);
  return static_cast<std::int32_t>(lo);
}
std::int32_t fix2012(double v) {
  const double d = v + 1649267441664.0;   // 1.5 * 2^40
  std::uint32_t lo; std::memcpy(&lo, &d, sizeof lo);
  return static_cast<std::int32_t>(lo);
}

// 0x54dc30 — OBSERVED: R[0] = 1.0f, R[k] = 1/k for k = 1..0x27f.
// The perspective evals index it by region length (only 1..32 occur
// on real spans). A negative index reads below the table in the
// original — the 12 drawer-address dwords sitting there are
// denormal-tiny floats, and the extracted step bits come out 0; the
// port folds the whole negative arm to 0.
float recipOf(int k) {
  static const float* t = [] {
    static float r[0x280];
    r[0] = 1.0f;
    for (int i = 1; i < 0x280; ++i) r[i] = 1.0f / static_cast<float>(i);
    return r;
  }();
  if (k < 0) return 0.0f;
  if (k >= 0x280) return t[0x27f];      // unreachable on real spans
  return t[k];
}

// The material-view fields the drawers consume (the 0x34-byte record
// fields at +0x00/+0x10/+0x14/+0x24, +0x0c flag bits 0-1 select the
// installed drawer).
struct TexMat {
  const std::uint8_t* tex = nullptr;      // record +0x24 (span base)
  std::size_t texSize = 0;                //   pixels span size
  int ush = 0;                            // record +0x00 (<12)
  std::int32_t pitch = 0;                 // 1<<ush (record math)
  std::uint32_t uMask = 0;                // record +0x10
  std::uint32_t vMask = 0;                // record +0x14
  bool keyed = false;                     // flags & 1
  bool wrap = false;                      // flags & 2
};

// A write into the indexed surface with the surface-extent bound —
// the only hardening the port adds: in-surface out-of-span writes
// (the +1 overrun, negative-x starts) land exactly where the
// original put them (flat 600-stride space); writes that would leave
// the surface are dropped. The destination cursor still advances
// either way, keeping position bookkeeping exact.
struct SpanWriter {
  std::uint8_t* base;
  std::uint8_t* end;
  std::uint8_t* d;
  StreamRibbonDiag* diag;
  std::int32_t dir;              // +1 forward, -1 (affine arm B)
  inline void put(std::uint8_t px, bool keyOk) {
    if (!keyOk) ++diag->texTransparent;
    else if (d >= base && d < end) { *d = px; ++diag->pixels; }
    d += dir;
  }
};

// Bounded texel fetch: offsets escaping the pixel span are folded
// back modulo the span (the original reads adjacent heap there —
// UB — OBSERVED reachable only via out-of-range uvs on real data).
inline std::uint8_t texAt(const TexMat& m, std::int32_t off) {
  std::size_t i = static_cast<std::size_t>(
      static_cast<std::uint32_t>(off));
  if (i >= m.texSize) i %= m.texSize;
  return m.tex[i];
}

// ------------------------------------------------------------------
// The A0-family perspective span machine — one call per scanline.
// `gr` carries the setup products; the FPU stack invariant
//   [r = 1/q, U_prev, V_prev, vq, uq, q]
//   is threaded through the boundary evals.
struct PerspGrads {
  float dqdx, duqdx, dvqdx;       // ec10/14/18 — per-pixel
  float dq32, duq32, dvq32;       // ec80/84/88 — x32 preset
};

template<bool Keyed, bool Wrap>
void perspSpan(std::uint8_t* dst, int leftpx, int count,
               float q0, float uq0, float vq0,
               const PerspGrads& gr, const TexMat& m,
               SpanWriter& w) {
  double q = q0, uq = uq0, vq = vq0;
  double r = 1.0 / q;
  // The invariant's st1/st2 carry slots — subtracted from the next
  // eval's U/V products to form the step deltas. OBSERVED quirk:
  // the entry eval leaves st1 = V0, st2 = U0 while boundary evals
  // leave st1 = U, st2 = V — so the FIRST eval per row subtracts
  // across channels (U1-V0, V1-U0) before the slots normalize.
  // OBSERVED cross-wire: the U-channel delta feeds the 20.12/<<ush
  // (row) step and the V-channel delta the 16.16/low (col) step,
  // while the SEED puts V0 in the row acc and U0 in the col acc.
  double subU = 0.0, subV = 0.0;
  std::uint32_t uAcc = 0, vAcc = 0;
  std::int32_t off = 0;           // esi — texel offset into m.tex
  std::uint8_t dl = 0;            // the delayed-write texel
  std::uint32_t vFStep = 0, uFStep = 0;
  std::int32_t pxNorm = 0, pxCarry = 0;
  const int ush = m.ush;
  const std::int32_t pitch = m.pitch;
  const std::uint32_t mask = m.uMask | m.vMask;   // wrap fetch mask

  // accs step + fetch + advance — the OBSERVED per-iter order:
  //   vAcc += vFStep (CFv); [write]; uAcc += uFStep (CFu);
  //   dl = tex[off]; off += (CFv ? carry : normal) + CFu;
  // For wrap the mask applies between the write and the fetch on the
  // running offset — modeled by masking inside the fetch.
  auto stepV = [&]() -> bool {
    const std::uint32_t o = vAcc;
    vAcc += vFStep;
    return vAcc < o;
  };
  auto stepU = [&]() -> bool {
    const std::uint32_t o = uAcc;
    uAcc += uFStep;
    return uAcc < o;
  };
  auto fetchNow = [&]() -> std::uint8_t {
    const std::int32_t o =
        Wrap ? (std::int32_t)((std::uint32_t)off & mask) : off;
    if (static_cast<std::uint32_t>(o) >= m.texSize)
      ++w.diag->texLookupMiss;
    return texAt(m, o);
  };
  auto advance = [&](bool cfv, bool cfu) {
    off += (cfv ? pxCarry : pxNorm) + (cfu ? 1 : 0);
  };
  // prime iter — no write (each region refetches the pending pos).
  auto prime = [&]() {
    const bool cfv = stepV();
    const bool cfu = stepU();
    dl = fetchNow();
    advance(cfv, cfu);
  };
  // write iter — vstep, write dl, ustep, fetch, advance.
  auto iter = [&]() {
    const bool cfv = stepV();
    w.put(dl, !Keyed || dl != 0);
    const bool cfu = stepU();
    dl = fetchNow();
    advance(cfv, cfu);
  };
  // boundary eval: deltas from prev boundary at the CURRENT reg
  // position, steps = delta * recipOf(stepDist), then regs advance
  // `adv` by the selected grad set.
  auto eval = [&](int stepDist, int adv, bool g32) {
    const double V = r * vq, U = r * uq;
    const std::int32_t vSB =
        fix2012((U - subU) * static_cast<double>(recipOf(stepDist)));
    const std::int32_t uSB =
        fix1616((V - subV) * static_cast<double>(recipOf(stepDist)));
    subU = U; subV = V;
    const double gdq = g32 ? gr.dq32 : gr.dqdx;
    const double gdu = g32 ? gr.duq32 : gr.duqdx;
    const double gdv = g32 ? gr.dvq32 : gr.dvqdx;
    q += gdq * adv; uq += gdu * adv; vq += gdv * adv;
    r = 1.0 / q;
    vFStep = static_cast<std::uint32_t>(vSB) << 20;
    uFStep = static_cast<std::uint32_t>(uSB) << 16;
    pxNorm = ((vSB >> 12) << ush) + (uSB >> 16);
    pxCarry = pxNorm + pitch;
  };
  // entry eval — pos0 boundary + accs/esi seed + regs += e1.
  // OBSERVED: [esp+8] gets V + 1.5·2^40 (20.12 -> vAcc + <<ush
  // texel), [esp] gets U + 1.5·2^36 (16.16 -> uAcc + low texel);
  // the V-int extraction is sar >>12, the U-int LOGICAL >>16.
  auto evalSeed = [&](int adv) {
    const double V0 = r * vq, U0 = r * uq;
    subU = V0; subV = U0;        // OBSERVED swapped carry-slot seed
    const std::int32_t vB = fix2012(V0);   // [esp+8] — row side
    const std::int32_t uB = fix1616(U0);   // [esp]   — column side
    vAcc = static_cast<std::uint32_t>(vB) << 20;
    uAcc = static_cast<std::uint32_t>(uB) << 16;
    off = ((vB >> 12) << ush) +
          static_cast<std::int32_t>(
              static_cast<std::uint32_t>(uB) >> 16);
    if (Wrap) off = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(off) & mask);
    q += static_cast<double>(gr.dqdx) * adv;
    uq += static_cast<double>(gr.duqdx) * adv;
    vq += static_cast<double>(gr.dvqdx) * adv;
    r = 1.0 / q;
  };

  // -- region partition (the prologue cmp/neg/sub tree) ----------
  int head, blocks, tail, e1, e2 = 0;
  const int pxInBlk = leftpx & 31;
  if (pxInBlk) {
    head = 32 - pxInBlk;
    const int rem = count - head;
    if (rem < 0) {                 // degenerate — head 0, all-tail
      head = 0; blocks = 0; tail = count; e1 = count;
    } else {
      blocks = rem >> 5;
      tail = rem & 31;
      e1 = head;
      e2 = blocks ? 32 : tail;
    }
  } else {
    head = 0;
    blocks = static_cast<int>(
        static_cast<std::uint32_t>(count) >> 5);   // logical >>5
    tail = count & 31;
    e1 = blocks ? 32 : tail;
  }
  // Hardening: an aligned negative count wraps blocks to a huge
  // value — the original walks off into unmapped writes; bound the
  // block loop (a 600-px row never needs more than ~20).
  if (blocks > 32) blocks = 32;

  w.d = dst;
  evalSeed(e1);
  if (head > 0) {
    eval(head, e2, false);
    // head draw — counted loop, single exit write
    {
      int n = head;
      prime();
      if (--n <= 0) w.put(dl, !Keyed || dl != 0);
      else {
        do { iter(); } while (--n != 0);
        w.put(dl, !Keyed || dl != 0);
      }
    }
  }
  // dispatch — jb tail / je last-block / else mid loop.
  // A block draw = prime + 31 write-iters + one BARE write (the
  // OBSERVED last iter drops the accs step + fetch + advance — the
  // carried texel is exactly the next region's prime fetch).
  --blocks;
  while (blocks > 0) {
    eval(32, 1, true);             // ÷32 steps, regs += the ×32 presets once
    {
      prime();
      for (int i = 0; i < 31; ++i) iter();
      w.put(dl, !Keyed || dl != 0);
    }
    --blocks;
  }
  if (blocks == 0) {
    eval(32, tail, false);         // ÷32 steps, regs += tail
    {
      prime();
      for (int i = 0; i < 31; ++i) iter();
      w.put(dl, !Keyed || dl != 0);
    }
  }
  if (tail != 0) {
    eval(tail, 0, false);
    int n = tail;
    prime();
    if (--n <= 0) {
      w.put(dl, !Keyed || dl != 0);
      w.put(dl, !Keyed || dl != 0);   // the duplicated final texel
    } else {
      do { iter(); } while (--n != 0);
      w.put(dl, !Keyed || dl != 0);
      w.put(dl, !Keyed || dl != 0);
    }
  } else {
    // tail==0 exit arm — count==0 fetches the anchor texel first;
    // otherwise the carried texel is re-written once.
    if (count == 0) dl = fetchNow();
    w.put(dl, !Keyed || dl != 0);
  }
}

// ------------------------------------------------------------------
// The A8-family affine machine — one call per triangle half. The
// drawer keeps its own row loop (46e52c seeds the state; the drawer
// runs ebd0 rows: the long edge stays in ebc0 across both halves,
// the short edge is reseeded to the mid vert between calls).
struct AffineDrawer {
  const TexMat* m;
  // edge accs (16.16) — ebc0 = the long (top->bot) edge, ebc4 = the
  // short edge for this half.
  std::int32_t longX, shortX;
  std::int32_t longSlope, shortSlope;
  int rows;                       // ebd0 — half rows + 1
  std::int32_t anchor;            // ebd4 — running texel offset
  std::uint32_t uAcc, vAcc;       // ebb8/ebbc row accumulators
  std::uint32_t uFStep, vFStep;   // eba0/eba4 — pixel frac steps
  std::uint32_t uRowF, vRowF;     // ebb0/ebb4 — row frac steps
  std::int32_t pxNorm, pxCarry;   // eb9c/eb98
  std::int32_t rowNorm, rowCarry; // ebac/eba8
  std::uint32_t wrapMask;         // ebe0
};

// One affine row span — the prime + 32-block/tail machine. No head
// partition: the whole count is consumed by 32-px blocks and the
// leftover tail, and block boundaries do NOT re-prime (the pipeline
// carries across them — the last unrolled iter still fetches).
template<bool Keyed, bool Wrap>
void affineSpan(const AffineDrawer& s, SpanWriter& w, int count,
                std::uint32_t uAcc, std::uint32_t vAcc,
                std::int32_t off, const TexMat& m) {
  auto stepV = [&]() -> bool {
    const std::uint32_t o = vAcc; vAcc += s.vFStep; return vAcc < o;
  };
  auto stepU = [&]() -> bool {
    const std::uint32_t o = uAcc; uAcc += s.uFStep; return uAcc < o;
  };
  auto fetchNow = [&]() -> std::uint8_t {
    const std::int32_t o =
        Wrap ? (std::int32_t)((std::uint32_t)off & s.wrapMask)
             : off;
    if (static_cast<std::uint32_t>(o) >= m.texSize)
      ++w.diag->texLookupMiss;
    return texAt(m, o);
  };
  auto advance = [&](bool cfv, bool cfu) {
    off += (cfv ? s.pxCarry : s.pxNorm) + (cfu ? 1 : 0);
  };
  auto iter = [&](std::uint8_t& dl) {
    const bool cfv = stepV();
    w.put(dl, !Keyed || dl != 0);
    const bool cfu = stepU();
    dl = fetchNow(); advance(cfv, cfu);
  };
  // prime — accs step once, fetch the anchor texel, addr advance.
  std::uint8_t dl;
  {
    const bool cfv = stepV(); const bool cfu = stepU();
    dl = fetchNow(); advance(cfv, cfu);
  }
  if (count <= 0) {
    w.put(dl, !Keyed || dl != 0);       // single anchor write
    return;
  }
  int rem = count;
  for (;;) {
    rem -= 32;
    if (rem <= 0) break;
    for (int k = 0; k < 32; ++k) iter(dl);
  }
  const int R = rem + 31;
  if (R < 0) {
    w.put(dl, !Keyed || dl != 0);       // unreachable on real counts
    return;
  }
  for (int k = 0; k < R; ++k) iter(dl);
  // the duplicated final texel (the +1 overrun — lands one pixel
  // past the span's far edge, left for arm B's backward walk)
  w.put(dl, !Keyed || dl != 0);
  w.put(dl, !Keyed || dl != 0);
}

template<bool Keyed, bool Wrap>
void affineHalf(AffineDrawer& s, std::uint8_t* rowPtr, SpanWriter& w) {
  const TexMat& m = *s.m;
  if (--s.rows <= 0) return;      // entry dec — dyH+1 rows seeded
  for (;;) {
    // per row: the accs/anchor reload from the persistent state —
    // pixel-loop mutations are discarded at the row advance.
    std::uint32_t uAcc = s.uAcc, vAcc = s.vAcc;
    std::int32_t off = s.anchor;
    if (s.longX <= s.shortX) {
      // arm A — long edge on the left, forward walk.
      const std::int32_t li = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(s.longX) >> 16);
      const std::int32_t ri = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(s.shortX) >> 16);
      w.dir = 1;
      w.d = rowPtr + li;
      if (rowPtr >= w.base && rowPtr < w.end)
        affineSpan<Keyed, Wrap>(s, w, ri - li, uAcc, vAcc, off, m);
    } else {
      // arm B — long edge on the right; writes run right-to-left
      // while the texel walk still steps forward (OBSERVED mirror).
      const std::int32_t ri = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(s.longX) >> 16);
      const std::int32_t li = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(s.shortX) >> 16);
      w.dir = -1;
      w.d = rowPtr + ri;
      if (rowPtr >= w.base && rowPtr < w.end)
        affineSpan<Keyed, Wrap>(s, w, ri - li, uAcc, vAcc, off, m);
    }
    // row advance — the anchor walks the LONG edge by the row step
    // pair + row-frac carries; edges walk by the 16.16 slopes.
    {
      const std::uint32_t ov = s.vAcc;
      s.vAcc += s.vRowF;
      const bool cfv = s.vAcc < ov;
      const std::uint32_t ou = s.uAcc;
      s.uAcc += s.uRowF;
      const bool cfu = s.uAcc < ou;
      s.anchor += (cfv ? s.rowCarry : s.rowNorm) + (cfu ? 1 : 0);
    }
    s.longX += s.longSlope;
    s.shortX += s.shortSlope;
    rowPtr += 600;                  // OBSERVED +0x258 surface stride
    if (--s.rows == 0) break;
  }
}

} // namespace

// StreamScene::project6b4f8 ports for the core's own events): row-major
// 3x4 transform, the flag pack's mov-arms, then the sel-0 fill.
void StreamRibbonRaster::projectVert(const float m[12],
                                     const float in[3],
                                     StreamTriVert& v) {
  v.x = static_cast<float>(
      static_cast<double>(in[0]) * m[0] +
      static_cast<double>(in[1]) * m[1] +
      static_cast<double>(in[2]) * m[2] + static_cast<double>(m[3]));
  v.y = static_cast<float>(
      static_cast<double>(in[0]) * m[4] +
      static_cast<double>(in[1]) * m[5] +
      static_cast<double>(in[2]) * m[6] + static_cast<double>(m[7]));
  v.z = static_cast<float>(
      static_cast<double>(in[0]) * m[8] +
      static_cast<double>(in[1]) * m[9] +
      static_cast<double>(in[2]) * m[10] + static_cast<double>(m[11]));
  // b559..b5da — {y'>z' -> 1; -z'<=y' -> 0; else 2}, then
  // x'>z' -> |4 else x'<-z' -> |8 (the |8 arm is skipped once |4
  // landed — the arms are mutually exclusive by construction).
  std::uint32_t flags = 0;
  if (v.y > v.z) flags = 1;
  else if (-v.z <= v.y) flags = 0;
  else flags = 2;
  if (v.x > v.z) flags |= 4;
  else if (v.x < -v.z) flags |= 8;
  if (static_cast<double>(v.z) < 0.05) flags |= 0x10;
  v.flags = flags;
  v.sx = v.sy = 0.0f;
  project(v);            // the fill runs whenever z' != 0
}

// ------------------------------------------------------------------
// FUN_0040c860 — sort the three verts by sy ascending (OBSERVED
// fcomp/ja order — strict swaps keep equal-key input order), then
// the negative-pen family dispatch. eax/edx/v-ptr plumbing and the
// 42fecc profiling counter are host plumbing, not modeled.

// OBSERVED compare chain, in asm order:
//   jns       -> material table (pen >= 0)
//   >= -989   -> 415260 flat           [-989,-1]
//   >= -1010  -> 47a770 effect         [-1010,-990]
//   >  -1024  -> 415260 flat           [-1023,-1011]
//   >= -1027  -> 412970, lut + (-1024-pen)*256   [-1027,-1024]
//   == -1028  -> 46e940 effect
//   else      -> 412970, lut + (-1029-pen)*256   <= -1029
StreamTriBranch StreamRibbonRaster::branchForPen(int pen) {
  if (pen >= 0) return StreamTriBranch::kMaterial;
  if (pen >= -989) return StreamTriBranch::kFlat;
  if (pen >= -1010) return StreamTriBranch::kFx47a770;
  if (pen > -1024) return StreamTriBranch::kFlat;
  if (pen >= -1027) return StreamTriBranch::kLut1024;
  if (pen == -1028) return StreamTriBranch::kFx46e940;
  return StreamTriBranch::kLut1029;
}

// ------------------------------------------------------------------
// 19B.2C — FUN_0046daac. `in` arrives sy-sorted by dispatch. All the
// rounded/int products are i32; the projection ratios are f32.
void StreamRibbonRaster::fillTextured(
    mdk::IndexedFramebuffer& fb, const mdk::ArenaRenderMaterial& m,
    const StreamTriVert in[3], StreamRibbonDiag& diag) const {
  const std::int32_t yT = magicRound(in[0].sy);      // ec04
  const std::int32_t yB = magicRound(in[2].sy);      // ec0c
  const std::int32_t totalH = yB - yT;               // ebfc
  if (totalH <= 0) { ++diag.matDegenerate; return; }
  const std::int32_t dxTB =
      magicRound(in[2].sx) - magicRound(in[0].sx);   // ebf8
  const std::int32_t negTM =
      magicRound(in[0].sx) - magicRound(in[1].sx);   // ebf4 (negated)
  const std::int32_t yM = magicRound(in[1].sy);      // ec08
  const std::int32_t dyTM = yM - yT;                 // ec00
  const std::int32_t cross = dxTB * dyTM + negTM * totalH;  // ebec
  if (cross == 0) { ++diag.matDegenerate; return; }
  if (m.shift >= 12 || m.width == 0 || m.height == 0)
    ++diag.texInvalidMeta;                           // drawn anyway —
                                                     // the original
                                                     // gates nothing

  const float minZ = std::min({in[0].z, in[1].z, in[2].z});
  const float maxZ = std::max({in[0].z, in[1].z, in[2].z});

  // The near-plane clamp — verts below maxZ/64 are swapped for a
  // scratch copy with z = thresh (OBSERVED: the threshold scales
  // with MAXIMUM z, not minimum — the whole arm is gated on
  // maxZ > 64*minZ). sx/sy/uv flow through unchanged.
  const StreamTriVert* pv[3] = {&in[0], &in[1], &in[2]};
  StreamTriVert clamped[3];
  if (maxZ > minZ * 64.0f) {
    const float thresh = maxZ * 0.015625f;           // 498fe4 = 1/64
    for (int i = 0; i < 3; ++i)
      if (pv[i]->z < thresh) {
        clamped[i] = *pv[i];
        clamped[i].z = thresh;
        pv[i] = &clamped[i];
      }
  } else {
    // The affine gate — DAT_00541482 (the ForcePCorrect user toggle)
    // is 0 in mode 5; the zoom ratio 0x540b58/0x540b64 is 1.0f.
    const float g0c = std::bit_cast<float>(m.param0c);
    const float g10 = std::bit_cast<float>(m.param10);
    if (g0c < minZ && (maxZ - minZ) * g10 < minZ) {
      const std::int32_t yR[3] = {yT, yM, yB};
      ++diag.matAffine;
      fillTexturedAffine(fb, m, pv, yR, dyTM, totalH, cross, diag);
      return;
    }
  }
  ++diag.matPersp;

  // Per-vert projective terms — z reads the (possibly clamped) pv,
  // uv reads the ORIGINAL bank — the uv pointers are never reseated.
  const float qT = 1.0f / pv[0]->z, qM = 1.0f / pv[1]->z,
              qB = 1.0f / pv[2]->z;
  const float uqT = in[0].u * qT, uqM = in[1].u * qM, uqB = in[2].u * qB;
  const float vqT = in[0].v * qT, vqM = in[1].v * qM, vqB = in[2].v * qB;

  // ebec += (ebec<1 ? -2*ebfc : +2*ebfc) — the cross doubled by sign.
  const std::int32_t ebec =
      cross + (cross < 1 ? -2 * totalH : 2 * totalH);
  const float invX = 1.0f / static_cast<float>(ebec); // ec18
  const float fH = static_cast<float>(totalH);
  const float fM = static_cast<float>(dyTM);
  PerspGrads gr;
  gr.dqdx  = ((qT  - qM)  * fH + (qB  - qT)  * fM) * invX;
  gr.duqdx = ((uqT - uqM) * fH + (uqB - uqT) * fM) * invX;
  gr.dvqdx = ((vqT - vqM) * fH + (vqB - vqT) * fM) * invX;
  gr.dq32  = gr.dqdx  * 32.0f;   // ec80 — the ×32 presets
  gr.duq32 = gr.duqdx * 32.0f;
  gr.dvq32 = gr.dvqdx * 32.0f;

  TexMat tm;
  tm.tex = m.pixels.data();
  tm.texSize = m.pixels.size();
  tm.ush = static_cast<int>(m.shift);
  tm.pitch = static_cast<std::int32_t>(1u << (m.shift & 31));
  tm.uMask = m.uMask;
  tm.vMask = m.vMask;
  tm.keyed = (m.flags & 1) != 0;
  tm.wrap = (m.flags & 2) != 0;

  SpanWriter w;
  w.base = fb.pixels();
  w.end = w.base + fb.pixelCount();
  w.diag = &diag;
  w.dir = 1;
  std::uint8_t* rowPtr = w.base + static_cast<std::size_t>(yT) * 600;
  int y = yT;

  // Row-walk accs (f32): the long T->B edge pair + the short edge
  // pair for the current half.
  const float invH = 1.0f / fH;
  float longX  = in[0].sx;
  float longQ  = qT, longUq = uqT, longVq = vqT;
  const float longDx  = (in[2].sx - in[0].sx) * invH;
  const float longDq  = (qB  - qT)  * invH;
  const float longDuq = (uqB - uqT) * invH;
  const float longDvq = (vqB - vqT) * invH;

  auto emit = [&](float leftX, float rightX, float qL, float uqL,
                  float vqL) {
    const std::int32_t li = magicRound(leftX);
    const std::int32_t count = magicRound(rightX) - li;
    if (rowPtr >= w.base && rowPtr < w.end) {
      if (tm.keyed) {
        if (tm.wrap)
          perspSpan<true, true>(rowPtr + li, li, count, qL, uqL, vqL,
                                gr, tm, w);
        else
          perspSpan<true, false>(rowPtr + li, li, count, qL, uqL, vqL,
                                 gr, tm, w);
      } else if (tm.wrap) {
        perspSpan<false, true>(rowPtr + li, li, count, qL, uqL, vqL,
                               gr, tm, w);
      } else {
        perspSpan<false, false>(rowPtr + li, li, count, qL, uqL, vqL,
                                gr, tm, w);
      }
    }
    rowPtr += 600;
    ++y;
  };

  // Top half — the T->M edge walks the short accs.
  if (dyTM > 0) {
    float shortX  = in[0].sx;
    float shortQ  = qT, shortUq = uqT, shortVq = vqT;
    const float invTM = 1.0f / fM;
    const float sdx  = (in[1].sx - in[0].sx) * invTM;
    const float sdq  = (qM  - qT)  * invTM;
    const float sduq = (uqM - uqT) * invTM;
    const float sdvq = (vqM - vqT) * invTM;
    if (sdx <= longDx) {
      // arm A — TM edge on the left.
      while (y < yM) {
        emit(shortX, longX, shortQ, shortUq, shortVq);
        shortX += sdx; shortQ += sdq; shortUq += sduq;
        shortVq += sdvq;
        longX += longDx; longQ += longDq; longUq += longDuq;
        longVq += longDvq;
      }
    } else {
      // arm B — TB edge on the left; emits the LONG-edge values.
      while (y < yM) {
        emit(longX, shortX, longQ, longUq, longVq);
        longX += longDx; longQ += longDq; longUq += longDuq;
        longVq += longDvq;
        shortX += sdx; shortQ += sdq; shortUq += sduq;
        shortVq += sdvq;
      }
    }
  }
  // Bottom half — the short accs reseed to the MID vert and walk
  // the M->B slopes; the long accs continue.
  if (yM < yB) {
    const std::int32_t dyMB = yB - yM;
    const float invMB = 1.0f / static_cast<float>(dyMB);
    float shortX  = in[1].sx;
    float shortQ  = qM, shortUq = uqM, shortVq = vqM;
    const float sdx  = (in[2].sx - in[1].sx) * invMB;
    const float sdq  = (qB  - qM)  * invMB;
    const float sduq = (uqB - uqM) * invMB;
    const float sdvq = (vqB - vqM) * invMB;
    if (shortX <= longX) {
      // arm A — MB edge on the left.
      while (y < yB) {
        emit(shortX, longX, shortQ, shortUq, shortVq);
        shortX += sdx; shortQ += sdq; shortUq += sduq;
        shortVq += sdvq;
        longX += longDx; longQ += longDq; longUq += longDuq;
        longVq += longDvq;
      }
    } else {
      while (y < yB) {
        emit(longX, shortX, longQ, longUq, longVq);
        longX += longDx; longQ += longDq; longUq += longDuq;
        longVq += longDvq;
        shortX += sdx; shortQ += sdq; shortUq += sduq;
        shortVq += sdvq;
      }
    }
  }
}

// ------------------------------------------------------------------
// FUN_0046e52c — the affine setup; tail-called by 46daac only on the
// no-clamp arm, so pv[] are the original verts. Runs both halves via
// the A8-family row-loop drawers (the drawer owns the row loop; the
// setup's state persists across the two calls).
void StreamRibbonRaster::fillTexturedAffine(
    mdk::IndexedFramebuffer& fb, const mdk::ArenaRenderMaterial& m,
    const StreamTriVert* const pv[3], const std::int32_t yR[3],
    std::int32_t dyTM, std::int32_t totalH, std::int32_t cross,
    StreamRibbonDiag& diag) const {
  // ebec' = 2*ebfc + |ebec| — the ABSOLUTE-cross denominator.
  const std::uint32_t cabs =
      cross < 0 ? 0u - static_cast<std::uint32_t>(cross)
                : static_cast<std::uint32_t>(cross);
  const std::int32_t ebec =
      static_cast<std::int32_t>(2u * static_cast<std::uint32_t>(totalH)
                                + cabs);
  const float invE = 1.0f / static_cast<float>(ebec);
  const float fH = static_cast<float>(totalH) * invE;   // fVar2
  const float fM = static_cast<float>(dyTM) * invE;     // fVar3

  const std::int32_t uT16 = fix1616(pv[0]->u);
  const std::int32_t vT16 = fix1616(pv[0]->v);
  // f32 products promoted for the magic-add — OBSERVED order.
  const std::int32_t duRow16 =
      fix1616((pv[2]->u - pv[0]->u) *
              (1.0f / static_cast<float>(totalH)));
  const std::int32_t dvRow16 =
      fix1616((pv[2]->v - pv[0]->v) *
              (1.0f / static_cast<float>(totalH)));
  const std::int32_t duCol16 =
      fix1616((pv[1]->u - pv[0]->u) * fH -
              (pv[2]->u - pv[0]->u) * fM);
  const std::int32_t dvCol16 =
      fix1616((pv[1]->v - pv[0]->v) * fH -
              (pv[2]->v - pv[0]->v) * fM);

  TexMat tm;
  tm.tex = m.pixels.data();
  tm.texSize = m.pixels.size();
  tm.ush = static_cast<int>(m.shift);
  tm.pitch = static_cast<std::int32_t>(1u << (m.shift & 31));
  tm.uMask = m.uMask;
  tm.vMask = m.vMask;
  tm.keyed = (m.flags & 1) != 0;
  tm.wrap = (m.flags & 2) != 0;

  AffineDrawer s;
  s.m = &tm;
  s.uAcc = static_cast<std::uint32_t>(uT16) << 16;     // ebb8
  s.vAcc = static_cast<std::uint32_t>(vT16) << 16;     // ebbc
  s.uFStep = static_cast<std::uint32_t>(duCol16) << 16; // eba0
  s.vFStep = static_cast<std::uint32_t>(dvCol16) << 16; // eba4
  s.uRowF = static_cast<std::uint32_t>(duRow16) << 16; // ebb0
  s.vRowF = static_cast<std::uint32_t>(dvRow16) << 16; // ebb4
  const int ush = tm.ush;
  s.pxNorm = ((dvCol16 >> 16) << ush) + (duCol16 >> 16);  // eb9c
  s.pxCarry = s.pxNorm + tm.pitch;                        // eb98
  s.rowNorm = ((dvRow16 >> 16) << ush) + (duRow16 >> 16); // ebac
  s.rowCarry = s.rowNorm + tm.pitch;                      // eba8
  // ebd4 — the running anchor as a tex-space OFFSET (the original
  // adds the tex base; the wrap drawers subtract it back inside the
  // call — folded to a plain offset here).
  s.anchor =
      static_cast<std::int32_t>(
          (uT16 >> 16) & m.uMask) +
      static_cast<std::int32_t>(
          (vT16 >> (16 - ush)) & m.vMask);
  s.wrapMask = m.uMask | m.vMask;                         // ebe0
  s.longX = fix1616(pv[0]->sx);                           // ebc0
  s.shortX = s.longX;                                     // ebc4
  s.longSlope = fix1616(                                  // ebc8
      totalH < 2 ? (pv[2]->sx - pv[0]->sx)
                 : static_cast<double>(
                       (pv[2]->sx - pv[0]->sx) /
                       static_cast<float>(totalH)));
  s.rows = dyTM + 1;                                      // ebd0
  s.shortSlope = fix1616(                                 // ebcc
      dyTM < 2 ? (pv[1]->sx - pv[0]->sx)
               : static_cast<double>(
                     (pv[1]->sx - pv[0]->sx) /
                     static_cast<float>(dyTM)));

  SpanWriter w;
  w.base = fb.pixels();
  w.end = w.base + fb.pixelCount();
  w.diag = &diag;
  std::uint8_t* rowPtr =
      w.base + static_cast<std::size_t>(yR[0]) * 600;    // ebd8

  // First half — rows [yT, yM); the drawer's rowPtr walks in place.
  if (tm.keyed) {
    if (tm.wrap) affineHalf<true, true>(s, rowPtr, w);
    else affineHalf<true, false>(s, rowPtr, w);
  } else if (tm.wrap) {
    affineHalf<false, true>(s, rowPtr, w);
  } else {
    affineHalf<false, false>(s, rowPtr, w);
  }
  // ebd8 persists across the call — recompute from the consumed
  // rows (the original keeps it in the global; rowPtr walked by
  // the callee there — here affineHalf walks its copy, so advance
  // the local by the consumed count).
  rowPtr += 600 * static_cast<std::size_t>(dyTM);   // rows consumed

  // Second half — shortX/shortSlope reseed to the mid vert; the long
  // edge acc, row accs and the anchor all continue.
  s.shortX = fix1616(pv[1]->sx);                          // ebc4
  const std::int32_t dyMB = yR[2] - yR[1];
  s.rows = dyMB + 1;                                      // ebd0
  s.shortSlope = fix1616(                                 // ebcc
      dyMB < 2 ? (pv[2]->sx - pv[1]->sx)
               : static_cast<double>(
                     (pv[2]->sx - pv[1]->sx) /
                     static_cast<float>(dyMB)));
  if (tm.keyed) {
    if (tm.wrap) affineHalf<true, true>(s, rowPtr, w);
    else affineHalf<true, false>(s, rowPtr, w);
  } else if (tm.wrap) {
    affineHalf<false, true>(s, rowPtr, w);
  } else {
    affineHalf<false, false>(s, rowPtr, w);
  }
}

void StreamRibbonRaster::dispatch(mdk::IndexedFramebuffer& fb, int pen,
                                  const StreamTriVert in[3],
                                  std::span<const mdk::ArenaRenderMaterial* const> mats,
                                  StreamRibbonDiag& diag) const {
  StreamTriVert v[3] = {in[0], in[1], in[2]};
  // The 0c860 three-fcomp sort — ascending by sy (offset +0x10).
  if (v[0].sy > v[1].sy) std::swap(v[0], v[1]);
  if (v[1].sy > v[2].sy) std::swap(v[1], v[2]);
  if (v[0].sy > v[1].sy) std::swap(v[0], v[1]);

  const StreamTriBranch br = branchForPen(pen);
  int lutRowIx = -1;
  if (br == StreamTriBranch::kLut1024) lutRowIx = -1024 - pen;
  else if (br == StreamTriBranch::kLut1029) lutRowIx = -1029 - pen;
  ++diag.branch[static_cast<int>(br)];

  switch (br) {
    case StreamTriBranch::kLut1029:
    case StreamTriBranch::kLut1024:
      if (!lutReady_ || lutRowIx < 0 || lutRowIx >= 6 * 64) {
        ++diag.lutMisses;
        break;
      }
      ++diag.rasterized;
      fillLut(fb, &lut_[lutRowIx * 256], v, diag);
      break;
    case StreamTriBranch::kFlat: {
      ++diag.rasterized;
      const std::uint8_t byte = static_cast<std::uint8_t>(-pen & 0xff);
      fillFlat(fb, byte, v, diag);
      break;
    }
    case StreamTriBranch::kMaterial: {
      // pen indexes the pushed record's material table (model +0x10)
      // — a miss takes the flat-0xff fallback, OBSERVED @0x40c9da.
      // The classes split for closure census: OOB/unresolved name
      // (lookup miss), resolved INDEX record (+0x24==0 — the
      // original's own flat arm), and non-index record with an
      // empty pixel span (invalid).
      const mdk::ArenaRenderMaterial* mat =
          (pen >= 0 && static_cast<std::size_t>(pen) < mats.size())
              ? mats[static_cast<std::size_t>(pen)]
              : nullptr;
      ++diag.rasterized;
      if (mat == nullptr) {
        ++diag.matLookupMiss; ++diag.matFlat;
        ++diag.matLookupMissPens[{pen, (int)mats.size()}];
        fillFlat(fb, 0xff, v, diag);
        break;
      }
      if (mat->isIndexRecord) {
        ++diag.matIndexRec; ++diag.matFlat;
        ++diag.matIndexRecHits[mat];
        fillFlat(fb, 0xff, v, diag);
        break;
      }
      if (mat->pixels.empty()) {
        ++diag.matInvalidRec; ++diag.matFlat;
        fillFlat(fb, 0xff, v, diag);
        break;
      }
      const std::uint64_t before = diag.pixels;
      fillTextured(fb, *mat, v, diag);
      diag.matPixels += diag.pixels - before;
      if (diag.pixels == before) ++diag.matZero;
      break;
    }
    default:
      ++diag.unsupported;                 // 47a770 / 46e940
      break;
  }
}

// ------------------------------------------------------------------
// FUN_0040ca00 top-level — the [0x5414d4] renderer-active gate is
// always on in the host (we only consume real draws). AND of the
// three flag bytes rejects first; OR == 0 dispatches direct; else
// the clipper runs its five gated passes and fans the survivor.

void StreamRibbonRaster::draw(mdk::IndexedFramebuffer& fb, int pen,
                              const float v9[9], std::uint32_t packedFlags,
                              StreamRibbonDiag& diag) const {
  StreamTriVert v[3];
  for (int i = 0; i < 3; ++i) {
    v[i].x = v9[i * 3 + 0];
    v[i].y = v9[i * 3 + 1];
    v[i].z = v9[i * 3 + 2];
    v[i].flags = (packedFlags >> (i * 8)) & 0xff;
    project(v[i]);                       // the upstream 6b4f8 fill
  }
  drawTri(fb, pen, v, {}, diag);
}

// 19B.2B1 — the same 0ca00 + 0c860 tail for a tri that arrives
// already projected and flagged (the model submit path; ca00's
// contract takes view-space verts and packed flags, which is
// exactly what projectVert produces). `mats` is the pushed record's
// material table — consulted only by the pen >= 0 arm.
void StreamRibbonRaster::drawTri(
    mdk::IndexedFramebuffer& fb, int pen, const StreamTriVert v[3],
    std::span<const mdk::ArenaRenderMaterial* const> mats,
    StreamRibbonDiag& diag) const {
  ++diag.commands;
  const std::uint32_t f0 = v[0].flags, f1 = v[1].flags, f2 = v[2].flags;
  if ((f0 & f1 & f2) != 0) return;       // trivial reject (never on
                                         // real data — core gates it)
  const std::uint32_t orIn = f0 | f1 | f2;
  if (orIn == 0) {
    dispatch(fb, pen, v, mats, diag);
    return;
  }

  ++diag.clipped;
  std::array<StreamTriVert, kMaxClipV> bankA, bankB;
  std::array<StreamTriVert, kMaxClipV>* cur = &bankA;
  std::array<StreamTriVert, kMaxClipV>* out = &bankB;

  // near pass always runs over the raw input (or copies it verbatim
  // when no vert carries the near bit) — the "copy-3" arm.
  int n;
  std::uint32_t orCur = 0;
  if (orIn & 0x10) {
    n = clipPass(v, 3, bankA.data(), 0x10, orCur);
  } else {
    n = 3;
    bankA[0] = v[0]; bankA[1] = v[1]; bankA[2] = v[2];
    orCur = orIn;
  }
  if (n < 3) { ++diag.clipDropped; return; }

  for (const std::uint32_t bit : {8u, 4u, 1u, 2u}) {
    if (!(orCur & bit)) continue;
    std::uint32_t orOut = 0;
    n = clipPass(cur->data(), n, out->data(), bit, orOut);
    if (n < 3) { ++diag.clipDropped; return; }
    std::swap(cur, out);
    orCur = orOut;
  }

  if (orCur != 0 || n < 3) { ++diag.clipDropped; return; }

  // Fan emit — OBSERVED (v0, v[k+1], v[k+2]) per out vert k = 1..n-2.
  for (int k = 1; k + 1 < n; ++k) {
    const StreamTriVert tri[3] = {(*cur)[0], (*cur)[k], (*cur)[k + 1]};
    if (pen >= 0) ++diag.matClipFan;
    dispatch(fb, pen, tri, mats, diag);
  }
}

void StreamRibbonRaster::drawTri(mdk::IndexedFramebuffer& fb, int pen,
                                 const StreamTriVert v[3],
                                 StreamRibbonDiag& diag) const {
  drawTri(fb, pen, v, {}, diag);
}

const char* streamTriBranchName(StreamTriBranch b) {
  switch (b) {
    case StreamTriBranch::kLut1029: return "lut<-1029";
    case StreamTriBranch::kLut1024: return "lut<-1027..-1024";
    case StreamTriBranch::kFlat: return "flat";
    case StreamTriBranch::kFx47a770: return "fx47a770";
    case StreamTriBranch::kFx46e940: return "fx46e940";
    case StreamTriBranch::kMaterial: return "material";
    default: return "?";
  }
}

} // namespace mdkbridge
