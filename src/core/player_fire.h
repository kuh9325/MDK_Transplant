// Phase 5N — player weapon fire: the FUN_0045f138 dispatch (sniper-
// scoped projectile spawn), the FUN_00432f84 normal-mode punch
// hitscan, the FUN_00437660 cadence/burst/ammo machine, the
// FUN_00469b98 scoped weapon selector, and the FUN_00465228 fire
// latch — all bounded to the spawn/scan boundary.
//
// EVIDENCE (instruction-level, MDK95.EXE BUILD_A; offsets in the
// original record/globals are noted per field):
//
//   FUN_0045f138 — the player weapon-fire dispatch. OBSERVED single
//   caller: the sniper core FUN_00464624 at 0x464a77, gated by
//   0x4ce770(fire) != 0 && 0x540d0c >= 5 && 0x54161b == 0.0. The
//   normal traversal mode never reaches it — normal "fire" is the
//   melee punch FUN_00432f84 (a hitscan, NOT a projectile).
//
//     Weapon 5 (0x541618 == 5): gated on the charge-probe flag
//     0x540e14 != 0 AND the latch 0x54163b == 0 (else the can't-fire
//     notify FUN_00402388(1, 0x54c650)). The charge level
//     0x541498 >= 4 sets 0x54163b = 1 AND still fires this shot.
//     Fire tail: 0x54161a(u8)--, 0x54161b += 1.0, 0x541633--,
//     0x54161b = 3.0 when the burst byte wraps to 0, then
//     FUN_0045a4dc (the thrown-object arc spawn — a bounded seam).
//
//     Weapons 0..4: scan the 3-slot player-shot pool 0x540ed4
//     (stride 0xfc) for state(+0x00) == 0; all busy -> silent return
//     (no fire). On a free slot: 0x540d0c = 0, fire-sound
//     FUN_004022b8(0x54c5d0), memset 0xfc, state=1, class record
//     +0x1c = 0x4edd48 (default), pos +0x20 = camera pos 0x540b28,
//     yaw +0x04 = 0x540c2c, pitch +0x08 = 0x540b54 + 0x540d58,
//     arena +0x18 = 0x540c48, +0xcc = 2.0. Then a jump table on
//     0x541618 sets the per-weapon fields; >4 skips the switch (the
//     memset-0 defaults stand). Shared tail: 0x54161b += 1.0,
//     0x54161a(u8)--, = 3.0 when it wraps to 0.
//
//       w0: d0=0 d4=FUN_004601d4 10=0x4b  e4=1100.0
//       w1: d0=1 d4=0x4602d8   10=0xf0  e4=400.0   ammo[1]--,
//           +0x1c = classIdx("SW_HOME")
//       w2: d0=2 d4=FUN_004601d4 10=0x4b  e4=1100.0 ammo[2]--,
//           +0x1c = classIdx("SW_SGREN")
//       w3: d0=3 d4=0x4602d8   10=0xf0  e4=400.0   ammo[3]--,
//           +0x1c = classIdx("SW_HGREN")
//       w4: d0=4 d4=0x4608bc   10=0x1c2 ammo[4]--; e4=cos(pitch)*150,
//           f0=-sin(pitch)*150, +0x1c = classIdx("SW_LGREN")
//
//     Homing tail (0x540cc8 != 0): slot.d8 = 0x540cd8 (the lock
//     target). For weapons 1/3 only, walk the target's element set:
//     an element whose name strstr()s "HEAD" is picked immediately;
//     else, gated on target+0x149 & 0x20, the FUN_0045f634 digit+
//     prefix predicate (elem name[digitOfs] is '0'..'9' AND name
//     prefix-matches the +0x302 string) admits a FUN_0045c230
//     segment-vs-AABB clip (pos -> pos - cameraBack*10000); the
//     nearest |outPoint| wins, stored to slot.dc (elem) and +0xe0
//     (index). 0x540e80 (shot serial) increments on every spawn.
//
//   FUN_00432f84 — the normal-mode punch/melee hitscan, called
//   unconditionally from the world tick FUN_00436100 at 0x43627a
//   (right after the per-frame player AABB rebuild). Gate inside:
//   0x540c74 (the fire latch) != 0, and NOT (excludeObj != 0 &&
//   !(mountClass & 2)). Port: the target scan, the +0x21e=0xff hit
//   mark, and — Phase 10A — the full damage/knockback/death tail
//   (int16 element wrap + element->whole fallthrough + the
//   +0x2a2/+0x31e event latches + charged corpse displacement +
//   bearing+180 kill facing) via the shared boundary in
//   player_projectiles.h. Effect/sound callsites stay counted seams.
//
//   FUN_00437660 — the cadence/burst/ammo machine, scoped to
//   c9c != 0 && ca0 > 1 inside FUN_00436d60 (NOT unconditional —
//   the sniper-scoped fire rate lives only there). Pending weapon
//   blend at deltaSec*8.0 to 3.0 -> adopt wpnSel1 + reset burst;
//   else cadence decays deltaSec*4.0 (floor 0) then a burst pip
//   recharges (capped at 3, gated by ammo[wpnSel1] when wpnSel1!=0);
//   a pip counter that stays 0 after the gate reloads to weapon 0
//   (wpnSel1=0, burstIndex=1).
//
//   FUN_00469b98 — the scoped weapon selector: hotkeys 0..5 write
//   wpnSel1 (weapon 0 unconditional, 1..5 need ammo[i] > 0), then
//   itemNext/itemPrev wrap-scan the pending selection skipping
//   empty-ammo weapons (weapon 0 always lands).
//
// See docs/reverse-engineering/GAMEPLAY_RECONSTRUCTION.md for the
// evidence-level breakdown.

#ifndef MDK_CORE_PLAYER_FIRE_H
#define MDK_CORE_PLAYER_FIRE_H

#include <cstdint>

#include "core/gameplay_input.h"

namespace mdk {

struct TraversalRuntime;
struct TraversalArena;
struct DynamicObject;
struct CollisionElement;

// ---------------------------------------------------------------------------
// The 3-slot player-shot pool (0x540ed4, stride 0xfc). The in-flight
// update is FUN_0045f9b8, invoked at the tail of each
// FUN_004572ac(arena) call — see player_projectiles.h (Phase 10A).
// ---------------------------------------------------------------------------

// The +0xd4 per-slot fly callback identity — the four original
// functions are kept as tags.
enum PlayerShotFly : int {
  kShotFlyNone = 0,
  kShotFlyTracer = 0x4601d4,  // FUN_004601d4 — w0/w2 ballistic tracer
  kShotFlyGrenade = 0x4602d8, // w1/w3 homing/grenade callback
  kShotFlyLobbed = 0x4608bc,  // w4 lobbed callback
  kShotFlyRibbon = 0x46075c,  // FUN_0046075c — ribbon path (FUN_00460860
                            // binds it when the 0x49b8e4 lobbed-shot
                            // latch is set on a type-4 slot)
};

struct PlayerShot {
  int state = 0;          // +0x00 — 0 free / 1 flight / 2,3,4,5 dying
  float yawDeg = 0.0f;    // +0x04 — launch yaw (0x540c2c); w4 bounce
                          // rewrites it from the reflected velocity
  float pitchDeg = 0.0f;  // +0x08 — launch pitch (0x540b54+0x540d58)
  float spinDeg = 0.0f;   // +0x0c — streak spin; +720*dt each no-hit
                          // tick (type != 4 only)
  int lifetime = 0;       // +0x10 — per-weapon life ticks (frameStep)
  int dyingTimer = 0;     // +0x14 — state>1 countdown; release at <=0
  TraversalArena* arena = nullptr;   // +0x18 — the 0x540c48 arena (the
                          // killZ deep-floor reference is read here)
  int classIdx = -1;      // +0x1c — class record (FUN_00454794 result;
                          // -1 = the 0x4edd48 default record)
  float pos[3] = {0, 0, 0};  // +0x20 — current pos; the object probe
                          // shortens it to the nearest hit point
  float tailLen = 0.0f;   // +0xbc — tracer tail length: +10*dt (cap
                          // 10); impact sets 15.0; dying w0/w1 shrink
                          // 5*dt
  float tail[3] = {0, 0, 0}; // +0xc0..0xc8 — pos - tailLen*dir,
                          // rebuilt by FUN_0045f94c
  float fieldCc = 0.0f;      // +0xcc — 2.0f at spawn, -0.5*dt floor 1.0
  int type = 0;              // +0xd0 — the weapon index 0..4
  int flyKind = 0;           // +0xd4 — PlayerShotFly tag
  const DynamicObject* homeObj = nullptr;      // +0xd8 — 0x540cd8 lock
  const CollisionElement* homeElem = nullptr;  // +0xdc — best element
  int homeElemIdx = 0;                         // +0xe0 — its index;
                          // the ribbon binder reuses this dword for
                          // the path record (dead while +0xf8&1)
  float speedH = 0.0f;    // +0xe4 — horizontal speed; the ribbon
                          // binder reuses this dword for the Hermite
                          // param (dead while +0xf8&1)
  float yawAccum = 0.0f;  // +0xe8 — homing yaw-rate accumulator
                          // (+-540 deg/s^2, +-270 cap, reset on
                          // reversal/zero)
  float speedV = 0.0f;    // +0xf0 — vertical speed (w4 lobbed only)
  int remnantIdx = 0;     // +0xf4 — remnant/effect slot (cleared on
                          // every impact transition; spawn is a seam)
  std::uint32_t flags = 0;// +0xf8 — bit0 = ribbon-bound (skips the
                          // collision + object scan entirely)
  const void* ribbonPath = nullptr; // +0xe0 alias — the Hermite key
                          // record while +0xf8&1 (overlaps homeElemIdx)
  float ribbonT = 0.0f;   // +0xe4 alias — the path param while +0xf8&1
                          // (overlaps speedH)
};

// ---------------------------------------------------------------------------
// Dispatch + scan
// ---------------------------------------------------------------------------

// FUN_0045f138 — the player weapon-fire dispatch (sniper callsite).
// Reads only globals; drives the weapon-5 thrown-object seam or the
// 0..4 shot-pool spawn + homing scan.
void playerFireDispatch(TraversalRuntime& rt);

// FUN_00437660 — the scoped cadence/burst/ammo machine. `dt` is
// timing.deltaSec (0x49b6f4). Call ONLY under the c9c && ca0 > 1
// gate (the caller scopes it).
void playerWeaponCadence(TraversalRuntime& rt, float dt);

// FUN_00469b98 — the scoped weapon selector. `ctrl` is the merged
// N-1 semantic frame; writes wpnSel1 (the pending weapon).
void playerWeaponSelect(TraversalRuntime& rt,
                        const GameplayInputFrame& ctrl);

// FUN_00432f84 — the normal-mode punch hitscan. `frameStep` is
// timing.frameStep (0x49b6e8). Runs the gate + charge drain + the
// object/element target scan; on a hit it runs the Phase 10A
// damage/knockback/death tail (the +0x21e mark, int16 element damage,
// the element->whole fallthrough, the event latches, tally + death);
// on a miss it runs the 150-unit wall stab + channel-2 dispatch.
void playerPunch(TraversalRuntime& rt, int frameStep);

// The FUN_00465228 fire latch — the normal-mode fire edge. Sets/
// clears 0x540c74 (the punch gate) and posts the fire anim event
// (0x12c/0x259, or none for the busy set). `ctrl` is the N-1 frame.
void playerFireLatch(TraversalRuntime& rt, const GameplayInputFrame& ctrl);

// FUN_00437aa8 — the scoped charge-probe flag update: 0x540e14 =
// (the FUN_0046145c charge probe is live). Bounded seam — the probe
// spawn is deferred; `weapon5Probe` carries the live flag for tests.
void playerChargeProbe(TraversalRuntime& rt);

// FUN_0045c230 — segment-vs-AABB Liang-Barsky ENTRY clipper, shared
// with the freefall missile/pickup collision path. Returns 2 when the
// start is inside (out=start), 1 when the segment enters the box
// (out=entry point), 0 on a miss / entry past the end.
int segClipAabb(const float* start, const float* end, const float* aabb,
                float* out);

} // namespace mdk

#endif // MDK_CORE_PLAYER_FIRE_H
