// SNI sound-record RIFF/WAVE decode (Phase 17C.2) — payload bytes only.
//
// EVIDENCE (BUILD_A data census + Ghidra disasm of the sound path):
//
//   Every reachable traversal SFX record's payload IS a raw RIFF/WAVE
//   stream starting at the record's stored offset (the +4 content-blob
//   base is already folded into SniEntry::payloadFileOffset — there is
//   NO extra payload prefix on these records). Census of
//   TRAVERSE.SNI, MISC/MDKSOUND.SNI and LEVEL3..8 S.SNI:
//
//     * every non-sentinel SFX record is RIFF/WAVE, format tag 1 (PCM)
//     * every observed record is MONO
//     * sample widths observed: 8-bit (unsigned bias, RIFF PCM) and
//       16-bit (signed LE); FOOT1..FOOT4 are 16-bit
//     * observed rates span 6000..22050 Hz, per record
//     * `fmt ` chunk is 18 bytes (WAVE PCM with cbSize)
//
//   The SNI directory's +0x0c dword is `flags16 | (volume16 << 16)` —
//   OBSERVED via the sound loader splitting it into the loaded
//   record's +0x4 (flags: bit0 = DS-looping, bit1 = music-class) and
//   +0x8 (authored volume, e.g. 0x7fff / 0x5000 for BREATH). Flags
//   bit1 marks the big *O.SNI ambient/music streams — those take the
//   music path (FUN_0041d774 family), never the SFX instance pool.
//
// Scope rule: this parser decodes the WAVE payload ONLY. Record
// flags/volume stay with the caller; music-class records are rejected
// by policy BEFORE calling here (the decoder itself doesn't know the
// flag bit — it just reports the wave contents).

#ifndef MDK_CORE_SNI_WAVE_H
#define MDK_CORE_SNI_WAVE_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdk {

struct SniWave {
  int rateHz = 0;        // fmt nSamplesPerSec — kept verbatim
  int channels = 0;      // observed corpus is mono (1)
  int bitsPerSample = 0; // 8 or 16
  int blockAlign = 0;    // fmt nBlockAlign (frames = pcm/blockAlign)
  std::vector<std::uint8_t> pcm;   // data chunk bytes, verbatim
  std::size_t frames = 0;
};

enum class SniWaveStatus {
  kOk,
  kNotWave,              // RIFF/WAVE tags absent — not a wave payload
  kTruncated,            // chunk header/payload escapes the span
  kBadFormat,            // fmt chunk missing/short, or tag != 1 (PCM)
  kUnsupportedChannels,  // reachable corpus is mono-only
  kUnsupportedBits,      // reachable corpus is 8/16-bit PCM
  kBadData,              // no data chunk / zero frames / bad align
};

// Decode one record's WAVE payload. `riff` is the record payload span
// (SniEntry::payloadFileOffset() .. +payloadSize). Never fails
// catastrophically; status distinguishes "not this format" from
// "malformed". On kOk, `out` carries verbatim PCM (no resample, no
// normalization — the DS buffer path played the bytes as-authored).
SniWaveStatus decodeSniWave(std::span<const std::byte> riff,
                            SniWave* out,
                            std::string* detail = nullptr);

std::string_view sniWaveStatusName(SniWaveStatus s);

} // namespace mdk

#endif // MDK_CORE_SNI_WAVE_H
