// Phase 19B.1 — Mode-5 (StreamScene) presentation. mdk_core owns the
// cinematic simulation and emits ordered StreamEvents; this class is
// the host half of the OBSERVED seams it documents: the 600x360
// indexed back buffer (0x541650), the 768B palette DAC surface
// (FUN_00413b40 -> FUN_0046d208), the toroidal backdrop copy
// (FUN_0042e684 tail), the scaled sprite blit (FUN_00403a40), the
// HUD subrect blit (FUN_004185fc), the TELETYPE text draws
// (FUN_00414d2c / FUN_0041518c), the terminal palette fill, and the
// kPresent frame boundary. Phase 19B.2A added the indexed-triangle
// raster backend (stream_raster.h) — kRibbonTri is drawn in-stream.
// Phase 19B.2B1 added the kModelDraw submitter — geometry walk +
// projection + deferred depth-sorted polygon drain (StreamModelDiag
// below) — no mutation, no reordering of the event stream.
//
// Godot-free by design: the bridge owns the instance, consumes the
// scene's event queue after each step, and palette-expands the
// indexed surface for the texture upload. Testable natively.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/arena_render.h"
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

// Phase 19B.2B1 — the kModelDraw consumer (FUN_00455e24 ->
// FUN_0040c3a0 on the 0x541500==1 deferred path, OBSERVED
// p19a_asm5.txt / g1_poly.txt). Per event the model's element set is
// walked in order (+0x2c8 element-disable mask applied), every
// element's verts are transformed once through the event's composed
// camProj-o-xform matrix into 6-float records (projectVert = the
// 46b4f8 fill), and each 0x24-byte tri record contributes {i16 pen @
// +0x06, u16 indices @ +0/2/4}: the winding predicate is the 2D
// projected cross when no vert is near-flagged else the 3D plane-sign
// triple product; passing tris PUSH into a per-batch deferred table.
// The batch drains (qsort by z-sum key descending — OBSERVED sort
// mode 0, bias global inert — then 0ca00+0c860 per tri) when the next
// non-kModelDraw event arrives — exactly the native per-bucket order
// (all model callbacks push, then FUN_0040c694 sorts+draws, then the
// flag-1 sprite drain follows).
struct StreamModelPoly {              // the pushed record's host form
  float key;                          // z'-sum sort key (499f88==0)
  int pen;                            // tri+0x06 i16 — dispatch scalar
  StreamTriVert v[3];                 // projected/flagged verts + UVs
  // the record +0xc slot — the model's resolved material table
  // (RuntimeModel::materials; aliases bridge-owned storage that
  // outlives the deferred drain).
  std::span<const mdk::ArenaRenderMaterial* const> mats;
};
struct StreamModelDiag {
  int commands = 0;        // kModelDraw events consumed
  int resolved = 0;        // resolved to a live object element set
  int lookupMiss = 0;      // aux out of range / no elements / foreign set
  int classRec0 = 0;       // +0x0c == 0x4edcc0 fuse-sentinel arm
  int elementsWalked = 0;  // elements surviving the elemMaskB gate
  int elementsMasked = 0;  // elements skipped by elemMaskB
  int trisWalked = 0;      // tri records visited
  int polysBackface = 0;   // winding < 0 (incl. NaN) — never pushed
  int polysOverflow = 0;   // pushed past the 0x1000 record bound
  int invalidGeometry = 0; // index OOB / truncated record / NaN vert
  int matCls[7] = {};      // submitted census, 0c860 arms A..G
  int flushes = 0;         // batch drains (non-empty)
  StreamRibbonDiag raster; // clip/fan/dispatch/pixel census
  std::uint64_t fbDigest = 0;  // fold over fb after each
                               // pixel-writing flush
};

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
  StreamModelDiag model;    // kModelDraw submitter census (19B.2B1)
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
  // 19B.2B1 — binds the kModelDraw aux (a StreamScene pool index) to
  // the live object. The callable must return a borrowed pointer that
  // stays valid across consume() calls (the bridge binds the scene's
  // object pool — std::array storage, stable until teardown, and the
  // resolver is cleared on reset()). nullptr unbinds.
  void bindModelResolver(
      std::function<const mdk::DynamicObject*(int)> fn);
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
  // 19B.2B1 — FUN_00455e24/0040c3a0: resolve the object, walk the
  // element set, transform verts, winding-test and push surviving
  // tris into pendingPolys_.
  void submitModel(const mdk::StreamEvent& ev);
  // FUN_0040c694 — sort the pushed records by key descending and run
  // each through the shared 0ca00+0c860 path. Called at the first
  // non-kModelDraw event (the per-bucket boundary) and no-ops when
  // the table is empty.
  void flushModelPolys();

  mdk::IndexedFramebuffer fb_;            // 600x360, the 0x541650
                                          // surface
  mdk::Palette palette_;                  // the applied DAC image
  StreamRibbonRaster ribbon_;             // 19B.2A indexed tri raster
  std::unordered_map<int, mdk::IndexedImage> images_;
  std::optional<mdk::FtiFont> fontBig_, fontSml_;
  std::function<const mdk::DynamicObject*(int)> modelResolver_;
  std::vector<StreamModelPoly> pendingPolys_;  // the c3a0 push table
  std::vector<StreamTriVert> modelVerts_;      // bc34 xform scratch
  StreamPresenterDiag diag_;
  bool framePending_ = false;
};

} // namespace mdkbridge
