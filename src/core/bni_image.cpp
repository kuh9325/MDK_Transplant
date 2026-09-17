#include "core/bni_image.h"

#include "core/binary_reader.h"

#include <cstdio>

namespace mdk {

namespace {

// Read the {u16le w, u16le h} pair at `off` and check the payload
// tiles exactly (size == off + 4 + w*h). The product is computed in
// 64-bit — u16*u16 cannot overflow size_t.
bool tilesExactly(std::span<const std::byte> payload, std::size_t off,
                  int& w, int& h) {
  if (payload.size() < off + kBniImageDimHeaderBytes) {
    return false;
  }
  BinaryReader r(payload);
  const auto vw = r.peekU16le(off);
  const auto vh = r.peekU16le(off + 2);
  if (!vw || !vh || *vw == 0 || *vh == 0) {
    return false;
  }
  const std::uint64_t need =
      static_cast<std::uint64_t>(off) + kBniImageDimHeaderBytes +
      static_cast<std::uint64_t>(*vw) * *vh;
  if (need != payload.size()) {
    return false;
  }
  w = *vw;
  h = *vh;
  return true;
}

} // namespace

BniImageProbe probeBniImage(std::span<const std::byte> payload) {
  BniImageProbe p;
  int w = 0, h = 0;
  // Paletted shape first: a 768-byte palette head can itself contain
  // u16s that mimic the indexed-only header.
  if (tilesExactly(payload, kBniImagePaletteBytes, w, h)) {
    p.shape = BniImageShape::kPaletted;
    p.width = w;
    p.height = h;
    p.headerBytes = kBniPalettedHeaderBytes;
    p.pixelBytes = static_cast<std::size_t>(w) * h;
    return p;
  }
  if (tilesExactly(payload, 0, w, h)) {
    p.shape = BniImageShape::kIndexedOnly;
    p.width = w;
    p.height = h;
    p.headerBytes = kBniImageDimHeaderBytes;
    p.pixelBytes = static_cast<std::size_t>(w) * h;
    return p;
  }
  return p;
}

std::string_view bniImageShapeName(BniImageShape s) {
  switch (s) {
    case BniImageShape::kPaletted:    return "paletted-indexed";
    case BniImageShape::kIndexedOnly: return "indexed-only";
    case BniImageShape::kOther:       return "not-an-image";
  }
  return "?";
}

std::optional<IndexedImage> decodeBniPalettedImage(
    std::span<const std::byte> payload, std::string* error) {
  const auto fail = [&](const char* msg) -> std::optional<IndexedImage> {
    if (error) {
      *error = msg;
    }
    return std::nullopt;
  };

  const BniImageProbe p = probeBniImage(payload);
  if (p.shape != BniImageShape::kPaletted) {
    char buf[160];
    std::snprintf(
        buf, sizeof(buf),
        "payload does not match the paletted layout "
        "{rgb[768],u16 w,u16 h,px[w*h]} exactly (probe: %s, %llu bytes)",
        std::string(bniImageShapeName(p.shape)).c_str(),
        static_cast<unsigned long long>(payload.size()));
    return fail(buf);
  }

  IndexedImage img;
  img.width = p.width;
  img.height = p.height;
  img.stride = p.width;
  img.hasPalette = true;
  for (int i = 0; i < compat::kPaletteEntries; ++i) {
    const std::size_t off = static_cast<std::size_t>(i) * 3;
    img.palette[i] = {static_cast<std::uint8_t>(payload[off + 0]),
                      static_cast<std::uint8_t>(payload[off + 1]),
                      static_cast<std::uint8_t>(payload[off + 2])};
  }
  img.pixels.resize(p.pixelBytes);
  for (std::size_t i = 0; i < p.pixelBytes; ++i) {
    img.pixels[i] =
        static_cast<std::uint8_t>(payload[kBniPalettedHeaderBytes + i]);
  }
  return img;
}

std::optional<IndexedImage> decodeBniIndexedImage(
    std::span<const std::byte> imagePayload,
    std::span<const std::byte> palettePayload, std::string* error) {
  const auto fail = [&](const char* msg) -> std::optional<IndexedImage> {
    if (error) {
      *error = msg;
    }
    return std::nullopt;
  };

  const BniImageProbe p = probeBniImage(imagePayload);
  if (p.shape != BniImageShape::kIndexedOnly) {
    char buf[160];
    std::snprintf(
        buf, sizeof(buf),
        "payload does not match the indexed-only layout "
        "{u16 w,u16 h,px[w*h]} exactly (probe: %s, %llu bytes)",
        std::string(bniImageShapeName(p.shape)).c_str(),
        static_cast<unsigned long long>(imagePayload.size()));
    return fail(buf);
  }
  if (palettePayload.size() != kBniImagePaletteBytes) {
    return fail("external palette must be exactly 768 bytes "
                "(256 RGB entries)");
  }

  IndexedImage img;
  img.width = p.width;
  img.height = p.height;
  img.stride = p.width;
  img.hasPalette = true;
  for (int i = 0; i < compat::kPaletteEntries; ++i) {
    const std::size_t off = static_cast<std::size_t>(i) * 3;
    img.palette[i] = {static_cast<std::uint8_t>(palettePayload[off + 0]),
                      static_cast<std::uint8_t>(palettePayload[off + 1]),
                      static_cast<std::uint8_t>(palettePayload[off + 2])};
  }
  img.pixels.resize(p.pixelBytes);
  for (std::size_t i = 0; i < p.pixelBytes; ++i) {
    img.pixels[i] =
        static_cast<std::uint8_t>(imagePayload[kBniImageDimHeaderBytes + i]);
  }
  return img;
}

} // namespace mdk
