// mdk-inspect — developer-facing read-only inspector for original MDK
// data files. Prints safe header/container/directory metadata only:
// never dumps payloads, never writes into the data root.
//
// Usage:
//   mdk-inspect --data-path DIR <relative-path>
//   mdk-inspect --data-path DIR --container <relative-path>
//   mdk-inspect --data-path DIR --entries <relative-path>
//   mdk-inspect --selftest        (synthetic in-memory checks)

#include "core/binary_reader.h"
#include "core/container.h"
#include "core/data_root.h"
#include "core/file_family.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/sni_directory.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace {

constexpr std::size_t kInspectHeadBytes = 64;
// Whole-file read cap for --entries: far above every observed file
// (largest in BUILD_A ≈ 8 MB) while staying a sane bound.
constexpr std::size_t kEntriesMaxBytes = 512ull * 1024 * 1024;

int usage() {
  std::fprintf(stderr,
               "usage: mdk-inspect --data-path DIR [--container | "
               "--entries] <relative-path>\n"
               "       mdk-inspect --selftest\n");
  return 2;
}

std::string stemOf(const std::string& relPath) {
  const auto slash = relPath.find_last_of("/\\");
  const auto dot = relPath.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return {};
  }
  const auto begin = slash == std::string::npos ? 0 : slash + 1;
  return relPath.substr(begin, dot - begin);
}

int selftest() {
  // Synthetic 28-byte tag-envelope fixture (not original data):
  //   u32 len = 24 (= size-4), name "TEST.MAT" + NUL pad.
  const std::byte raw[] = {
      std::byte{0x18}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{'T'},  std::byte{'E'},  std::byte{'S'},  std::byte{'T'},
      std::byte{'.'},  std::byte{'M'},  std::byte{'A'},  std::byte{'T'},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x0c}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };
  const auto info = mdk::inspectContainer(raw, sizeof(raw));
  bool ok = info.hasDeclaredLength && info.lengthValid &&
            info.declaredLength == 24 &&
            info.shape == mdk::ContainerShape::kTaggedName &&
            info.logicalName == "TEST.MAT" &&
            mdk::nameStemMatches(info, "test");
  std::fprintf(stderr, "selftest envelope: %s\n", ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic SNI-like fixture: envelope + count=1 + one 24-byte
  // record {name[12], u32, blobOff=0x18, size=4} + 4 payload bytes +
  // name trailer. Total 0x18+0x18+4+12 = 0x48 = 72 bytes.
  std::byte sni[72] = {};
  const auto put32 = [&](std::size_t off, std::uint32_t v) {
    sni[off + 0] = static_cast<std::byte>(v & 0xff);
    sni[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    sni[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    sni[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto putName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(sni); ++i) {
      sni[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  put32(0x00, sizeof(sni) - 4);
  putName(0x04, "TEST.SND");
  put32(0x10, sizeof(sni) - 12);
  put32(0x14, 1);                  // count
  putName(0x18, "ENTRY1");
  put32(0x18 + 0x0c, 3);           // unknown field
  put32(0x18 + 0x10, 0x30 - 4);    // blobOffset → file 0x30
  put32(0x18 + 0x14, 4);           // payloadSize
  putName(sizeof(sni) - 12, "TEST.SND");

  const auto dir = mdk::inspectSniDirectory(
      std::span<const std::byte>(sni, sizeof(sni)));
  ok = dir.status == mdk::SniDirectoryStatus::kOk &&
       dir.count == 1 && dir.entries.size() == 1 &&
       dir.entries[0].name() == "ENTRY1" &&
       dir.entries[0].payloadFileOffset() == 0x30 &&
       dir.entries[0].payloadSize == 4 && dir.trailerPresent &&
       dir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest sni-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic MTI-like fixture: envelope + count=2 + two 24-byte
  // records: one extended-header payload record and one index record.
  // Layout: 0x18 + 2*24 = 0x48 dir end; payload at 0x48 (8-byte ext
  // header: n=2,a=64,b=32 + 4 data bytes) then name trailer.
  // Total = 0x48 + 12 + 12 = 0x60 = 96 bytes.
  std::byte mti[96] = {};
  const auto mput32 = [&](std::size_t off, std::uint32_t v) {
    mti[off + 0] = static_cast<std::byte>(v & 0xff);
    mti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    mti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    mti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto mput16 = [&](std::size_t off, std::uint16_t v) {
    mti[off + 0] = static_cast<std::byte>(v & 0xff);
    mti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto mputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(mti); ++i) {
      mti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  mput32(0x00, sizeof(mti) - 4);
  mputName(0x04, "TEST.MTI");
  mput32(0x10, sizeof(mti) - 12);
  mput32(0x14, 2);                    // count
  mputName(0x18, "MAT0");             // record 0: payload (ext header)
  mput32(0x18 + 0x08, 0x00010001);    // flags: extended header
  mput32(0x18 + 0x0c, 0);             // raw param
  mput32(0x18 + 0x10, 0x40600000);    // raw param (float-looking)
  mput32(0x18 + 0x14, 0x48 - 4);      // blobOffset -> file 0x48
  mputName(0x30, "IDX0");             // record 1: index record
  mput32(0x30 + 0x08, 0xffffffff);    // index discriminator
  mput32(0x30 + 0x0c, 7);             // index value
  mput32(0x30 + 0x10, 0);
  mput32(0x30 + 0x14, 0);
  mput16(0x48, 2);                    // payload header: u16 @+0 (n)
  mput16(0x4c, 64);                   // u16 @+4 (fieldA)
  mput16(0x4e, 32);                   // u16 @+6 (fieldB)
  mputName(sizeof(mti) - 12, "TEST.MTI");

  const auto mdir = mdk::inspectMtiDirectory(
      std::span<const std::byte>(mti, sizeof(mti)));
  ok = mdir.status == mdk::MtiDirectoryStatus::kOk &&
       mdir.count == 2 && mdir.entries.size() == 2 &&
       mdir.entries[0].name() == "MAT0" &&
       !mdir.entries[0].isIndexRecord() &&
       mdir.entries[0].hasExtendedHeader() &&
       mdir.entries[0].payloadFileOffset() == 0x48 &&
       mdir.entries[0].headerCount == 2 &&
       mdir.entries[0].headerFieldA == 64 &&
       mdir.entries[0].headerFieldB == 32 &&
       mdir.entries[0].payloadDataFileOffset == 0x50 &&
       mdir.entries[1].isIndexRecord() &&
       mdir.entries[1].fieldAt0x0C == 7 &&
       mdir.trailerPresent && mdir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest mti-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic MTO-like fixture (no original data): tagged envelope +
  // count=1 + one 12-byte record {name[8]="OV1", blockOff=0x24} + one
  // overlay block + name trailer. File size 0xb8.
  //
  // Block @0x24, len=0x88 → [0x24, 0xac):
  //   +0x00 len=0x88, +0x04 ofsA=0x4c, +0x08 ofsB=0x5c, +0x0c ofsC=0x70
  //   embedded file @0x34 (innerSize=0x40 → innerEnd=0x74):
  //     innerLen=0x3c, name "OV1.MAT"@0x38, sec=0x34@0x44, count=1@0x48,
  //     rec @0x4c {"TEX", 0,0,0, imgOff=0x2c}, payload @0x64 {64,32},
  //     trailer "OV1.MAT"@0x68
  //   regionA: size=0x0c @0x74, struct {0,0,0} @0x78 → end 0x84
  //   regionB [0x84, 0x98) — off+4+0x5c = 0x84
  //   regionC @0x98 (off+4+0x70): c1..c4 = 0, u32, extra → 0xac = bend
  std::byte mto[0xb8] = {};
  const auto oput32 = [&](std::size_t off, std::uint32_t v) {
    mto[off + 0] = static_cast<std::byte>(v & 0xff);
    mto[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    mto[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    mto[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto oput16 = [&](std::size_t off, std::uint16_t v) {
    mto[off + 0] = static_cast<std::byte>(v & 0xff);
    mto[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto oputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(mto); ++i) {
      mto[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  oput32(0x00, sizeof(mto) - 4);
  oputName(0x04, "TEST.MAT");
  oput32(0x10, sizeof(mto) - 12);
  oput32(0x14, 1);                // overlay count
  oputName(0x18, "OV1");          // table-1 record
  oput32(0x20, 0x24);             // block file offset
  oput32(0x24, 0x88);             // blockLength → bend 0xac
  oput32(0x28, 0x4c);             // ofsA → 0x24+8+0x4c = 0x78
  oput32(0x2c, 0x5c);             // ofsB → 0x24+4+0x5c = 0x84
  oput32(0x30, 0x70);             // ofsC → 0x24+4+0x70 = 0x98
  oput32(0x34, 0x3c);             // innerLen → innerSize 0x40
  oputName(0x38, "OV1.MAT");
  oput32(0x44, 0x34);             // inner secondary = innerSize-12
  oput32(0x48, 1);                // inner count
  oputName(0x4c, "TEX");          // inner record name[8]
  oput32(0x60, 0x2c);             // rec+0x14: img(0x38)+0x2c = 0x64
  oput16(0x64, 64);               // payload header {64, 32}
  oput16(0x66, 32);
  oputName(0x68, "OV1.MAT");      // inner trailer → innerEnd 0x74
  oput32(0x74, 0x0c);             // regionA size (self-exclusive)
  // regionA struct @0x78: ca=cb=cc=0 (zeros)
  // regionB [0x84,0x98) zeros; regionC @0x98: c1..c4=0 + u32 (zeros)
  oputName(sizeof(mto) - 12, "TEST.MAT");

  const auto odir = mdk::inspectMtoDirectory(
      std::span<const std::byte>(mto, sizeof(mto)));
  ok = odir.status == mdk::MtoDirectoryStatus::kOk &&
       odir.count == 1 && odir.entries.size() == 1 &&
       odir.entries[0].name() == "OV1" &&
       odir.entries[0].blockFileOffset == 0x24 &&
       odir.blocks.size() == 1 &&
       odir.blocks[0].blockLength == 0x88 &&
       odir.blocks[0].innerName() == "OV1.MAT" &&
       odir.blocks[0].innerCount == 1 &&
       odir.blocks[0].innerRecords.size() == 1 &&
       odir.blocks[0].innerRecords[0].name() == "TEX" &&
       odir.blocks[0].innerRecords[0].headerFieldA == 64 &&
       odir.blocks[0].innerRecords[0].headerFieldB == 32 &&
       odir.blocks[0].innerRecords[0].payloadDataFileOffset == 0x68 &&
       odir.blocks[0].innerTrailerPresent &&
       odir.blocks[0].innerSecondaryEqualsTrailerOffset &&
       odir.blocks[0].regionASize == 0x0c &&
       odir.blocks[0].regionACountA == 0 &&
       odir.blocks[0].regionBOffset == 0x84 &&
       odir.blocks[0].regionBSize == 0x14 &&
       odir.blocks[0].regionCOffset == 0x98 &&
       odir.blocks[0].regionCCount1 == 0 &&
       odir.blocks[0].regionCExtraOffset == 0xac &&
       odir.trailerPresent && odir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest mto-directory: %s\n",
               ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  std::optional<std::string> dataPath;
  std::optional<std::string> target;
  bool entriesMode = false;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "--data-path")) {
      const char* v = value(a);
      if (!v) return usage();
      dataPath = v;
    } else if (!std::strcmp(a, "--container")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
    } else if (!std::strcmp(a, "--entries")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      entriesMode = true;
    } else if (!std::strcmp(a, "--selftest")) {
      return selftest();
    } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
      return usage();
    } else if (a[0] == '-') {
      std::fprintf(stderr, "unknown argument: %s\n", a);
      return usage();
    } else {
      target = a;
    }
  }

  if (!dataPath || !target) {
    return usage();
  }

  std::string err;
  const auto root = mdk::DataRoot::open(*dataPath, &err);
  if (!root) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }

  std::printf("request:   %s\n", target->c_str());
  const auto resolved = root->resolve(*target, &err);
  if (!resolved) {
    std::fprintf(stderr, "resolve:   FAILED (%s)\n", err.c_str());
    return 1;
  }
  std::printf("resolved:  %s\n", resolved->string().c_str());

  const auto size = root->fileSize(*target, &err);
  if (!size) {
    std::fprintf(stderr, "stat:      FAILED (%s)\n", err.c_str());
    return 1;
  }
  std::printf("size:      %llu bytes\n",
              static_cast<unsigned long long>(*size));

  const auto head = root->readPrefix(*target, kInspectHeadBytes, &err);
  if (!head) {
    std::fprintf(stderr, "read:      FAILED (%s)\n", err.c_str());
    return 1;
  }

  const auto family = mdk::fileFamilyForPath(*target);
  const auto support = mdk::fileFamilySupport(family);
  std::printf("family:    %s\n", std::string(mdk::fileFamilyName(family)).c_str());
  std::printf("envelope:  %s (by extension)\n",
              std::string(mdk::parserFamilyName(
                            mdk::parserFamilyForPath(*target))
                          )
                  .c_str());
  std::printf("support:   %s\n",
              std::string(mdk::familySupportName(support)).c_str());

  const auto info = mdk::inspectContainer(
      std::span<const std::byte>(head->data(), head->size()), *size);
  if (info.hasDeclaredLength) {
    std::printf("u32@0:     %u (0x%08x) — %s size-4\n", info.declaredLength,
                info.declaredLength,
                info.lengthValid ? "equals" : "DOES NOT equal");
  } else {
    std::printf("u32@0:     (file too small)\n");
  }

  switch (info.shape) {
    case mdk::ContainerShape::kNone:
      std::printf("envelope:  none (not this container format)\n");
      break;
    case mdk::ContainerShape::kLengthEnvelope:
      std::printf("envelope:  length-only (u32 valid; no tag/name "
                  "field)\n");
      break;
    case mdk::ContainerShape::kTaggedName:
      std::printf("envelope:  tagged-name\n");
      break;
  }

  if (info.hasTag) {
    std::printf("tag@4:     %s\n", mdk::tagToString(info.tag).c_str());
  }
  if (info.shape == mdk::ContainerShape::kTaggedName) {
    std::printf("name:      %s\n", info.logicalName.c_str());
    const std::string stem = stemOf(*target);
    if (!stem.empty()) {
      std::printf("stem:      %s — %s\n", stem.c_str(),
                  mdk::nameStemMatches(info, stem)
                      ? "match (case-insensitive)"
                      : "MISMATCH");
    }
    // Second observed u32 (offset 16): reported raw, not interpreted.
    mdk::BinaryReader r2(
        std::span<const std::byte>(head->data(), head->size()));
    if (r2.seek(16)) {
      if (const auto d2 = r2.u32le()) {
        std::printf("u32@16:    %u (0x%08x) — %s size-12 "
                    "(interior field; semantics not decoded)\n",
                    *d2, *d2,
                    *size >= 12 &&
                            static_cast<std::uint64_t>(*d2) == *size - 12
                        ? "equals"
                        : "does not equal");
      }
    }
  }

  if (!entriesMode) {
    return 0;
  }

  // --entries: enumerate interior directory metadata where a proven
  // parser exists (SNI Phase 3C; MTI Phase 3D; MTO Phase 3E). Never
  // prints payload bytes.
  if (support != mdk::FamilySupport::kDirectoryMetadata) {
    std::printf("entries:   unsupported for family %s (support: %s) — "
                "no evidence-backed interior parser\n",
                std::string(mdk::fileFamilyName(family)).c_str(),
                std::string(mdk::familySupportName(support)).c_str());
    return 1;
  }

  const auto file = root->readFile(*target, kEntriesMaxBytes, &err);
  if (!file) {
    std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
    return 1;
  }

  if (family == mdk::MdkFileFamily::kMti) {
    const auto dir = mdk::inspectMtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   MTI directory (count u32 @0x14, records "
                "24 bytes @0x18, name[8])\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::mtiDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::MtiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const auto& e = dir.entries[i];
      if (e.isIndexRecord()) {
        std::printf("  [%3zu] %-8s INDEX (field0x08=0xffffffff) "
                    "index=%u (0x%08x) field0x10=0x%08x "
                    "field0x14=0x%08x\n",
                    i, e.name().c_str(), e.fieldAt0x0C, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14);
      } else if (e.headerCount) {
        std::printf("  [%3zu] %-8s field0x08=0x%08x field0x0c=0x%08x "
                    "field0x10=0x%08x blobOff=0x%08x fileOff=0x%08llx "
                    "hdr{n=%u,a=%u,b=%u} dataOff=0x%08llx\n",
                    i, e.name().c_str(), e.fieldAt0x08, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14,
                    static_cast<unsigned long long>(e.payloadFileOffset()),
                    *e.headerCount, e.headerFieldA, e.headerFieldB,
                    static_cast<unsigned long long>(
                        e.payloadDataFileOffset));
      } else {
        std::printf("  [%3zu] %-8s field0x08=0x%08x field0x0c=0x%08x "
                    "field0x10=0x%08x blobOff=0x%08x fileOff=0x%08llx "
                    "hdr{a=%u,b=%u} dataOff=0x%08llx\n",
                    i, e.name().c_str(), e.fieldAt0x08, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14,
                    static_cast<unsigned long long>(e.payloadFileOffset()),
                    e.headerFieldA, e.headerFieldB,
                    static_cast<unsigned long long>(
                        e.payloadDataFileOffset));
      }
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kMto) {
    const auto dir = mdk::inspectMtoDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   MTO overlay directory (count u32 @0x14, "
                "records 12 bytes @0x18, name[8] + file offset)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::mtoDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::MtoDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const auto& e = dir.entries[i];
      const auto& b = dir.blocks[i];
      std::printf("  [%3zu] %-8s blockOff=0x%08llx blockLen=0x%x "
                  "inner=%-12s innerRecs=%u\n",
                  i, e.name().c_str(),
                  static_cast<unsigned long long>(e.blockFileOffset),
                  b.blockLength, b.innerName().c_str(), b.innerCount);
      std::printf("         fields{0x04=0x%x 0x08=0x%x 0x0c=0x%x} "
                  "innerTrailer=%s regionA{size=0x%x ca=%u cb=%u "
                  "cc=%u} regionB@0x%llx(0x%llx) regionC@0x%llx{c1=%u "
                  "c2=%u c3=%u c4=%u extra@0x%llx}\n",
                  b.fieldAt0x04, b.fieldAt0x08, b.fieldAt0x0C,
                  b.innerTrailerPresent ? "yes" : "no",
                  b.regionASize, b.regionACountA, b.regionACountB,
                  b.regionACountC,
                  static_cast<unsigned long long>(b.regionBOffset),
                  static_cast<unsigned long long>(b.regionBSize),
                  static_cast<unsigned long long>(b.regionCOffset),
                  b.regionCCount1, b.regionCCount2, b.regionCCount3,
                  b.regionCCount4,
                  static_cast<unsigned long long>(b.regionCExtraOffset));
      for (std::size_t j = 0; j < b.innerRecords.size(); ++j) {
        const auto& mr = b.innerRecords[j];
        if (mr.isIndexRecord()) {
          std::printf("           mat[%3zu] %-8s INDEX index=%u "
                      "field0x10=0x%08x field0x14=0x%08x\n",
                      j, mr.name().c_str(), mr.fieldAt0x0C,
                      mr.fieldAt0x10, mr.fieldAt0x14);
        } else if (mr.headerCount) {
          std::printf("           mat[%3zu] %-8s flags=0x%08x "
                      "f0x0c=0x%08x f0x10=0x%08x imgOff=0x%08x "
                      "fileOff=0x%08llx hdr{n=%u,a=%u,b=%u} "
                      "dataOff=0x%08llx\n",
                      j, mr.name().c_str(), mr.fieldAt0x08,
                      mr.fieldAt0x0C, mr.fieldAt0x10, mr.fieldAt0x14,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset -
                          mr.payloadHeaderBytes()),
                      *mr.headerCount, mr.headerFieldA, mr.headerFieldB,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset));
        } else {
          std::printf("           mat[%3zu] %-8s flags=0x%08x "
                      "f0x0c=0x%08x f0x10=0x%08x imgOff=0x%08x "
                      "fileOff=0x%08llx hdr{a=%u,b=%u} "
                      "dataOff=0x%08llx\n",
                      j, mr.name().c_str(), mr.fieldAt0x08,
                      mr.fieldAt0x0C, mr.fieldAt0x10, mr.fieldAt0x14,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset -
                          mr.payloadHeaderBytes()),
                      mr.headerFieldA, mr.headerFieldB,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset));
        }
      }
      for (std::size_t j = 0; j < b.regionAArrayA.size(); ++j) {
        const auto& nr = b.regionAArrayA[j];
        std::printf("           regA-A[%3zu] %-8s off=0x%08x\n",
                    j, nr.name().c_str(), nr.fieldAt0x08);
      }
      for (std::size_t j = 0; j < b.regionAArrayB.size(); ++j) {
        const auto& nr = b.regionAArrayB[j];
        std::printf("           regA-B[%3zu] %-8s off=0x%08x "
                    "(overlay-alien lookup target)\n",
                    j, nr.name().c_str(), nr.fieldAt0x08);
      }
      for (std::size_t j = 0; j < b.regionAArrayC.size(); ++j) {
        const auto& sr = b.regionAArrayC[j];
        std::printf("           regA-C[%3zu] {0x%08x 0x%08x 0x%08x "
                    "0x%04x 0x%04x off=0x%08x 0x%08x} "
                    "(overlay-sound record)\n",
                    j, sr.fieldAt0x00, sr.fieldAt0x04, sr.fieldAt0x08,
                    sr.fieldAt0x0C, sr.fieldAt0x0E, sr.fieldAt0x10,
                    sr.fieldAt0x14);
      }
      for (std::size_t j = 0; j < b.regionCNames.size(); ++j) {
        std::printf("           regC-name[%3zu] %s\n",
                    j, b.regionCNames[j].name().c_str());
      }
    }
    return 0;
  }

  const auto dir = mdk::inspectSniDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  std::printf("entries:   SNI directory (count u32 @0x14, records "
              "24 bytes @0x18)\n");
  std::printf("status:    %s%s%s\n",
              std::string(mdk::sniDirectoryStatusName(dir.status)).c_str(),
              dir.detail.empty() ? "" : " — ",
              dir.detail.empty() ? "" : dir.detail.c_str());
  if (dir.status != mdk::SniDirectoryStatus::kOk) {
    return 1;
  }
  std::printf("count:     %u\n", dir.count);
  std::printf("dir-end:   0x%llx\n",
              static_cast<unsigned long long>(dir.directoryEnd));
  std::printf("trailer:   name[12] @ size-12 %s\n",
              dir.trailerPresent ? "present" : "ABSENT");
  std::printf("u32@0x10:  %u — %s trailer offset\n",
              dir.secondaryLength,
              dir.secondaryEqualsTrailerOffset ? "equals" : "does not equal");

  for (std::size_t i = 0; i < dir.entries.size(); ++i) {
    const auto& e = dir.entries[i];
    if (e.isSentinel()) {
      std::printf("  [%3zu] %-12s SENTINEL (field0x0c=size=0xffffffff) "
                  "marker fileOff=0x%08llx\n",
                  i, e.name().c_str(),
                  static_cast<unsigned long long>(e.payloadFileOffset()));
    } else {
      std::printf("  [%3zu] %-12s field0x0c=0x%08x blobOff=0x%08x "
                  "fileOff=0x%08llx size=%u\n",
                  i, e.name().c_str(), e.fieldAt0x0C, e.blobOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset()),
                  e.payloadSize);
    }
  }
  return 0;
}
