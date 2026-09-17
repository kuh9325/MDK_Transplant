#include "core/mti_directory.h"

#include "core/binary_reader.h"
#include "core/container.h"

#include <algorithm>
#include <cstdio>

namespace mdk {

std::string MtiEntry::name() const {
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

MtiDirectory inspectMtiDirectory(std::span<const std::byte> file) {
  MtiDirectory result;
  const std::uint64_t size = file.size();

  // The directory format is defined inside the tagged-name envelope;
  // anything else is simply not this format (not "malformed MTI").
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

  if (size < kMtiCountOffset + 4) {
    result.status = MtiDirectoryStatus::kTruncatedHeader;
    result.detail = "file too small for the count field at 0x14";
    return result;
  }
  const auto count = r.peekU32le(kMtiCountOffset);
  if (!count) {
    result.status = MtiDirectoryStatus::kTruncatedHeader;
    result.detail = "cannot read the count field at 0x14";
    return result;
  }
  result.count = *count;

  // Directory must fit inside the file. Division-first form cannot
  // overflow; the bound derives entirely from file size and stride.
  const std::uint64_t directoryEnd = kMtiRecordBase +
      static_cast<std::uint64_t>(*count) * kMtiRecordStride;
  if (*count > (size - kMtiRecordBase) / kMtiRecordStride) {
    result.status = MtiDirectoryStatus::kDirectoryOutOfBounds;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "count=%u needs %llu bytes at 0x18; file has %llu",
                  *count,
                  static_cast<unsigned long long>(
                      static_cast<std::uint64_t>(*count) *
                      kMtiRecordStride),
                  static_cast<unsigned long long>(
                      size - kMtiRecordBase));
    result.detail = buf;
    return result;
  }
  result.directoryEnd = directoryEnd;

  // Payload region upper bound: the OBSERVED 12-byte name trailer
  // begins at size-12 when present; otherwise payloads may extend to
  // EOF. Payloads must also start at or after the directory end
  // (OBSERVED in all 13 BUILD_A .MTI files).
  const std::uint64_t payloadRegionEnd =
      result.trailerPresent ? size - kContainerNameFieldSize : size;

  result.entries.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const std::uint64_t rec = kMtiRecordBase +
        static_cast<std::uint64_t>(i) * kMtiRecordStride;
    MtiEntry e;
    for (std::size_t j = 0; j < e.nameField.size(); ++j) {
      e.nameField[j] = file[rec + j];
    }
    e.fieldAt0x08 = *r.peekU32le(rec + 0x08);
    e.fieldAt0x0C = *r.peekU32le(rec + 0x0c);
    e.fieldAt0x10 = *r.peekU32le(rec + 0x10);
    e.fieldAt0x14 = *r.peekU32le(rec + 0x14);

    // Index records: the original reads only +0x0c; the remaining
    // fields are ignored and left unvalidated (preserved raw).
    if (e.isIndexRecord()) {
      result.entries.push_back(e);
      continue;
    }

    // Payload records: +0x14 is a blob-relative offset the original
    // dereferences; it must land at/after the directory end with room
    // for the payload header inside the payload region.
    const std::uint64_t start = e.payloadFileOffset();
    const std::uint64_t headerEnd = start + e.payloadHeaderBytes();
    if (start < directoryEnd || headerEnd > payloadRegionEnd) {
      result.status = MtiDirectoryStatus::kEntryOutOfBounds;
      result.badEntryIndex = i;
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "entry %u: payload header [%llu, %llu) outside "
                    "[%llu, %llu)",
                    i,
                    static_cast<unsigned long long>(start),
                    static_cast<unsigned long long>(headerEnd),
                    static_cast<unsigned long long>(directoryEnd),
                    static_cast<unsigned long long>(payloadRegionEnd));
      result.detail = buf;
      return result;
    }

    // Read the payload header u16s the original itself reads
    // (positions differ per the extended-header variant).
    if (e.hasExtendedHeader()) {
      e.headerCount = *r.peekU16le(start);
      e.headerFieldA = *r.peekU16le(start + 4);
      e.headerFieldB = *r.peekU16le(start + 6);
    } else {
      e.headerFieldA = *r.peekU16le(start);
      e.headerFieldB = *r.peekU16le(start + 2);
    }
    e.payloadDataFileOffset = headerEnd;
    result.entries.push_back(e);
  }

  result.status = MtiDirectoryStatus::kOk;
  return result;
}

std::string_view mtiDirectoryStatusName(MtiDirectoryStatus s) {
  switch (s) {
    case MtiDirectoryStatus::kOk:                  return "ok";
    case MtiDirectoryStatus::kNotTaggedEnvelope:   return "not-tagged-envelope";
    case MtiDirectoryStatus::kTruncatedHeader:     return "truncated-header";
    case MtiDirectoryStatus::kDirectoryOutOfBounds:return "directory-out-of-bounds";
    case MtiDirectoryStatus::kEntryOutOfBounds:    return "entry-out-of-bounds";
  }
  return "?";
}

} // namespace mdk
