#include "core/flic_decoder.h"

#include <cstring>

namespace mdk {

namespace {

bool rd16(std::span<const std::byte> f, std::size_t o, std::uint16_t* v) {
  if (o + 2 > f.size()) return false;
  *v = static_cast<std::uint16_t>(static_cast<unsigned char>(f[o]) |
       (static_cast<unsigned char>(f[o + 1]) << 8));
  return true;
}
bool rd32(std::span<const std::byte> f, std::size_t o, std::uint32_t* v) {
  if (o + 4 > f.size()) return false;
  *v = static_cast<std::uint32_t>(static_cast<unsigned char>(f[o])) |
       (static_cast<std::uint32_t>(static_cast<unsigned char>(f[o + 1]))
        << 8) |
       (static_cast<std::uint32_t>(static_cast<unsigned char>(f[o + 2]))
        << 16) |
       (static_cast<std::uint32_t>(static_cast<unsigned char>(f[o + 3]))
        << 24);
  return true;
}
bool fail(std::string* d, const char* msg) {
  if (d) *d = msg;
  return false;
}

}  // namespace

bool FlicDecoder::open(std::span<const std::byte> file,
                       std::string* detail) {
  if (file.size() < 128) return fail(detail, "FLIC header truncated");
  std::uint16_t magic, frames, w, h, depth;
  std::uint32_t speed, dataOff;
  if (!rd16(file, 4, &magic) || !rd16(file, 6, &frames) ||
      !rd16(file, 8, &w) || !rd16(file, 10, &h) ||
      !rd16(file, 12, &depth) || !rd32(file, 0x10, &speed) ||
      !rd32(file, 0x50, &dataOff)) {
    return fail(detail, "FLIC header truncated");
  }
  if (magic == 0xaf11) {
    frameMs_ = static_cast<int>((speed * 1000u) / 70u);
    pos_ = 0x80;   // FUN_00413c20: ctx+0x130 = 0x80
  } else if (magic == 0xaf12) {
    frameMs_ = static_cast<int>(speed);
    pos_ = dataOff != 0 ? dataOff : 0x80;   // ctx+0x130 = header@0x50
  } else {
    return fail(detail, "not FLIC (magic)");
  }
  if (depth != 8 || w == 0 || h == 0 ||
      w > 4096 || h > 4096) {
    return fail(detail, "unsupported FLIC geometry");
  }
  file_ = file;
  w_ = w;
  h_ = h;
  totalFrames_ = frames;
  decoded_ = 0;
  palDirty_ = false;
  pix_.assign(static_cast<std::size_t>(w_) * h_, 0);
  pal_.fill(0);
  return true;
}

bool FlicDecoder::nextFrame(std::string* detail) {
  palDirty_ = false;
  if (decoded_ >= totalFrames_) return false;
  while (pos_ + 16 <= file_.size()) {
    std::uint32_t fsize;
    std::uint16_t fmagic, chunks;
    rd32(file_, pos_, &fsize);
    rd16(file_, pos_ + 4, &fmagic);
    rd16(file_, pos_ + 6, &chunks);
    if (fmagic != 0xf1fa) {
      // 0xF100 prefix / any non-frame record — skip by size.
      if (fsize < 6 || pos_ + fsize > file_.size()) {
        return fail(detail, "FLIC record escapes file");
      }
      pos_ += fsize;
      continue;
    }
    if (fsize < 16 || pos_ + fsize > file_.size()) {
      return fail(detail, "FLIC frame escapes file");
    }
    std::size_t cp = pos_ + 16;
    const std::size_t fend = pos_ + fsize;
    for (std::uint16_t c = 0; c < chunks; ++c) {
      if (cp + 6 > fend) return fail(detail, "chunk head truncated");
      std::uint32_t csize;
      std::uint16_t ctype;
      rd32(file_, cp, &csize);
      rd16(file_, cp + 4, &ctype);
      if (csize < 6 || cp + csize > fend) {
        return fail(detail, "chunk escapes frame");
      }
      const std::span<const std::byte> payload =
          file_.subspan(cp + 6, csize - 6);
      bool ok = true;
      switch (ctype) {
        case 0x4:   ok = chunkColor256(payload, detail); break;
        case 0xb:   ok = chunkColor(payload, detail);    break;
        case 0x7:   ok = chunkDeltaFlc(payload, detail); break;
        case 0xf:   ok = chunkBrun(payload, detail);     break;
        case 0x10:  ok = chunkLcopy(payload, detail);    break;
        default:    break;   // 0xc/0xd/0x12 etc. — skipped (OBSERVED)
      }
      if (!ok) return false;
      cp += csize;
    }
    pos_ = fend;
    ++decoded_;
    return true;
  }
  return false;   // stream exhausted before frameCount — tolerated
}

// FUN_00414550 — u16 packets; per packet u8 entry-skip (palette byte
// cursor += skip*3) then u8 byte-count; count raw bytes -> ctx+0x144.
bool FlicDecoder::chunkColor(std::span<const std::byte> p,
                             std::string* detail) {
  std::uint16_t packets;
  if (!rd16(p, 0, &packets)) return fail(detail, "COLOR head short");
  std::size_t r = 2;
  std::size_t at = 0;                 // palette byte cursor
  for (std::uint16_t i = 0; i < packets; ++i) {
    if (r + 2 > p.size()) return fail(detail, "COLOR pkt truncated");
    const std::uint8_t skip = static_cast<std::uint8_t>(p[r]);
    const std::uint8_t count = static_cast<std::uint8_t>(p[r + 1]);
    r += 2;
    at += static_cast<std::size_t>(skip) * 3;
    if (at + count > pal_.size() || r + count > p.size()) {
      return fail(detail, "COLOR pkt escapes");
    }
    for (std::uint8_t k = 0; k < count; ++k)
      pal_[at++] = static_cast<std::uint8_t>(p[r++]);
  }
  palDirty_ = true;
  return true;
}

// 0x4144b8 (handler table slot 0, label only — body not captured) —
// COLOR256. Same packet walk as FUN_00414550 but the count byte is
// an ENTRY count where 0 -> 256: each entry is 3 raw bytes. The
// corpus confirms it: MDK12/MDKEND's lone type-4 chunk is exactly
// 2+2+768 bytes — one skip=0/count=0 packet covering 256 entries.
bool FlicDecoder::chunkColor256(std::span<const std::byte> p,
                                std::string* detail) {
  std::uint16_t packets;
  if (!rd16(p, 0, &packets)) return fail(detail, "COLOR256 head short");
  std::size_t r = 2;
  std::size_t at = 0;
  for (std::uint16_t i = 0; i < packets; ++i) {
    if (r + 2 > p.size()) return fail(detail, "COLOR256 pkt truncated");
    const std::uint8_t skip = static_cast<std::uint8_t>(p[r]);
    const std::size_t count =
        p[r + 1] == std::byte(0) ? 256 : static_cast<std::uint8_t>(p[r + 1]);
    r += 2;
    at += static_cast<std::size_t>(skip) * 3;
    if (at + count * 3 > pal_.size() || r + count * 3 > p.size()) {
      return fail(detail, "COLOR256 pkt escapes");
    }
    for (std::size_t k = 0; k < count * 3; ++k)
      pal_[at++] = static_cast<std::uint8_t>(p[r++]);
  }
  palDirty_ = true;
  return true;
}

// FUN_0041465c — DELTA_FLC. First u16 = line-ops count; per line the
// op-word walk then the word-pair run.
bool FlicDecoder::chunkDeltaFlc(std::span<const std::byte> p,
                                std::string* detail) {
  std::uint16_t lineOps;
  if (!rd16(p, 0, &lineOps)) return fail(detail, "DELTA head short");
  std::size_t r = 2;
  std::size_t line = 0;
  const std::size_t pitch = static_cast<std::size_t>(w_);
  for (std::uint32_t n = 0; n < static_cast<std::uint32_t>(lineOps);
       ++n) {
    std::uint16_t w;
    std::size_t ops = 0;
    for (;;) {
      if (!rd16(p, r, &w)) return fail(detail, "DELTA op truncated");
      r += 2;
      if ((w & 0xc000) == 0) { ops = w; break; }
      if (w & 0x4000) {
        line += (static_cast<std::uint32_t>(-static_cast<std::int32_t>(
                    static_cast<std::int32_t>(w) & 0xffffu)) &
                 0xffu);   // (-w) & 0xff line skip
      }
      // 0x8000-only words are read and dropped (OBSERVED).
      if (line > static_cast<std::size_t>(h_)) {
        return fail(detail, "DELTA line overflow");
      }
    }
    if (line >= static_cast<std::size_t>(h_)) {
      return fail(detail, "DELTA past last line");
    }
    std::size_t col = line * pitch;
    for (std::size_t pair = 0; pair < ops; ++pair) {
      if (r + 2 > p.size()) return fail(detail, "DELTA pair short");
      const std::uint8_t skip = static_cast<std::uint8_t>(p[r]);
      const std::int8_t count = static_cast<std::int8_t>(
          static_cast<unsigned char>(p[r + 1]));
      r += 2;
      col += skip;                       // byte (pixel) units
      if (col > line * pitch + pitch) {
        return fail(detail, "DELTA column overflow");
      }
      if (count < 1) {
        // count <= 0 — replicate the next u16 -count times.
        std::uint16_t wv;
        if (!rd16(p, r, &wv)) return fail(detail, "DELTA run short");
        r += 2;
        const std::uint32_t reps = static_cast<std::uint32_t>(-count);
        if (col + reps * 2 > line * pitch + pitch) {
          return fail(detail, "DELTA run overflow");
        }
        for (std::uint32_t k = 0; k < reps; ++k) {
          pix_[col++] = static_cast<std::uint8_t>(wv & 0xff);
          pix_[col++] = static_cast<std::uint8_t>(wv >> 8);
        }
      } else {
        if (col + static_cast<std::size_t>(count) * 2 >
                line * pitch + pitch ||
            r + static_cast<std::size_t>(count) * 2 > p.size()) {
          return fail(detail, "DELTA literal overflow");
        }
        for (std::size_t k = 0; k < static_cast<std::size_t>(count) * 2;
             ++k) {
          pix_[col++] = static_cast<std::uint8_t>(p[r++]);
        }
      }
    }
    ++line;
  }
  return true;
}

// BRUN (handler 0x4145d8 was not captured; the sign convention below
// is EMPIRICAL — only this direction consumes exactly the chunk span
// on the corpus files: positive count replicates the next byte,
// negative copies literals; the per-line packet count byte is read
// and unused).
bool FlicDecoder::chunkBrun(std::span<const std::byte> p,
                            std::string* detail) {
  std::size_t r = 0;
  const std::size_t pitch = static_cast<std::size_t>(w_);
  for (std::size_t line = 0; line < static_cast<std::size_t>(h_);
       ++line) {
    if (r >= p.size()) return fail(detail, "BRUN line truncated");
    ++r;                               // packet-count byte (unused)
    std::size_t col = line * pitch;
    const std::size_t lend = col + pitch;
    while (col < lend) {
      if (r >= p.size()) return fail(detail, "BRUN packet truncated");
      const std::int8_t count = static_cast<std::int8_t>(
          static_cast<unsigned char>(p[r++]));
      if (count > 0) {
        if (r >= p.size()) return fail(detail, "BRUN run short");
        const std::uint8_t v = static_cast<std::uint8_t>(p[r++]);
        if (col + count > lend) return fail(detail, "BRUN overflow");
        std::memset(pix_.data() + col, v,
                    static_cast<std::size_t>(count));
        col += static_cast<std::size_t>(count);
      } else {
        const std::size_t n = static_cast<std::size_t>(-count);
        if (r + n > p.size() || col + n > lend) {
          return fail(detail, "BRUN literal overflow");
        }
        std::memcpy(pix_.data() + col, p.data() + r, n);
        r += n;
        col += n;
      }
    }
  }
  return true;
}

// FUN_0041474c — literal w*h copy into the buffer.
bool FlicDecoder::chunkLcopy(std::span<const std::byte> p,
                             std::string* detail) {
  const std::size_t need =
      static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
  if (p.size() < need) return fail(detail, "LCOPY short");
  std::memcpy(pix_.data(), p.data(), need);
  return true;
}

}  // namespace mdk
