#include "core/sni_wave.h"

#include "core/binary_reader.h"

#include <cstdio>

namespace mdk {

namespace {

bool tagEq(std::span<const std::byte> s, const char* t) {
  return s.size() >= 4 && s[0] == std::byte(t[0]) &&
         s[1] == std::byte(t[1]) && s[2] == std::byte(t[2]) &&
         s[3] == std::byte(t[3]);
}

void fail(std::string* d, const char* msg) {
  if (d) *d = msg;
}

} // namespace

SniWaveStatus decodeSniWave(std::span<const std::byte> riff,
                            SniWave* out, std::string* detail) {
  if (detail) detail->clear();
  // RIFF header: "RIFF" u32 size "WAVE" — the size field is the
  // observed file-style byte count (payload minus 8); tolerate a
  // short/long file the same way a chunk walker does (bounds win).
  if (riff.size() < 12 || !tagEq(riff.subspan(0), "RIFF") ||
      !tagEq(riff.subspan(8), "WAVE")) {
    fail(detail, "not RIFF/WAVE");
    return SniWaveStatus::kNotWave;
  }
  BinaryReader r(riff);
  if (!r.seek(12)) {
    fail(detail, "header truncated");
    return SniWaveStatus::kTruncated;
  }
  bool haveFmt = false;
  std::uint16_t tag = 0;
  std::uint16_t channels = 0, blockAlign = 0, bits = 0;
  std::uint32_t rateHz = 0;
  std::size_t dataOff = 0, dataLen = 0;
  while (r.remaining() >= 8) {
    const std::size_t chunkOff = r.position();
    const auto cid = riff.subspan(chunkOff, 4);
    r.seek(chunkOff + 4);
    const auto sizeOpt = r.u32le();
    if (!sizeOpt) break;
    const std::uint32_t csz = *sizeOpt;
    const std::size_t cdata = r.position();
    if (csz > riff.size() - cdata) {
      // Chunk size escapes the payload — stop; the data chunk may
      // already be collected.
      break;
    }
    if (tagEq(cid, "fmt ")) {
      if (csz < 16) {
        fail(detail, "fmt chunk short");
        return SniWaveStatus::kBadFormat;
      }
      BinaryReader f(riff.subspan(cdata, csz));
      const auto tg = f.u16le();
      const auto ch = f.u16le();
      const auto hz = f.u32le();
      f.u32le();                        // nAvgBytesPerSec
      const auto ba = f.u16le();
      const auto bp = f.u16le();
      if (!tg || !ch || !hz || !ba || !bp) {
        fail(detail, "fmt chunk truncated");
        return SniWaveStatus::kBadFormat;
      }
      tag = *tg;
      channels = *ch;
      rateHz = *hz;
      blockAlign = *ba;
      bits = *bp;
      haveFmt = true;
    } else if (tagEq(cid, "data")) {
      dataOff = cdata;
      dataLen = csz;
      break;  // the observed records carry data last
    }
    // Chunks are word-aligned in RIFF — skip pad byte on odd sizes.
    const std::size_t next = cdata + csz + (csz & 1u);
    if (next > riff.size()) break;
    if (!r.seek(next)) break;
  }
  if (!haveFmt) {
    fail(detail, "no fmt chunk");
    return SniWaveStatus::kBadFormat;
  }
  if (tag != 1) {
    fail(detail, "non-PCM wave tag");
    return SniWaveStatus::kBadFormat;
  }
  if (channels != 1 && channels != 2) {
    fail(detail, "non-mono/stereo wave");
    return SniWaveStatus::kUnsupportedChannels;
  }
  if (bits != 8 && bits != 16) {
    fail(detail, "unsupported sample width");
    return SniWaveStatus::kUnsupportedBits;
  }
  if (dataOff == 0 || dataLen == 0) {
    fail(detail, "no data chunk");
    return SniWaveStatus::kBadData;
  }
  if (out) {
    out->channels = channels;
    out->rateHz = static_cast<int>(rateHz);
    out->blockAlign = blockAlign;
    out->bitsPerSample = bits;
    if (blockAlign == 0) {
      fail(detail, "zero block align");
      return SniWaveStatus::kBadData;
    }
    out->frames = dataLen / static_cast<std::size_t>(blockAlign);
    if (out->frames == 0) {
      fail(detail, "zero frames");
      return SniWaveStatus::kBadData;
    }
    out->pcm.assign(reinterpret_cast<const std::uint8_t*>(
                        riff.data() + dataOff),
                    reinterpret_cast<const std::uint8_t*>(
                        riff.data() + dataOff) + dataLen);
  }
  return SniWaveStatus::kOk;
}

std::string_view sniWaveStatusName(SniWaveStatus s) {
  switch (s) {
    case SniWaveStatus::kOk:                 return "ok";
    case SniWaveStatus::kNotWave:            return "not_wave";
    case SniWaveStatus::kTruncated:          return "truncated";
    case SniWaveStatus::kBadFormat:          return "bad_format";
    case SniWaveStatus::kUnsupportedChannels:return "channels";
    case SniWaveStatus::kUnsupportedBits:    return "bits";
    case SniWaveStatus::kBadData:            return "bad_data";
  }
  return "?";
}

} // namespace mdk
