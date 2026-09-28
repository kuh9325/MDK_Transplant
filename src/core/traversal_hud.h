// Phase 17B — RE CONTRACT ONLY. This header is the evidence-derived
// declaration surface for the traversal HUD / scope-view
// reconstruction (the FUN_00436d60 draw tail and the HUD-side state
// ticks it depends on). IMPLEMENTATION IS NOT STARTED: there is no
// traversal_hud.cpp, no compilation unit includes this header, and no
// caller depends on any symbol here. Every declared behaviour is the
// observed original contract recorded for the implementation phase —
// not a redesign. See docs/GAMEPLAY_RECONSTRUCTION.md "Phase 17B —
// RE checkpoint".
//
// Everything below is OBSERVED at instruction level in BUILD_A's
// MDK95.EXE (Ghidra disassembly; constants re-dumped from the file
// with the AUTO/DGROUP VA->file maps -0x400c00/-0x401c00). The whole
// tail draws into the 600x360 indexed work framebuffer (0x541650,
// +600 bias) — the native reproduces it as a 600x360 overlay buffer
// (pen 0 = transparent) presented above the world render.
//
// Original draw order inside the frame tail (FUN_00436d60, OBSERVED):
//   scope gate A (c9c && ca0>1): FUN_0045f030(0) world shot pass +
//     FUN_00437aa8 scope-warp (cosmetic — deferred, counted seam)
//   FUN_00469f7c — inventory slide lerp + icon draw + selection
//     frame + the 0x541558 visibility timer
//   scope gate B (c9c && ca0>1): FUN_0045f030(1) window fills +
//     SNIPERGA gauges, then FUN_00436f08 (CROSS frame 0 at
//     (299,219) + the SNIPERS2 u16 pen-command stream)
//   FUN_00436f2c — mounted reticle (FUN_0046911c), then under
//     hudActive (0x5414d4): event bars, FUN_00417e20 (mission-timer
//     pie + health digits + scoped tail), then unconditionally the
//     message flush (FUN_0041cb44 — a seam here) and the SKULL
//     death overlay (541554==0 && 540dac>0 && 540cac==0x3ea).
//
// Deferred/counted seams (NOT composed — documented):
//   - FUN_00437aa8 per-row scope-zoom transition warp (cosmetic).
//   - FUN_0041cb44 status-message flush (the OOT_L%d posts are
//     counted as seams; text render needs the message queue).
//   - the 0x540b20 per-level shade LUT remap on the selected
//     inventory cell (the LUT is raster-subsystem data; the border
//     box is drawn but the 46x46 darken is skipped).
//   - FUN_0041664c/FUN_00416700 scope-entry palette fades.
//
#ifndef MDK_CORE_TRAVERSAL_HUD_H
#define MDK_CORE_TRAVERSAL_HUD_H

#include <cstdint>
#include <vector>

#include "core/framebuffer.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"

namespace mdk {

struct TraversalRuntime;

// The FUN_00403a00 record form: {u16 w, u16 h, px @payload+4}.
struct TraversalHudImage {
  int w = 0;
  int h = 0;
  const std::uint8_t* px = nullptr;  // aliases level.travsprtBytes
};

struct TraversalHudState {
  bool bound = false;

  // FUN_00418688's 21-name table subset (0x49a7d4): entries 2..19.
  TraversalHudImage scStat, scBstat;                    // [2],[3]
  TraversalHudImage snipRng, snipWep, snipTxt;          // [5],[6],[7]
  TraversalHudImage snipL[6];                           // [8..13]
  TraversalHudImage snipW[6];                           // [14..19]
  TraversalHudImage skull;                              // 0x4978a0 slot

  // K_-style sprite tables (record payload+4 — the FUN_004039d8/ec
  // bind variant): CROSS, BOMBTARG, SNIPERGA, PICKUPS.
  FtiSprite cross, bombtarg, sniperga, pickups;

  // SNIPERS2 (0x54c684, payload+4): the FUN_0040c7c0 u16 pen-command
  // stream decoded once into a 600x360 indexed layer.
  std::vector<std::uint8_t> overlayPx;
  // SC_STAT's mutable working copy — FUN_00418378 stamps the elapsed
  // wedge into it from SC_BSTAT's mask.
  std::vector<std::uint8_t> statWork;
  // SNIPERS1 verbatim (640x480, px @payload+0 — the FUN_004039c8 raw
  // bind). The scope bezel; presented by the frontend.
  std::vector<std::uint8_t> bezelPx;

  FtiFont fontBig;                    // MDKFONT.FTI FONTBIG — reticle %d
  bool fontBigOk = false;

  int rngLatch = 0;                   // 0x49a8e0 — SNIP_RNG slide latch
  int blinkPhase = 0;                 // 0x49a8dc — blink counter &0x1f
  std::uint8_t fieldD9b = 0;          // 0x540d9b — status flag byte;
                                      //   |= 0x20 on timer expiry
                                      //   (OBSERVED); other bits
                                      //   UNKNOWN

  IndexedFramebuffer fb{600, 360};    // the composed overlay
};

// FUN_00433d40's binds + FUN_00418688's table build — call once per
// level load, after level.travsprtBytes is populated. Clears the
// per-level HUD latches (a fresh original BSS state).
void traversalHudBind(TraversalRuntime& rt);

// Supply the decoded FONTBIG (MDKFONT.FTI "FONTBIG") for the mounted
// reticle's bomb-count text (FUN_0046911c -> FUN_00414be8/00414c34).
void traversalHudBindFontBig(TraversalRuntime& rt, const FtiFont& font);

// FUN_0041b654 — the mission countdown. OBSERVED gates: fadeTimer
// (0x5414a0) > 0, level index != 5 (0x541498 — the level id), the
// master-move gate 0x540d9c == 0. Fixed -1.0 per call (0x49b6f0) —
// frame-rate coupled, NOT frameStep-scaled. On expiry: a0 clamps 0,
// camera shakeMag arms >= 5.0 (FUN_00465200), the OOT_L%d status
// message posts (FUN_0041cad0 — counted as a hudMsg seam here), and
// 0x540d9b |= 0x20. Call site: the FUN_00436100 head, gated
// fieldE9c == 0.
void traversalHudMissionTick(TraversalRuntime& rt);

// FUN_00469f7c — the per-frame inventory update + the type-4 shot
// +0xf4 gauge counter (FUN_0045f030's HUD-pass side effect).
// OBSERVED: inventoryCount==0 exits before the lerp; scoped
// (c9c && ca0) pins 0x541558 to 0 (draw side hides); unscoped re-arms
// it to 60; the record lerp (FUN_00469e94, fixed 1/30 step) runs in
// both paths; the tail decrements 0x541558 (clamp 0).
void traversalHudUpdate(TraversalRuntime& rt);

// Compose the whole tail into rt.hud.fb (cleared first). Draw side —
// deterministic given rt state; safe to call once per presented
// frame or on demand from tests.
void traversalHudCompose(TraversalRuntime& rt);

// OBSERVED HUD layout constants (disasm of FUN_00417e20/FUN_00436f2c/
// FUN_00469f7c/FUN_0046911c):
inline constexpr int kHudScreenW = 600;
inline constexpr int kHudScreenH = 360;
// The SNIPERS1 bezel sits under the fb in the 640x480 screen; the fb's
// screen position is .bss — UNOBSERVABLE statically. (20,55) aligns the
// three monitor apertures (x0=93/249/405, top rows ~60) with the shot
// HUD rects (72,10)/(228,0)/(384,10) — HYPOTHESIS derived from that
// alignment; +-1px plausible.
inline constexpr int kHudBezelFbOfsX = 20;
inline constexpr int kHudBezelFbOfsY = 55;
inline constexpr int kHudBezelW = 640;
inline constexpr int kHudBezelH = 480;
// Scoped viewport rect in fb space (projector mode 1): 0x49b758/760.
inline constexpr int kHudScopeX = 108, kHudScopeY = 80;
inline constexpr int kHudScopeW = 384, kHudScopeH = 280;
// Scoped crosshair (FUN_00436088) — CROSS frame 0 at (299,219).
inline constexpr int kHudScopeCrossX = 299, kHudScopeCrossY = 219;
// Mounted reticle (FUN_0046911c): BOMBTARG at (300,180), CROSS at the
// moveVel/strafeVel channels, bomb count right-aligned x=472 y=56.
inline constexpr int kHudBombTargX = 300, kHudBombTargY = 180;
inline constexpr int kHudBombTextRightX = 472, kHudBombTextY = 56;
// Event bars (FUN_00436f2c): solid x[0..w1] pen 3, hollow x[0..w2]
// pen 4, rows 4..10; width = rint(field * 500.0/900.0) clamp 500.
inline constexpr int kHudEventBarY = 4, kHudEventBarH = 7;
inline constexpr int kHudEventBarMax = 500;
// Inventory (FUN_00469f7c): 48px slot cells along the bottom bar;
// border box at (8+48*sel, 304); the ~46x46 LUT remap inset +1,+1
// (skipped — see header); the visibility timer re-arms to 60
// unscoped. OBSERVED; +-1 px pinning deferred to implementation.
inline constexpr int kHudInvCellX = 8, kHudInvCellY = 304;
inline constexpr int kHudInvCellSize = 48;
inline constexpr int kHudInvTimerRearm = 60;
// Scoped tail (FUN_00417e20): zoom% digits at (564,155); SNIP_RNG
// latch bar x=552 window y=176..176+h; SNIP_WEP at (112,304);
// SNIP_W[i] at table 0x49a8ac pairs; SNIP_L[sel] at 0x49a87c pairs;
// ammo digits centered x=64 y=315; SNIP_L/L-icon px tables.
inline constexpr int kHudZoomTxtX = 564, kHudZoomTxtY = 155;
inline constexpr int kHudRngX = 552, kHudRngY = 176;
inline constexpr int kHudWepX = 112, kHudWepY = 304;
inline constexpr int kHudAmmoTxtX = 64, kHudAmmoTxtY = 315;
extern const int kHudSnipWPos[6][2];   // 0x49a8ac pairs (OBSERVED)
extern const int kHudSnipLPos[6][2];   // 0x49a87c pairs (OBSERVED)
// Mission-timer pie (FUN_00417e20): SC_STAT blit at
// (600-w-16, 360-h-10); health digits centered in it; the elapsed
// wedge (angles (1-t/max)*2PI latch->current, epsilon 0.01745 rad)
// stamps SC_BSTAT mask px into the statWork copy.
inline constexpr int kHudPieMarginX = 16, kHudPieMarginY = 10;
inline constexpr int kHudHealthBlinkThreshold = 20;   // 0x14
inline constexpr double kHudWedgeEpsilon = 0.01745329251994328;  // 0x495218
// SKULL death overlay: scaled 256x256 -> dac x dac centered (300,180).
inline constexpr int kHudSkullCx = 300, kHudSkullCy = 180;

} // namespace mdk

#endif // MDK_CORE_TRAVERSAL_HUD_H
