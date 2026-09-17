#include "core/sni_directory.h"

#include "core/binary_reader.h"
#include "core/container.h"

#include <algorithm>
#include <cstdio>

namespace mdk {

std::string SniEntry::name() const {
  std::string out;
  char buf[5];
  for (std::byte b : nameField) {
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

SniDirectory inspectSniDirectory(std::span<const std::byte> file) {
  SniDirectory result;
  const std::uint64_t size = file.size();

  // The directory format is defined inside the tagged-name envelope;
  // anything else is simply not this format (not "malformed SNI").
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
      size >= kSniNameFieldSize && env.hasNameField &&
      std::equal(env.nameField.begin(), env.nameField.end(),
                 file.end() - kSniNameFieldSize);

  if (size < kSniCountOffset + 4) {
    result.status = SniDirectoryStatus::kTruncatedHeader;
    result.detail = "file too small for the count field at 0x14";
    return result;
  }
  const auto count = r.peekU32le(kSniCountOffset);
  if (!count) {
    result.status = SniDirectoryStatus::kTruncatedHeader;
    result.detail = "cannot read the count field at 0x14";
    return result;
  }
  result.count = *count;

  // Directory must fit inside the file. Division-first form cannot
  // overflow; the bound derives entirely from file size and stride.
  const std::uint64_t directoryEnd = kSniRecordBase +
      static_cast<std::uint64_t>(*count) * kSniRecordStride;
  if (*count > (size - kSniRecordBase) / kSniRecordStride) {
    result.status = SniDirectoryStatus::kDirectoryOutOfBounds;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "count=%u needs %llu bytes at 0x18; file has %llu",
                  *count,
                  static_cast<unsigned long long>(
                      static_cast<std::uint64_t>(*count) *
                      kSniRecordStride),
                  static_cast<unsigned long long>(
                      size - kSniRecordBase));
    result.detail = buf;
    return result;
  }
  result.directoryEnd = directoryEnd;

  // Payload region upper bound: the OBSERVED 12-byte name trailer
  // begins at size-12 when present; otherwise payloads may extend to
  // EOF. Payloads must also start at or after the directory end
  // (OBSERVED in all BUILD_A .SNI files).
  const std::uint64_t payloadRegionEnd =
      result.trailerPresent ? size - kSniNameFieldSize : size;

  result.entries.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const std::uint64_t rec = kSniRecordBase +
        static_cast<std::uint64_t>(i) * kSniRecordStride;
    SniEntry e;
    for (std::size_t j = 0; j < e.nameField.size(); ++j) {
      e.nameField[j] = file[rec + j];
    }
    e.fieldAt0x0C = *r.peekU32le(rec + 0x0c);
    e.blobOffset = *r.peekU32le(rec + 0x10);
    e.payloadSize = *r.peekU32le(rec + 0x14);

    const std::uint64_t start = e.payloadFileOffset();
    // Sentinel records carry a position only: the stored offset must
    // still land inside the payload region, but the size field is a
    // marker value, never a byte count.
    if (e.isSentinel()
            ? (start < directoryEnd || start > payloadRegionEnd)
            : (start < directoryEnd ||
               e.payloadFileEnd() > payloadRegionEnd)) {
      result.status = SniDirectoryStatus::kEntryOutOfBounds;
      result.badEntryIndex = i;
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "entry %u: payload [%llu, %llu) outside [%llu, %llu)",
                    i,
                    static_cast<unsigned long long>(start),
                    static_cast<unsigned long long>(
                        e.isSentinel() ? start : e.payloadFileEnd()),
                    static_cast<unsigned long long>(directoryEnd),
                    static_cast<unsigned long long>(payloadRegionEnd));
      result.detail = buf;
      return result;
    }
    result.entries.push_back(e);
  }

  result.status = SniDirectoryStatus::kOk;
  return result;
}

std::string_view sniDirectoryStatusName(SniDirectoryStatus s) {
  switch (s) {
    case SniDirectoryStatus::kOk:                  return "ok";
    case SniDirectoryStatus::kNotTaggedEnvelope:   return "not-tagged-envelope";
    case SniDirectoryStatus::kTruncatedHeader:     return "truncated-header";
    case SniDirectoryStatus::kDirectoryOutOfBounds:return "directory-out-of-bounds";
    case SniDirectoryStatus::kEntryOutOfBounds:    return "entry-out-of-bounds";
  }
  return "?";
}

} // namespace mdk
