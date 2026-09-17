#include "core/bni_directory.h"

#include "core/binary_reader.h"
#include "core/container.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace mdk {

namespace {

// Printable run up to first NUL; non-printable bytes become '\xNN'.
// Display-only — nameField is the lossless form.
std::string fieldName(const std::array<std::byte, kBniNameFieldSize>& field) {
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

std::string BniRecord::name() const { return fieldName(nameField); }

BniDirectory inspectBniDirectory(std::span<const std::byte> file) {
  BniDirectory result;
  const std::uint64_t size = file.size();

  const ContainerInfo env = inspectContainer(file, size);
  if (!env.lengthValid || env.shape != ContainerShape::kLengthEnvelope) {
    result.detail = "not a length-only envelope";
    return result;
  }

  BinaryReader r(file);
  const auto count = r.peekU32le(kBniCountOffset);
  if (!count) {
    result.status = BniDirectoryStatus::kTruncatedHeader;
    result.detail = "cannot read the count field at 0x04";
    return result;
  }
  result.count = *count;

  // Directory must fit inside the file (division-first, no overflow).
  if (*count > (size - kBniRecordBase) / kBniRecordStride) {
    result.status = BniDirectoryStatus::kDirectoryOutOfBounds;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "count=%u needs %llu bytes at 0x08; file has %llu",
                  *count,
                  static_cast<unsigned long long>(
                      static_cast<std::uint64_t>(*count) *
                      kBniRecordStride),
                  static_cast<unsigned long long>(
                      size - kBniRecordBase));
    result.detail = buf;
    return result;
  }
  const std::uint64_t directoryEnd =
      kBniRecordBase + static_cast<std::uint64_t>(*count) * kBniRecordStride;
  result.directoryEnd = directoryEnd;

  const auto fail = [&](BniDirectoryStatus st, std::size_t idx,
                        const char* fmt, ...) {
    result.status = st;
    result.badRecordIndex = idx;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    result.detail = buf;
  };

  // First pass: raw records. A stored offset resolves to file offset
  // 4 + value and must land inside [directoryEnd, size] (hardening —
  // the original dereferences it unconditionally; offset == size is a
  // legal empty-tail form).
  result.records.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const std::uint64_t rec =
        kBniRecordBase + static_cast<std::uint64_t>(i) * kBniRecordStride;
    BniRecord e;
    e.recordFileOffset = rec;
    for (std::size_t j = 0; j < e.nameField.size(); ++j) {
      e.nameField[j] = file[rec + j];
      if (e.nameField[j] == std::byte{0}) {
        e.nameHasTerminator = true;
      }
    }
    e.imageOffset = *r.peekU32le(rec + 0x0c);
    e.payloadFileOffset = kBniImageBaseOffset + e.imageOffset;
    if (e.payloadFileOffset < directoryEnd || e.payloadFileOffset > size) {
      fail(BniDirectoryStatus::kOffsetOutOfBounds, i,
           "record %u: offset 0x%x -> file 0x%llx outside payload "
           "region [0x%llx, 0x%llx]", i, e.imageOffset,
           static_cast<unsigned long long>(e.payloadFileOffset),
           static_cast<unsigned long long>(directoryEnd),
           static_cast<unsigned long long>(size));
      return result;
    }
    result.records.push_back(e);
  }

  // Payload boundaries: each payload spans to the next DISTINCT stored
  // offset (records may alias — none observed), the last to EOF.
  // OBSERVED 6/6: offsets are unique and ascending in record order.
  std::vector<std::uint64_t> sorted;
  sorted.reserve(result.records.size());
  for (const auto& e : result.records) {
    sorted.push_back(e.payloadFileOffset);
  }
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (auto& e : result.records) {
    const auto it =
        std::upper_bound(sorted.begin(), sorted.end(), e.payloadFileOffset);
    e.payloadEnd = it == sorted.end() ? size : *it;
  }

  result.offsetsSortedAscending = std::is_sorted(
      result.records.begin(), result.records.end(),
      [](const BniRecord& a, const BniRecord& b) {
        return a.payloadFileOffset < b.payloadFileOffset;
      });
  result.offsetsUnique = sorted.size() == result.records.size();
  result.firstPayloadAtDirectoryEnd =
      !sorted.empty() && sorted.front() == directoryEnd;

  result.status = BniDirectoryStatus::kOk;
  return result;
}

std::string_view bniDirectoryStatusName(BniDirectoryStatus s) {
  switch (s) {
    case BniDirectoryStatus::kOk:                  return "ok";
    case BniDirectoryStatus::kNotLengthEnvelope:   return "not-length-envelope";
    case BniDirectoryStatus::kTruncatedHeader:     return "truncated-header";
    case BniDirectoryStatus::kDirectoryOutOfBounds:
      return "directory-out-of-bounds";
    case BniDirectoryStatus::kOffsetOutOfBounds:   return "offset-out-of-bounds";
  }
  return "?";
}

} // namespace mdk
