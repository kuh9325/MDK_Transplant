#include "core/dti_structure.h"

#include "core/binary_reader.h"
#include "core/container.h"

#include <algorithm>
#include <bit>
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

float DtiKeyedRecord::floatAt(std::size_t i) const {
  return std::bit_cast<float>(raw[i]);
}

std::string DtiSubRecord::name18() const {
  // Record bytes [0x18, 0x24) = fields[5..7], little-endian as stored
  // — the name window the loader strcmp's for types 2/4.
  std::byte bytes[12];
  for (std::size_t i = 0; i < 3; ++i) {
    const std::uint32_t v = fields[5 + i];
    bytes[i * 4 + 0] = static_cast<std::byte>(v & 0xff);
    bytes[i * 4 + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    bytes[i * 4 + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    bytes[i * 4 + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  }
  return fieldName(bytes);
}

float DtiSubRecord::fieldAsFloat(std::size_t i) const {
  return std::bit_cast<float>(fields[i]);
}

std::string DtiArenaRecord::name() const { return fieldName(nameBytes); }

float DtiArenaRecord::scalar() const {
  return std::bit_cast<float>(scalarBits);
}

DtiStructure inspectDtiStructure(std::span<const std::byte> file) {
  DtiStructure result;
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

  if (size < kDtiTocFileOffset + kDtiTocEntries * 4) {
    result.status = DtiStructureStatus::kTruncatedHeader;
    result.detail = "file too small for the five-entry TOC at 0x14";
    return result;
  }

  const auto fail = [&](DtiStructureStatus st, std::size_t section,
                        std::size_t record, const char* fmt, ...) {
    result.status = st;
    result.badSection = section;
    result.badRecord = record;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    result.detail = buf;
  };

  // TOC: five image-relative offsets at file 0x14. The original
  // dereferences img + toc[i] directly; for safe enumeration we
  // require them strictly increasing inside [0x24, image span) so the
  // five sections tile the interior. OBSERVED in all 6 files.
  for (std::size_t i = 0; i < kDtiTocEntries; ++i) {
    const auto v = r.peekU32le(
        static_cast<std::size_t>(kDtiTocFileOffset + i * 4));
    result.tocImageOffsets[i] = *v;  // in-bounds: size >= 0x28
  }

  const std::uint64_t imageSpan = trailerStart - kDtiImageBaseOffset;
  for (std::size_t i = 0; i < kDtiTocEntries; ++i) {
    const std::uint64_t v = result.tocImageOffsets[i];
    const std::uint64_t prev =
        (i == 0) ? kDtiTocFileOffset - kDtiImageBaseOffset +
                       kDtiTocEntries * 4
                 : result.tocImageOffsets[i - 1];
    // toc[0] must sit at/after the TOC end (image 0x24); each later
    // entry must not decrease (an empty span fails its own section
    // minimum check below).
    if (v < prev || v > imageSpan) {
      fail(DtiStructureStatus::kSectionOutOfBounds, i,
           static_cast<std::size_t>(-1),
           "toc[%zu]=0x%08x not in (0x%llx, 0x%llx] ordering/bounds", i,
           result.tocImageOffsets[i],
           static_cast<unsigned long long>(prev),
           static_cast<unsigned long long>(imageSpan));
      return result;
    }
  }

  // Section spans in file offsets: s_i = [4 + toc[i], 4 + toc[i+1]),
  // s4 = [4 + toc[4], trailerStart).
  for (std::size_t i = 0; i < kDtiTocEntries; ++i) {
    result.sections[i].fileStart =
        kDtiImageBaseOffset + result.tocImageOffsets[i];
    result.sections[i].fileEnd =
        (i + 1 < kDtiTocEntries)
            ? kDtiImageBaseOffset + result.tocImageOffsets[i + 1]
            : trailerStart;
  }

  // ---- s0: parameter block (proven reads cover 29 u32s) ----
  const DtiSpan s0 = result.sections[kDtiSecParams];
  if (s0.size() < kDtiParamBlockSize) {
    fail(DtiStructureStatus::kSectionOutOfBounds, kDtiSecParams,
         static_cast<std::size_t>(-1),
         "s0 params: span 0x%llx smaller than the proven 0x%zx "
         "(29 u32s read by the loader)",
         static_cast<unsigned long long>(s0.size()),
         kDtiParamBlockSize);
    return result;
  }
  for (std::size_t i = 0; i < kDtiParamWords; ++i) {
    result.params[i] = *r.peekU32le(
        static_cast<std::size_t>(s0.fileStart + i * 4));
  }

  // ---- s1: keyed records {u32 count, count x 24B} ----
  const DtiSpan s1 = result.sections[kDtiSecKeyed];
  if (s1.size() < 4) {
    fail(DtiStructureStatus::kSectionOutOfBounds, kDtiSecKeyed,
         static_cast<std::size_t>(-1),
         "s1 span 0x%llx too small for the count field",
         static_cast<unsigned long long>(s1.size()));
    return result;
  }
  {
    const std::uint32_t count =
        *r.peekU32le(static_cast<std::size_t>(s1.fileStart));
    const std::uint64_t need =
        4 + static_cast<std::uint64_t>(count) * kDtiKeyedRecordStride;
    if (need > s1.size()) {
      fail(DtiStructureStatus::kRecordOutOfBounds, kDtiSecKeyed,
           static_cast<std::size_t>(-1),
           "s1: count=%u x %u + 4 = 0x%llx exceeds span 0x%llx", count,
           static_cast<unsigned>(kDtiKeyedRecordStride),
           static_cast<unsigned long long>(need),
           static_cast<unsigned long long>(s1.size()));
      return result;
    }
    result.keyedRecords.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint64_t rp =
          s1.fileStart + 4 + static_cast<std::uint64_t>(i) *
                                 kDtiKeyedRecordStride;
      DtiKeyedRecord rec;
      rec.fileOffset = rp;
      rec.word0 = *r.peekU32le(static_cast<std::size_t>(rp));
      rec.key = *r.peekU32le(static_cast<std::size_t>(rp + 4));
      for (std::size_t f = 0; f < 4; ++f) {
        rec.raw[f] =
            *r.peekU32le(static_cast<std::size_t>(rp + 8 + f * 4));
      }
      result.keyedRecords.push_back(rec);
    }
    result.s1TrailingBytes = s1.size() - need;
  }

  // ---- s2: arena table + tiled payloads ----
  const DtiSpan s2 = result.sections[kDtiSecArenas];
  if (s2.size() < 4) {
    fail(DtiStructureStatus::kSectionOutOfBounds, kDtiSecArenas,
         static_cast<std::size_t>(-1),
         "s2 span 0x%llx too small for the count field",
         static_cast<unsigned long long>(s2.size()));
    return result;
  }
  {
    const std::uint32_t count =
        *r.peekU32le(static_cast<std::size_t>(s2.fileStart));
    const std::uint64_t tableBytes =
        4 + static_cast<std::uint64_t>(count) * kDtiArenaRecordStride;
    if (tableBytes > s2.size()) {
      fail(DtiStructureStatus::kRecordOutOfBounds, kDtiSecArenas,
           static_cast<std::size_t>(-1),
           "s2: count=%u x %u + 4 = 0x%llx exceeds span 0x%llx", count,
           static_cast<unsigned>(kDtiArenaRecordStride),
           static_cast<unsigned long long>(tableBytes),
           static_cast<unsigned long long>(s2.size()));
      return result;
    }
    const std::uint64_t recordsEnd = s2.fileStart + tableBytes;
    result.s2PayloadRegionStart = recordsEnd;

    result.arenas.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint64_t rp =
          s2.fileStart + 4 + static_cast<std::uint64_t>(i) *
                                 kDtiArenaRecordStride;
      DtiArenaRecord rec;
      rec.fileOffset = rp;
      std::copy_n(file.begin() + rp, 8, rec.nameBytes.begin());
      rec.nameEndsWithTerminator =
          std::find(rec.nameBytes.begin(), rec.nameBytes.end(),
                    std::byte{0}) != rec.nameBytes.end();
      rec.payloadImageOffset =
          *r.peekU32le(static_cast<std::size_t>(rp + 8));
      rec.scalarBits = *r.peekU32le(static_cast<std::size_t>(rp + 12));
      rec.payloadFileOffset =
          kDtiImageBaseOffset + rec.payloadImageOffset;

      // Native hardening: the original dereferences the payload
      // pointer unconditionally; require it inside s2's payload
      // region [recordsEnd, s2End).
      if (rec.payloadFileOffset + 4 > s2.fileEnd ||
          rec.payloadFileOffset < recordsEnd) {
        fail(DtiStructureStatus::kOffsetOutOfBounds, kDtiSecArenas, i,
             "arena %u: payload imageOff 0x%08x -> file 0x%llx outside "
             "payload region [0x%llx, 0x%llx)", i,
             rec.payloadImageOffset,
             static_cast<unsigned long long>(rec.payloadFileOffset),
             static_cast<unsigned long long>(recordsEnd),
             static_cast<unsigned long long>(s2.fileEnd));
        return result;
      }
      rec.subRecordCount = *r.peekU32le(
          static_cast<std::size_t>(rec.payloadFileOffset));
      const std::uint64_t subBytes =
          4 + static_cast<std::uint64_t>(rec.subRecordCount) *
                  kDtiSubRecordStride;
      if (rec.payloadFileOffset + subBytes > s2.fileEnd) {
        fail(DtiStructureStatus::kRecordOutOfBounds, kDtiSecArenas, i,
             "arena %u: payload count=%u x %u + 4 escapes s2 end "
             "0x%llx", i, rec.subRecordCount,
             static_cast<unsigned>(kDtiSubRecordStride),
             static_cast<unsigned long long>(s2.fileEnd));
        return result;
      }
      rec.subRecords.reserve(rec.subRecordCount);
      for (std::uint32_t j = 0; j < rec.subRecordCount; ++j) {
        const std::uint64_t sp =
            rec.payloadFileOffset + 4 +
            static_cast<std::uint64_t>(j) * kDtiSubRecordStride;
        DtiSubRecord sub;
        sub.fileOffset = sp;
        sub.type = *r.peekU32le(static_cast<std::size_t>(sp));
        for (std::size_t f = 0; f < 8; ++f) {
          sub.fields[f] =
              *r.peekU32le(static_cast<std::size_t>(sp + 4 + f * 4));
        }
        rec.subRecords.push_back(sub);
      }
      result.arenas.push_back(std::move(rec));
    }
  }

  // ---- s3: palette table {u32 count, u8 rgb[768]} ----
  const DtiSpan s3 = result.sections[kDtiSecPalette];
  if (s3.size() < 4 + kDtiPaletteBytes) {
    fail(DtiStructureStatus::kSectionOutOfBounds, kDtiSecPalette,
         static_cast<std::size_t>(-1),
         "s3 span 0x%llx smaller than the proven 4 + %zu", 
         static_cast<unsigned long long>(s3.size()), kDtiPaletteBytes);
    return result;
  }
  result.paletteCount =
      *r.peekU32le(static_cast<std::size_t>(s3.fileStart));
  result.paletteBytes.fileStart = s3.fileStart + 4;
  result.paletteBytes.fileEnd = s3.fileStart + 4 + kDtiPaletteBytes;
  result.s3TrailingBytes = s3.size() - (4 + kDtiPaletteBytes);

  // ---- s4: backdrop grid; size derived from s0 words ----
  // planeSize = (params[9] + 4) * params[10]; a second plane follows
  // when params[0x0b] > 0 (signed compare in the loader).
  const std::uint64_t planeSize =
      (static_cast<std::uint64_t>(result.params[9]) + 4) *
      result.params[10];
  const std::uint32_t planes =
      1 + (static_cast<std::int32_t>(result.params[0x0b]) > 0 ? 1 : 0);
  result.gridPlaneSize = planeSize;
  result.gridPlaneCount = planes;
  const DtiSpan s4 = result.sections[kDtiSecGrid];
  if (planeSize * planes > s4.size()) {
    fail(DtiStructureStatus::kSectionOutOfBounds, kDtiSecGrid,
         static_cast<std::size_t>(-1),
         "s4 span 0x%llx smaller than %u plane(s) x 0x%llx "
         "(grid (%u+4)x%u from s0[9]/s0[10])",
         static_cast<unsigned long long>(s4.size()), planes,
         static_cast<unsigned long long>(planeSize),
         result.params[9], result.params[10]);
    return result;
  }
  result.s4TrailingBytes = s4.size() - planeSize * planes;

  result.status = DtiStructureStatus::kOk;
  return result;
}

std::string_view dtiStructureStatusName(DtiStructureStatus s) {
  switch (s) {
    case DtiStructureStatus::kOk:                 return "ok";
    case DtiStructureStatus::kNotTaggedEnvelope:  return "not-tagged-envelope";
    case DtiStructureStatus::kTruncatedHeader:    return "truncated-header";
    case DtiStructureStatus::kSectionOutOfBounds: return "section-out-of-bounds";
    case DtiStructureStatus::kRecordOutOfBounds:  return "record-out-of-bounds";
    case DtiStructureStatus::kOffsetOutOfBounds:  return "offset-out-of-bounds";
  }
  return "?";
}

} // namespace mdk
