// object_path.h — Phase 11B/G5-RE: the generic object path sampler +
// per-object path follower.
//
// OBSERVED (MDK95.EXE BUILD_A, Ghidra decompile):
//   FUN_00456bc8 @ 0x456bc8 — cubic Hermite path sampler.
//   FUN_00456d28 @ 0x456d28 — per-object path follower (+0xec gate).
//   FUN_00457264 @ 0x457264 — bind-time snap (sample + lateral add).
//
// Path record layout (OBSERVED, op 0x02 + FUN_00456bc8):
//   {i32 count; entry[count] x 0x28}
//   entry = {i32 frame @0x00; f32 pos[3] @0x04; f32 tanIn[3] @0x10;
//            f32 tanOut[3] @0x1c}
//
// The sampler walks segments from count-2 DOWN to 0 and picks the
// first entry whose .frame <= f (so the first entry wins ties). There
// is NO clamping: f below/above the range extrapolates along the
// first/last segment's Hermite curve.
#pragma once

struct DynamicObject;

namespace mdk {

// FUN_00456bc8 — evaluate the path at `frame` into out[3]. `rec` is the
// +0xec image pointer (the {i32 count, ...} blob). Returns false when
// the record is null/empty (out untouched — the original dereferences
// unconditionally, the guard is a port robustness bound).
bool pathSample(const void* rec, float frame, float out[3]);

// Record frame bounds — entry[0].frame / entry[count-1].frame (the
// FUN_00456d28 wrap/release limits and the weapon-5 kamikaze latch
// midpoint read them). 0 on a null/degenerate record.
float pathFirstFrame(const void* rec);
float pathLastFrame(const void* rec);

// FUN_00457264 — snap pos = pathSample(+0xec, +0xf0) + lateral +0xf4.
// Runs once at op-0x02 bind time.
void objectPathSnap(DynamicObject& o);

// FUN_00456d28 — the per-frame path follower, gated by +0xec != 0 in
// the FUN_004572ac update loop. `playerPos` is 0x540bfc and
// `playerYawDeg` is 0x540c2c — consumed only by the +0x14b&0x10
// speed-lane branch.
void objectPathFollow(DynamicObject& o, const float playerPos[3],
                      float playerYawDeg);

} // namespace mdk
