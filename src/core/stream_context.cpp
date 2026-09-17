#include "core/stream_context.h"

#include "core/bni_directory.h"
#include "core/fti_directory.h"

#include <array>
#include <cstdio>

namespace mdk {

namespace {

// ASCII-fold + separator-normalized compare for a DOS-style relative
// path against `expected` (which is written with '/').
bool pathEquals(std::string_view path, std::string_view expected) {
  if (path.size() != expected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < path.size(); ++i) {
    char a = path[i] == '\\' ? '/' : path[i];
    if (a >= 'a' && a <= 'z') {
      a = static_cast<char>(a - 0x20);
    }
    if (a != expected[i]) {
      return false;
    }
  }
  return true;
}

// ASCII case-insensitive compare (record names are ASCII).
bool nameEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 0x20);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 0x20);
    if (x != y) {
      return false;
    }
  }
  return true;
}

} // namespace

bool isStreamBackdropRequest(std::string_view relFile,
                             std::string_view record) {
  return pathEquals(relFile, kStreamBniFile) &&
         nameEquals(record, kStreamImageRecord);
}

std::optional<std::array<std::byte, kBniImagePaletteBytes>>
composeStreamPalette(std::span<const std::byte> sysPalRecord,
                     std::span<const std::byte> palRecord,
                     std::string* error) {
  const auto fail = [&](const char* msg)
      -> std::optional<std::array<std::byte, kBniImagePaletteBytes>> {
    if (error) {
      *error = msg;
    }
    return std::nullopt;
  };

  if (sysPalRecord.size() != kStreamSystemHeadBytes) {
    return fail("SYS_PAL record must be exactly 192 bytes "
                "(64 RGB entries)");
  }
  if (palRecord.size() != kBniImagePaletteBytes) {
    return fail("PAL record must be exactly 768 bytes "
                "(256 RGB entries)");
  }

  std::array<std::byte, kBniImagePaletteBytes> pal{};
  for (std::size_t i = 0; i < kStreamSystemHeadBytes; ++i) {
    pal[i] = sysPalRecord[i];
  }
  for (std::size_t i = 0; i < kStreamPaletteTailBytes; ++i) {
    pal[kStreamPaletteTailOffset + i] =
        palRecord[kStreamPaletteTailOffset + i];
  }
  return pal;
}

std::optional<IndexedImage> decodeStreamBackdrop(
    std::span<const std::byte> streamBniFile,
    std::span<const std::byte> systemFtiFile, std::string* error) {
  const auto fail = [&](const std::string& msg)
      -> std::optional<IndexedImage> {
    if (error) {
      *error = msg;
    }
    return std::nullopt;
  };

  const auto bni = inspectBniDirectory(streamBniFile);
  if (bni.status != BniDirectoryStatus::kOk) {
    return fail("BNI directory: " +
                std::string(bniDirectoryStatusName(bni.status)) +
                " — " + bni.detail);
  }
  const BniRecord* image = findBniRecord(bni, kStreamImageRecord);
  if (!image) {
    return fail("record not found: " + std::string(kStreamImageRecord));
  }
  const BniRecord* pal = findBniRecord(bni, kStreamPaletteRecord);
  if (!pal) {
    return fail("record not found: " +
                std::string(kStreamPaletteRecord));
  }

  const auto fti = inspectFtiDirectory(systemFtiFile);
  if (fti.status != FtiDirectoryStatus::kOk) {
    return fail("FTI directory: " +
                std::string(ftiDirectoryStatusName(fti.status)) +
                " — " + fti.detail);
  }
  const FtiRecord* sysPal = findFtiRecord(fti, kStreamSystemRecord);
  if (!sysPal) {
    return fail("record not found: " +
                std::string(kStreamSystemRecord));
  }

  const std::span<const std::byte> sysPalPayload(
      systemFtiFile.data() + sysPal->payloadFileOffset,
      sysPal->payloadSize());
  const std::span<const std::byte> palPayload(
      streamBniFile.data() + pal->payloadFileOffset,
      pal->payloadSize());

  std::string perr;
  const auto palette = composeStreamPalette(sysPalPayload, palPayload,
                                            &perr);
  if (!palette) {
    return fail(perr);
  }

  const std::span<const std::byte> imagePayload(
      streamBniFile.data() + image->payloadFileOffset,
      image->payloadSize());
  auto img = decodeBniIndexedImage(imagePayload, *palette, &perr);
  if (!img) {
    return fail(perr);
  }
  return img;
}

} // namespace mdk
