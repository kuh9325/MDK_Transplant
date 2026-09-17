#include "core/fti_sprite.h"

#include "core/binary_reader.h"
#include "core/framebuffer.h"
#include "core/indexed_image.h"

#include <algorithm>

namespace mdk {

namespace {

// Parse one command stream starting at `pos` inside `payload`.
// Consumes through the terminating 0xff (inclusive) or fails on
// input exhaustion. Gathers the frame statistics.
bool parseStream(std::span<const std::byte> payload, std::size_t pos,
                 FtiSpriteFrame& frame, std::size_t* end) {
  while (true) {
    if (pos >= payload.size()) {
      return false;  // input exhausted before 0xff
    }
    const std::uint8_t cmd =
        static_cast<std::uint8_t>(payload[pos]);
    ++pos;
    if (cmd == kFtiSpriteStreamEnd) {
      break;
    }
    if (cmd == kFtiSpriteRowBreak) {
      ++frame.rowBreaks;
      continue;
    }
    if (cmd < 0x80) {
      // literal packet: cmd+1 pixel bytes
      const std::size_t count = static_cast<std::size_t>(cmd) + 1;
      if (count > payload.size() - pos) {
        return false;  // truncated literal
      }
      for (std::size_t k = 0; k < count; ++k) {
        const std::uint8_t v =
            static_cast<std::uint8_t>(payload[pos + k]);
        ++frame.pixelAdvances;
        if (v != 0) {
          ++frame.opaqueWrites;
          if (v > frame.maxPixelIndex) {
            frame.maxPixelIndex = v;
          }
          if (frame.minPixelIndex == 0 || v < frame.minPixelIndex) {
            frame.minPixelIndex = v;
          }
        }
      }
      pos += count;
      ++frame.literalPackets;
      continue;
    }
    // run packet: count = cmd-0x7c, one value byte
    const std::size_t count =
        static_cast<std::size_t>(cmd) - kFtiSpriteRunBase;
    if (pos >= payload.size()) {
      return false;  // truncated run
    }
    const std::uint8_t v = static_cast<std::uint8_t>(payload[pos]);
    ++pos;
    frame.pixelAdvances += count;
    ++frame.runPackets;
    if (v == 0) {
      ++frame.transparentRuns;
    } else {
      frame.opaqueWrites += count;
      if (v > frame.maxPixelIndex) {
        frame.maxPixelIndex = v;
      }
      if (frame.minPixelIndex == 0 || v < frame.minPixelIndex) {
        frame.minPixelIndex = v;
      }
    }
  }
  *end = pos;
  return true;
}

} // namespace

std::optional<FtiSprite> decodeFtiSprite(
    std::span<const std::byte> payload, std::string* err) {
  auto fail = [&](const char* msg) -> std::optional<FtiSprite> {
    if (err) {
      *err = msg;
    }
    return std::nullopt;
  };

  BinaryReader r(payload);
  const auto blockBytes = r.u32le();
  if (!blockBytes) {
    return fail("sprite record: truncated header (blockBytes)");
  }
  const auto count = r.u32le();
  if (!count) {
    return fail("sprite record: truncated header (frameCount)");
  }
  if (*count == 0) {
    return fail("sprite record: zero frames");
  }
  // offsets live at +0x08, each relative to +0x04.
  if (*count > (payload.size() - kFtiSpriteOffsetsOffset) / 4) {
    return fail("sprite record: offset table out of bounds");
  }

  FtiSprite out;
  out.blockBytes = *blockBytes;
  out.payloadBytes = payload.size();
  out.frames.reserve(*count);
  std::uint64_t lastEnd = kFtiSpriteOffsetsOffset + 4 * (*count);

  for (std::uint32_t i = 0; i < *count; ++i) {
    const auto off =
        r.peekU32le(kFtiSpriteOffsetsOffset + 4 * i);
    if (!off) {
      return fail("sprite record: truncated offset table");
    }
    // frame = payload + 4 + offset
    const std::uint64_t framePos = 4 + static_cast<std::uint64_t>(*off);
    if (framePos > payload.size() ||
        payload.size() - framePos < kFtiSpriteFrameHeaderBytes) {
      return fail("sprite record: frame header out of bounds");
    }
    const auto w = r.peekU16le(framePos + 0);
    const auto h = r.peekU16le(framePos + 2);
    const auto hx = r.peekU16le(framePos + 4);
    const auto hy = r.peekU16le(framePos + 6);
    if (!w || !h || !hx || !hy) {
      return fail("sprite record: truncated frame header");
    }
    FtiSpriteFrame f;
    f.frameFileOffset = framePos;
    f.width = *w;
    f.height = *h;
    f.hotspotX = static_cast<std::int16_t>(*hx);
    f.hotspotY = static_cast<std::int16_t>(*hy);

    const std::size_t streamStart = framePos + kFtiSpriteFrameHeaderBytes;
    std::size_t streamEnd = streamStart;
    if (!parseStream(payload, streamStart, f, &streamEnd)) {
      return fail("sprite record: stream has no 0xff terminator "
                  "in bounds or a truncated packet");
    }
    f.stream.resize(streamEnd - streamStart);
    std::transform(payload.begin() + streamStart,
                   payload.begin() + streamEnd, f.stream.begin(),
                   [](std::byte b) { return static_cast<std::uint8_t>(b); });
    lastEnd = std::max<std::uint64_t>(lastEnd, streamEnd);
    out.frames.push_back(std::move(f));
  }
  out.trailingBytes = payload.size() - std::min<std::uint64_t>(
      lastEnd, payload.size());
  return out;
}

std::uint64_t ftiSpriteDigest(const FtiSprite& sprite) {
  std::uint64_t h = fnv1a64(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(&sprite.blockBytes), 4));
  for (const auto& f : sprite.frames) {
    const std::byte hdr[8] = {
        static_cast<std::byte>(f.width & 0xff),
        static_cast<std::byte>(f.width >> 8),
        static_cast<std::byte>(f.height & 0xff),
        static_cast<std::byte>(f.height >> 8),
        static_cast<std::byte>(f.hotspotX & 0xff),
        static_cast<std::byte>((f.hotspotX >> 8) & 0xff),
        static_cast<std::byte>(f.hotspotY & 0xff),
        static_cast<std::byte>((f.hotspotY >> 8) & 0xff),
    };
    h = fnv1a64(std::span<const std::byte>(hdr, sizeof(hdr)), h);
    h = fnv1a64(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(f.stream.data()),
            f.stream.size()),
        h);
  }
  return h;
}

void blitFtiSpriteFrame(const FtiSpriteFrame& f, IndexedFramebuffer& fb,
                        int x, int y) {
  // FUN_00409760: hotspot offset applied to the caller position.
  x -= f.hotspotX;
  y -= f.hotspotY;

  const int w = f.width;
  const int h = f.height;
  const int fbW = fb.width();
  const int fbH = fb.height();
  const std::size_t fbSize =
      static_cast<std::size_t>(fbW) * static_cast<std::size_t>(fbH);

  // FUN_00415ff0 entry checks (original hardcodes 600x360 — the work
  // buffer; we use the actual framebuffer which is the same size).
  if (x >= fbW || y >= fbH) {
    return;
  }
  if (x + w <= 0 || y + h <= 0) {
    return;
  }
  if (x >= 0 && x + w > fbW) {
    return;  // right edge: all-or-nothing, no per-pixel clip
  }

  // Logical pen: (col, row). dest flat offset = row*fbW + col — the
  // original's EDI walks the same flat address, so a packet spilling
  // past the row width lands in the next row exactly like the
  // original; only framebuffer bounds are enforced (hardening).
  int row = y;
  int col = x;
  const int rowStartCol = x;
  const std::uint8_t* s = f.stream.data();
  const std::size_t n = f.stream.size();
  std::size_t i = 0;

  auto put = [&](std::uint8_t v) {
    if (v != 0 && col >= 0) {
      const long long off =
          static_cast<long long>(row) * fbW + col;
      if (off >= 0 &&
          off < static_cast<long long>(fbSize)) {
        fb.pixels()[static_cast<std::size_t>(off)] = v;
      }
    }
    ++col;
  };

  while (i < n) {
    const std::uint8_t c = s[i++];
    if (c == kFtiSpriteStreamEnd) {
      return;
    }
    if (c == kFtiSpriteRowBreak) {
      ++row;
      col = rowStartCol;
      if (row >= fbH) {
        return;  // original: row counter >= 360 -> return
      }
      continue;
    }
    if (c < 0x80) {
      const int count = c + 1;
      for (int k = 0; k < count; ++k) {
        if (i >= n) {
          return;  // hardened: truncated literal stops the blit
        }
        put(s[i++]);
      }
      continue;
    }
    const int count = c - kFtiSpriteRunBase;
    if (i >= n) {
      return;  // hardened: truncated run stops the blit
    }
    const std::uint8_t v = s[i++];
    for (int k = 0; k < count; ++k) {
      put(v);
    }
  }
  // stream exhausted without 0xff — decode-time validation makes this
  // unreachable for decoded frames; for hand-built spans we simply stop.
}

} // namespace mdk
