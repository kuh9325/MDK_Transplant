// Phase 19D — bounded Autodesk FLIC decoder for the mode-8 ending
// pipeline (FUN_00413c20 ctx init / FUN_00414158 frame decode;
// OBSERVED captures analysis-private/logs/p19a_flc_handlers.txt and
// p19a_mode8.txt).
//
// Format, per the captures + corpus files:
//   Header 0x80 bytes: u32 size @0, u16 magic @4 (0xAF11 -> speed is
//   jiffies -> ms = speed*1000/70; 0xAF12 -> speed is ms), u16
//   frames @6, u16 w @8, u16 h @10, u16 depth @12, u32 speed @0x10.
//   For 0xAF12 the frame stream starts at the u32 @0x50 (OBSERVED:
//   MDK12 carries 128 — no prefix; MDKEND carries 2906 — the 0xF100
//   prefix block between header and frame 0 is skipped). For 0xAF11
//   the stream starts at 0x80 unconditionally (FUN_00413c20's
//   `ctx+0x130 = 0x80` arm).
//
//   Frame record: u32 size, u16 magic, u16 chunks, u8[8] pad.
//   Magic 0xF1FA = frame; anything else (0xF100 prefix) is skipped
//   by size. Chunk: u32 size, u16 type. Dispatch table 0x49a750
//   (variant 0) handles:
//     4        COLOR256 — u16 packet count; per packet u8 entry-skip
//              (palette byte cursor += skip*3) then u8 entry count
//              (0 -> 256); count*3 raw bytes -> ctx+0x144 (the
//              0x4144b8 handler — EMPIRICAL on the corpus: MDK12's
//              frame-0 packet is skip=0/count=0 + exactly 768 bytes)
//     0xb      COLOR — same packet walk but count is a RAW BYTE
//              count (FUN_00414550 writes bVar1 bytes verbatim — a
//              count==0 packet writes nothing; preserve that)
//     7      DELTA_FLC — u16 line-ops; op words with 0xC000 set are
//              line controls (0x4000 -> line += (-w & 0xff) * pitch;
//              0x8000-only words are read and dropped), first clear
//              word = the line's word-pair count; per pair u8 column
//              skip (pixels) then i8 count: count >= 1 -> count
//              literal u16s, count <= 0 -> next u16 replicated -count
//              times (FUN_0041465c)
//     0xf    BRUN — per line u8 packet count (read, unused); packets:
//              i8 count: count > 0 -> next byte replicated count
//              times, count < 0 -> -count literal bytes (EMPIRICAL:
//              both directions tried against MDK12 frame 0 — only
//              this sign convention consumes exactly the chunk span)
//     0x10   LCOPY — literal w*h rows into the buffer (FUN_0041474c)
//     0xc/0xd/0x12 (PSTAMP thumbnail etc.) — skipped, OBSERVED.
//
// The decode buffer is indexed 8bpp, pitch == width; the palette is
// the 768-byte RGB table the COLOR chunks maintain.

#ifndef MDK_CORE_FLIC_DECODER_H
#define MDK_CORE_FLIC_DECODER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdk {

class FlicDecoder {
public:
  // Parse the 0x80 header and position the stream at frame 0.
  // `file` must outlive the decoder (the span aliases it).
  bool open(std::span<const std::byte> file, std::string* detail);

  // FUN_00414158 — decode the next frame record into pixels().
  // Returns false at the end of the stream (decoded == frameCount).
  // paletteDirty() reports whether a COLOR chunk ran this frame.
  bool nextFrame(std::string* detail);

  int width() const { return w_; }
  int height() const { return h_; }
  int frameCount() const { return totalFrames_; }
  int decoded() const { return decoded_; }
  int frameMs() const { return frameMs_; }
  bool paletteDirty() const { return palDirty_; }
  std::span<const std::byte> pixels() const {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pix_.data()), pix_.size());
  }
  const std::array<std::uint8_t, 768>& palette() const {
    return pal_;
  }

private:
  std::span<const std::byte> file_;
  std::vector<std::uint8_t> pix_;
  std::array<std::uint8_t, 768> pal_{};
  std::size_t pos_ = 0;
  int w_ = 0, h_ = 0;
  int totalFrames_ = 0;
  int decoded_ = 0;
  int frameMs_ = 33;
  bool palDirty_ = false;

  bool chunkColor256(std::span<const std::byte> p, std::string* detail);
  bool chunkColor(std::span<const std::byte> p, std::string* detail);
  bool chunkDeltaFlc(std::span<const std::byte> p, std::string* detail);
  bool chunkBrun(std::span<const std::byte> p, std::string* detail);
  bool chunkLcopy(std::span<const std::byte> p, std::string* detail);
};

}  // namespace mdk

#endif  // MDK_CORE_FLIC_DECODER_H
