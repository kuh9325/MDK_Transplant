// mve_player.h — Phase 19E: Interplay MVE playback via FFmpeg.
//
// FUN_0047b674 (p19a_mode8.txt) runs MDKBZK.MVE through the original's
// Interplay MVE library: FUN_0041b004 opens the file (0 -> the
// FUN_00408eb0 frontend edge), FUN_00489a50 allocates a 640x480 output
// surface (the 432x320 movie presents aspect-preserved), and the
// PeekMessage pump ends on natural EOF or a WndProc-driven abort —
// FUN_0047d30a then tears down into FUN_00413b20's post-movie route.
//
// The codec itself is delegated to FFmpeg (LGPL 2.1+, dynamically
// linked — see THIRD_PARTY_NOTICES.md): ipmovie demuxer +
// interplay_video / interplay_dpcm decoders, verified against the
// shipped MDKBZK.MVE (3126 frames, 432x320 PAL8, 22050Hz stereo
// DPCM, zero decode errors). This file is the ONLY FFmpeg consumer;
// the core stays dependency-free.
//
// Memory bound: the audio stream is fully decoded at open (~18.5MB
// PCM for the shipped clip) while video packets are stored
// compressed (~28MB) — decode is strictly sequential, one frame at
// a time, nothing re-encoded.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace mdkbridge {

class MvePlayer {
public:
  MvePlayer() = default;
  ~MvePlayer();
  MvePlayer(const MvePlayer&) = delete;
  MvePlayer& operator=(const MvePlayer&) = delete;

  bool open(const char* path, std::string* err);

  int width() const { return vWidth_; }
  int height() const { return vHeight_; }
  // Stored compressed video packets == demuxed frame count.
  int64_t videoPackets() const { return static_cast<int64_t>(vPkts_.size()); }
  int64_t videoDecoded() const { return vDecoded_; }

  int audioRate() const { return aRate_; }
  int audioChannels() const { return aCh_; }
  double audioDurationSec() const {
    return aRate_ > 0 && aCh_ > 0
               ? static_cast<double>(aPcm_.size()) /
                     (aRate_ * aCh_ * 2.0)
               : 0.0;
  }
  // Interleaved s16 PCM ready for an AudioStreamWAV (FORMAT_16_BITS,
  // stereo, mix_rate = audioRate()).
  const std::vector<std::uint8_t>& audioPcm() const { return aPcm_; }
  int64_t audioPackets() const { return aPackets_; }
  int64_t audioDecodeErrors() const { return aErrors_; }
  int64_t videoDecodeErrors() const { return vErrors_; }

  // Sequential decode — Interplay frames are delta-coded; every
  // packet must decode even when the presenter skips a display.
  // Returns false at EOF or on unrecoverable error (err set).
  // `idx` refills with a pal8 surface `width*height`; `pal` only
  // when the decoder publishes a new palette (palDirty set).
  bool nextVideoFrame(std::vector<std::uint8_t>* idx,
                      std::array<std::uint8_t, 768>* pal,
                      bool* palDirty, double* ptsMs,
                      std::string* err);

  double lastPtsMs() const { return vLastPtsMs_; }
  bool eof() const { return vEof_; }

private:
  bool openStreams_(std::string* err);
  bool drainAudio_(std::string* err);
  void close_();

  // One AVFormatContext; video packets are stored compressed for
  // lazy sequential decode, audio packets decode in the open pass.
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* decA_ = nullptr;
  AVCodecContext* decV_ = nullptr;
  int aStream_ = -1;
  int vStream_ = -1;
  int vWidth_ = 0;
  int vHeight_ = 0;
  double vTbMs_ = 0.0;   // video stream time_base -> ms

  // av_packet_clone keeps the demuxer's side data (palette updates
  // ride AV_PKT_DATA_PALETTE into ff_copy_palette).
  std::vector<AVPacket*> vPkts_;
  std::size_t vNext_ = 0;
  int64_t vDecoded_ = 0;
  int64_t vErrors_ = 0;
  double vLastPtsMs_ = -1.0;
  bool vEof_ = false;
  bool vFlushed_ = false;

  std::vector<std::uint8_t> aPcm_;
  int aRate_ = 0;
  int aCh_ = 0;
  int64_t aPackets_ = 0;
  int64_t aErrors_ = 0;
};

}  // namespace mdkbridge
