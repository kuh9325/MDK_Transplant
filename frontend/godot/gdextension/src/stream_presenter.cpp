// Phase 19B.1 — Mode-5 presentation seams. See stream_presenter.h.
// Every blit here ports an OBSERVED native body; the event payloads
// are the authoritative boundary (core semantics are CLOSED — this
// file only consumes them).

#include "stream_presenter.h"

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

void StreamPresenter::reset() {
  fb_.clear(0);
  palette_ = mdk::Palette{};
  images_.clear();
  fontBig_.reset();
  fontSml_.reset();
  diag_ = StreamPresenterDiag{};
  framePending_ = false;
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
      ++diag_.modelsDeferred;   // 19B.2 — counted, not rasterized
      break;
    case mdk::StreamEvent::kRibbonTri:
      ++diag_.ribbonsDeferred;  // 19B.2 — counted, not rasterized
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
