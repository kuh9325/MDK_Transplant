#include "gif_decode.h"

#include <array>
#include <cstring>
#include <vector>

namespace mdkbridge {

namespace {

struct Reader {
  std::span<const std::uint8_t> b;
  std::size_t pos = 0;
  bool take(std::uint8_t* out, std::size_t n) {
    if (pos + n > b.size()) return false;
    std::memcpy(out, b.data() + pos, n);
    pos += n;
    return true;
  }
  bool u8(std::uint8_t& v) { return take(&v, 1); }
  bool u16(std::uint16_t& v) {
    std::uint8_t t[2];
    if (!take(t, 2)) return false;
    v = static_cast<std::uint16_t>(t[0] | (t[1] << 8));
    return true;
  }
  bool skip(std::size_t n) {
    if (pos + n > b.size()) return false;
    pos += n;
    return true;
  }
  // GIF sub-block chain: repeated [u8 len][len bytes] until len==0.
  bool skipSubBlocks() {
    std::uint8_t len;
    do {
      if (!u8(len)) return false;
      if (!skip(len)) return false;
    } while (len != 0);
    return true;
  }
};

// GIF LZW: the sub-block chain is concatenated (bounded by the
// input size) then read LSB-first at the current code width.
// Standard table: clear = 1<<minCode, eoi = clear+1, codes grow to
// 12 bits. Emits indices into `out` (cap = pixel capacity); the
// stream may legally end by terminator block or EOI code.
bool lzwDecode(Reader& r, std::uint8_t* out, std::size_t cap) {
  std::uint8_t minCode;
  if (!r.u8(minCode) || minCode < 2 || minCode > 8) return false;

  std::vector<std::uint8_t> stream;
  for (;;) {
    std::uint8_t blkLen;
    if (!r.u8(blkLen)) return false;
    if (blkLen == 0) break;
    const std::size_t at = stream.size();
    stream.resize(at + blkLen);
    if (!r.take(stream.data() + at, blkLen)) return false;
  }

  const int clear = 1 << minCode;
  const int eoi = clear + 1;
  std::array<std::uint16_t, 4096> prefix{};
  std::array<std::uint8_t, 4096> suffix{};
  std::array<std::uint8_t, 4096> stack{};
  int next, codeSize;
  auto resetTable = [&] {
    next = eoi + 1;
    codeSize = minCode + 1;
  };
  resetTable();

  std::size_t outCount = 0;
  std::size_t stackLen = 0;
  int prevCode = -1;
  std::uint32_t acc = 0;
  int accBits = 0;
  for (std::size_t i = 0; i < stream.size(); ++i) {
    acc |= static_cast<std::uint32_t>(stream[i]) << accBits;
    accBits += 8;
    while (accBits >= codeSize) {
      const int code =
          static_cast<int>(acc & ((1u << codeSize) - 1u));
      acc >>= codeSize;
      accBits -= codeSize;
      if (code == clear) {
        resetTable();
        prevCode = -1;
        continue;
      }
      if (code == eoi) return true;
      if (code > next) return false;          // unminted dict entry
      const bool kwkwk = (code == next);      // emit = prev + prev[0]
      if (kwkwk && prevCode < 0) return false;

      // Emit string: dict codes walk the prefix chain; literals emit
      // directly. KwKwK appends the first byte of the prev string.
      int w = kwkwk ? prevCode : code;
      while (w > clear) {
        if (w >= next || stackLen >= 4096) return false;
        stack[stackLen++] = suffix[w];
        w = prefix[w];
      }
      const std::uint8_t first = static_cast<std::uint8_t>(w);
      if (stackLen >= 4096) return false;
      stack[stackLen++] = first;
      while (stackLen > 0) {
        if (outCount >= cap) return false;
        out[outCount++] = stack[--stackLen];
      }
      if (kwkwk) {
        if (outCount >= cap) return false;
        out[outCount++] = first;
      }
      if (prevCode >= 0 && next < 4096) {
        prefix[next] = static_cast<std::uint16_t>(prevCode);
        suffix[next] = first;
        ++next;
        if (next == (1 << codeSize) && codeSize < 12) ++codeSize;
      }
      prevCode = code;
    }
  }
  // Stream exhausted without EOI — accept only if the pixel buffer
  // is exactly full (the original tolerates the same boundary).
  return outCount == cap;
}

}  // namespace

std::optional<mdk::IndexedImage> decodeGifImage(
    std::span<const std::uint8_t> bytes) {
  Reader r{bytes, 0};
  std::uint8_t hdr[6];
  if (!r.take(hdr, 6)) return std::nullopt;
  if (!(hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F' &&
        hdr[3] == '8' && (hdr[4] == '7' || hdr[4] == '9') &&
        hdr[5] == 'a')) {
    return std::nullopt;
  }
  std::uint16_t lw, lh;
  std::uint8_t packed, bg, aspect;
  if (!r.u16(lw) || !r.u16(lh) || !r.u8(packed) || !r.u8(bg) ||
      !r.u8(aspect)) {
    return std::nullopt;
  }
  // The slide gate is exactly 600x360 (FUN_00416e98's decode
  // dimension — the core probe slides the same check).
  if (lw != 600 || lh != 360) return std::nullopt;

  std::array<std::uint8_t, 768> gct{};
  bool haveGct = false;
  if (packed & 0x80) {
    const std::size_t n =
        3u * (1u << ((packed & 0x07) + 1));
    if (n > gct.size() || !r.take(gct.data(), n)) return std::nullopt;
    haveGct = true;
  }

  // Block walk: extensions skipped, THE image decoded once; a second
  // image descriptor (animation/multi-image) or any other shape is a
  // reject — the corpus is exactly one still + trailer.
  std::optional<mdk::IndexedImage> decoded;
  for (;;) {
    std::uint8_t sep;
    if (!r.u8(sep)) return std::nullopt;
    if (sep == 0x3B) return decoded;       // trailer
    if (sep == 0x21) {                     // extension
      std::uint8_t label;
      if (!r.u8(label)) return std::nullopt;
      if (!r.skipSubBlocks()) return std::nullopt;
      continue;
    }
    if (sep != 0x2C || decoded) return std::nullopt;  // unknown / 2nd image
    std::uint16_t ix, iy, iw, ih;
    std::uint8_t ipacked;
    if (!r.u16(ix) || !r.u16(iy) || !r.u16(iw) || !r.u16(ih) ||
        !r.u8(ipacked)) {
      return std::nullopt;
    }
    if (ipacked & 0x40) return std::nullopt;  // interlaced — not in corpus
    if (ipacked & 0x80) return std::nullopt;  // local CT — corpus is GCT-only
    // Corpus shape is a full-frame image at the origin; a sub-rect
    // descriptor would need the original's offset write semantics
    // (UNKNOWN) — reject rather than misdraw.
    if (ix != 0 || iy != 0 || iw != lw || ih != lh) {
      return std::nullopt;
    }
    if (!haveGct) return std::nullopt;

    mdk::IndexedImage img;
    img.width = iw;
    img.height = ih;
    img.stride = iw;
    img.pixels.assign(static_cast<std::size_t>(iw) * ih, 0);
    img.hasPalette = true;
    for (int i = 0; i < mdk::compat::kPaletteEntries; ++i) {
      img.palette[i].r = gct[i * 3 + 0];
      img.palette[i].g = gct[i * 3 + 1];
      img.palette[i].b = gct[i * 3 + 2];
    }
    if (!lzwDecode(r, img.pixels.data(), img.pixels.size())) {
      return std::nullopt;
    }
    decoded = std::move(img);
  }
}

}  // namespace mdkbridge
