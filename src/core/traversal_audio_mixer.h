// Traversal audio voice pool (Phase 17C.2) — the BUILD_A instance-pool
// semantics as a pure scalar state machine, so the Godot presenter can
// stay a thin AudioStreamPlayer host. Emits player commands; nothing
// here knows about Godot.
//
// EVIDENCE (BUILD_A disasm):
//
//   Pool — FUN_00402604 pops the 63-slot free list (0x49ffd8) onto the
//   active list (0x49ff90); failure -> no instance -> the caller's
//   owner slot keeps its old value (the spawn writes *slot only on
//   success). Hard cap, first-free, no stealing.
//
//   Release — FUN_004026cc marks inst+0xb |= 0xc0 (stopping) and
//   clears *ownerSlot; the next FUN_004026f8 sweep reaps flags&0x40
//   nodes. Modeled synchronously: a stop frees the slot at once —
//   indistinguishable downstream since commands drain once per frame.
//
//   Name scoping — FUN_0040210c releases EVERY instance of a record;
//   FUN_00402658's ensure query counts an instance iff its record
//   matches AND flags&0xc0 == 0. Owner keys never participate.
//
//   Owner slots — FUN_00402160 writes *ownerSlot = inst on success.
//   A second spawn for the same owner OVERWRITES the slot: the old
//   instance is orphaned (keeps playing; name-scoped stops still hit
//   it). FUN_004020b4/FUN_004026cc release through the slot only.
//
//   Playhead/finish — a non-looping DS buffer stops at the end of its
//   data; the mixer sweep reaps it. Modeled as playhead += freqHz*dt
//   (SetFrequency IS the consumption rate: one input frame per output
//   sample, mono, unresampled on the DS path). Loop-flagged records
//   (SNI flags bit0) never finish on their own — they die by name or
//   owner release exactly like the original.

#ifndef MDK_CORE_TRAVERSAL_AUDIO_MIXER_H
#define MDK_CORE_TRAVERSAL_AUDIO_MIXER_H

#include "core/traversal_audio.h"
#include "core/traversal_audio_dsp.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mdk {

// Resolved record metadata a spawn needs — supplied by the host (the
// bridge's SNI lookup + decode) or a test.
struct TraversalAudioSoundDef {
  int volume = 0x7fff;         // record authored vol (snd+0x8)
  int rateHz = 0;              // fmt sample rate (snd+0xc equivalent)
  std::uint32_t frames = 0;    // decoded PCM frame count
  bool loop = false;           // record flags bit0 (DS PLAY_LOOPING)
};

// One command to the playback host. `name` is the stream identity —
// the presenter resolves/caches the AudioStreamWAV once per record.
enum class TraversalAudioCmdOp : int {
  kStart = 0,   // spawn voice -> create player, play stream
  kParams,      // pushed update -> apply vol/pan/freq fields
  kStop,        // released/finished -> stop + free the player
};

struct TraversalAudioCmd {
  TraversalAudioCmdOp op = TraversalAudioCmdOp::kStart;
  int handle = -1;               // voice slot 0..kMaxVoices-1
  std::string name;              // SNI record name (stream identity)
  int vol = 0;                   // effective volume, 0..32767 domain
  int pan = 0;                   // +-32767 domain
  int freqHz = 0;                // DS SetFrequency domain
  int rateHz = 0;                // record rate -> pitch = freqHz/rateHz
  bool loop = false;             // record loop flag at spawn
};

class TraversalAudioMixer {
public:
  static constexpr int kMaxVoices = 63;

  // Host callbacks ---------------------------------------------------
  // resolve: name -> record meta (false = record missing/unplayable).
  // ownerPos: ownerKey -> live world pos for the 0x20000 live-pos
  // mode (false = owner not found; the voice keeps its last pos,
  // matching the original's stale-pointer deref).
  using ResolveFn =
      std::function<bool(const std::string& name,
                         TraversalAudioSoundDef& out)>;
  using OwnerPosFn =
      std::function<bool(int ownerCat, const void* key, float pos[3])>;

  void reset();
  // Bank-free teardown — release every live voice (FUN_004371bc and
  // FUN_0042c824 kill the mode's sound bank: every still-playing
  // instance dies there, not by name). Emits kStop per live voice so
  // the host players stop before their nodes are reaped.
  void stopAll();
  void setListener(const TraversalAudioListener& l) { listener_ = l; }

  // Apply one core event. Spawns/stops emit commands immediately.
  void applyEvent(const TraversalAudioEvent& ev, const ResolveFn& res);

  // One mixer pass (FUN_004026f8): live-pos refresh, updater dispatch,
  // finish reap. Emits kParams/kStop commands.
  void tick(double dtSec, const OwnerPosFn& ownerPos);

  const std::vector<TraversalAudioCmd>& pending() const {
    return cmds_;
  }
  void drain(std::vector<TraversalAudioCmd>& out) {
    out.insert(out.end(), cmds_.begin(), cmds_.end());
    cmds_.clear();
  }

  // Diagnostics (development counters — the smoke harness prints them).
  int activeCount() const;
  int resolvedCount() const { return resolved_; }
  int missingCount() const { return missing_; }
  int poolExhaustedCount() const { return exhausted_; }

  // Test/inspection: is any live voice bound to this owner?
  bool ownerHasVoice(int ownerCat, const void* key) const;
  bool nameActive(const std::string& name) const;

private:
  struct Voice {
    bool live = false;
    std::string name;
    TraversalAudioVoice dsp;
    int ownerCat = 0;
    const void* ownerKey = nullptr;
    bool loop = false;
    std::uint32_t pcmFrames = 0;
    double playhead = 0.0;       // source frames consumed
    int rateHz = 0;              // record rate (pitch denominator)
  };

  int allocSlot();               // first-free; -1 = pool exhausted
  void stopVoice(int slot);      // emit kStop + free
  void stopByName(const std::string& name);
  void startVoice(int slot, const std::string& name,
                  const TraversalAudioSoundDef& def);

  Voice voices_[kMaxVoices];
  TraversalAudioListener listener_{};
  std::vector<TraversalAudioCmd> cmds_;
  std::map<std::pair<int, const void*>, int> ownerVoices_;
  int resolved_ = 0;
  int missing_ = 0;
  int exhausted_ = 0;
};

} // namespace mdk

#endif // MDK_CORE_TRAVERSAL_AUDIO_MIXER_H
