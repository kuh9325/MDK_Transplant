#include "core/traversal_audio_mixer.h"

namespace mdk {

void TraversalAudioMixer::reset() {
  for (auto& v : voices_) v = Voice{};
  cmds_.clear();
  ownerVoices_.clear();
  resolved_ = missing_ = exhausted_ = 0;
}

int TraversalAudioMixer::activeCount() const {
  int n = 0;
  for (const auto& v : voices_) n += v.live ? 1 : 0;
  return n;
}

bool TraversalAudioMixer::ownerHasVoice(int cat, const void* key) const {
  const auto it = ownerVoices_.find({cat, key});
  return it != ownerVoices_.end() && voices_[it->second].live;
}

bool TraversalAudioMixer::nameActive(const std::string& name) const {
  for (const auto& v : voices_)
    if (v.live && v.name == name) return true;
  return false;
}

int TraversalAudioMixer::allocSlot() {
  // FUN_00402604 — first free slot off the pool; failure is silent
  // (no instance, no steal).
  for (int i = 0; i < kMaxVoices; ++i)
    if (!voices_[i].live) return i;
  ++exhausted_;
  return -1;
}

void TraversalAudioMixer::stopVoice(int slot) {
  Voice& v = voices_[slot];
  if (!v.live) return;
  TraversalAudioCmd c;
  c.op = TraversalAudioCmdOp::kStop;
  c.handle = slot;
  c.name = v.name;
  cmds_.push_back(c);
  // Clear the owner binding only when it still names THIS slot — a
  // respawn overwrites *ownerSlot, so an orphaned voice dying later
  // (name stop or natural finish) must not detach the live binding.
  if (v.ownerKey) {
    const auto it = ownerVoices_.find({v.ownerCat, v.ownerKey});
    if (it != ownerVoices_.end() && it->second == slot)
      ownerVoices_.erase(it);
  }
  v = Voice{};
}

void TraversalAudioMixer::stopByName(const std::string& name) {
  // FUN_0040210c — every live instance of the record, owner-agnostic.
  for (int i = 0; i < kMaxVoices; ++i)
    if (voices_[i].live && voices_[i].name == name) stopVoice(i);
}

void TraversalAudioMixer::startVoice(int slot, const std::string& name,
                                     const TraversalAudioSoundDef& def) {
  Voice& v = voices_[slot];
  v.name = name;
  v.loop = def.loop;
  v.pcmFrames = def.frames;
  v.rateHz = def.rateHz;
  TraversalAudioCmd c;
  c.op = TraversalAudioCmdOp::kStart;
  c.handle = slot;
  c.name = name;
  c.vol = v.dsp.effVol;
  c.pan = v.dsp.pan;
  c.freqHz = v.dsp.freqHz;
  c.rateHz = def.rateHz;
  c.loop = def.loop;
  cmds_.push_back(c);
}

void TraversalAudioMixer::applyEvent(const TraversalAudioEvent& ev,
                                     const ResolveFn& res) {
  switch (ev.op) {
    case TraversalAudioOp::kPlayOnce: {
      // FUN_004022b8 — flat, unowned, record-volume.
      TraversalAudioSoundDef def;
      if (!res(ev.name, def)) { ++missing_; return; }
      ++resolved_;
      const int s = allocSlot();
      if (s < 0) return;
      Voice& v = voices_[s];
      v = Voice{};
      v.live = true;
      traversalAudioVoiceInit(v.dsp, /*mode=*/1, nullptr, def.volume,
                              1.0f, 0.0f, def.rateHz);
      startVoice(s, ev.name, def);
      return;
    }
    case TraversalAudioOp::kEnsurePlaying: {
      // FUN_00402388(,0) — start iff no live instance of the record.
      if (nameActive(ev.name)) return;
      TraversalAudioSoundDef def;
      if (!res(ev.name, def)) { ++missing_; return; }
      ++resolved_;
      const int s = allocSlot();
      if (s < 0) return;
      Voice& v = voices_[s];
      v = Voice{};
      v.live = true;
      traversalAudioVoiceInit(v.dsp, 1, nullptr, def.volume, 1.0f,
                              0.0f, def.rateHz);
      startVoice(s, ev.name, def);
      return;
    }
    case TraversalAudioOp::kRestart: {
      // FUN_00402388(,1) — stop-all-by-name then play.
      stopByName(ev.name);
      TraversalAudioSoundDef def;
      if (!res(ev.name, def)) { ++missing_; return; }
      ++resolved_;
      const int s = allocSlot();
      if (s < 0) return;
      Voice& v = voices_[s];
      v = Voice{};
      v.live = true;
      traversalAudioVoiceInit(v.dsp, 1, nullptr, def.volume, 1.0f,
                              0.0f, def.rateHz);
      startVoice(s, ev.name, def);
      return;
    }
    case TraversalAudioOp::kStop:
      // FUN_0040210c — release every live instance of the record.
      stopByName(ev.name);
      return;

    case TraversalAudioOp::kSpawnPositional:
    case TraversalAudioOp::kRestartPositional: {
      // FUN_00402288's restart form stops all instances of the name
      // first; the bare spawn (FUN_00402160) does not.
      if (ev.op == TraversalAudioOp::kRestartPositional)
        stopByName(ev.name);
      TraversalAudioSoundDef def;
      if (!res(ev.name, def)) { ++missing_; return; }
      ++resolved_;
      const int s = allocSlot();
      if (s < 0) return;    // pool exhausted — slot write skipped,
                            // owner keeps its stale handle (OBSERVED)
      Voice& v = voices_[s];
      v = Voice{};
      v.live = true;
      traversalAudioVoiceInit(v.dsp, ev.mode, ev.pos, ev.volume,
                              ev.rate, ev.range, def.rateHz);
      if (ev.ownerKey) {
        v.ownerCat = static_cast<int>(ev.owner);
        v.ownerKey = ev.ownerKey;
        // *ownerSlot = inst — overwrites any previous binding; the
        // orphaned instance keeps playing (OBSERVED).
        ownerVoices_[{v.ownerCat, v.ownerKey}] = s;
      }
      startVoice(s, ev.name, def);
      return;
    }

    case TraversalAudioOp::kRelease: {
      // FUN_004020b4 — release the owner's bound instance and clear
      // the slot. Ownerless release hits nothing (OBSERVED: the call
      // goes through the slot pointer).
      const auto it = ownerVoices_.find(
          {static_cast<int>(ev.owner), ev.ownerKey});
      if (it != ownerVoices_.end()) stopVoice(it->second);
      return;
    }

    case TraversalAudioOp::kSetVolume: {
      // FUN_00402698 — per-instance volume write. Stream seam: no
      // ported callsite emits this yet; apply to the owner-bound
      // voice's effective volume and push it.
      const auto it = ownerVoices_.find(
          {static_cast<int>(ev.owner), ev.ownerKey});
      if (it == ownerVoices_.end() || !voices_[it->second].live)
        return;
      Voice& v = voices_[it->second];
      v.dsp.effVol = ev.volume;
      TraversalAudioCmd c;
      c.op = TraversalAudioCmdOp::kParams;
      c.handle = it->second;
      c.name = v.name;
      c.vol = v.dsp.effVol;
      c.pan = v.dsp.pan;
      c.freqHz = v.dsp.freqHz;
      c.rateHz = v.rateHz;
      c.loop = v.loop;
      cmds_.push_back(c);
      return;
    }
    case TraversalAudioOp::kSetRate: {
      // FUN_0040247c/0046c2c4 path — inst+0x34 = rate, +0x44 =
      // trunc(recRate*rate); same stream-seam caveat as kSetVolume.
      const auto it = ownerVoices_.find(
          {static_cast<int>(ev.owner), ev.ownerKey});
      if (it == ownerVoices_.end() || !voices_[it->second].live)
        return;
      Voice& v = voices_[it->second];
      v.dsp.rate = ev.rate;
      v.dsp.freqHz = static_cast<int>(
          static_cast<double>(v.rateHz) * static_cast<double>(ev.rate));
      TraversalAudioCmd c;
      c.op = TraversalAudioCmdOp::kParams;
      c.handle = it->second;
      c.name = v.name;
      c.vol = v.dsp.effVol;
      c.pan = v.dsp.pan;
      c.freqHz = v.dsp.freqHz;
      c.rateHz = v.rateHz;
      c.loop = v.loop;
      cmds_.push_back(c);
      return;
    }
  }
}

void TraversalAudioMixer::tick(double dtSec, const OwnerPosFn& ownerPos) {
  for (int i = 0; i < kMaxVoices; ++i) {
    Voice& v = voices_[i];
    if (!v.live) continue;
    // mode bits 16..23 bit1 (0x20000): live owner position source —
    // the original reads *inst+0x10 each update. Refresh from the
    // owner; a missing owner keeps the last pos (stale-pointer read).
    if (v.ownerKey && (v.dsp.mode & 0x20000u)) {
      float p[3];
      if (ownerPos && ownerPos(v.ownerCat, v.ownerKey, p)) {
        v.dsp.pos[0] = p[0];
        v.dsp.pos[1] = p[1];
        v.dsp.pos[2] = p[2];
      }
    }
    const TraversalAudioPush push =
        traversalAudioVoiceTick(v.dsp, listener_);
    // Playhead — DS consumes freqHz source frames/sec; non-looping
    // buffers auto-stop at the end and the sweep reaps them.
    v.playhead += static_cast<double>(v.dsp.freqHz) * dtSec;
    if (!v.loop && v.pcmFrames > 0 &&
        v.playhead >= static_cast<double>(v.pcmFrames)) {
      stopVoice(i);
      continue;
    }
    if (push.vol || push.pan || push.freq) {
      TraversalAudioCmd c;
      c.op = TraversalAudioCmdOp::kParams;
      c.handle = i;
      c.name = v.name;
      c.vol = v.dsp.effVol;
      c.pan = v.dsp.pan;
      c.freqHz = v.dsp.freqHz;
      c.rateHz = v.rateHz;
      c.loop = v.loop;
      cmds_.push_back(c);
    }
  }
}

} // namespace mdk
