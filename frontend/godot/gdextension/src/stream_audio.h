// Phase 19B.3B1 — Mode-5 audio host (the StreamScene sound boundary).
//
// Translates the scene's kPlaySound/kStopSound events into
// TraversalAudioEvent records for the bridge's shared
// TraversalAudioMixer — the same FUN_004026f8 pass the traversal
// audio path already feeds (the original's instance pool is
// process-global; there is no second mode-5 audio engine). The host
// also replays the counted 0x4026f8 listener seam
// (StreamSeams::listener) — once per counted seam call, on the
// post-step camera.
//
// EVIDENCE (BUILD_A disasm, mode-5 init 0x42b3d8..0x42b586):
//
//   Registration — eleven FUN_004039c8(name,&rec) +
//   FUN_00402e2c(name,rec,EBX,ECX) pairs register the STREAM.BNI
//   records into the sound table. EBX=0x7fff on every call (the
//   authored volume); ECX bit0 is the DS-loop flag — set ONLY for
//   WIND (eda58). Bound slot order: eda58=WIND, eda5c=HITSIDE,
//   eda7c=RESCUE, eda80=APPLE, eda60..eda78=HURT1..HURT7 — the tags
//   are the BNI directory indices StreamAssets bound.
//
//   Play calls (all tag-bound plays are slot-pointer calls; the
//   port's kPlaySound aux mirrors the OBSERVED call-side flag):
//     WIND    init   FUN_004022b8(eda58) -> eda84   flat spawn; the
//              loop lives on the registered record, not the call
//     HITSIDE d87d   FUN_00402388(eda5c, EDX-res)   ensure (aux=0)
//     HURTn   d898   FUN_00402388(eda60+n, EDX-res) ensure (aux=0)
//     APPLE   d22c   FUN_00402388(eda80, 1)         restart (aux=1)
//     RESCUE  ca3x   FUN_00402388(eda7c, 0)         ensure (aux=0)
//   aux==1 maps to kRestart — verbatim for APPLE; for WIND it is
//   behaviorally identical to the flat 022b8 because emission is
//   once-per-init with no live WIND instance (the stop-by-name arm
//   is dead code at every reachable state).
//
//   Marker — the anim +0x140 sound arm emits tag=-1, name=+0x140
//   text, f[0..2]=+0x10 pos, aux=0x1000e:
//   FUN_00402160(0, snd, 0x1000e, &pos, 0, 0x7fff, 1.0, 50.0) —
//   an unowned positional spawn with a baked position.
//
//   Stop — teardown FUN_004020b4(eda84) releases the WIND instance;
//   the port carries the record tag, so the stop lands name-scoped
//   (a single WIND instance exists by construction — identical).
//
//   Listener — the 026f8 arg is the 0x540bb0 view snapshot:
//   StreamScene::camView_ verbatim. zoom (0x540b58) is inert: the
//   0x49ff58 updater select stays byte1=1 through mode 5 (the
//   sniper-scope byte1=2 write is a traversal path), so the 2D
//   updater runs. frame is the 0x49b6f0 smoothed scalar — pinned at
//   its 1.0 steady state while host pacing is deferred (33 ms
//   steps == one frame unit).
//
//   TELETYPE — the STATS.BNI record named TELETYPE is a mode-6
//   briefing typing WAVE, NOT a mode-5 sound: no STREAM.BNI record,
//   no registration, no emission path in the scene. Nothing is
//   manufactured for it here.
//
// The host is Godot-free: it emits mixer ops + a per-lifetime census;
// the bridge owns the mixer, the resource resolve, and the player
// command drain.

#ifndef MDK_BRIDGE_STREAM_AUDIO_H
#define MDK_BRIDGE_STREAM_AUDIO_H

#include "core/bni_directory.h"
#include "core/stream_scene.h"
#include "core/traversal_audio.h"
#include "core/traversal_audio_mixer.h"

#include <cstdint>
#include <map>
#include <string>

namespace mdkbridge {

// One FUN_00402e2c registration — the OBSERVED init args the BNI
// records carry no word for (vol/flags arrive from the call sites).
struct StreamSndReg {
  std::string name;     // the 039c8/02e2c name arg (STREAM.BNI name)
  int volume = 0x7fff;  // EBX — authored volume, 0x7fff on all eleven
  bool loop = false;    // ECX bit0 — DS PLAY_LOOPING (WIND only)
};

// Per-lifetime census — reset() clears; the bridge mirrors it into
// stream_diag for the smoke census.
struct StreamAudioDiag {
  int events = 0;          // kPlaySound + kStopSound consumed
  int plays = 0;           // play events translated (tag or name)
  int ensurePlays = 0;     // tag plays, aux==0 (02388(,0) sites)
  int restartPlays = 0;    // tag plays, aux!=0 (d22c form + WIND)
  int loopPlays = 0;       // plays whose registration is looped
  int positional = 0;      // name-bound plays (the 02160 marker arm)
  int stops = 0;           // kStopSound translated
  int unknownTags = 0;     // tag had no registration — dropped
  int resolveMisses = 0;   // resolver returned false — dropped
  int listenerUpdates = 0; // updateListener calls (== seams.listener)
  std::map<std::string, int> playNames;   // per-name play counts
  std::map<std::string, int> stopNames;   // per-name stop counts
};

class StreamAudioHost {
public:
  void reset();

  // tag -> registration (the OBSERVED slot table). Registration is
  // by BNI directory index — the tag StreamAssets bound.
  void bindSound(int tag, StreamSndReg reg);
  const StreamSndReg* soundReg(int tag) const;
  const StreamSndReg* regForName(const std::string& name) const;

  // The frame's 026f8 listener feed: camView_ (0x540bb0) verbatim;
  // frame = the 0x49b6f0 smoothed scalar. 2D updater mode.
  void updateListener(const float camView[12], float frameScalar);
  const mdk::TraversalAudioListener& listener() const {
    return listener_;
  }

  // One StreamEvent -> the shared mixer's op set. Non-audio kinds
  // return without counting. The resolver is the bridge's: stream
  // registrations first, then the still-loaded SNI banks (the
  // process-global record list).
  void consume(const mdk::StreamEvent& ev,
               mdk::TraversalAudioMixer& mixer,
               const mdk::TraversalAudioMixer::ResolveFn& res);

  // One FUN_004026f8 pass — the listener applies, then the sweep.
  // Mode-5 spawns are all unowned (the 02160 marker writes a null
  // owner slot), so no ownerPos refresh exists.
  void tick(mdk::TraversalAudioMixer& mixer, double dtSec) {
    mixer.setListener(listener_);
    mixer.tick(dtSec, nullptr);
  }

  const StreamAudioDiag& diag() const { return diag_; }

private:
  std::map<int, StreamSndReg> regs_;
  mdk::TraversalAudioListener listener_{};
  StreamAudioDiag diag_;
  std::uint32_t seq_ = 0;   // monotonic per-lifetime event order
};

// The eleven OBSERVED registrations — binds each STREAM.BNI sound
// record's tag to its name + registered flags. Returns the bound
// count (11 on the canonical bank).
int streamAudioBindRegs(StreamAudioHost& host,
                        const mdk::BniDirectory& bdir);

} // namespace mdkbridge

#endif // MDK_BRIDGE_STREAM_AUDIO_H
