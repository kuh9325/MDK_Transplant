#include "core/cmi_directory.h"

#include "core/binary_reader.h"
#include "core/container.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace mdk {

namespace {

// Printable run up to first NUL; non-printable bytes become '\xNN'.
std::string fieldName(std::span<const std::byte> field) {
  std::string out;
  char buf[5];
  for (std::byte b : field) {
    if (b == std::byte{0}) {
      break;
    }
    const auto v = static_cast<unsigned char>(b);
    if (v >= 0x20 && v <= 0x7e) {
      out.push_back(static_cast<char>(v));
    } else {
      std::snprintf(buf, sizeof(buf), "\\x%02x", v);
      out += buf;
    }
  }
  return out;
}

} // namespace

std::string CmiRecord::name() const { return fieldName(nameBytes); }

std::optional<std::uint64_t> CmiRecord::valueFileOffset() const {
  if (value == 0) {
    return std::nullopt;
  }
  return kCmiImageBaseOffset + static_cast<std::uint64_t>(value);
}

CmiDirectory inspectCmiDirectory(std::span<const std::byte> file) {
  CmiDirectory result;
  const std::uint64_t size = file.size();

  const ContainerInfo env = inspectContainer(file, size);
  if (!env.lengthValid || env.shape != ContainerShape::kTaggedName) {
    result.detail = "not a tagged-name envelope";
    return result;
  }

  BinaryReader r(file);
  const auto secondary = r.peekU32le(0x10);
  if (secondary) {
    result.secondaryLength = *secondary;
    result.secondaryEqualsTrailerOffset =
        static_cast<std::uint64_t>(*secondary) == size - 12;
  }
  result.trailerPresent =
      size >= kContainerNameFieldSize && env.hasNameField &&
      std::equal(env.nameField.begin(), env.nameField.end(),
                 file.end() - kContainerNameFieldSize);

  // Interior content lives in [0x14, trailerStart); when the trailer
  // is absent the region extends to EOF.
  const std::uint64_t trailerStart =
      result.trailerPresent ? size - kContainerNameFieldSize : size;

  if (size < kCmiInteriorOffset + 4) {
    result.status = CmiDirectoryStatus::kTruncatedHeader;
    result.detail = "file too small for the first table count at 0x14";
    return result;
  }

  const auto fail = [&](CmiDirectoryStatus st, std::size_t table,
                        std::size_t record, const char* fmt, ...) {
    result.status = st;
    result.badTableIndex = table;
    result.badRecordIndex = record;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    result.detail = buf;
  };

  // Four counted variable-length tables — the depth proven by the
  // original walker chain (FUN_0045840c -> 43c -> 46c) and observed in
  // all 6 BUILD_A files. Every record is bounded by trailerStart.
  std::uint64_t pos = kCmiInteriorOffset;
  result.tables.reserve(kCmiTableCount);
  for (std::size_t t = 0; t < kCmiTableCount; ++t) {
    CmiTable tab;
    tab.countFileOffset = pos;
    if (pos + 4 > trailerStart) {
      fail(CmiDirectoryStatus::kTableOutOfBounds, t,
           static_cast<std::size_t>(-1),
           "table %zu: count field at 0x%llx escapes [0x14, 0x%llx)", t,
           static_cast<unsigned long long>(pos),
           static_cast<unsigned long long>(trailerStart));
      return result;
    }
    tab.count = *r.peekU32le(static_cast<std::size_t>(pos));
    tab.recordsFileOffset = pos + 4;

    std::uint64_t rp = tab.recordsFileOffset;
    tab.records.reserve(tab.count <= 0x10000 ? tab.count : 0);
    for (std::uint32_t i = 0; i < tab.count; ++i) {
      // Each record needs len+5 bytes inside the interior region.
      if (rp + 1 > trailerStart) {
        fail(CmiDirectoryStatus::kTableOutOfBounds, t, i,
             "table %zu record %u: length byte at 0x%llx escapes "
             "[0x14, 0x%llx) (count=%u)", t, i,
             static_cast<unsigned long long>(rp),
             static_cast<unsigned long long>(trailerStart), tab.count);
        return result;
      }
      const std::uint8_t len = static_cast<std::uint8_t>(file[rp]);
      if (rp + kCmiRecordMinStride + len > trailerStart) {
        fail(CmiDirectoryStatus::kTableOutOfBounds, t, i,
             "table %zu record %u: len=%u record at 0x%llx needs bytes "
             "past 0x%llx", t, i, len,
             static_cast<unsigned long long>(rp),
             static_cast<unsigned long long>(trailerStart));
        return result;
      }
      CmiRecord rec;
      rec.fileOffset = rp;
      rec.nameLength = len;
      rec.nameBytes.assign(file.begin() + rp + 1,
                           file.begin() + rp + 1 + len);
      rec.nameEndsWithTerminator =
          len > 0 && rec.nameBytes.back() == std::byte{0};
      rec.value =
          *r.peekU32le(static_cast<std::size_t>(rp + 1 + len));

      // Native hardening: a nonzero value is an image-relative offset
      // the original dereferences unconditionally; it must stay inside
      // the file. (value==0 is the table-1 consumer's tested null.)
      const std::uint64_t target =
          kCmiImageBaseOffset + static_cast<std::uint64_t>(rec.value);
      if (rec.value != 0 && target >= trailerStart) {
        fail(CmiDirectoryStatus::kValueOutOfBounds, t, i,
             "table %zu record %u: value 0x%08x -> file 0x%llx outside "
             "[0, 0x%llx)", t, i, rec.value,
             static_cast<unsigned long long>(target),
             static_cast<unsigned long long>(trailerStart));
        return result;
      }
      tab.records.push_back(std::move(rec));
      rp += kCmiRecordMinStride + len;
    }
    tab.endFileOffset = rp;
    pos = rp;
    result.tables.push_back(std::move(tab));
  }

  // Data region: everything between the last table's end and the
  // trailer. Bounded only — its interior organization is UNKNOWN.
  result.dataRegionOffset = result.tables.back().endFileOffset;
  result.dataRegionEnd = trailerStart;

  // Second pass: flag the OBSERVED corpus property "nonzero value
  // lands in the data region" (818/818 in BUILD_A). Reported, never
  // load-bearing.
  for (auto& tab : result.tables) {
    for (auto& rec : tab.records) {
      const auto target = rec.valueFileOffset();
      rec.valueReachesDataRegion =
          target && *target >= result.dataRegionOffset &&
          *target < result.dataRegionEnd;
    }
  }

  result.status = CmiDirectoryStatus::kOk;
  return result;
}

std::string_view cmiDirectoryStatusName(CmiDirectoryStatus s) {
  switch (s) {
    case CmiDirectoryStatus::kOk:                 return "ok";
    case CmiDirectoryStatus::kNotTaggedEnvelope:  return "not-tagged-envelope";
    case CmiDirectoryStatus::kTruncatedHeader:    return "truncated-header";
    case CmiDirectoryStatus::kTableOutOfBounds:   return "table-out-of-bounds";
    case CmiDirectoryStatus::kValueOutOfBounds:   return "value-out-of-bounds";
  }
  return "?";
}

} // namespace mdk
