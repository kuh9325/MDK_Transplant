#include "core/fti_font.h"

#include "core/binary_reader.h"
#include "core/framebuffer.h"
#include "core/indexed_image.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace mdk {

std::optional<FtiFont> decodeFtiFont(
    std::span<const std::byte> payload, std::string* err) {
  auto fail = [&](const char* fmt, auto... args) -> std::nullopt_t {
    if (err) {
      if constexpr (sizeof...(args) == 0) {
        *err = fmt;
      } else {
        char buf[160];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        *err = buf;
      }
    }
    return std::nullopt;
  };

  // The char byte indexes a 256-entry u32 table — the payload must
  // cover it entirely (FUN_00414dd4 reads payload[ch*4] for any byte).
  if (payload.size() < kFtiFontTableBytes) {
    return fail("payload too small for 256-entry offset table: %llu "
                "bytes",
                static_cast<unsigned long long>(payload.size()));
  }

  FtiFont font;
  font.payloadBytes = payload.size();
  font.glyphs.resize(kFtiFontGlyphSlots);

  std::vector<std::uint32_t> offsets(kFtiFontGlyphSlots);
  BinaryReader table(
      payload.subspan(0, kFtiFontTableBytes));
  for (std::size_t i = 0; i < kFtiFontGlyphSlots; ++i) {
    offsets[i] = *table.u32le();  // in-table by construction
  }

  // Reported-only corpus properties.
  font.offsetsSortedAscending = true;
  font.offsetsUnique = true;
  {
    std::unordered_set<std::uint32_t> seen;
    std::uint32_t prev = 0;
    bool first = true;
    for (const std::uint32_t o : offsets) {
      if (o == 0) {
        continue;  // unmapped entries are not part of the ordering
      }
      if (!seen.insert(o).second) {
        font.offsetsUnique = false;
      }
      if (!first && o < prev) {
        font.offsetsSortedAscending = false;
      }
      prev = o;
      first = false;
    }
  }

  std::uint64_t lastGlyphEnd = kFtiFontTableBytes;
  for (std::size_t i = 0; i < kFtiFontGlyphSlots; ++i) {
    const std::uint32_t off = offsets[i];
    if (off == 0) {
      continue;  // unmapped byte — the draw path skips the lookup
    }
    // Native hardening: the original dereferences payload+off without
    // checks; a real font never points back into the table region.
    if (off < kFtiFontTableBytes) {
      return fail("glyph offset 0x%08x for code 0x%02zx points inside "
                  "the offset table",
                  off, i);
    }
    if (std::uint64_t(off) + kFtiGlyphHeaderBytes > payload.size()) {
      return fail("glyph offset 0x%08x for code 0x%02zx escapes the "
                  "payload (%llu bytes)",
                  off, i,
                  static_cast<unsigned long long>(payload.size()));
    }

    FtiGlyph g;
    g.code = static_cast<std::uint8_t>(i);
    g.offset = off;
    g.top = static_cast<std::int8_t>(payload[off]);
    g.bottom = static_cast<std::int8_t>(payload[off + 1]);
    g.width = static_cast<std::uint8_t>(payload[off + 2]);

    const int rows = g.rows();
    const std::uint64_t extent = g.consumedBytes();
    if (std::uint64_t(off) + extent > payload.size()) {
      return fail("glyph for code 0x%02zx at 0x%08x needs %llu bytes "
                  "(w=%u rows=%d) but the payload ends at 0x%llx",
                  i, off, static_cast<unsigned long long>(extent),
                  unsigned(g.width), rows,
                  static_cast<unsigned long long>(payload.size()));
    }
    if (rows > 0 && g.width > 0) {
      const std::size_t n = static_cast<std::size_t>(g.width) *
                            static_cast<std::size_t>(rows);
      const std::byte* src = payload.data() + off + kFtiGlyphHeaderBytes;
      g.pixels.resize(n);
      std::memcpy(g.pixels.data(), src, n);
      for (const std::uint8_t v : g.pixels) {
        if (v > font.maxPixelIndex) {
          font.maxPixelIndex = v;
        }
      }
    }
    font.glyphs[i] = std::move(g);
    ++font.mappedCount;
    if (font.firstMapped < 0) {
      font.firstMapped = static_cast<int>(i);
    }
    font.lastMapped = static_cast<int>(i);
    if (std::uint64_t(off) + extent > lastGlyphEnd) {
      lastGlyphEnd = std::uint64_t(off) + extent;
    }
  }

  if (font.mappedCount == 0) {
    return fail("no mapped glyphs — not a FONTSML/FONTBIG font record");
  }
  font.trailingBytes = payload.size() - lastGlyphEnd;
  return font;
}

std::uint64_t ftiFontDigest(const FtiFont& font) {
  std::uint64_t h = fnv1a64({});
  for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
    const std::byte code = static_cast<std::byte>(i);
    h = fnv1a64(std::span<const std::byte>(&code, 1), h);
    const auto& g = font.glyphs[i];
    const std::byte mapped = std::byte(g ? 1 : 0);
    h = fnv1a64(std::span<const std::byte>(&mapped, 1), h);
    if (g) {
      const std::byte hdr[3] = {std::byte(g->top), std::byte(g->bottom),
                                std::byte(g->width)};
      h = fnv1a64(std::span<const std::byte>(hdr, 3), h);
      h = fnv1a64(std::as_bytes(std::span(g->pixels)), h);
    }
  }
  return h;
}

void drawFtiGlyph(const FtiGlyph& glyph, IndexedFramebuffer& fb,
                  int penX, int penY) {
  const int rows = glyph.rows();
  const std::size_t need =
      rows > 0 ? std::size_t(glyph.width) * std::size_t(rows) : 0;
  if (need == 0 || glyph.pixels.size() < need) {
    return;  // matches the original's early-out on non-positive bounds
  }
  const int topRow = penY - int(glyph.top);
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < int(glyph.width); ++c) {
      const std::uint8_t b =
          glyph.pixels[std::size_t(r) * glyph.width + std::size_t(c)];
      if (b != 0) {
        fb.put(penX + c, topRow + r, b);  // 0 = transparent (skip)
      }
    }
  }
}

int drawFtiText(const FtiFont& font, std::string_view text,
                IndexedFramebuffer& fb, int penX, int penY,
                int missingAdvance) {
  int x = penX;
  for (const char ch : text) {
    const auto code = static_cast<std::uint8_t>(ch);
    if (const FtiGlyph* g = font.glyphFor(code)) {
      drawFtiGlyph(*g, fb, x, penY);
      x += g->width;
    } else {
      x += missingAdvance;
    }
  }
  return x;
}

int measureFtiText(const FtiFont& font, std::string_view text,
                   int missingAdvance) {
  int w = 0;
  for (const char ch : text) {
    const auto code = static_cast<std::uint8_t>(ch);
    if (const FtiGlyph* g = font.glyphFor(code)) {
      w += g->width;
    } else {
      w += missingAdvance;
    }
  }
  return w;
}

} // namespace mdk
