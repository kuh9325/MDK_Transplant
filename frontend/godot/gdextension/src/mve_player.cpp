// mve_player.cpp — FFmpeg-backed Interplay MVE player. See header
// for the evidence trail (FUN_0047b674) and the LGPL notice.

#include "mve_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#include <cstring>

namespace mdkbridge {

MvePlayer::~MvePlayer() { close_(); }

void MvePlayer::close_() {
  for (AVPacket* p : vPkts_) av_packet_free(&p);
  vPkts_.clear();
  if (decA_) avcodec_free_context(&decA_);
  if (decV_) avcodec_free_context(&decV_);
  if (fmt_) avformat_close_input(&fmt_);
  aPcm_.clear();
  aPcm_.shrink_to_fit();
}

bool MvePlayer::open(const char* path, std::string* err) {
  close_();
  aRate_ = 0;
  aCh_ = 0;
  aPackets_ = 0;
  aErrors_ = 0;
  vDecoded_ = 0;
  vErrors_ = 0;
  vLastPtsMs_ = -1.0;
  vEof_ = false;
  vFlushed_ = false;
  vNext_ = 0;
  aStream_ = -1;
  vStream_ = -1;
  vWidth_ = vHeight_ = 0;

  int rc = avformat_open_input(&fmt_, path, nullptr, nullptr);
  if (rc < 0) {
    char b[128];
    av_strerror(rc, b, sizeof(b));
    if (err) *err = std::string("avformat_open_input: ") + b;
    return false;
  }
  rc = avformat_find_stream_info(fmt_, nullptr);
  if (rc < 0) {
    char b[128];
    av_strerror(rc, b, sizeof(b));
    if (err) *err = std::string("find_stream_info: ") + b;
    close_();
    return false;
  }
  for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
    const AVCodecParameters* cp = fmt_->streams[i]->codecpar;
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO && vStream_ < 0)
      vStream_ = static_cast<int>(i);
    else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && aStream_ < 0)
      aStream_ = static_cast<int>(i);
  }
  if (vStream_ < 0 && aStream_ < 0) {
    if (err) *err = "no audio/video streams";
    close_();
    return false;
  }
  return openStreams_(err) && drainAudio_(err);
}

bool MvePlayer::openStreams_(std::string* err) {
  if (aStream_ >= 0) {
    const AVCodecParameters* cp = fmt_->streams[aStream_]->codecpar;
    const AVCodec* c = avcodec_find_decoder(cp->codec_id);
    if (!c || !(decA_ = avcodec_alloc_context3(c)) ||
        avcodec_parameters_to_context(decA_, cp) < 0 ||
        avcodec_open2(decA_, c, nullptr) < 0) {
      if (err) *err = "audio decoder open failed";
      return false;
    }
    aRate_ = decA_->sample_rate;
    aCh_ = decA_->ch_layout.nb_channels;
  }
  if (vStream_ >= 0) {
    const AVCodecParameters* cp = fmt_->streams[vStream_]->codecpar;
    const AVCodec* c = avcodec_find_decoder(cp->codec_id);
    if (!c || !(decV_ = avcodec_alloc_context3(c)) ||
        avcodec_parameters_to_context(decV_, cp) < 0 ||
        avcodec_open2(decV_, c, nullptr) < 0) {
      if (err) *err = "video decoder open failed";
      return false;
    }
    vWidth_ = cp->width;
    vHeight_ = cp->height;
    const AVRational tb = fmt_->streams[vStream_]->time_base;
    vTbMs_ = 1000.0 * static_cast<double>(tb.num) /
             static_cast<double>(tb.den);
  }
  return true;
}

// Single demux pass: audio packets decode immediately into aPcm_;
// video packets store compressed for the sequential lazy pump.
bool MvePlayer::drainAudio_(std::string* err) {
  AVPacket* pkt = av_packet_alloc();
  AVFrame* fr = av_frame_alloc();
  if (!pkt || !fr) {
    if (err) *err = "alloc failed";
    av_packet_free(&pkt);
    av_frame_free(&fr);
    return false;
  }
  bool ok = true;
  while (true) {
    const int rc = av_read_frame(fmt_, pkt);
    if (rc == AVERROR_EOF) break;
    if (rc < 0) {
      ++aErrors_;
      av_packet_unref(pkt);
      continue;   // demuxer-level hiccup — keep scanning
    }
    if (pkt->stream_index == vStream_) {
      AVPacket* cp = av_packet_clone(pkt);
      if (cp) vPkts_.push_back(cp);
      else ++vErrors_;
    } else if (pkt->stream_index == aStream_ && decA_) {
      ++aPackets_;
      if (avcodec_send_packet(decA_, pkt) == 0) {
        while (avcodec_receive_frame(decA_, fr) == 0) {
          const int nb = fr->nb_samples;
          const int ch = aCh_;
          if (av_sample_fmt_is_planar(
                  static_cast<AVSampleFormat>(fr->format))) {
            for (int s = 0; s < nb; ++s)
              for (int c = 0; c < ch; ++c) {
                const std::int16_t v =
                    reinterpret_cast<const std::int16_t*>(
                        fr->extended_data[c])[s];
                aPcm_.push_back(v & 0xff);
                aPcm_.push_back((v >> 8) & 0xff);
              }
          } else {
            const std::uint8_t* p =
                reinterpret_cast<const std::uint8_t*>(
                    fr->extended_data[0]);
            aPcm_.insert(aPcm_.end(), p,
                         p + static_cast<std::size_t>(nb) * ch * 2);
          }
          av_frame_unref(fr);
        }
      } else {
        ++aErrors_;
      }
    }
    av_packet_unref(pkt);
  }
  // Flush the audio decoder tail.
  if (decA_) {
    avcodec_send_packet(decA_, nullptr);
    while (avcodec_receive_frame(decA_, fr) == 0) {
      const std::uint8_t* p =
          reinterpret_cast<const std::uint8_t*>(fr->extended_data[0]);
      aPcm_.insert(aPcm_.end(), p,
                   p + static_cast<std::size_t>(fr->nb_samples) *
                           aCh_ * 2);
      av_frame_unref(fr);
    }
  }
  av_packet_free(&pkt);
  av_frame_free(&fr);
  if (!ok && err) *err = "audio drain errors";
  return true;
}

bool MvePlayer::nextVideoFrame(std::vector<std::uint8_t>* idx,
                               std::array<std::uint8_t, 768>* pal,
                               bool* palDirty, double* ptsMs,
                               std::string* err) {
  if (palDirty) *palDirty = false;
  if (vEof_ || !decV_) return false;
  AVFrame* fr = av_frame_alloc();
  if (!fr) {
    if (err) *err = "alloc failed";
    return false;
  }
  while (true) {
    const int rc = avcodec_receive_frame(decV_, fr);
    if (rc == AVERROR_EOF) {
      av_frame_free(&fr);
      vEof_ = true;
      return false;
    }
    if (rc == AVERROR(EAGAIN)) {
      // Decoder needs input — feed the next stored packet. FFmpeg
      // treats the packet as consumed even on send error, so a
      // hard failure advances the queue rather than stalling.
      if (vNext_ < vPkts_.size()) {
        AVPacket* pkt = vPkts_[vNext_];
        vPkts_[vNext_++] = nullptr;   // consumed — close_ skips it
        const int src = avcodec_send_packet(decV_, pkt);
        av_packet_free(&pkt);
        if (src < 0 && src != AVERROR(EAGAIN)) ++vErrors_;
        continue;
      }
      if (!vFlushed_) {
        vFlushed_ = true;
        avcodec_send_packet(decV_, nullptr);   // drain
        continue;
      }
      av_frame_free(&fr);
      vEof_ = true;
      return false;
    }
    if (rc < 0) {
      av_frame_free(&fr);
      ++vErrors_;
      continue;   // decoder resyncs on the next packet
    }
    ++vDecoded_;
    if (fr->best_effort_timestamp != AV_NOPTS_VALUE)
      vLastPtsMs_ = fr->best_effort_timestamp * vTbMs_;
    else
      vLastPtsMs_ += 1000.0 / 15.0;   // MVE nominal 15fps
    if (ptsMs) *ptsMs = vLastPtsMs_;

    const int w = vWidth_, h = vHeight_;
    idx->resize(static_cast<std::size_t>(w) * h);
    if (fr->linesize[0] == w) {
      std::memcpy(idx->data(), fr->data[0],
                  static_cast<std::size_t>(w) * h);
    } else {
      for (int y = 0; y < h; ++y)
        std::memcpy(idx->data() + y * w,
                    fr->data[0] + y * fr->linesize[0], w);
    }
    // FFmpeg 9: PAL8 palettes live in data[1] (AVPALETTE_SIZE B,
    // 256 x RGBA u32), populated on every frame.
    if (fr->data[1] && pal && palDirty) {
      const std::uint32_t* p32 =
          reinterpret_cast<const std::uint32_t*>(fr->data[1]);
      std::array<std::uint8_t, 768> np{};
      for (int i = 0; i < 256; ++i) {
        const std::uint32_t v = p32[i];
        np[i * 3 + 0] = static_cast<std::uint8_t>((v >> 16) & 0xff);
        np[i * 3 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
        np[i * 3 + 2] = static_cast<std::uint8_t>(v & 0xff);
      }
      if (np != *pal || vDecoded_ == 1) {
        *pal = np;
        *palDirty = true;
      }
    }
    av_frame_free(&fr);
    return true;
  }
  vEof_ = true;
  return false;
}

}  // namespace mdkbridge
