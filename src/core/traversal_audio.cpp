#include "core/traversal_audio.h"

#include "core/traversal_runtime.h"

namespace mdk {

TraversalAudioEvent& traversalAudioEmit(TraversalRuntime& rt,
                                        TraversalAudioOp op,
                                        const char* name) {
  TraversalAudioEvent ev;
  ev.op = op;
  ev.seq = rt.audioSeq++;
  ev.frame = rt.frameCounter;
  if (name != nullptr) ev.name = name;
  rt.audioFx.push_back(std::move(ev));
  return rt.audioFx.back();
}

TraversalAudioEvent& traversalAudioEmit(TraversalRuntime& rt,
                                        TraversalAudioOp op,
                                        const std::string& name) {
  TraversalAudioEvent ev;
  ev.op = op;
  ev.seq = rt.audioSeq++;
  ev.frame = rt.frameCounter;
  ev.name = name;
  rt.audioFx.push_back(std::move(ev));
  return rt.audioFx.back();
}

TraversalAudioEvent& traversalAudioEmitPositional(
    TraversalRuntime& rt, TraversalAudioOp op, const char* name,
    const float pos[3], const void* ownerKey) {
  TraversalAudioEvent& ev = traversalAudioEmit(rt, op, name);
  ev.hasPos = true;
  for (int i = 0; i < 3; ++i) ev.pos[i] = pos[i];
  if (ownerKey != nullptr) {
    ev.owner = TraversalAudioOwner::kObject;
    ev.ownerKey = ownerKey;
  }
  return ev;
}

TraversalAudioEvent& traversalAudioEmitPositional(
    TraversalRuntime& rt, TraversalAudioOp op, const std::string& name,
    const float pos[3], const void* ownerKey) {
  return traversalAudioEmitPositional(rt, op, name.c_str(), pos, ownerKey);
}

std::string traversalAudioCmiName(const TraversalRuntime& rt,
                                  std::int32_t off) {
  if (off <= 0) return {};
  const std::size_t o = static_cast<std::size_t>(off);
  if (o >= rt.level.cmiBytes.size()) return {};
  const char* s =
      reinterpret_cast<const char*>(rt.level.cmiBytes.data() + o);
  std::size_t n = 0;
  while (o + n < rt.level.cmiBytes.size() && s[n] != '\0') ++n;
  return std::string(s, n);
}

const char* traversalAudioOpName(TraversalAudioOp op) {
  switch (op) {
    case TraversalAudioOp::kPlayOnce:          return "play";
    case TraversalAudioOp::kEnsurePlaying:     return "ensure";
    case TraversalAudioOp::kRestart:           return "restart";
    case TraversalAudioOp::kStop:              return "stop";
    case TraversalAudioOp::kSpawnPositional:   return "pos";
    case TraversalAudioOp::kRestartPositional: return "repos";
    case TraversalAudioOp::kRelease:           return "release";
    case TraversalAudioOp::kSetVolume:         return "vol";
    case TraversalAudioOp::kSetRate:           return "rate";
  }
  return "?";
}

} // namespace mdk
