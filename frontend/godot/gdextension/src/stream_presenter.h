// Phase 19B.1 — Mode-5 (StreamScene) presentation. mdk_core owns the
// cinematic simulation and emits ordered StreamEvents; this class is
// the host half of the OBSERVED seams it documents: the 600x360
// indexed back buffer (0x541650), the 768B palette DAC surface
// (FUN_00413b40 -> FUN_0046d208), the toroidal backdrop copy
// (FUN_0042e684 tail), the scaled sprite blit (FUN_00403a40), the
// HUD subrect blit (FUN_004185fc), the TELETYPE text draws
// (FUN_00414d2c / FUN_0041518c), the terminal palette fill, and the
// kPresent frame boundary. Phase 19B.2A added the indexed-triangle
// raster backend (stream_raster.h) — kRibbonTri is drawn in-stream;
// kModelDraw is still counted and deferred to Phase 19B.2B — no
// mutation, no reordering.
//
// Godot-free by design: the bridge owns the instance, consumes the
// scene's event queue after each step, and palette-expands the
// indexed surface for the texture upload. Testable natively.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>

#include "core/framebuffer.h"
#include "core/fti_font.h"
#include "core/indexed_image.h"
#include "core/stream_scene.h"
#include "stream_raster.h"

namespace mdkbridge {

// Phase 19B.1A — sprite presentation outcome classes. Only the two
// Miss* classes are resource failures (the tag never reached the
// table, or the bound image is unusable); the rest are faithful
// FUN_00403a40 raster outcomes — zero footprint after the >>8 size
// collapse, a fully offscreen dest rect, or a blit that ran but
// committed no non-key pixel. They are classified separately so a
// clipped/transparent sprite is never reported as a missing asset.
enum class StreamSpriteResult : int {
  kDrew = 0,       // raster ran, >=1 non-key pixel committed
  kMissResource,   // event tag never bound — TRUE MISS
  kMissMetadata,   // bound image empty / nonpos dims — TRUE MISS
  kZeroSize,       // (src.dim*dst)>>8 -> 0 footprint — faithful
  kClipped,        // dest rect fully outside the 600x360 view
  kTransparent,    // raster ran, committed no pixel (all key / OOB)
};

// Deterministic per-bucket miss census row. Key = (cls, tag, srcW,
// srcH); the row accumulates count and the observed dst-size range
// (the record's 8.8 projector size). `tag` is the opaque image
// identity the core echoed — on real data that is the BNI record
// index (4 = PLANET/marker, 11 = LIGHT/debris); the bridge joins
// the record name for the report.
struct StreamSpriteMissRow {
  int count = 0;
  int sizeMin = 0;  // min/max dst size observed in this bucket
  int sizeMax = 0;
};
using StreamSpriteMissKey = std::tuple<int, int, int, int>;
using StreamSpriteMissCensus =
    std::map<StreamSpriteMissKey, StreamSpriteMissRow>;

const char* streamSpriteResultName(StreamSpriteResult r);

// Presentation-side counters (all diagnostic — none of this feeds
// back into the core). `terminalFill` stays -1 until kExitMode.
struct StreamPresenterDiag {
  int presented = 0;        // kPresent frame boundaries consumed
  int backdropBlits = 0;    // kBackdropBlit copies
  int sprites = 0;          // kSpriteDraw events consumed
  int spriteDrawn = 0;      //   blit committed >=1 pixel
  int spriteMisses = 0;     //   TRUE misses = missRes + missMeta
  int spriteMissRes = 0;    //   tag never bound (no image)
  int spriteMissMeta = 0;   //   bound image empty / nonpos dims
  int spriteZeroSize = 0;   //   dst size collapses footprint to 0
  int spriteClipped = 0;    //   dest rect fully outside the view
  int spriteTransparent = 0;//   raster ran, wrote no pixel
  int hudBlits = 0;         // kHudBlit copies
  int hudMisses = 0;        //   unbound image / empty source
  int teletypeDraws = 0;    // kTeletypeDraw lines
  int paletteSets = 0;      // kPaletteSet DAC uploads applied
  int modelsDeferred = 0;   // kModelDraw carried through (19B.2B)
  StreamRibbonDiag ribbon;  // kRibbonTri raster census (19B.2A)
  int soundEvents = 0;      // kPlaySound/kStopSound (audio deferred)
  int terminalFills = 0;    // kExitMode palette fills
  int terminalFill = -1;    // the aux fill byte (0x00/0xff)
  std::uint64_t fbHash = 0;      // FNV-1a over indexed pixels at
                                 // the last present (pre-palette)
  std::uint64_t paletteHash = 0; // FNV-1a over the applied 768B
  // Every non-kDrew sprite outcome, bucketed — deterministic order.
  StreamSpriteMissCensus spriteCensus;
};

class StreamPresenter {
public:
  StreamPresenter();

  // Resource binding — copies the decoded image; the tag is the
  // opaque id the core echoes in draw events (assets().bgTag /
  // lightTag / planetTag[0] / hudIconTag / hudDigitTag).
  void bindImage(int tag, mdk::IndexedImage img);
  // Copies the decoded fonts (renderer 0 picks FONTBIG-or-FONTSML
  // by the 600px measure rule; renderer 1 is FONTBIG scaled).
  void bindFonts(const mdk::FtiFont& fontBig,
                 const mdk::FtiFont& fontSml);
  // Binds the scene's composed 768B base palette — the LUT source
  // for the mode-5 ribbon material ramp (native 0x4ed758; the DAC
  // surface is the faded copy and must NOT feed the LUT).
  void bindRibbonPalette(const std::uint8_t* palette768);
  void reset();

  // One StreamEvent, in the core's emission order. `paletteDac` is
  // the scene's live 768-byte DAC surface (StreamScene::paletteDac())
  // — copied into the presentation palette on the upload events.
  void consume(const mdk::StreamEvent& ev,
               const std::uint8_t* paletteDac);

  const mdk::IndexedFramebuffer& framebuffer() const { return fb_; }
  mdk::IndexedFramebuffer& framebuffer() { return fb_; }
  const mdk::Palette& palette() const { return palette_; }
  const StreamPresenterDiag& diag() const { return diag_; }
  // A frame boundary was crossed since the last take — kPresent
  // (normal) or kExitMode (the terminal-fill frame, presented once).
  bool framePending() const { return framePending_; }
  void clearFramePending() { framePending_ = false; }

private:
  void blitBackdrop(const mdk::IndexedImage& src, int u, int v);
  // FUN_00403a40 (the shared scaled sprite blit — the same body
  // traversal_hud.cpp carries for SKULL): center-anchored at
  // (cx,cy), effW/H = (src*dst)>>8, 16.16 DDA, pen 0 transparent.
  // Returns the classified outcome (never kMissResource — the
  // caller owns tag resolution).
  StreamSpriteResult blitScaled(int cx, int cy, int dstW, int dstH,
                                const mdk::IndexedImage& src);
  // Bump the outcome's diag counter and (for every non-kDrew
  // result) accumulate the miss-census bucket.
  void noteSprite(StreamSpriteResult r, int tag, int srcW, int srcH,
                  int dstSize);
  // FUN_004185fc — transparent-keyed subrect blit. srcOff is a byte
  // offset into the source pixels (row-major, srcStride rows pitch);
  // the key is the transparent pen (0 here).
  void blitSubRect(const mdk::IndexedImage& src, int srcOff,
                   int dstX, int dstY, int w, int h, int srcStride,
                   std::uint8_t key, bool& drew);
  void drawTeletype(int renderer, int y, float scale,
                    const std::string& text);

  mdk::IndexedFramebuffer fb_;            // 600x360, the 0x541650
                                          // surface
  mdk::Palette palette_;                  // the applied DAC image
  StreamRibbonRaster ribbon_;             // 19B.2A indexed tri raster
  std::unordered_map<int, mdk::IndexedImage> images_;
  std::optional<mdk::FtiFont> fontBig_, fontSml_;
  StreamPresenterDiag diag_;
  bool framePending_ = false;
};

} // namespace mdkbridge
