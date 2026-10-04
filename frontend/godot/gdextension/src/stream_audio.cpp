#include "stream_audio.h"

#include <cstdio>

namespace mdkbridge {

void StreamAudioHost::reset() {
  regs_.clear();
  listener_ = mdk::TraversalAudioListener{};
  diag_ = StreamAudioDiag{};
  seq_ = 0;
}

void StreamAudioHost::bindSound(int tag, StreamSndReg reg) {
  regs_[tag] = std::move(reg);
}

const StreamSndReg* StreamAudioHost::soundReg(int tag) const {
  const auto it = regs_.find(tag);
  return it == regs_.end() ? nullptr : &it->second;
}

const StreamSndReg* StreamAudioHost::regForName(
    const std::string& name) const {
  for (const auto& [tag, reg] : regs_)
    if (reg.name == name) return &reg;
  return nullptr;
}

void StreamAudioHost::updateListener(const float camView[12],
                                     float frameScalar) {
  ++diag_.listenerUpdates;
  // 0x49ff5c <- the 0x540bb0 view snapshot — copied verbatim like
  // the original's 48-byte memcpy (row-major 3x4).
  for (int r = 0; r != 3; ++r)
    for (int c = 0; c != 4; ++c)
      listener_.m[r][c] = camView[r * 4 + c];
  listener_.zoom = 1.0f;   // 0x540b58 — only the 3D updater reads it
  listener_.frame = frameScalar;
  listener_.mode3d = false;  // 0x49ff58 byte1 stays 1 in mode 5
}

void StreamAudioHost::consume(
    const mdk::StreamEvent& ev, mdk::TraversalAudioMixer& mixer,
    const mdk::TraversalAudioMixer::ResolveFn& res) {
  using Kind = mdk::StreamEvent::Kind;
  if (ev.kind != Kind::kPlaySound && ev.kind != Kind::kStopSound)
    return;
  ++diag_.events;
  const auto counted = [this, &res](const std::string& n,
                                    mdk::TraversalAudioSoundDef& d) {
    if (res(n, d)) return true;
    ++diag_.resolveMisses;
    return false;
  };

  if (ev.kind == Kind::kStopSound) {
    // FUN_004020b4(eda84) — the port carries the record tag; resolve
    // the registered name, then the mixer's name-scoped release.
    const StreamSndReg* reg = soundReg(ev.tag);
    if (!reg) { ++diag_.unknownTags; return; }
    ++diag_.stops;
    ++diag_.stopNames[reg->name];
    mdk::TraversalAudioEvent tae;
    tae.op = mdk::TraversalAudioOp::kStop;
    tae.seq = ++seq_;
    tae.name = reg->name;
    mixer.applyEvent(tae, counted);
    return;
  }

  mdk::TraversalAudioEvent tae;
  tae.seq = ++seq_;
  if (ev.tag >= 0) {
    // Tag-bound slot play — the 02388 call form: aux==0 is the
    // ensure arm (d87d/d898/ca3x), aux!=0 the restart arm (d22c —
    // and WIND's flat 022b8, for which restart is degenerate
    // identical: no live instance exists at the only emission).
    const StreamSndReg* reg = soundReg(ev.tag);
    if (!reg) { ++diag_.unknownTags; return; }
    ++diag_.plays;
    ++diag_.playNames[reg->name];
    if (ev.aux == 0) {
      tae.op = mdk::TraversalAudioOp::kEnsurePlaying;
      ++diag_.ensurePlays;
    } else {
      tae.op = mdk::TraversalAudioOp::kRestart;
      ++diag_.restartPlays;
    }
    if (reg->loop) ++diag_.loopPlays;
    tae.name = reg->name;
  } else {
    // Name-bound 02160 marker: tag=-1, name=+0x140 text,
    // f[0..2]=+0x10 pos, aux=0x1000e — the unowned positional spawn
    // (vol 0x7fff, rate 1.0, range 50.0 are the OBSERVED stack args).
    ++diag_.plays;
    ++diag_.positional;
    ++diag_.playNames[ev.name];
    tae.op = mdk::TraversalAudioOp::kSpawnPositional;
    tae.name = ev.name;
    tae.mode = static_cast<std::uint32_t>(ev.aux);
    tae.pos[0] = ev.f[0];
    tae.pos[1] = ev.f[1];
    tae.pos[2] = ev.f[2];
    tae.hasPos = true;
    tae.volume = 0x7fff;
    tae.rate = 1.0f;
    tae.range = 50.0f;
  }
  mixer.applyEvent(tae, counted);
}

int streamAudioBindRegs(StreamAudioHost& host,
                        const mdk::BniDirectory& bdir) {
  // OBSERVED slot order: WIND,HITSIDE,RESCUE,APPLE into
  // eda58/eda5c/eda7c/eda80, then HURT1..7 into eda60..eda78 (the
  // binds are name->tag, so table order doesn't matter — only the
  // registration args: vol 0x7fff, ECX=1 loop on WIND alone).
  int bound = 0;
  const auto reg = [&](const char* name, bool loop) {
    const mdk::BniRecord* r = mdk::findBniRecord(bdir, name);
    if (!r) return;
    host.bindSound(static_cast<int>(r - bdir.records.data()),
                   {name, 0x7fff, loop});
    ++bound;
  };
  reg("WIND", true);
  reg("HITSIDE", false);
  reg("RESCUE", false);
  reg("APPLE", false);
  for (int i = 1; i != 8; ++i) {
    char nm[8];
    std::snprintf(nm, sizeof nm, "HURT%d", i);
    reg(nm, false);
  }
  return bound;
}

std::vector<std::uint8_t> pcmForGodotWav(const mdk::SniWave& wv) {
  std::vector<std::uint8_t> out = wv.pcm;
  if (wv.bitsPerSample == 8) {
    // RIFF PCM8 unsigned -> signed byte: s = u - 128 == u ^ 0x80.
    for (auto& b : out) b ^= 0x80;
  }
  return out;
}

} // namespace mdkbridge
