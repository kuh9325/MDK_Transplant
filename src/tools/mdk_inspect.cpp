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
  // parser exists (SNI only in Phase 3C). Never prints payload bytes.
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
