#pragma once

// Phase 17C.1 — traversal audio event contract.
//
// Presentation-neutral, drain-once event records emitted by the core at
// the same points the original calls its DirectSound manager
// (FUN_00402xxx family). No playback happens here — a frontend consumes
// TraversalRuntime::audioFx the same way it drains combatFx
// (std::move + clear after each frame batch).
//
// Original API mapping (OBSERVED, BUILD_A disassembly):
//   FUN_004022b8(snd)             one-shot instance, unowned
//   FUN_00402388(snd,0)           start iff no active instance of snd;
//                                 the query FUN_00402658(snd) matches by
//                                 sound record (node+0x28) and requires
//                                 flags&0xc0==0 (still playing)
//   FUN_00402388(snd,1)           FUN_0040210c stop-all-by-name + play
//   FUN_0040210c(snd)             release every live instance of snd —
//                                 name-scoped, ignores ownership
//   FUN_00402160(slot,snd,flags,pos,...) positional spawn; *slot=inst
//   FUN_00402288(snd,...)         stop-by-name + positional respawn
//   FUN_004020b4(inst)            release one instance; clears *ownerSlot
//   FUN_00402698(inst,vol)        per-instance volume (zone-ambient fades)
//   rate path via FUN_0040247c/0046c2c4  frequency update on the instance
//
// Scoping: the name-keyed ops (kPlayOnce/kEnsurePlaying/kRestart/kStop)
// act on EVERY instance of the record — the original never passes an
// owner there. owner/ownerKey only exist on the positional + release
// ops, where the original binds the instance through an owner slot.
//
// Event ordering is the emission order (seq), which equals the original
// call order within the frame. Sounds are identified by the SNI record
// name — the same string the original resolves through FUN_00402fe8.

#include <cstdint>
#include <string>

namespace mdk {

enum class TraversalAudioOp : int {
  kPlayOnce = 0,       // FUN_004022b8
  kEnsurePlaying,      // FUN_00402388(h,0)
  kRestart,            // FUN_00402388(h,1)
  kStop,               // FUN_0040210c
  kSpawnPositional,    // FUN_00402160
  kRestartPositional,  // FUN_00402288
  kRelease,            // FUN_004020b4 — release the owner's instance
  kSetVolume,          // FUN_00402698 — volume update on an instance
  kSetRate,            // FUN_0040247c/0046c2c4 — rate update
};

enum class TraversalAudioOwner : int {
  kNone = 0,   // unowned instance (original +0xc owner slot null)
  kPlayer,     // player-owned sound state (CHUTEON latch, scope, ...)
  kObject,     // DynamicObject +0x158 voice slot
  kZone,       // zone-ambient pair (0x54c620/0x624 slots)
};

struct TraversalAudioEvent {
  TraversalAudioOp op = TraversalAudioOp::kPlayOnce;
  std::uint32_t seq = 0;          // monotonic emission counter
  int frame = 0;                  // rt.frameCounter (0x540ce0) at emit
  std::string name;               // SNI record name
  TraversalAudioOwner owner = TraversalAudioOwner::kNone;
  const void* ownerKey = nullptr; // owner identity token (the owning
                                  // DynamicObject*; never dereferenced —
                                  // matches combatFx's obj convention)
  float pos[3] = {0.f, 0.f, 0.f}; // world position (hasPos)
  bool hasPos = false;            // positional spawn/update flag
  // FUN_00402160's mode word (inst+0x8), OBSERVED per callsite:
  //   0x1000e object anim sounds / ALERT heartbeat
  //   0x2000e object voice spawn (+0x158 owner slot)
  //   0x10106 shot impacts (RICO/CMI-named)
  //   0x10006 EXPLODE remnant (FUN_004575fc)
  std::uint32_t mode = 0;
  int volume = 0x7fff;            // original 0..0x7fff volume domain
  float rate = 1.0f;              // rate scale (1.0 = record sample rate)
  float range = 50.0f;            // FUN_00402160 range arg (50.0 default)
};

struct TraversalRuntime;

// Emit an event; returns the pushed record so callers can fill pos/owner.
TraversalAudioEvent& traversalAudioEmit(TraversalRuntime& rt,
                                        TraversalAudioOp op,
                                        const char* name);
TraversalAudioEvent& traversalAudioEmit(TraversalRuntime& rt,
                                        TraversalAudioOp op,
                                        const std::string& name);

// Convenience: positional spawn/restart with a world position.
TraversalAudioEvent& traversalAudioEmitPositional(
    TraversalRuntime& rt, TraversalAudioOp op, const char* name,
    const float pos[3], const void* ownerKey = nullptr,
    std::uint32_t mode = 0);
TraversalAudioEvent& traversalAudioEmitPositional(
    TraversalRuntime& rt, TraversalAudioOp op, const std::string& name,
    const float pos[3], const void* ownerKey = nullptr,
    std::uint32_t mode = 0);

// Resolve a CMI-offset string marker (field150 / field154 form:
// offset of a length-prefixed string's data, or 0/-1 when unbound).
// Returns "" when out of range.
std::string traversalAudioCmiName(const TraversalRuntime& rt,
                                  std::int32_t off);

const char* traversalAudioOpName(TraversalAudioOp op);

} // namespace mdk
