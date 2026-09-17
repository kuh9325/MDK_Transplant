#include "core/mto_directory.h"

#include "core/binary_reader.h"
#include "core/container.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace mdk {

namespace {

// Printable run up to first NUL; non-printable bytes become '\xNN'.
template <std::size_t N>
std::string fieldName(const std::array<std::byte, N>& field) {
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

// OBSERVED name-field shape: every byte NUL or printable ASCII with at
// least one printable byte, and nothing after the first NUL (all 60
// inner ".MAT" names and all 60 table-1 names in BUILD_A conform).
// This implementation's own hardening — the original performs no such
// validation.
template <std::size_t N>
bool plausibleNameField(const std::array<std::byte, N>& field) {
  bool any = false;
  bool seenNul = false;
  for (std::byte b : field) {
    const auto v = static_cast<unsigned char>(b);
    if (v == 0) {
      seenNul = true;
      continue;
    }
    if (seenNul || v < 0x20 || v > 0x7e) {
      return false;
    }
    any = true;
  }
  return any;
}

constexpr std::uint64_t align4(std::uint64_t v) { return (v + 3) & ~3ull; }

} // namespace

std::string MtoInnerRecord::name() const { return fieldName(nameField); }
std::string MtoNameOffsetRecord::name() const { return fieldName(nameField); }
std::string MtoRegionCName::name() const { return fieldName(nameField); }
std::string MtoEntry::name() const { return fieldName(nameField); }
std::string MtoBlock::innerName() const { return fieldName(innerNameField); }

MtoDirectory inspectMtoDirectory(std::span<const std::byte> file) {
  MtoDirectory result;
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

  if (size < kMtoCountOffset + 4) {
    result.status = MtoDirectoryStatus::kTruncatedHeader;
    result.detail = "file too small for the count field at 0x14";
    return result;
  }
  const auto count = r.peekU32le(kMtoCountOffset);
  if (!count) {
    result.status = MtoDirectoryStatus::kTruncatedHeader;
    result.detail = "cannot read the count field at 0x14";
    return result;
  }
  result.count = *count;

  // Table 1 must fit inside the file (division-first, no overflow).
  const std::uint64_t directoryEnd = kMtoRecordBase +
      static_cast<std::uint64_t>(*count) * kMtoRecordStride;
  if (*count > (size - kMtoRecordBase) / kMtoRecordStride) {
    result.status = MtoDirectoryStatus::kDirectoryOutOfBounds;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "count=%u needs %llu bytes at 0x18; file has %llu",
                  *count,
                  static_cast<unsigned long long>(
                      static_cast<std::uint64_t>(*count) *
                      kMtoRecordStride),
                  static_cast<unsigned long long>(
                      size - kMtoRecordBase));
    result.detail = buf;
    return result;
  }
  result.directoryEnd = directoryEnd;

  // Blocks live in [directoryEnd, trailerEnd]; the OBSERVED trailer
  // occupies the last 12 bytes when present.
  const std::uint64_t trailerEnd =
      result.trailerPresent ? size - kContainerNameFieldSize : size;

  const auto fail = [&](MtoDirectoryStatus st, std::size_t idx,
                        const char* fmt, ...) {
    result.status = st;
    result.badEntryIndex = idx;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    result.detail = buf;
  };

  result.entries.reserve(*count);
  result.blocks.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const std::uint64_t rec = kMtoRecordBase +
        static_cast<std::uint64_t>(i) * kMtoRecordStride;
    MtoEntry e;
    for (std::size_t j = 0; j < e.nameField.size(); ++j) {
      e.nameField[j] = file[rec + j];
    }
    e.blockFileOffset = *r.peekU32le(rec + 0x08);
    result.entries.push_back(e);

    // --- Overlay block ----------------------------------------------
    MtoBlock b;
    b.fileOffset = e.blockFileOffset;
    const std::uint64_t off = e.blockFileOffset;
    if (off < directoryEnd || off + 4 > trailerEnd) {
      fail(MtoDirectoryStatus::kBlockOutOfBounds, i,
           "entry %u: block offset 0x%llx outside [%llu, %llu)", i,
           static_cast<unsigned long long>(off),
           static_cast<unsigned long long>(directoryEnd),
           static_cast<unsigned long long>(trailerEnd));
      return result;
    }
    b.blockLength = *r.peekU32le(off);
    const std::uint64_t bend = off + b.blockLength;
    // Minimum block: header (0x10) + smallest embedded file (0x24) +
    // region-A size field + smallest region-A struct (12).
    constexpr std::uint64_t kMinBlock =
        kMtoBlockHeaderSize + kMtoInnerMinSize + 4 + 12;
    if (b.blockLength < kMinBlock || bend > trailerEnd) {
      fail(MtoDirectoryStatus::kBlockOutOfBounds, i,
           "entry %u: block [0x%llx, 0x%llx) escapes [%llu, %llu)",
           i, static_cast<unsigned long long>(off),
           static_cast<unsigned long long>(bend),
           static_cast<unsigned long long>(directoryEnd),
           static_cast<unsigned long long>(trailerEnd));
      return result;
    }

    b.fieldAt0x04 = *r.peekU32le(off + 0x04);
    b.fieldAt0x08 = *r.peekU32le(off + 0x08);
    b.fieldAt0x0C = *r.peekU32le(off + 0x0c);

    // --- Embedded tagged ".MAT" file at off+0x10 ----------------------
    const std::uint64_t inner = off + kMtoInnerFileOffset;
    b.innerLength = *r.peekU32le(inner);
    const std::uint64_t innerSize =
        static_cast<std::uint64_t>(b.innerLength) + 4;
    b.innerEndOffset = inner + innerSize;
    if (innerSize < kMtoInnerMinSize || b.innerEndOffset > bend) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: embedded file size 0x%llx escapes block", i,
           static_cast<unsigned long long>(innerSize));
      return result;
    }
    for (std::size_t j = 0; j < b.innerNameField.size(); ++j) {
      b.innerNameField[j] = file[inner + 4 + j];
    }
    if (!plausibleNameField(b.innerNameField)) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: embedded name field is not a plausible "
           "NUL-padded ASCII name", i);
      return result;
    }
    b.innerSecondaryLength = *r.peekU32le(inner + 0x10);
    b.innerSecondaryEqualsTrailerOffset =
        static_cast<std::uint64_t>(b.innerSecondaryLength) ==
        innerSize - 12;
    b.innerCount = *r.peekU32le(inner + kMtoInnerCountOffset);
    const std::uint64_t innerTrailerPos = b.innerEndOffset - 12;
    b.innerTrailerPresent =
        std::equal(b.innerNameField.begin(), b.innerNameField.end(),
                   file.begin() + innerTrailerPos);
    const std::uint64_t innerDirEnd =
        inner + kMtoInnerRecordBase +
        static_cast<std::uint64_t>(b.innerCount) * kMtoInnerRecordStride;
    if (b.innerCount >
        (innerTrailerPos - inner - kMtoInnerRecordBase) /
            kMtoInnerRecordStride) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: embedded count=%u needs records past the inner "
           "trailer", i, b.innerCount);
      return result;
    }

    // Inner ".MAT" records: the MTI 24-byte record form; +0x14 payload
    // offsets are relative to the embedded name field (inner+4).
    const std::uint64_t img = inner + kMtoInnerBlobOffset;  // off+0x14
    for (std::uint32_t j = 0; j < b.innerCount; ++j) {
      const std::uint64_t ro =
          inner + kMtoInnerRecordBase +
          static_cast<std::uint64_t>(j) * kMtoInnerRecordStride;
      MtoInnerRecord mr;
      for (std::size_t k = 0; k < mr.nameField.size(); ++k) {
        mr.nameField[k] = file[ro + k];
      }
      mr.fieldAt0x08 = *r.peekU32le(ro + 0x08);
      mr.fieldAt0x0C = *r.peekU32le(ro + 0x0c);
      mr.fieldAt0x10 = *r.peekU32le(ro + 0x10);
      mr.fieldAt0x14 = *r.peekU32le(ro + 0x14);
      if (mr.isIndexRecord()) {
        b.innerRecords.push_back(mr);
        continue;
      }
      // Payload record: img+f14 must land inside [innerDirEnd,
      // innerTrailerPos) with room for the payload header.
      const std::uint64_t pstart = img + mr.fieldAt0x14;
      const std::uint64_t hdrEnd = pstart + mr.payloadHeaderBytes();
      if (pstart < innerDirEnd || hdrEnd > innerTrailerPos) {
        fail(MtoDirectoryStatus::kInteriorMalformed, i,
             "entry %u: inner record %u payload [%llu, %llu) outside "
             "[%llu, %llu)", i, j,
             static_cast<unsigned long long>(pstart),
             static_cast<unsigned long long>(hdrEnd),
             static_cast<unsigned long long>(innerDirEnd),
             static_cast<unsigned long long>(innerTrailerPos));
        return result;
      }
      if (mr.hasExtendedHeader()) {
        mr.headerCount = *r.peekU16le(pstart);
        mr.headerFieldA = *r.peekU16le(pstart + 4);
        mr.headerFieldB = *r.peekU16le(pstart + 6);
      } else {
        mr.headerFieldA = *r.peekU16le(pstart);
        mr.headerFieldB = *r.peekU16le(pstart + 2);
      }
      mr.payloadDataFileOffset = hdrEnd;
      b.innerRecords.push_back(mr);
    }

    // --- Region A: {u32 size, struct}; struct base tA = off+8+f04 -----
    b.regionAOffset = off + 8 + b.fieldAt0x04;
    const std::uint64_t tA = b.regionAOffset;
    if (tA < b.innerEndOffset || tA > bend) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: region A target 0x%llx precedes the embedded file "
           "or escapes the block", i,
           static_cast<unsigned long long>(tA));
      return result;
    }
    // The self-exclusive size field immediately precedes the struct
    // (OBSERVED: tA == innerEnd+4 in all 60 BUILD_A blocks, making the
    // size field innerEnd+0). Bounds here are our own hardening.
    b.regionASize = *r.peekU32le(tA - 4);
    if (b.regionASize < 12 || tA + b.regionASize > bend) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: region A size %u escapes the block", i,
           b.regionASize);
      return result;
    }
    b.regionACountA = *r.peekU32le(tA + 0x00);
    b.regionACountB = *r.peekU32le(tA + 0x04);
    b.regionACountC = *r.peekU32le(tA + 0x08);
    const std::uint64_t arraysEnd =
        tA + 12 +
        static_cast<std::uint64_t>(b.regionACountA) * kMtoRegionABStride +
        static_cast<std::uint64_t>(b.regionACountB) * kMtoRegionABStride +
        static_cast<std::uint64_t>(b.regionACountC) * kMtoRegionACStride;
    if (arraysEnd > tA + b.regionASize) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: region A record arrays escape the declared region "
           "size", i);
      return result;
    }
    b.regionAEndOffset = align4(tA + b.regionASize);

    std::uint64_t p = tA + 12;
    for (std::uint32_t j = 0; j < b.regionACountA;
         ++j, p += kMtoRegionABStride) {
      MtoNameOffsetRecord nr;
      for (std::size_t k = 0; k < nr.nameField.size(); ++k)
        nr.nameField[k] = file[p + k];
      nr.fieldAt0x08 = *r.peekU32le(p + 8);
      // tA-relative target must stay inside region A (hardening bound;
      // the original dereferences unconditionally).
      if (nr.fieldAt0x08 < 12 || nr.fieldAt0x08 >= b.regionASize) {
        fail(MtoDirectoryStatus::kInteriorMalformed, i,
             "entry %u: region A record %u offset 0x%x escapes region A",
             i, j, nr.fieldAt0x08);
        return result;
      }
      b.regionAArrayA.push_back(nr);
    }
    for (std::uint32_t j = 0; j < b.regionACountB;
         ++j, p += kMtoRegionABStride) {
      MtoNameOffsetRecord nr;
      for (std::size_t k = 0; k < nr.nameField.size(); ++k)
        nr.nameField[k] = file[p + k];
      nr.fieldAt0x08 = *r.peekU32le(p + 8);
      if (nr.fieldAt0x08 < 12 || nr.fieldAt0x08 >= b.regionASize) {
        fail(MtoDirectoryStatus::kInteriorMalformed, i,
             "entry %u: region A array-B record %u offset 0x%x escapes "
             "region A", i, j, nr.fieldAt0x08);
        return result;
      }
      b.regionAArrayB.push_back(nr);
    }
    for (std::uint32_t j = 0; j < b.regionACountC;
         ++j, p += kMtoRegionACStride) {
      MtoSoundRecord sr;
      sr.fieldAt0x00 = *r.peekU32le(p + 0x00);
      sr.fieldAt0x04 = *r.peekU32le(p + 0x04);
      sr.fieldAt0x08 = *r.peekU32le(p + 0x08);
      sr.fieldAt0x0C = *r.peekU16le(p + 0x0c);
      sr.fieldAt0x0E = *r.peekU16le(p + 0x0e);
      sr.fieldAt0x10 = *r.peekU32le(p + 0x10);
      sr.fieldAt0x14 = *r.peekU32le(p + 0x14);
      if (sr.fieldAt0x10 < 12 || sr.fieldAt0x10 >= b.regionASize) {
        fail(MtoDirectoryStatus::kInteriorMalformed, i,
             "entry %u: region A array-C record %u offset 0x%x escapes "
             "region A", i, j, sr.fieldAt0x10);
        return result;
      }
      b.regionAArrayC.push_back(sr);
    }

    // --- Regions B and C ----------------------------------------------
    b.regionBOffset = off + 4 + b.fieldAt0x08;  // CODE-CORROBORATED base
    b.regionCOffset = off + 4 + b.fieldAt0x0C;  // CODE-CORROBORATED base
    if (b.regionBOffset > b.regionCOffset || b.regionCOffset > bend) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: region B/C targets out of order or outside the "
           "block (B=0x%llx C=0x%llx end=0x%llx)", i,
           static_cast<unsigned long long>(b.regionBOffset),
           static_cast<unsigned long long>(b.regionCOffset),
           static_cast<unsigned long long>(bend));
      return result;
    }
    b.regionBSize = b.regionCOffset - b.regionBOffset;

    // Region C: the FUN_00419ee0 counted-array walk; every step bounded
    // by the block end. {c1, rec10[c1], pad2 iff c1 odd, c2, rec44[c2],
    // c3, rec36[c3], c4, rec12[c4], u32, extra data}.
    std::uint64_t q = b.regionCOffset;
    const auto rdCount = [&](std::uint32_t& out) -> bool {
      if (q + 4 > bend) return false;
      out = *r.peekU32le(q);
      q += 4;
      return true;
    };
    const auto skip = [&](std::uint64_t n) -> bool {
      if (n > bend - q) return false;
      q += n;
      return true;
    };
    if (!rdCount(b.regionCCount1)) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: region C count1 unreadable", i);
      return result;
    }
    const std::uint64_t arr1End = q +
        static_cast<std::uint64_t>(b.regionCCount1) * kMtoRegionCStride1 +
        (b.regionCCount1 & 1 ? 2 : 0);
    if (arr1End > bend ||
        !skip(static_cast<std::uint64_t>(b.regionCCount1) *
                  kMtoRegionCStride1 + (b.regionCCount1 & 1 ? 2 : 0)) ||
        !rdCount(b.regionCCount2) ||
        !skip(static_cast<std::uint64_t>(b.regionCCount2) *
              kMtoRegionCStride2) ||
        !rdCount(b.regionCCount3) ||
        !skip(static_cast<std::uint64_t>(b.regionCCount3) *
              kMtoRegionCStride3) ||
        !rdCount(b.regionCCount4) ||
        !skip(static_cast<std::uint64_t>(b.regionCCount4) *
              kMtoRegionCStride4) ||
        !skip(4)) {
      fail(MtoDirectoryStatus::kInteriorMalformed, i,
           "entry %u: region C counted arrays escape the block", i);
      return result;
    }
    b.regionCExtraOffset = q;  // array4 end + 4 (the skipped u32)
    // Preserve the array-1 name[10] fields (lookup-key-shaped metadata).
    for (std::uint32_t j = 0; j < b.regionCCount1; ++j) {
      MtoRegionCName nm;
      const std::uint64_t no =
          b.regionCOffset + 4 +
          static_cast<std::uint64_t>(j) * kMtoRegionCStride1;
      for (std::size_t k = 0; k < nm.nameField.size(); ++k)
        nm.nameField[k] = file[no + k];
      b.regionCNames.push_back(nm);
    }

    result.blocks.push_back(std::move(b));
  }

  result.status = MtoDirectoryStatus::kOk;
  return result;
}

std::string_view mtoDirectoryStatusName(MtoDirectoryStatus s) {
  switch (s) {
    case MtoDirectoryStatus::kOk:                  return "ok";
    case MtoDirectoryStatus::kNotTaggedEnvelope:   return "not-tagged-envelope";
    case MtoDirectoryStatus::kTruncatedHeader:     return "truncated-header";
    case MtoDirectoryStatus::kDirectoryOutOfBounds:return "directory-out-of-bounds";
    case MtoDirectoryStatus::kBlockOutOfBounds:    return "block-out-of-bounds";
    case MtoDirectoryStatus::kInteriorMalformed:   return "interior-malformed";
  }
  return "?";
}

} // namespace mdk
