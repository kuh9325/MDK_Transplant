// ---------------------------------------------------------------------------
// Object model animation — Phase 11A / G5-RE.
//
// Port of the original per-object animation driver chain (OBSERVED,
// BUILD_A MDK95.EXE):
//
//   FUN_004555bc — the per-tick driver. Gates: +0x04==-1 routes to
//     FUN_00455500 (classless timing path — the timing-record source
//     FUN_0041a5ec is unresolved, bounded seam); model==&classTable
//     routes to a 1.0/frame fuse ending in FUN_0045828c teardown. The
//     normal path: idle gate (+0x118>=0 && +0xe4==+0x118, or the
//     0xff00 done latch), the +0x140/+0x144 sound marker consume,
//     +0xdc += rate * +0xe0 * (1/30), target clamp at +0x118, loop or
//     clamp at frameCount-1, then FUN_00455890(obj, rec,
//     FRNDINT(+0xdc) - +0xe4) and the done latch on frameCount-1.
//
//   FUN_00455890 — the frame applier. Repeats `steps` times:
//     *0x118>=0 && +0xe4==+0x118 -> resync +0xdc=+0xe4, return (idle)
//     *++e4; wrap at frameCount (when the anim record loops)
//     *root-motion: rootKey[frame] (model-space vec3) rotated by the
//       object 3x3 into +0x294/+0x298/+0x29c scaled by 1/DT — skipped
//       at frame 0 (except on wrap) and while +0x14b bit7 set
//     *refKeys: model +0x24 refPoints[i] = refKey[i][frame], i <
//       min(refCount,8)
//     *per element: name-match a channel (FUN_0042fa50 strcmp on the
//       12-byte name fields); scale==0 -> FUN_00455c48 rigid decode;
//       frame==0 -> absolute basePose copy; else scan {i16 tag; i8
//       delta[3*vc]} keys for tag==frame and apply d*scale; every
//       applied channel ends in FUN_00459d54 local-bounds recompute.
//
//   FUN_00455c48 — rigid channel decode: per-frame 12xi16 fixed-point
//     3x4 row-major transform (rotation entries / (0x8000>>shiftA),
//     translation entries / (0x8000>>shiftB)) applied to the channel's
//     base pose -> element verts (absolute rewrite, not additive).
//
// Record layout (OBSERVED — disasm + LEVEL3/6/8 record bytes):
//   +0x00 f32 rate
//   +0x04 u32 channelCount
//   +0x08 u32 frameCount
//   +0x0c u32 chanOff[channelCount]   — channel rec = rec+4+chanOff[i]
//   +0x0c+4N     vec3 rootKey[frameCount]   (model-space displacement)
//   +0x0c+4N+12F u32 refCount               (only the first 8 apply)
//   +0x10+4N+12F vec3 refKey[refCount][frameCount]
// channel record, delta form (scale != 0):
//   +0x00 char name[<=12, NUL-terminated]
//   +0x0c u32 vertCount     (informational — the applier uses the
//                            ELEMENT's vert count for all strides)
//   +0x10 f32 scale
//   +0x14 vec3 basePose[vc]            — absolute copy at frame 0
//   +0x14+12V  {i16 frameTag; i8 delta[3*vc]}+ — sorted; scan stops at
//              tag<0 or tag>=frame; applies only when tag==frame
// channel record, rigid form (scale == 0):
//   +0x00 name[12], +0x0c vertCount, +0x10 f32 == 0
//   +0x14 u8 rotShift, +0x15 u8 transShift
//   +0x16 vec3 basePose[vc]
//   +0x16+12V i16 xform[frameCount][12] — {r00,r01,r02,tx, r10...,ty,
//              r20...,tz} / (0x8000>>shift); dense (no tags)
//
// Both channel forms occur in real data (LEVEL3: 62 rigid channels,
// LEVEL6: 7, LEVEL8: 54 across the census — OBSERVED). Element vert
// buffers are therefore authoritative animation output: the renderer
// consumes whatever the applier leaves in elemVerts.
//
// Copy-safety: ObjectAnimView is a bounded view over image bytes the
// level owns (CMI span); it never outlives TraversalLevel. No pointers
// cross into snapshots — snapshot consumers read DynamicObject fields
// and model.elemVerts.
// ---------------------------------------------------------------------------
#ifndef MDK_CORE_OBJECT_ANIMATION_H
#define MDK_CORE_OBJECT_ANIMATION_H

#include <cstddef>
#include <cstdint>

#include "core/dynamic_objects.h"

namespace mdk {

// Bounded view over one animation record inside the level image.
// `limit` is the end of the containing image (cmiBytes end); all walks
// bounds-check against it (native hardening — the original trusts the
// stream). An invalid read marks `ok=false` and yields zeroed values.
struct ObjectAnimView {
  const std::uint8_t* rec = nullptr;   // record base (image pointer)
  const std::uint8_t* limit = nullptr; // image end
  mutable bool ok = true;              // sticky bounds flag

  const std::uint8_t* bytes(std::size_t off, std::size_t n) const;
  float rate() const;                       // +0x00
  std::uint32_t channelCount() const;       // +0x04
  std::uint32_t frameCount() const;         // +0x08
  const std::uint8_t* channel(std::uint32_t i) const;  // rec+4+off[i]
  const float* rootKey(std::uint32_t frame) const;     // vec3
  std::uint32_t refCount() const;           // raw u32 (clamp 8 outside)
  const float* refKey(std::uint32_t slot,
                      std::uint32_t frame) const;      // vec3
};

// Channel helpers (record returned by ObjectAnimView::channel).
const char* animChannelName(const std::uint8_t* ch);    // +0x00
std::uint32_t animChannelVertCount(const std::uint8_t* ch); // +0x0c
float animChannelScale(const std::uint8_t* ch);         // +0x10
bool animChannelIsRigid(const std::uint8_t* ch);        // scale==0

// FUN_00455890 — advance o.animFrame by `steps` frames applying every
// intermediate frame (delta keys are cumulative; skipping frames would
// lose state). Mutates model.elemVerts / refPoints / localAabb /
// animImpulse. steps <= 0 is a no-op (the original JLEs out).
void objectAnimApply(DynamicObject& o, const ObjectAnimView& anim,
                     int steps);

// FUN_004555bc — the per-tick driver. `recLimit` bounds the animRec
// walk (pass the owning image end; nullptr disables bounds checks —
// test path). Sound emission is a documented seam: the marker consume
// (+0x140 clear on frame crossing) is preserved as state.
void objectAnimTick(DynamicObject& o, const std::uint8_t* recLimit);

} // namespace mdk

#endif // MDK_CORE_OBJECT_ANIMATION_H
