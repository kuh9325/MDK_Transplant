// See stream_raster.h for the phase framing. Everything below is a
// host-side reproduction of the OBSERVED BUILD_A chain
//   FUN_0040ca00 (clip/fan) -> FUN_0040c860 (material dispatch) ->
//   FUN_00412970 (LUT remap filler) / FUN_00415260 (flat filler)
// with the mode-5 LUT build FUN_00406b80/FUN_00406d84 — all writes
// are palette indices into the 600x360 indexed surface.
#include "stream_raster.h"

#include <algorithm>
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
// on the plane via the forced coordinate; the aux 2-float payload
// native interpolates is unused by the index fillers and dropped.
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

// ------------------------------------------------------------------
// FUN_0046b4f8 — the model submitter's per-vertex fill (the same body
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

void StreamRibbonRaster::dispatch(mdk::IndexedFramebuffer& fb, int pen,
                                  const StreamTriVert in[3],
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
    default:
      ++diag.unsupported;                 // 47a770 / 46e940 / material
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
  drawTri(fb, pen, v, diag);
}

// 19B.2B1 — the same 0ca00 + 0c860 tail for a tri that arrives
// already projected and flagged (the model submit path; ca00's
// contract takes view-space verts and packed flags, which is
// exactly what projectVert produces).
void StreamRibbonRaster::drawTri(mdk::IndexedFramebuffer& fb, int pen,
                                 const StreamTriVert v[3],
                                 StreamRibbonDiag& diag) const {
  ++diag.commands;
  const std::uint32_t f0 = v[0].flags, f1 = v[1].flags, f2 = v[2].flags;
  if ((f0 & f1 & f2) != 0) return;       // trivial reject (never on
                                         // real data — core gates it)
  const std::uint32_t orIn = f0 | f1 | f2;
  if (orIn == 0) {
    dispatch(fb, pen, v, diag);
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
    dispatch(fb, pen, tri, diag);
  }
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
