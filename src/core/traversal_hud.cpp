// Phase 17B.1 — traversal HUD / scope-view compositor (native core).
//
// Reproduces the FUN_00436d60 draw tail into a 600x360 indexed overlay
// (pen 0 = transparent) plus the HUD-side state ticks it depends on.
// Everything here is OBSERVED at instruction level in BUILD_A's
// MDK95.EXE (see traversal_hud.h + docs/GAMEPLAY_RECONSTRUCTION.md
// "Phase 17B — RE checkpoint" for the evidence record).
//
// Original draw order (FUN_00436d60, OBSERVED):
//   head: FUN_0046ec60 viewport registers (0x5414d4 gate; counted
//     seam), FUN_00436ea8 anim drain (existing machine).
//   scope gate A (c9c && ca0>1): FUN_0045f030(0) — per-slot mode-0
//     in-flight model re-render into the shot windows (world side —
//     no overlay pixels) + FUN_00437aa8 scope-warp (counted seam).
//   FUN_00469f7c — inventory: per-record slide lerp (FUN_00469e94),
//     selection box + deferred 0x540b20 shade-LUT remap, icon draw
//     (PICKUPS[id] centered), count digits; the 0x541558 visibility
//     timer gates every draw inside it.
//   scope gate B (c9c && ca0>1): FUN_0045f030(1) — window fills
//     (deferred to the shotWinFill channel) + SNIPERGA gauges + the
//     +0xf4 remnant counter (FUN_0045ee7c mode 1), then FUN_00436f08
//     (CROSS frame 0 at (299,219) + the SNIPERS2 u16 pen-command
//     stream — internally gated on 0x5414d4).
//   FUN_00436f2c — mounted reticle (FUN_0046911c), then under
//     hudActive (0x5414d4): event bars, FUN_00417e20 (mission-timer
//     pie + health digits + scoped tail), then unconditionally the
//     message flush (FUN_0041cb44 — counted seam) and the SKULL
//     death overlay (541554==0 && 540dac>0 && 540cac==0x3ea).
//
// Deferred/counted seams (documented, not composed):
//   - FUN_00437aa8 per-row scope-zoom transition warp (cosmetic).
//   - FUN_0041cb44 status-message flush (hudMsgPosts counts the
//     OOT_L%d posts the mission timer makes).
//   - the 0x540b20 per-level shade-LUT remap on the selected
//     inventory cell (raster-subsystem data; the border box IS drawn).
//   - FUN_0041664c/FUN_00416700 scope-entry palette fades.
//   - FUN_0045ee7c's window fills (pens 0/0x3c/0xf4): the frontend
//     presents them through the shotWinFill channel (pen-0 fills are
//     opaque black in the world fb — unrepresentable in the overlay).
//   - FUN_0046ec60 viewport registers (camera pose already carries
//     the scope rect — presentation-side).

#include "core/traversal_hud.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/bni_directory.h"
#include "core/traversal_runtime.h"

namespace mdk {

// OBSERVED position tables (0x49a87c / 0x49a8ac, DGROUP dump).
const int kHudSnipLPos[6][2] = {
    {12, 268}, {12, 288}, {12, 308}, {20, 320}, {24, 328}, {36, 336}};
const int kHudSnipWPos[6][2] = {
    {0, 256}, {0, 280}, {0, 300}, {4, 320}, {16, 336}, {32, 344}};

namespace {

// --- float/int constants (all file-verified) ------------------------
// 0x495210/0x495238/0x495250 — 2pi; 0x495240 — -2pi.
constexpr double kTwoPi = 6.28318530717958;
constexpr double kNegTwoPi = -6.28318530717958;
// 0x495230/0x495248 — pi (the wedge hemisphere mirror bound).
constexpr double kPiD = 3.14159265358979;
// 0x495258/0x495260 — the pi/2 dead-band (vertical ray -> step 0).
constexpr double kWedgeVertLo = 1.570482167529536;
constexpr double kWedgeVertHi = 1.571110486060254;
// 0x495268/0x495270 — the 0/pi dead-bands (horizontal saturate).
constexpr double kWedgeHorLo = 3.14159265358979e-05;
constexpr double kWedgeHorHi = 3.141561237663254;
// 0x40490fb1 / 0x40490fdb — the pi-split sentinel floats.
constexpr float kWedgePiLo = 3.141582727432251f;
constexpr float kWedgePiHi = 3.1415927410125732f;
// 0x40490fdb / 0x40c90fdb — float pi/2pi for the boundary compares
// (the original's branch inputs were f32; comparing against the
// double constants flips the hemisphere mirror at a == pi).
constexpr float kPiF = 3.1415927410125732f;
constexpr float kTwoPiF = 6.2831854820251465f;
// 0x495220 — (1-zoom)^2 scale multiplier; 0x495228 — 100.0 (zoom%).
constexpr double kZoomScaleK = 1.051939513477975;
// 0x49b6f4 — the inventory lerp step (frame-rate coupled, NOT
// deltaSec): 0.03333333507180214f == 1/30.
constexpr float kInvLerpStep = 0.03333333507180214f;
// 0x4978a8 / 0x4978ac — event-bar scale (two-step multiply).
constexpr float kEventBarScale = 500.0f;
constexpr float kEventBarInv = 1.0f / 900.0f;

// The original's x87 trunc-toward-zero (FUN_0047d59a — RC=11 FRNDINT;
// returns the value on the x87 stack, consumed by fistp).
inline int hudRint(double v) {
  return static_cast<int>(v);            // trunc toward zero
}
inline int hudRintf(float v) {
  return static_cast<int>(v);
}

// --- record/image helpers -------------------------------------------

inline std::uint16_t rd16(const void* p) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

// FUN_00403a00 form: {u16 w, u16 h} head, px at payload+4.
TraversalHudImage readHudImage(const void* rec) {
  TraversalHudImage im;
  if (!rec) return im;
  im.w = rd16(rec);
  im.h = rd16(static_cast<const std::uint8_t*>(rec) + 2);
  im.px = static_cast<const std::uint8_t*>(rec) + 4;
  return im;
}

// FUN_00416aa8 — inclusive rect fill, bounds-hardened.
void hudFillRect(IndexedFramebuffer& fb, int x0, int y0, int x1, int y1,
                 std::uint8_t pen) {
  if (x1 < x0 || y1 < y0) return;
  if (x1 < 0 || y1 < 0 || x0 >= fb.width() || y0 >= fb.height()) return;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= fb.width()) x1 = fb.width() - 1;
  if (y1 >= fb.height()) y1 = fb.height() - 1;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) fb.put(x, y, pen);
}

// FUN_00416a20 — hollow rect (top/bottom rows + side columns).
void hudHollowRect(IndexedFramebuffer& fb, int x0, int y0, int x1,
                   int y1, std::uint8_t pen) {
  for (int x = x0; x <= x1; ++x) {
    fb.put(x, y0, pen);
    fb.put(x, y1, pen);
  }
  for (int y = y0 + 1; y < y1; ++y) {
    fb.put(x0, y, pen);
    fb.put(x1, y, pen);
  }
}

// FUN_004185fc — sub-rect image blit: src stride = desc.w, skips
// pixels == transPen; the original performs NO clipping (this version
// bounds-checks each write — native hardening only, same observable
// output for in-bounds draws).
void blitSubRect(const TraversalHudImage& im, IndexedFramebuffer& fb,
                 int dstX, int dstY, int srcX, int srcY, int w, int h,
                 std::uint8_t transPen) {
  if (!im.px || w <= 0 || h <= 0) return;
  for (int y = 0; y < h; ++y) {
    const int dy = dstY + y, sy = srcY + y;
    if (dy < 0 || dy >= fb.height() || sy < 0 || sy >= im.h) continue;
    for (int x = 0; x < w; ++x) {
      const int dx = dstX + x, sx = srcX + x;
      if (dx < 0 || dx >= fb.width() || sx < 0 || sx >= im.w) continue;
      const std::uint8_t p = im.px[sy * im.w + sx];
      if (p != transPen) fb.put(dx, dy, p);
    }
  }
}

// The same full-image form: srcRect = the descriptor itself.
void blitImage(const TraversalHudImage& im, IndexedFramebuffer& fb,
               int dstX, int dstY) {
  blitSubRect(im, fb, dstX, dstY, 0, 0, im.w, im.h, 0);
}

// FUN_004181c0 — the SNIP_TXT centered digit printer. Decimal digits
// are 8px column slices of SNIP_TXT (digit d at src x = d*8). Left
// edge = centerX - ndigits*4; value >= 1000 prints 999; negative
// values print nothing (checked before EVERY digit — the original
// terminates the loop the same way).
void hudPrintCentered(const TraversalHudState& hud, int value,
                      int centerX, int y, IndexedFramebuffer& fb) {
  int v = value, penX, div;
  if (v >= 1000) {
    v = 999;
    penX = centerX - 12;                 // 0x4181db
    div = 100;
  } else if (v >= 100) {
    penX = centerX - 12;
    div = 100;
  } else if (v >= 10) {
    penX = centerX - 8;                  // 0x41826d
    div = 10;
  } else {
    penX = centerX - 4;                  // 0x41827d
    div = 1;
  }
  for (;;) {
    if (v < 0) return;                   // 0x4181ed — suppress
    const int digit = v / div;
    if (div == 1)
      v = -1;                            // 0x418207 — last digit
    else
      v -= digit * div;                  // 0x41828d
    // SNIP_TXT slice: {w=8, h=snipTxt.h} at src x = digit*8.
    blitSubRect(hud.snipTxt, fb, penX, y, digit * 8, 0, 8,
                hud.snipTxt.h, 0);
    penX += 8;
    if (div == 1) return;
    div /= 10;
  }
}

// FUN_0040c7c0 — the SNIPERS2 u16 pen-command stream, decoded once
// into a 600x360 layer (untouched pixels stay 0 = transparent; the
// real stream never writes pen 0 — transparency is carried by skips,
// verified against BUILD_A's record: 216000 px exactly).
//   0x0000..0x7fff  literal packet: count DWORDS = count*4 px bytes
//   0x8000..0xfeff  skip: advance (w & 0x0fff) dest bytes
//   0xff00          end of stream
//   0xff01..0xff03  inline literal (1..3 bytes follow, copied
//                   verbatim — the original's unrolled copy)
void decodeSnipers2(const std::byte* stream, std::size_t avail,
                    std::vector<std::uint8_t>& out) {
  out.assign(kHudScreenW * kHudScreenH, 0);
  const auto* s = reinterpret_cast<const std::uint8_t*>(stream);
  std::size_t p = 0, dst = 0;
  for (;;) {
    if (p + 2 > avail) break;            // hardening: pad/truncation
    const std::uint16_t ax =
        static_cast<std::uint16_t>(s[p] | (s[p + 1] << 8));
    p += 2;
    if (ax & 0x8000) {
      if ((ax & 0xff00) == 0xff00) {
        const int n = ax & 0xff;
        if (n == 0) break;
        for (int i = 0; i < n && p < avail; ++i) {
          if (dst < out.size()) out[dst] = s[p];
          ++dst;
          ++p;
        }
      } else {
        dst += (ax & 0x0fff);
      }
    } else {
      const std::size_t n = static_cast<std::size_t>(ax) * 4;
      for (std::size_t i = 0; i < n && p + i < avail; ++i) {
        if (dst + i < out.size()) out[dst + i] = s[p + i];
      }
      dst += n;
      p += n;
    }
  }
}

// FUN_004182a0's pair normalization: wrap f into [0, 2pi).
float wedgeNormalize(float f) {
  double a = f;                          // f32->f64 store/fetch like
  while (a < 0.0) a += kTwoPi;           // the original's stack loop
  while (a > kTwoPiF) a += kNegTwoPi;
  return static_cast<float>(a);
}

// FUN_00418378's ray-edge step (16.16 int): dead-band table then the
// cotangent term rint(65536 / tan(a)).
int wedgeEdgeStep(float a, int w) {
  if (a >= kWedgeVertLo && a <= kWedgeVertHi) return 0;
  if (a < kWedgeHorLo) return w << 16;
  if (a > kWedgeHorHi) return -static_cast<int>(w << 16);
  return hudRint(65536.0 / std::tan(static_cast<double>(a)));
}

// FUN_00418378 — stamp the wedge between ray angles a1 (larger = the
// "left"/cur edge) and a2 (smaller = the "right"/latch edge), drawing
// SC_BSTAT's nonzero mask pixels into the statWork copy. Rows walk
// from the image center: -w stride for angles <= pi (upper half), +w
// after the mirror through 2pi (angles > pi swap+reflect).
void wedgeStamp(TraversalHudState& hud, float a1, float a2) {
  const int w = hud.scStat.w, h = hud.scStat.h;
  const int mw = hud.scBstat.w, mh = hud.scBstat.h;
  if (!hud.scBstat.px || hud.statWork.empty() || w <= 0 || h <= 0)
    return;
  int rowStride = -w;                    // upper hemisphere default
  float e1 = a1, e2 = a2;
  if (e1 > kPiF || e2 > kPiF) {          // 0x4184d1 — mirror through 2pi
    const float n1 = static_cast<float>(kTwoPi - e2);
    const float n2 = static_cast<float>(kTwoPi - e1);
    e1 = n1;
    e2 = n2;
    rowStride = w;
  }
  const int step1 = wedgeEdgeStep(e1, w);   // [ebp-0x30] — cur edge
  const int step2 = wedgeEdgeStep(e2, w);   // [ebp-0x2c] — latch edge
  int edge1 = (w >> 1) << 16;               // edi — a1 side
  int edge2 = (w >> 1) << 16;               // esi — a2 side
  int maskRow = (mh >> 1) * mw;             // [ebp-0x1c] row offset
  int statRow = (h >> 1) * w;               // [ebp-0x20] row offset
  for (int rows = 1; rows < h; rows += 2) { // [ebp-0x18] += 2 (OBSERVED)
    edge1 += step1;
    edge2 += step2;
    if (edge1 < 0)
      edge1 = 0;                            // 0x4185ac — left stick
    if (edge1 >= (w << 16)) break;          // 0x418482 — a1 past right
    if (edge2 < 0) break;                   // 0x41848a — a2 past left
    if (edge2 >= (w << 16))                 // 0x4185b3 — right stick
      edge2 = (w << 16) - 1;
    const int c1 = edge1 >> 16, c2 = edge2 >> 16;
    const int span = c2 - c1;
    if (span != -1) {                       // 0x4184bd — the -1 skip
      for (int i = 0; i <= span; ++i) {     // mask gates the write
        const int col = c1 + i;
        if (col < 0 || col >= w || col >= mw) continue;
        const std::size_t midx =
            static_cast<std::size_t>(maskRow + col);
        const std::size_t sidx =
            static_cast<std::size_t>(statRow + col);
        if (midx >= static_cast<std::size_t>(mw) * mh ||
            sidx >= static_cast<std::size_t>(w) * h)
          continue;
        const std::uint8_t b = hud.scBstat.px[midx];
        if (b) hud.statWork[sidx] = b;
      }
    }
    maskRow += rowStride == -w ? -mw : mw;  // bstat row per stat row
    statRow += rowStride;
  }
}

// FUN_004182a0 — normalize both angles, skip cur<=latch, split the
// interval at pi's dead-band when it straddles.
void wedgeStampNorm(TraversalHudState& hud, float cur, float latch) {
  cur = wedgeNormalize(cur);
  latch = wedgeNormalize(latch);
  if (cur <= latch) return;              // 0x418329
  const bool curGtPi = cur > kPiF;
  const bool latchGtPi = latch > kPiF;
  if (curGtPi && !latchGtPi) {           // 0x418347 — split at pi
    wedgeStamp(hud, kWedgePiLo, latch);
    wedgeStamp(hud, cur, kWedgePiHi);
  } else {
    wedgeStamp(hud, cur, latch);
  }
}

// FUN_00403a40 + FUN_0046d680 — the scaled (DDA nearest-neighbor)
// blit used for SKULL. effW = (srcW*dstW)>>8 / effH = (srcH*dstH)>>8;
// dst rect centered at (cx,cy) minus the view scroll, clipped to the
// view rect; source steps (srcW<<16)/effW per dest px; pen 0 skipped.
void blitScaled(IndexedFramebuffer& fb, int cx, int cy, int dstW,
                int dstH, const TraversalHudImage& src, int viewOX,
                int viewOY, int viewW, int viewH) {
  if (!src.px) return;
  int effW = static_cast<int>(
      (static_cast<std::int64_t>(src.w) * dstW) >> 8);
  int effH = static_cast<int>(
      (static_cast<std::int64_t>(src.h) * dstH) >> 8);
  if (effW <= 0 || effH <= 0) return;
  const int stepX = static_cast<int>(
      (static_cast<std::int64_t>(src.w) << 16) / effW);
  const int stepY = static_cast<int>(
      (static_cast<std::int64_t>(src.h) << 16) / effH);
  int x = cx - (effW >> 1) - viewOX;      // 0x403a85..0x403aa2
  int srcX0 = 0;
  if (x < 0) {                            // 0x403bbf — left clip
    effW += x;
    if (effW <= 0) return;
    srcX0 = -x * stepX;
    x = 0;
  }
  int y = cy - (effH >> 1) - viewOY;      // 0x403aaf..0x403acf
  std::int64_t srcY0 = 0;
  if (y < 0) {                            // 0x403bed — top clip
    effH += y;
    if (effH <= 0) return;
    srcY0 = -static_cast<std::int64_t>(y) * stepY;
    y = 0;
  }
  if (x >= viewW || y >= viewH) return;   // 0x403aed..0x403afb
  if (effW > viewW - x) effW = viewW - x; // 0x403c13 arm
  if (effH > viewH - y) effH = viewH - y;
  std::int64_t srcY = srcY0;
  for (int r = 0; r < effH; ++r) {
    const int sy = static_cast<int>(srcY >> 16);
    std::int64_t srcX = srcX0;
    const int dy = y + r + viewOY;        // scroll folds back in —
    for (int c = 0; c < effW; ++c) {      // net pos (cx-effW/2, ...)
      const int sx = static_cast<int>(srcX >> 16);
      if (sy < src.h && sx < src.w) {
        const std::uint8_t p = src.px[sy * src.w + sx];
        if (p != 0) fb.put(x + c + viewOX, dy, p);
      }
      srcX += stepX;
    }
    srcY += stepY;
  }
}

// The scoped-operation gate (OBSERVED at every gated site).
inline bool scopedHud(const TraversalRuntime& rt) {
  return rt.flagC9c != 0 && rt.transitionPhase > 1;
}

} // namespace

// ---------------------------------------------------------------------------
// Bind — FUN_00433d40's binds + FUN_00418688's 21-name table build.
// ---------------------------------------------------------------------------

void traversalHudBind(TraversalRuntime& rt) {
  TraversalHudState& hud = rt.hud;
  hud = TraversalHudState{};             // fresh BSS state per level

  if (!rt.level.travsprtBytes.empty()) {
    const BniDirectory bd = inspectBniDirectory(
        std::span<const std::byte>(rt.level.travsprtBytes));
    if (bd.status == BniDirectoryStatus::kOk) {
      const auto base = rt.level.travsprtBytes.data();
      const auto payload = [&](const char* name,
                               std::size_t* sizeOut = nullptr)
          -> const std::uint8_t* {
        const BniRecord* r = findBniRecord(bd, name);
        if (!r) return nullptr;
        if (sizeOut) *sizeOut = r->payloadSize();
        return reinterpret_cast<const std::uint8_t*>(base) +
               r->payloadFileOffset;
      };
      const auto image = [&](const char* name) {
        const auto* p = payload(name);
        return p ? readHudImage(p) : TraversalHudImage{};
      };
      // FUN_00418688 table subset (entries 2..19 of the 21-name
      // table at 0x49a7d4) — image records {w,h,px@+4}.
      hud.scStat = image("SC_STAT");      // [2]
      hud.scBstat = image("SC_BSTAT");    // [3]
      hud.snipRng = image("SNIP_RNG");    // [5]
      hud.snipWep = image("SNIP_WEP");    // [6]
      hud.snipTxt = image("SNIP_TXT");    // [7]
      for (int i = 0; i < 6; ++i) {       // [8..13] / [14..19]
        char nm[10];
        std::snprintf(nm, sizeof nm, "SNIP_L%d", i + 1);
        hud.snipL[i] = image(nm);
        std::snprintf(nm, sizeof nm, "SNIP_W%d", i + 1);
        hud.snipW[i] = image(nm);
      }
      hud.skull = image("SKULL");         // the 0x4978a0 slot

      // K_-style sprite tables at payload+4 (FUN_004039d8/ec bind).
      const auto sprite = [&](const char* name, FtiSprite& out) {
        std::size_t sz = 0;
        if (const auto* p = payload(name, &sz); p && sz >= 4) {
          std::string err;
          if (auto s = decodeSpriteTable(
                  std::span<const std::byte>(
                      reinterpret_cast<const std::byte*>(p + 4),
                      sz - 4),
                  &err))
            out = std::move(*s);
        }
      };
      sprite("CROSS", hud.cross);
      sprite("BOMBTARG", hud.bombtarg);
      sprite("SNIPERGA", hud.sniperga);
      sprite("PICKUPS", hud.pickups);

      // SNIPERS2 — the 0x54c684 u16 pen-command stream at payload+4.
      if (std::size_t sz = 0;
          const auto* p = payload("SNIPERS2", &sz)) {
        if (sz >= 4)
          decodeSnipers2(
              reinterpret_cast<const std::byte*>(p + 4), sz - 4,
              hud.overlayPx);
      }
      // SNIPERS1 — the 640x480 bezel verbatim at payload+0
      // (FUN_004039c8 raw bind; presented by the frontend).
      if (std::size_t sz = 0;
          const auto* p = payload("SNIPERS1", &sz))
        hud.bezelPx.assign(p, p + sz);

      hud.bound = hud.scStat.px && hud.snipTxt.px &&
                  !hud.cross.frames.empty() && !hud.overlayPx.empty();
    }
  }

  // SC_STAT's mutable working copy — FUN_00418378 stamps into it.
  if (hud.scStat.px)
    hud.statWork.assign(hud.scStat.px,
                        hud.scStat.px + hud.scStat.w * hud.scStat.h);

  // FUN_00433d40's difficulty-keyed mission-timer init (OBSERVED):
  //   0x479e3400 = 81000.0 / 0x4752f000 = 54000.0 /
  //   0x470ca000 = 36000.0 — all three timer fields (a0/a4/a8).
  float init = 1000.0f;
  if (rt.difficulty == 0) init = 81000.0f;
  else if (rt.difficulty == 1) init = 54000.0f;
  else if (rt.difficulty == 2) init = 36000.0f;
  rt.fadeTimer5414a0 = init;
  rt.fadeTimer5414a4 = init;
  rt.fadeTimer5414a8 = init;
}

void traversalHudBindFontBig(TraversalRuntime& rt,
                             const FtiFont& font) {
  rt.hud.fontBig = font;
  rt.hud.fontBigOk = true;
}

// ---------------------------------------------------------------------------
// FUN_0041b654 — the mission countdown (call site: the FUN_00436100
// head, gated fieldE9c == 0; caller mirrors that here).
// ---------------------------------------------------------------------------

void traversalHudMissionTick(TraversalRuntime& rt) {
  TraversalHudState& hud = rt.hud;
  // OBSERVED gates: a0 <= 0 is dead; level 5 (0x541498) and the
  // master-move gate 0x540d9c skip the decrement.
  if (rt.fadeTimer5414a0 <= 0.0f) return;
  if (rt.field541498 == 5) return;
  if (rt.masterMoveGate) return;
  // 0x49b6f0 == 1.0f — fixed -1.0 per call, frame-rate coupled.
  rt.fadeTimer5414a0 -= 1.0f;
  if (rt.fadeTimer5414a0 > 0.0f) return;
  // Expiry (OBSERVED): clamp, camera shake arm >= 5.0, the OOT_L%d
  // status post (seam), and the 0x540d9b flag byte — |= 0x20 when its
  // bit7 (scriptGFlags byte3 bit7) is set, else |= 0x40.
  rt.fadeTimer5414a0 = 0.0f;
  if (rt.camera.shakeMag < 5.0f) rt.camera.shakeMag = 5.0f;
  ++rt.seams.hudMsgPosts;                // FUN_0041cad0 OOT_L%d — seam
  const bool bit7 = (rt.scriptGFlags & 0x80000000u) != 0;
  rt.scriptGFlags |= bit7 ? 0x20000000u : 0x40000000u;
  hud.fieldD9b = static_cast<std::uint8_t>(rt.scriptGFlags >> 24);
}

// ---------------------------------------------------------------------------
// FUN_00469f7c — the per-frame inventory update. OBSERVED: count==0
// exits before the lerp; scoped (c9c && ca0 — the weaker != 0 form)
// pins 0x541558 to 0; unscoped re-arms it to 60; the record lerp
// (FUN_00469e94, fixed 1/30 step) runs in BOTH paths; the tail
// decrements 0x541558 by frameStep (clamp 0).
// ---------------------------------------------------------------------------

void traversalHudUpdate(TraversalRuntime& rt) {
  TraversalHudState& hud = rt.hud;
  if (rt.inventoryCount == 0) return;    // 0x469f8d
  const bool scoped = rt.flagC9c != 0 && rt.transitionPhase != 0;
  if (scoped)
    rt.invHudTimer = 0;                  // 0x469fb4
  else
    rt.invHudTimer = kHudInvTimerRearm;  // 0x46a0ba — 60

  for (int i = 0; i < rt.inventoryCount && i < 5; ++i) {
    InventoryRecord& rec = rt.inventory[i];
    // FUN_00469e94 — id==0 early-out; the combined X/Y arrival test
    // (slotX/slotY as f32 == animX/animY) skips the integrator
    // INSIDE the call — the draw path below still runs for settled
    // records.
    if (rec.id == 0) continue;
    if (rec.animX != static_cast<float>(rec.slotX) ||
        rec.animY != static_cast<float>(rec.slotY)) {
      rec.animX += rec.animVel * kInvLerpStep;
      rec.animY += rec.animAux * kInvLerpStep;
      // Sign-based overshoot clamps (velocity direction picks the
      // comparison) — symmetric across both axes.
      if (rec.animVel > 0.0f) {
        if (rec.animX > static_cast<float>(rec.slotX))
          rec.animX = static_cast<float>(rec.slotX);
      } else if (rec.animVel < 0.0f) {
        if (rec.animX < static_cast<float>(rec.slotX))
          rec.animX = static_cast<float>(rec.slotX);
      }
      if (rec.animAux > 0.0f) {
        if (rec.animY > static_cast<float>(rec.slotY))
          rec.animY = static_cast<float>(rec.slotY);
      } else if (rec.animAux < 0.0f) {
        if (rec.animY < static_cast<float>(rec.slotY))
          rec.animY = static_cast<float>(rec.slotY);
      }
    }
    // id==6 charge sync (0x46a024) — reads 0x54161f == ammo[0]. The
    // write sits INSIDE the draw path: scoped frames (timer 0) skip it.
    if (rt.invHudTimer != 0 && rec.id == 6) rec.charges = rt.ammo[0];
  }

  // Tail: invHudTimer -= frameStep, clamp 0 (0x46a07c..0x46a176).
  rt.invHudTimer -= hud.frameStep;
  if (rt.invHudTimer < 0) rt.invHudTimer = 0;
}

// ---------------------------------------------------------------------------
// The composed tail — the whole FUN_00436d60 draw side in order.
// ---------------------------------------------------------------------------

void traversalHudCompose(TraversalRuntime& rt) {
  TraversalHudState& hud = rt.hud;
  IndexedFramebuffer& fb = hud.fb;
  fb.clear(0);                           // pen 0 = transparent

  const auto centered = [&](const FtiSprite& tab, std::size_t f,
                            int x, int y) {
    if (const FtiSpriteFrame* fr = tab.frame(f))
      blitFtiSpriteFrame(*fr, fb, x, y);  // FUN_00409760 + FUN_00415ff0
  };

  // --- scope gate A (c9c && ca0>1): FUN_0045f030(0)'s per-slot mode-0
  //     path re-renders the in-flight projectile model into the shot
  //     windows — world/display-list side, no overlay pixels. The pen
  //     fills stay on the shotWinFill channel (see file head). -------

  // --- FUN_00469f7c's draw side (inside the tail's order): the
  //     selection border + deferred LUT remap run in the unscoped
  //     pre-branch (0x46a0e7..0x46a169) BEFORE the record loop draws
  //     icons/counts. Draw gate: invHudTimer != 0 (scoped pins 0). ---
  if (rt.inventoryCount != 0 && rt.invHudTimer != 0) {
    // Selection border (OBSERVED): complete 48px outline at
    // (8 + 48*sel, 304)..(x0+47, 351), pen 1 — drawn only when the
    // selected record's id != 6 (0x46a0c7's early-out). The 46x46
    // interior shade-LUT remap runs there too — counted seam.
    const int sel = rt.inventorySel;
    if (sel >= 0 && sel < rt.inventoryCount && sel < 5 &&
        rt.inventory[sel].id != 6) {
      const int x0 = kHudInvCellX + kHudInvCellSize * sel;
      hudHollowRect(fb, x0, kHudInvCellY, x0 + kHudInvCellSize - 1,
                    kHudInvCellY + kHudInvCellSize - 1, 1);
      ++rt.seams.hudLutRemaps;           // 0x540b20 remap — counted,
    }                                    // not reproduced
    for (int i = 0; i < rt.inventoryCount && i < 5; ++i) {
      const InventoryRecord& rec = rt.inventory[i];
      if (rec.id == 0) continue;
      const int rx = hudRintf(rec.animX);
      const int ry = hudRintf(rec.animY);
      if (const FtiSpriteFrame* f = hud.pickups.frame(
              static_cast<std::size_t>(rec.id)))
        blitFtiSpriteFrame(*f, fb, rx, ry);
      if (rec.charges > 1)
        hudPrintCentered(hud,
                         rec.charges > 999 ? 999 : rec.charges, rx,
                         ry - 12, fb);
    }
  }

  // --- scope gate B (c9c && ca0>1): FUN_0045f030(1) — the SNIPERGA
  //     gauges + the +0xf4 remnant counters (FUN_0045ee7c mode 1), then
  //     FUN_00436f08 (CROSS frame 0 at (299,219) + the SNIPERS2 u16
  //     stream) which additionally gates internally on 0x5414d4. -----
  if (scopedHud(rt)) {
    for (int i = 0; i < 3; ++i) {
      PlayerShot& s = rt.shots[i];
      // FUN_0045ee7c's frameSel: -1 in-flight; 0 free; 3 state-4
      // expired; 0x3c state-3; 0xf4 other expired. Only state-4
      // expired slots take the SNIPERGA path (the rest ret after
      // their pen fill — deferred to the shotWinFill channel).
      if (s.state != 4 || s.lifetime > 0) continue;
      const FtiSpriteFrame* g =
          hud.sniperga.frame(static_cast<std::size_t>(
              (static_cast<std::uint32_t>(s.remnantIdx) >> 1)));
      if (g) {
        // Per-slot window centers (0x45f030's arg triple): the
        // FUN_00409760 centered draw lands the hotspot here.
        static constexpr int kGaugeX[3] = {141, 297, 453};
        static constexpr int kGaugeY[3] = {44, 34, 44};
        blitFtiSpriteFrame(*g, fb, kGaugeX[i], kGaugeY[i]);
      }
      // The +0xf4 remnant tick — OBSERVED: draw first, then += min
      // (frameStep, 2) capped 2*count-1, gated 0x5414d4 != 0 &&
      // (541548 == 0 || 4999d0 == 1).
      if (rt.hudActive != 0 &&
          (!rt.flag541548 || rt.flag4999d0)) {
        const int step = hud.frameStep > 2 ? 2 : hud.frameStep;
        const int cap = static_cast<int>(hud.sniperga.frames.size()) *
                            2 - 1;
        s.remnantIdx += step;
        if (s.remnantIdx > cap) s.remnantIdx = cap;
      }
    }
    // FUN_00436f08 — CROSS frame 0 at (299,219) then the SNIPERS2
    // u16 stream into the whole overlay (predecoded layer at bind);
    // internal gate: 0x5414d4 != 0.
    if (rt.hudActive != 0) {
      centered(hud.cross, 0, kHudScopeCrossX, kHudScopeCrossY);
      const std::uint8_t* o = hud.overlayPx.data();
      const std::size_t n = hud.overlayPx.size() <
              static_cast<std::size_t>(fb.pixelCount())
          ? hud.overlayPx.size()
          : static_cast<std::size_t>(fb.pixelCount());
      std::uint8_t* d = fb.pixels();
      for (std::size_t i = 0; i < n; ++i)
        if (o[i] != 0) d[i] = o[i];
    }
  }

  // --- Level-3 scripted sphere ride (player_sphere.h): the center
  //     reticle + right-edge ammunition ladder "x N" — OBSERVED
  //     (SPHERE_ENTRY.md section 2). The porthole mask itself is the
  //     pod's cockpit geometry (frontend presentation; X_STRIKB is a
  //     separate model/anim decode). Ladder position HYPOTHESIS —
  //     right-edge mid per the video. Distinct from the sniper scope
  //     and the class-4 object mount.
  if (rt.spherePhase == 2) {
    centered(hud.cross, 0, hudRintf(rt.motion.moveVel),
             hudRintf(rt.motion.strafeVel));
    if (hud.fontBigOk) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "x %d", rt.bombs);
      const int w = measureFtiText(hud.fontBig, buf,
                                   kFtiFontBigMissingAdvance);
      drawFtiText(hud.fontBig, buf, fb, kHudScreenW - w - 8,
                  kHudBombTargY, kFtiFontBigMissingAdvance);
    }
  }

  // --- FUN_0046911c — mounted reticle. Caller gate (0x436f37):
  //     excludeObj != 0 && byte[0x540e72] & 4 == mountClass & 0x40000.
  //     Inner gate: suppress when object +0x14b bit2 set. ------------
  if (rt.cs.excludeObj && (rt.mountClass & 0x40000u) != 0 &&
      (rt.cs.excludeObj->flags14b & 4) == 0) {
    centered(hud.bombtarg, 0, kHudBombTargX, kHudBombTargY);
    centered(hud.cross, 0, hudRintf(rt.motion.moveVel),
             hudRintf(rt.motion.strafeVel));
    // Bombs via FONTBIG right-aligned at (472,56): sprintf("%d") +
    // FUN_00414be8 measure + FUN_00414c34 draw.
    if (hud.fontBigOk) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "%d", rt.bombs);
      const int w = measureFtiText(hud.fontBig, buf,
                                   kFtiFontBigMissingAdvance);
      drawFtiText(hud.fontBig, buf, fb,
                  kHudBombTextRightX - w, kHudBombTextY,
                  kFtiFontBigMissingAdvance);
    }
  }

  // --- hudActive block: event bars, then FUN_00417e20. -------------
  if (rt.hudActive) {
    // Event bars (0x436f5b): 0 >= eventTimer skips. Width =
    // rint(field * 500.0 * (1/900)) two-step multiply, clamp [0,500].
    // Solid fill x[0..w1] rows 4..10 pen 3; hollow box x[0..w2] pen 4.
    if (rt.eventTimer > 0.0f && rt.eventTimerObj) {
      const DynamicObject* o = rt.eventTimerObj;
      int w1 = hudRint(static_cast<float>(o->health) *
                       kEventBarScale * kEventBarInv);
      int w2 = hudRint(static_cast<float>(
                           o->healthMirror2a2 & 0xffffu) *
                       kEventBarScale * kEventBarInv);
      if (w1 < 0) w1 = 0;
      if (w1 > kHudEventBarMax) w1 = kHudEventBarMax;
      if (w2 < 0) w2 = 0;
      if (w2 > kHudEventBarMax) w2 = kHudEventBarMax;
      hudFillRect(fb, 0, kHudEventBarY, w1,
                  kHudEventBarY + kHudEventBarH - 1, 3);
      hudHollowRect(fb, 0, kHudEventBarY, w2,
                    kHudEventBarY + kHudEventBarH - 1, 4);
    }

    // ---- FUN_00417e20 — mission pie + health digits + tail. -------
    // Angles (double math): cur = (1 - a0/a4)*2pi,
    // latch = (1 - a8/a4)*2pi; stamp when cur > latch + eps
    // (0x495218 = pi/180); a8 = a0 after the stamp.
    {
      // Direct division (the original's fdiv+FSUBRP): identical a/a4
      // ratios are exactly 1.0 — a multiply-by-inverse leaves a tiny
      // negative that normalize() wraps to 2pi.
      const double a4 = static_cast<double>(rt.fadeTimer5414a4);
      const float cur = static_cast<float>(
          (1.0 - static_cast<double>(rt.fadeTimer5414a0) / a4) *
          kTwoPi);
      const float latch = static_cast<float>(
          (1.0 - static_cast<double>(rt.fadeTimer5414a8) / a4) *
          kTwoPi);
      if (cur > latch + kHudWedgeEpsilon) {
        wedgeStampNorm(hud, cur, latch);
        rt.fadeTimer5414a8 = rt.fadeTimer5414a0;
      }
      // SC_STAT blit at (600-w-16, 360-h-10) from the working copy.
      const int sx = kHudScreenW - hud.scStat.w - kHudPieMarginX;
      const int sy = kHudScreenH - hud.scStat.h - kHudPieMarginY;
      if (!hud.statWork.empty()) {
        TraversalHudImage work = hud.scStat;
        work.px = hud.statWork.data();
        blitImage(work, fb, sx, sy);
      }
      // blinkPhase += frameStep & 0x1f — suppressed only when the
      // 0x541544 secondary path AND 0x4999d0 both hold (steady-state
      // 0 here -> always advances).
      if (!(rt.field541544 && rt.flag4999d0))
        hud.blinkPhase = (hud.blinkPhase + hud.frameStep) & 0x1f;
      // Health digits (centered in SC_STAT): gated by the damage
      // window (0x540e10 > 0 draws only on ODD 0x5414d8 frames) and
      // the low-health blink (health <= 20 draws only while
      // blinkPhase <= 15).
      const bool dmgOk =
          rt.fieldE10 <= 0.0f || (rt.field5414d8 & 1) != 0;
      const bool blinkOk = rt.fieldHealth > kHudHealthBlinkThreshold ||
                           hud.blinkPhase <= 15;
      if (dmgOk && blinkOk && hud.scStat.px) {
        const int cx = sx + (hud.scStat.w >> 1);
        const int cy = sy + ((hud.scStat.h - hud.snipTxt.h) >> 1);
        hudPrintCentered(hud, rt.fieldHealth, cx, cy, fb);
      }

      // ---- scoped tail (0x417f3a): c9c && ca0>1 && mode 3 ---------
      if (scopedHud(rt)) {
        // Zoom scale: s = clamp((1-zoom)^2 * 1.051939513477975, 0,1)
        // (double math per the originals' fmul/fcomp chain).
        const double z =
            (1.0 - static_cast<double>(rt.camera.zoom));
        double scale = z * z * kZoomScaleK;
        if (scale < 0.0) scale = 0.0;
        if (scale > 1.0) scale = 1.0;
        // Zoom% digits centered (564,155), value rint(s*100).
        hudPrintCentered(hud, hudRint(scale * 100.0),
                         kHudZoomTxtX, kHudZoomTxtY, fb);
        // SNIP_RNG latch: target = rint(desc.h * s); the latch moves
        // toward it by +-3*frameStep without overshooting, then the
        // bottom `latch` rows of the image draw at x=552 inside the
        // y=176..176+h window.
        const int target =
            hudRint(static_cast<double>(hud.snipRng.h) * scale);
        int latchv = hud.rngLatch;
        const int step3 = 3 * hud.frameStep;
        if (target >= latchv) {
          latchv += step3;
          if (latchv > target) latchv = target;
        } else {
          latchv -= step3;
          if (latchv < target) latchv = target;
        }
        hud.rngLatch = latchv;
        if (latchv != 0) {
          const int rows = hud.snipRng.h - latchv;   // src/dst offset
          blitSubRect(hud.snipRng, fb, kHudRngX,
                      kHudRngY + rows, 0, rows,
                      hud.snipRng.w, latchv, 0);
        }
        // SNIP_WEP at (112,304).
        blitImage(hud.snipWep, fb, kHudWepX, kHudWepY);
        // SNIP_W[i] at the 0x49a8ac pairs: i==0 always; i>0 only
        // while ammo[i] != 0. The selected weapon's ammo prints
        // centered at (64,315) — value -1 for i==0 (suppressed).
        for (int i = 0; i < 6; ++i) {
          int v = -1;
          if (i > 0) {
            v = rt.ammo[i];
            if (v == 0) continue;
          }
          blitImage(hud.snipW[i], fb, kHudSnipWPos[i][0],
                    kHudSnipWPos[i][1]);
          if (i == rt.wpnSel1)
            hudPrintCentered(hud, v, kHudAmmoTxtX,
                             kHudAmmoTxtY, fb);
        }
        // SNIP_L[wpnSel1] at the 0x49a87c pairs.
        if (rt.wpnSel1 >= 0 && rt.wpnSel1 < 6) {
          const int s = rt.wpnSel1;
          blitImage(hud.snipL[s], fb, kHudSnipLPos[s][0],
                    kHudSnipLPos[s][1]);
        }
      }
    }
  }

  // --- FUN_0041cb44 — status-message flush: counted seam (the text
  //     render needs the unported message queue). --------------------
  ++rt.seams.hudMsgFlush;

  // --- SKULL death overlay (0x437016): health == 0 && dac > 0 &&
  //     locoState == 0x3ea — scaled 256x256 -> dac x dac centered
  //     (300,180) via the FUN_00403a40 DDA blit. ----------------------
  if (rt.fieldHealth == 0 && rt.fieldDac > 0 &&
      rt.locoState == 0x3ea) {
    const int vw = rt.camera.pose.viewW > 0 ? rt.camera.pose.viewW
                                            : kHudScreenW;
    const int vh = rt.camera.pose.viewH > 0 ? rt.camera.pose.viewH
                                            : kHudScreenH;
    blitScaled(fb, kHudSkullCx, kHudSkullCy, rt.fieldDac, rt.fieldDac,
               hud.skull, rt.camera.pose.viewOX, rt.camera.pose.viewOY,
               vw, vh);
  }
}

} // namespace mdk
