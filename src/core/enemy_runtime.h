// enemy_runtime.h — Phase 11B/G5-RE: the native object-side motion,
// command-dispatch and attack-boundary subsystem.
//
// OBSERVED (MDK95.EXE BUILD_A, Ghidra decompile + raw disasm):
//   FUN_0045b9fc @ 0x45b9fc — gravity (vel.z, medium damp, terminal).
//   FUN_0045bac0 @ 0x45bac0 — object collision: drag, impulse add,
//     axis-separated FUN_0045d174 sweeps, +0x14c contacts, kill plane.
//   FUN_0045d174 @ 0x45d174 — the swept-move primitive over
//     FUN_00407fc0 (collisionSweep) with the +0x27c clamp box and the
//     partner-arena retry.
//   FUN_004533d4 @ 0x4533d4 — the +0x11e subtype dispatcher
//     (leader-follow, fly-to-camera, heartbeat, chain, timed shot,
//     attach, speed-ramp, ground-timer + the 0x2b/0x4e/0xc5 steering
//     tail through FUN_00452b80 / FUN_004524e0 / FUN_00452140).
//   FUN_00457ab8 @ 0x457ab8 — pendulum orbit about +0x1c..0x24.
//   FUN_0045ab44 @ 0x45ab44 — command runner (+0x14b&0x40 gate).
//   FUN_0045897c @ 0x45897c — enemy command dispatcher
//     (+0x149&0x10 gate; +0x30a command id, +0x30e tick countdown).
//   FUN_004585c4 @ 0x4585c4 — the +0x14a&0x20 mover.
//   FUN_004574d0 @ 0x4574d0 — pending-arena transfer (+0x2bc gate).
//   FUN_00458354 @ 0x458354 — kill-plane / floor-death boundary.
//   FUN_0045828c @ 0x45828c — immediate record teardown.
//   FUN_00459618 @ 0x459618 — AABB-delta touch scan (+0x14c bit4).
//   FUN_00454c6c @ 0x454c6c — runner proximity/damage scan.
//   FUN_00401ed4 / FUN_0047d2b5 — MSVC CRT rand + (rand*n)>>15.
//
// Command bodies (FUN_0045897c callees) ported: cmd1 FUN_00459330,
// cmd2 FUN_00459c5c, cmd3 FUN_00459968, cmd4 FUN_00459160,
// cmd5/0x81 detonate block, cmd7 FUN_00459a9c, cmd8 FUN_00459450,
// cmd9 FUN_00459554, cmd0x80 path-dropper block, +0x14a&4 carry
// FUN_004599e8. Sound/FX/debris spawns (FUN_00402160, FUN_00405ffc,
// FUN_00403f6c, FUN_00404108, FUN_004575fc, FUN_004387ec,
// FUN_00465200, FUN_00408eb0) remain counted seams — presentation.
#pragma once

#include <cstdint>

namespace mdk {

struct TraversalRuntime;
struct TraversalArena;
struct DynamicArena;
struct DynamicObject;
struct CollisionNode;
struct CollisionPoly;

// ---------------------------------------------------------------------------
// RNG — FUN_0047d2b5 (MSVC CRT rand LCG) + FUN_00401ed4.
//   state = state * 0x41c64e6d + 0x3039; result = (state >> 16) & 0x7fff
//   enemyRandBelow(n) = (rand * n) >> 15  ->  0..n-1
// The original keeps the seed in the CRT __threadseed slot; the port
// carries it on TraversalRuntime::rngState (default 1, the CRT
// default) so sequences are deterministic per runtime.
// ---------------------------------------------------------------------------
std::uint32_t enemyRandNext(std::uint32_t& state);
int enemyRandBelow(std::uint32_t& state, int n);

// ---------------------------------------------------------------------------
// Per-object update pieces — called from traversal_runtime.cpp's
// FUN_004572ac-ordered loop. `home` is the object's +0x60 arena (the
// arena whose storage list owns it); `otherArena` is the partner
// arena for the +0x14a&8 migration retry (nullptr = none / d3c set).
// ---------------------------------------------------------------------------

// FUN_00457ab8 — pendulum orbit. Runs when +0x14a&0x40.
void objectOrbit(DynamicObject& o, const float playerPos[3], float dt);

// FUN_0045ab44 — command runner. Runs when +0x14b&0x40. May kill the
// object (health -> 0 / teardown) — the loop checks +0x08 after it.
void objectCommandRunner(TraversalRuntime& rt, DynamicObject& o,
                         DynamicArena& home, float dt);

// FUN_0045d174 — swept move. Returns the FUN_00407fc0 contact handle
// (the hit CollisionPoly*, nullptr = free move); `outNode` receives
// the hit BSP node (the +0x2b4 normal source) when nonnull. Ghost
// objects (+0x148&4 == 0) move directly and clamp to +0x27c..0x290.
const CollisionPoly* objectSweptMove(TraversalRuntime& rt,
                                     DynamicObject& o, DynamicArena& home,
                                     TraversalArena* otherArena,
                                     float dx, float dy, float dz,
                                     const float extOfs[6], float scale,
                                     const CollisionNode** outNode);

// FUN_0045b9fc — gravity. Runs unconditionally in the loop (the
// +0x148&2 check is inside).
void objectGravity(TraversalRuntime& rt, DynamicObject& o,
                   DynamicArena& home, float dt);

// FUN_0045bac0 — object collision/integration. Runs unconditionally
// after gravity; clears +0x14c&0x13, integrates (vel + +0x294..0x29c
// impulse)*dt through the axis-separated sweeps, handles bounce
// (+0x14b&0x20), floor contact (+0x14c bit1, +0x2b0/+0x2b4), the
// kill plane (arena deepFloorZ - 200) and the +0x14a&8 portal
// migration queue.
void objectCollide(TraversalRuntime& rt, DynamicObject& o,
                   DynamicArena& home, TraversalArena* otherArena,
                   float dt);

// FUN_004533d4 — the +0x11e subtype dispatcher. Runs unconditionally
// in the loop after the path follower.
void objectSubtypeUpdate(TraversalRuntime& rt, DynamicObject& o,
                         DynamicArena& home, float dt);

// FUN_0045897c — enemy command dispatch. Runs when +0x149&0x10,
// REPLACING the mover branch (OBSERVED: the dispatch path jumps
// straight to the anim step).
void enemyCommandDispatch(TraversalRuntime& rt, DynamicObject& o,
                          DynamicArena& home, float dt);

// FUN_004585c4 — the mover (+0x14a&0x20 gate, enemy-dispatch else
// branch). Ported mechanics: the hop/child-spawn block
// (+0x148&0x20002)==2, the +0x312 child fade-out, the SW_H150 /
// SW_SEAL / SW_SBONE name branches and the anim-state pushes.
void objectMover(TraversalRuntime& rt, DynamicObject& o,
                 DynamicArena& home, float dt, int frameStep);

// FUN_0045d578 — rolling-contact ride update (+0x148 & 0x40 gate in
// the FUN_004572ac tail): the frame XY delta rolls the +0x302 raw
// matrix (log/roller objects). OBSERVED from raw disasm.
void objectRollRide(DynamicObject& o);

// FUN_00437f30 — norm360(deg(atan2(dy,dx))) with (dx|dy)==0
// passthrough. Shared by the seek ops and the script view-cone/
// face-camera ops.
float bearingDeg(float dy, float dx);
// FUN_00437f98 — degree sin/cos pair (deg * pi/180 -> {sin,cos}).
void sincosDeg(float deg, float* sinOut, float* cosOut);
// The shared seek-op tail (OBSERVED in the 0x4e/0x2b/0x2b-broadcast
// handlers): clears the +0x2a0..+0x2ac mover block and +0x14c bit3.
void seekOpcodeTail(DynamicObject& o);

// FUN_00451ee8 — bind-time waypoint re-seek: +0x12c..0x134 =
// +0x120..+0x128; when the z+8-lifted pos->target segment is blocked,
// probe candidate points along the segment direction (OBSERVED raw
// disasm). Shared by the seek opcodes (0x2a/0xa6/0x4e).
void objectWaypointReseek(DynamicObject& o, DynamicArena& home);

// tr_alcmd 0x2a (handler 0x439aef) — camera-relative seek target;
// runs only while the object lives in the current arena.
void objectOpSeekCamera(TraversalRuntime& rt, DynamicObject& o,
                        DynamicArena& cur, float fwd, float lat);

// tr_alcmd 0xa6 (handler 0x442de1) — flee-the-cmd2-object seek
// target; no-op while 0x540e60 (rt.cmdObj60) is clear.
void objectOpSeekAway(TraversalRuntime& rt, DynamicObject& o,
                      float dist);

// FUN_004574d0 — pending-arena transfer (+0x2bc gate, runs before the
// script VM). Returns 1 when the object migrated (the loop skips the
// rest of its update) or 0 to continue.
int objectArenaTransfer(TraversalRuntime& rt, DynamicObject& o,
                        DynamicArena& home);

// FUN_00458354 — kill-plane / floor death: +0x110!=0 -> deferred
// script + arena-deep floor snap; else immediate teardown.
void objectFloorDeath(TraversalRuntime& rt, DynamicObject& o);

// FUN_0045828c — the immediate record teardown: the original memsets
// the record keeping +0x00/+0x60 (so +0x06 clears -> dead for all
// live checks) after releasing sound/FX/model handles (counted
// seams). Port: fx event + global-ref clears + named=false +
// health=0 + state wipe preserving arena membership.
void objectTeardownNow(TraversalRuntime& rt, DynamicObject& o);

// FUN_00459618 — the AABB-delta touch scan. Expands own AABB by the
// frame delta, tests every other named+living object's element AABBs
// in `home` (and `otherArena` when valid), resolves the prevPos->pos
// segment via FUN_0045c838 (collisionSegAabbResolve) against the
// element AABB Minkowski-grown by the scanner's own span. On
// contact: +0x14c|=0x10, pos clamps to the resolve point, *hitObj
// receives the touched object, and +0x14a&8 queues the +0x2bc arena
// migration. `exclMask` skips flagged objects (+0x148 dword test),
// `reqMask` requires a flag when nonzero. Returns 1 on touch.
int objectTouchScan(TraversalRuntime& rt, DynamicObject& o,
                    DynamicArena& home, TraversalArena* otherArena,
                    std::uint32_t exclMask, std::uint32_t reqMask,
                    DynamicObject** hitObj);

// FUN_00454c6c — the runner's proximity/damage scan: scales a copy of
// the object AABB by +0x2c0 and tests it against the player AABB
// (mask&1 -> FUN_0046771c) and same-arena named objects (mask&2 ->
// punch marks + health-dmg; dmg is the per-hit decrement). Returns 1
// on any contact; when `killOnHit` is set the scanner dies.
int objectMeleeScan(TraversalRuntime& rt, DynamicObject& o,
                    DynamicArena& home, int dmg, std::uint32_t mask,
                    bool killOnHit);

} // namespace mdk
