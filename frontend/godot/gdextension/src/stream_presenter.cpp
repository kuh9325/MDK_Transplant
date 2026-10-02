// Phase 19B.1 — Mode-5 presentation seams. See stream_presenter.h.
// Every blit here ports an OBSERVED native body; the event payloads
// are the authoritative boundary (core semantics are CLOSED — this
// file only consumes them).

#include "stream_presenter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace mdkbridge {

StreamPresenter::StreamPresenter() : fb_(600, 360) { fb_.clear(0); }

void StreamPresenter::bindImage(int tag, mdk::IndexedImage img) {
  images_[tag] = std::move(img);
}

void StreamPresenter::bindFonts(const mdk::FtiFont& fontBig,
                                const mdk::FtiFont& fontSml) {
  fontBig_ = fontBig;
  fontSml_ = fontSml;
}

void StreamPresenter::bindRibbonPalette(const std::uint8_t* pal768) {
  ribbon_.bindPalette(pal768);
}

void StreamPresenter::bindModelResolver(
    std::function<const mdk::DynamicObject*(int)> fn) {
  modelResolver_ = std::move(fn);
}

void StreamPresenter::reset() {
  fb_.clear(0);
  palette_ = mdk::Palette{};
  images_.clear();
  fontBig_.reset();
  fontSml_.reset();
  ribbon_.bindPalette(nullptr);
  modelResolver_ = nullptr;
  pendingPolys_.clear();
  modelVerts_.clear();
  diag_ = StreamPresenterDiag{};
  framePending_ = false;
}

// ------------------------------------------------------------------
// Phase 19B.2B1 — the mode-5 model submitter.
//
// OBSERVED chain (p19a_asm5.txt / g1_poly.txt):
//   FUN_0042e100 record build — objects with +0x0c != 0 become flag-0
//   records with callback FUN_00455e24. In mode 5 (0x541500 != 0)
//   FUN_00409a00 calls the callback inline during the chain walk —
//   FUN_00455e24 -> FUN_0040c3a0 — which PUSHES each winding-passing
//   tri into the shared 0x10-byte record table; FUN_0040c694 then
//   sorts the table by the mode-0 key (z'-sum, descending via the
//   40bd2c i32 comparator) and calls FUN_0040ca00 -> dispatch.
//
// Per element (not masked by object +0x2c8 — mode-5 records carry
// rec+0x2c == 0 so that is always the mask source):
//   FUN_0040bc34 allocates a transformed-vert scratch -> elem +0x1c;
//   FUN_0046b4f8 fills each vert (6-float record) through the event's
//   composed camProj-o-xform 3x4.
// Per tri (0x24-byte record: u16 idx @ +0/2/4, i16 pen @ +0x06,
// float2 uv @ +0x08/+0x10/+0x18):
//   flags-OR & 0x10 == 0 -> winding = the 2D projected cross
//     (v2.sy-v0.sy)*(v1.sx-v0.sx) - (v2.sx-v0.sx)*(v1.sy-v0.sy)
//   else -> winding = v1 . ((v1-v0) x (v2-v1))  (3D plane sign)
//   winding >= 0 (NaN rejects) -> push {key=z0+z1+z2, pen, verts}.
// ------------------------------------------------------------------
namespace {

// The 0c860 arm id (spec census A..G) for a raw pen scalar — same
// compare chain as branchForPen, with the two flat arms split.
int modelMatClass(int pen) {
  switch (StreamRibbonRaster::branchForPen(pen)) {
    case StreamTriBranch::kMaterial: return 0;      // A textured
    case StreamTriBranch::kFlat: return pen >= -989 ? 1 : 3;  // B / D
    case StreamTriBranch::kFx47a770: return 2;      // C
    case StreamTriBranch::kLut1024: return 4;       // E
    case StreamTriBranch::kFx46e940: return 5;      // F
    default: return 6;                              // G lut <=-1029
  }
}

bool vertFinite(const StreamTriVert& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) &&
         std::isfinite(v.z) && std::isfinite(v.sx) &&
         std::isfinite(v.sy);
}

} // namespace

void StreamPresenter::submitModel(const mdk::StreamEvent& ev) {
  auto& d = diag_.model;
  ++d.commands;
  const mdk::DynamicObject* o =
      modelResolver_ ? modelResolver_(ev.aux) : nullptr;
  if (!o || o->col.elements == nullptr) {
    ++d.lookupMiss;
    return;
  }
  if (o->col.elements == mdk::StreamScene::classRec0()) {
    // The 0x4edcc0 class-table sentinel arm (fuse objects) — the
    // port binds an empty set; there is no geometry to walk.
    ++d.classRec0;
    return;
  }
  // Resolve only the object's OWN live model set — spawn binds
  // elemSet = model.elementSet() and animation mutates elemVerts in
  // place, so the views here always see the current vertex state.
  // A foreign +0x0c binding is a lookup failure class (unreachable
  // on real data), not invalid geometry.
  const mdk::CollisionElementSet& es = *o->col.elements;
  const mdk::RuntimeModel& m = o->model;
  if (&es != &o->elemSet || es.elems != m.elems.data() ||
      m.elems.size() != m.elemVerts.size() ||
      m.elems.size() != m.elemTris.size()) {
    ++d.lookupMiss;
    return;
  }
  ++d.resolved;

  const std::uint32_t mask = o->col.elemMaskB;    // +0x2c8
  const int ec = es.count < 0 ? 0 : es.count;
  for (int e = 0; e < ec; ++e) {
    // Native mask bit: 1 << (i & 0x1f) — index wraps mod 32.
    if (((mask >> (e & 0x1f)) & 1u) != 0) {
      ++d.elementsMasked;
      continue;
    }
    ++d.elementsWalked;
    if (static_cast<std::size_t>(e) >= m.elems.size()) {
      ++d.invalidGeometry;          // count runs past the bound array
      continue;
    }
    const mdk::CollisionElement& el = es.elems[e];
    const std::vector<float>& lv = m.elemVerts[e];
    const std::vector<std::uint8_t>& lt = m.elemTris[e];
    int vc = static_cast<int>(lv.size() / 3);
    const int tc = el.triCount < 0 ? 0 : el.triCount;
    if (el.verts == nullptr && vc > 0) { ++d.invalidGeometry; vc = 0; }
    if (static_cast<std::size_t>(tc) * 0x24 > lt.size() ||
        (tc > 0 && el.tris == nullptr))
      ++d.invalidGeometry;
    const int triWalk = el.tris == nullptr ? 0 : std::min<int>(
        tc, static_cast<int>(lt.size() / 0x24));
    // 46b4f8 fill — transform every element vert once. el.verts
    // aliases lv (rebind() keeps the +0x14 view on the owned vector),
    // so this reads the live (possibly animated) vertex state.
    modelVerts_.resize(static_cast<std::size_t>(vc));
    for (int i = 0; i < vc; ++i)
      StreamRibbonRaster::projectVert(ev.f, el.verts + i * 3,
                                      modelVerts_[i]);
    for (int t = 0; t < triWalk; ++t) {
      ++d.trisWalked;
      const std::uint8_t* tr = el.tris + t * 0x24;
      const int i0 = tr[0] | tr[1] << 8;
      const int i1 = tr[2] | tr[3] << 8;
      const int i2 = tr[4] | tr[5] << 8;
      const int pen =
          static_cast<std::int16_t>(tr[6] | tr[7] << 8);
      if (i0 >= vc || i1 >= vc || i2 >= vc) {
        ++d.invalidGeometry;
        continue;
      }
      const StreamTriVert& v0 = modelVerts_[i0];
      const StreamTriVert& v1 = modelVerts_[i1];
      const StreamTriVert& v2 = modelVerts_[i2];
      if (!vertFinite(v0) || !vertFinite(v1) || !vertFinite(v2)) {
        ++d.invalidGeometry;
        continue;
      }
      const std::uint32_t orF = v0.flags | v1.flags | v2.flags;
      float c;
      if ((orF & 0x10) == 0) {
        // No near flag — the 2D projected-winding cross.
        c = (v2.sy - v0.sy) * (v1.sx - v0.sx) -
            (v2.sx - v0.sx) * (v1.sy - v0.sy);
      } else {
        // Near-flagged — the 3D plane-sign triple product
        // v1 . ((v1-v0) x (v2-v1)) (eye at view origin).
        c = v1.z * ((v1.x - v0.x) * (v2.y - v1.y) -
                    (v2.x - v1.x) * (v1.y - v0.y)) +
            v1.x * ((v1.y - v0.y) * (v2.z - v1.z) -
                    (v2.y - v1.y) * (v1.z - v0.z)) +
            v1.y * ((v2.x - v1.x) * (v1.z - v0.z) -
                    (v1.x - v0.x) * (v2.z - v1.z));
      }
      if (!(c >= 0.0f)) {             // backface / NaN — never pushed
        ++d.polysBackface;
        continue;
      }
      ++d.matCls[modelMatClass(pen)];
      if (pendingPolys_.size() >= 0x1000) {
        ++d.polysOverflow;            // the native 0x1000 record bound
        continue;
      }
      StreamModelPoly p;
      p.key = v0.z + v1.z + v2.z;     // 499f88==0: z'-sum key
      p.pen = pen;
      p.v[0] = v0; p.v[1] = v1; p.v[2] = v2;
      // The tri record's float2 uv pairs (+0x08/+0x10/+0x18) bind
      // positionally to indices i0/i1/i2 — the clipper lerps them
      // with the same t as the verts.
      std::memcpy(&p.v[0].u, tr + 0x08, 8);
      std::memcpy(&p.v[1].u, tr + 0x10, 8);
      std::memcpy(&p.v[2].u, tr + 0x18, 8);
      p.mats = m.materials;
      pendingPolys_.push_back(p);
    }
  }
}

// FUN_0040c694 — qsort the pushed records by key descending (the
// 40bd2c comparator returns keyB-keyA on the i32 bit patterns —
// stable_sort keeps a deterministic order for exact ties), then
// 0ca00 + 0c860 per record in sorted order.
void StreamPresenter::flushModelPolys() {
  auto& d = diag_.model;
  if (pendingPolys_.empty()) return;
  std::stable_sort(pendingPolys_.begin(), pendingPolys_.end(),
                   [](const StreamModelPoly& a,
                      const StreamModelPoly& b) {
                     return std::bit_cast<std::int32_t>(a.key) >
                            std::bit_cast<std::int32_t>(b.key);
                   });
  ++d.flushes;
  const std::uint64_t px0 = d.raster.pixels;
  for (const StreamModelPoly& p : pendingPolys_)
    ribbon_.drawTri(fb_, p.pen, p.v, p.mats, d.raster);
  pendingPolys_.clear();
  if (d.raster.pixels != px0)
    d.fbDigest = d.fbDigest * 0x100000001b3ull ^
                 mdk::fnv1a64(std::span<const std::byte>(
                     reinterpret_cast<const std::byte*>(fb_.pixels()),
                     fb_.pixelCount()));
}

// FUN_0042e684 tail (OBSERVED, p19a_e684.asm): the two-piece toroidal
// copy into the 600x360 back buffer. Per row the native copies
// dst[0..600-U) <- src[U..600) then dst[600-U..600) <- src[0..U);
// dst row band [0..360-V) reads src [V..360) and the remainder wraps
// to src row 0 — i.e. dst(x,y) = src((x+U) mod 600, (y+V) mod 360).
// Core already produced the post-wrap nonneg accumulators; the mod
// stays for bounds hardening.
void StreamPresenter::blitBackdrop(const mdk::IndexedImage& src,
                                   int u, int v) {
  const int w = fb_.width(), h = fb_.height();
  if (src.width != w || src.height != h || src.pixels.empty()) return;
  u = ((u % w) + w) % w;
  v = ((v % h) + h) % h;
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* srow =
        src.pixels.data() + static_cast<std::size_t>(
            ((y + v) % h) * src.stride);
    std::uint8_t* drow = fb_.pixels() + static_cast<std::size_t>(y) *
                                         fb_.stride();
    for (int x = 0; x < w; ++x) drow[x] = srow[(x + u) % w];
  }
}

const char* streamSpriteResultName(StreamSpriteResult r) {
  switch (r) {
    case StreamSpriteResult::kDrew:        return "drew";
    case StreamSpriteResult::kMissResource: return "resource";
    case StreamSpriteResult::kMissMetadata: return "metadata";
    case StreamSpriteResult::kZeroSize:    return "zerosize";
    case StreamSpriteResult::kClipped:     return "clipped";
    case StreamSpriteResult::kTransparent: return "transparent";
  }
  return "?";
}

// FUN_00403a40 (OBSERVED — traversal_hud.cpp's SKULL blit is the same
// port): effW/H = (src.dim*dst)>>8 (the record's dst field is the 8.8
// projector size), center-anchored at (cx,cy), 16.16 DDA sampling,
// pen 0 transparent, clipped to the view rect. The return class is
// diagnostic only — the raster itself is unchanged.
StreamSpriteResult StreamPresenter::blitScaled(
    int cx, int cy, int dstW, int dstH, const mdk::IndexedImage& src) {
  if (src.pixels.empty() || src.width <= 0 || src.height <= 0)
    return StreamSpriteResult::kMissMetadata;
  int effW = static_cast<int>(
      (static_cast<std::int64_t>(src.width) * dstW) >> 8);
  int effH = static_cast<int>(
      (static_cast<std::int64_t>(src.height) * dstH) >> 8);
  if (effW <= 0 || effH <= 0) return StreamSpriteResult::kZeroSize;
  const int stepX = static_cast<int>(
      (static_cast<std::int64_t>(src.width) << 16) / effW);
  const int stepY = static_cast<int>(
      (static_cast<std::int64_t>(src.height) << 16) / effH);
  int x = cx - (effW >> 1);
  int srcX0 = 0;
  if (x < 0) {                          // left clip
    effW += x;
    if (effW <= 0) return StreamSpriteResult::kClipped;
    srcX0 = -x * stepX;
    x = 0;
  }
  int y = cy - (effH >> 1);
  std::int64_t srcY0 = 0;
  if (y < 0) {                          // top clip
    effH += y;
    if (effH <= 0) return StreamSpriteResult::kClipped;
    srcY0 = -static_cast<std::int64_t>(y) * stepY;
    y = 0;
  }
  if (x >= fb_.width() || y >= fb_.height())
    return StreamSpriteResult::kClipped;
  if (effW > fb_.width() - x) effW = fb_.width() - x;
  if (effH > fb_.height() - y) effH = fb_.height() - y;
  StreamSpriteResult res = StreamSpriteResult::kTransparent;
  std::int64_t srcY = srcY0;
  for (int r = 0; r < effH; ++r) {
    const int sy = static_cast<int>(srcY >> 16);
    std::int64_t srcX = srcX0;
    for (int c = 0; c < effW; ++c) {
      const int sx = static_cast<int>(srcX >> 16);
      if (sy < src.height && sx < src.width) {
        const std::uint8_t p =
            src.pixels[static_cast<std::size_t>(sy) * src.stride + sx];
        if (p != 0) {
          fb_.put(x + c, y + r, p);
          res = StreamSpriteResult::kDrew;
        }
      }
      srcX += stepX;
    }
    srcY += stepY;
  }
  return res;
}

void StreamPresenter::noteSprite(StreamSpriteResult r, int tag,
                                 int srcW, int srcH, int dstSize) {
  switch (r) {
    case StreamSpriteResult::kDrew:        ++diag_.spriteDrawn; return;
    case StreamSpriteResult::kMissResource:
      ++diag_.spriteMissRes; break;
    case StreamSpriteResult::kMissMetadata:
      ++diag_.spriteMissMeta; break;
    case StreamSpriteResult::kZeroSize:    ++diag_.spriteZeroSize; break;
    case StreamSpriteResult::kClipped:     ++diag_.spriteClipped; break;
    case StreamSpriteResult::kTransparent:
      ++diag_.spriteTransparent; break;
  }
  if (r == StreamSpriteResult::kMissResource ||
      r == StreamSpriteResult::kMissMetadata)
    ++diag_.spriteMisses;
  auto& row = diag_.spriteCensus[
      StreamSpriteMissKey{static_cast<int>(r), tag, srcW, srcH}];
  if (row.count == 0) {
    row.sizeMin = row.sizeMax = dstSize;
  } else {
    row.sizeMin = dstSize < row.sizeMin ? dstSize : row.sizeMin;
    row.sizeMax = dstSize > row.sizeMax ? dstSize : row.sizeMax;
  }
  ++row.count;
}

// FUN_004185fc (OBSERVED): the HUD blit — transparent-keyed subrect
// copy, pen `key` skipped, bounds-hardened (the original clips only
// by construction).
void StreamPresenter::blitSubRect(const mdk::IndexedImage& src,
                                  int srcOff, int dstX, int dstY,
                                  int w, int h, int srcStride,
                                  std::uint8_t key, bool& drew) {
  drew = false;
  if (src.pixels.empty() || w <= 0 || h <= 0) return;
  if (srcOff < 0 ||
      static_cast<std::size_t>(srcOff) >= src.pixels.size())
    return;
  for (int r = 0; r < h; ++r) {
    const int dy = dstY + r;
    if (dy < 0 || dy >= fb_.height()) continue;
    const std::size_t rowBase =
        static_cast<std::size_t>(srcOff) +
        static_cast<std::size_t>(r) * static_cast<std::size_t>(
            srcStride > 0 ? srcStride : src.stride);
    for (int c = 0; c < w; ++c) {
      const std::size_t si = rowBase + static_cast<std::size_t>(c);
      if (si >= src.pixels.size()) break;
      const int dx = dstX + c;
      if (dx < 0 || dx >= fb_.width()) continue;
      const std::uint8_t p = src.pixels[si];
      if (p != key) { fb_.put(dx, dy, p); drew = true; }
    }
  }
}

// TELETYPE line draws (OBSERVED): renderer 0 = FUN_00414d2c — one
// centered line, FONTBIG while its measure stays under the 600px
// frame width else the FONTSML centered fallback (FUN_00414f1c);
// renderer 1 = FUN_0041518c — centered FONTBIG scaled,
// x = trunc((600 - w*scale)*0.5).
void StreamPresenter::drawTeletype(int renderer, int y, float scale,
                                   const std::string& text) {
  if (!fontBig_ || !fontSml_) return;
  const int fw = fb_.width();
  if (renderer == 0) {
    const int wBig = mdk::measureFtiText(
        *fontBig_, text, mdk::kFtiFontBigMissingAdvance);
    if (wBig >= fw) {
      const int w = mdk::measureFtiText(
          *fontSml_, text, mdk::kFtiFontSmlMissingAdvance);
      mdk::drawFtiText(*fontSml_, text, fb_, (fw - w) / 2, y,
                       mdk::kFtiFontSmlMissingAdvance);
    } else {
      mdk::drawFtiText(*fontBig_, text, fb_, (fw - wBig) / 2, y,
                       mdk::kFtiFontBigMissingAdvance);
    }
  } else {
    const int w = mdk::measureFtiText(
        *fontBig_, text, mdk::kFtiFontBigMissingAdvance);
    const int x = static_cast<int>(
        (fw - w * static_cast<double>(scale)) * 0.5);
    mdk::drawFtiTextScaled(*fontBig_, text, fb_, x, y, scale,
                           mdk::kFtiFontBigMissingAdvance);
  }
}

void StreamPresenter::consume(const mdk::StreamEvent& ev,
                              const std::uint8_t* paletteDac) {
  // 19B.2B1 — the kModelDraw submitter only PUSHES tris (the native
  // chain walk); the first non-model event is the per-bucket drain
  // boundary (FUN_0040c694 inside 09a00) — the sorted poly draw then
  // precedes the bucket's sprite drain, exactly the native order.
  if (ev.kind != mdk::StreamEvent::kModelDraw) flushModelPolys();
  switch (ev.kind) {
    case mdk::StreamEvent::kBackdropBlit: {
      ++diag_.backdropBlits;
      const auto it = images_.find(ev.tag);
      if (it != images_.end())
        blitBackdrop(it->second, static_cast<int>(ev.f[0]),
                     static_cast<int>(ev.f[1]));
      break;
    }
    case mdk::StreamEvent::kSpriteDraw: {
      ++diag_.sprites;
      const auto it = images_.find(ev.tag);
      if (it == images_.end()) {
        noteSprite(StreamSpriteResult::kMissResource, ev.tag, 0, 0,
                   static_cast<int>(ev.f[2]));
        break;
      }
      // f[0..2] = {sx, sy, size}; the record's dst W/H are the same
      // projected size (e49c/e55c write it to both fields); f[4]/f[5]
      // mirror the source image dims the native caches (0x40 LIGHT,
      // planetTag[1..2] marker) — blitScaled reads the image's own.
      const StreamSpriteResult r =
          blitScaled(static_cast<int>(ev.f[0]),
                     static_cast<int>(ev.f[1]),
                     static_cast<int>(ev.f[2]),
                     static_cast<int>(ev.f[2]), it->second);
      noteSprite(r, ev.tag, it->second.width, it->second.height,
                 static_cast<int>(ev.f[2]));
      break;
    }
    case mdk::StreamEvent::kHudBlit: {
      ++diag_.hudBlits;
      const auto it = images_.find(ev.tag);
      bool drew = false;
      if (it != images_.end())
        blitSubRect(it->second, ev.aux,
                    static_cast<int>(ev.f[0]),
                    static_cast<int>(ev.f[1]),
                    static_cast<int>(ev.f[2]),
                    static_cast<int>(ev.f[3]),
                    static_cast<int>(ev.f[4]),
                    static_cast<std::uint8_t>(
                        static_cast<int>(ev.f[5]) & 0xff),
                    drew);
      if (!drew) ++diag_.hudMisses;
      break;
    }
    case mdk::StreamEvent::kTeletypeDraw:
      ++diag_.teletypeDraws;
      drawTeletype(ev.tag, ev.aux, ev.f[0], ev.name);
      break;
    case mdk::StreamEvent::kPaletteSet:
      // The fade stage's DAC upload (FUN_00413b40 -> 46d208 host
      // boundary). paletteDac_ is already the transformed table.
      ++diag_.paletteSets;
      if (paletteDac) {
        for (int i = 0; i < mdk::Palette::size(); ++i)
          palette_.set(i, {paletteDac[i * 3 + 0],
                           paletteDac[i * 3 + 1],
                           paletteDac[i * 3 + 2], 255});
        diag_.paletteHash = mdk::fnv1a64(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(paletteDac), 768));
      }
      break;
    case mdk::StreamEvent::kModelDraw:
      submitModel(ev);
      break;
    case mdk::StreamEvent::kRibbonTri:
      // 19B.2A — the host raster chain (project -> clip -> 0c860
      // dispatch -> filler) writes indexed pixels in event order.
      ribbon_.draw(fb_, ev.tag, ev.f,
                   static_cast<std::uint32_t>(ev.aux), diag_.ribbon);
      break;
    case mdk::StreamEvent::kPlaySound:
    case mdk::StreamEvent::kStopSound:
      ++diag_.soundEvents;      // audio listener/output deferred
      break;
    case mdk::StreamEvent::kPresent:
      // Frame-submit boundary: digest the indexed surface as it
      // stands (pre-palette — the DAC applies to the whole buffer).
      ++diag_.presented;
      diag_.fbHash = mdk::fnv1a64(
          std::span<const std::byte>(
              reinterpret_cast<const std::byte*>(fb_.pixels()),
              fb_.pixelCount()));
      framePending_ = true;
      break;
    case mdk::StreamEvent::kExitMode:
      // Terminal palette fill — paletteDac_ was memset to the fill
      // byte before this event (fade stage tail); the upload maps
      // every index to the uniform color over the stale buffer.
      ++diag_.terminalFills;
      diag_.terminalFill = ev.aux;
      if (paletteDac) {
        for (int i = 0; i < mdk::Palette::size(); ++i)
          palette_.set(i, {paletteDac[i * 3 + 0],
                           paletteDac[i * 3 + 1],
                           paletteDac[i * 3 + 2], 255});
        diag_.paletteHash = mdk::fnv1a64(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(paletteDac), 768));
      }
      framePending_ = true;
      break;
  }
}

} // namespace mdkbridge
