// .MTI interior directory parser (Phase 3D) — metadata only.
//
// EVIDENCE (bytes: all 13 .MTI files in BUILD_A; statics: Ghidra
// disassembly of the original loader FUN_0041a1e0 — see
// docs/DATA_FORMATS.md):
//
//   The file is the common tagged envelope. Its interior is:
//
//     u32le @0x14      entry count N   (CODE-CORROBORATED: the original
//                                      parser reads *(img+0x10) where
//                                      img is the content blob loaded
//                                      from file offset 4)
//     N x 24 bytes     records at file offset 0x18 (img+0x14;
//                                      CODE-CORROBORATED stride: the
//                                      original advances the source
//                                      pointer by 6 u32s per record):
//                        +0x00  name[8]  (CODE-CORROBORATED: copied as
//                                       2 u32s to in-memory +0x28;
//                                       lookups compare it as a C
//                                       string via FUN_0042fa50)
//                        +0x08  u32      class/flag word:
//                                       0xffffffff => INDEX record
//                                       else bits 0x00030000 select
//                                       the extended payload header;
//                                       low 16 bits preserved verbatim
//                        +0x0c  u32      INDEX records: the index value
//                                       (CODE-CORROBORATED — copied to
//                                       in-memory +0x08). PAYLOAD
//                                       records: raw field, copied to
//                                       in-memory +0x1c, semantics
//                                       UNKNOWN (observed 0 or
//                                       0x469c4000 — float bit pattern)
//                        +0x10  u32      INDEX records: ignored by the
//                                       original (observed 0). PAYLOAD
//                                       records: raw field, copied to
//                                       in-memory +0x20, semantics
//                                       UNKNOWN (observed only
//                                       0x40600000 / 0x40c00000 —
//                                       float bit patterns)
//                        +0x14  u32      INDEX records: ignored by the
//                                       original (observed 0). PAYLOAD
//                                       records: stored payload offset,
//                                       RELATIVE TO THE CONTENT BLOB
//                                       (file position = stored + 4;
//                                       CODE-CORROBORATED — original
//                                       dereferences img+stored)
//     payloads         OBSERVED to tile [dirEnd, size-12) contiguously
//                      in ascending +0x14 order among payload records;
//                      index records interleave freely. The original
//                      performs NO bounds checking — the bounds here
//                      are this implementation's own hardening.
//     bytes[size-12..) trailer: 12-byte repeat of the name field
//                      (shared tagged-envelope observation)
//
//   PAYLOAD HEADER (first bytes at file position 4+stored; the record
//   itself stores no byte count — the original reads dimensions out of
//   the payload to bound it):
//     flags & 0x00030000 == 0   ->  u16 @+0, u16 @+2;  data starts +4
//     flags & 0x00030000 != 0   ->  u16 @+0 (merged into the in-memory
//                                 flags high word), u16 @+4, u16 @+6;
//                                 data starts +8
//   The u16 fields are consumed by the original as the values it
//   derives shift counts/masks from; their semantics are UNKNOWN
//   beyond "two (or three) u16 header fields".
//
//   The original in-memory record is 0x34 bytes (name at +0x28,
//   CODE-CORROBORATED by the lookup loops' 0x34 stride); the on-disk
//   record is the 24-byte form described above.
//
// Scope rule: this parser enumerates records as metadata ONLY. It
// validates offsets against file bounds and reads the payload header
// u16s the original itself reads; it never interprets payload data.
//
// The original subsystem calls these "materials" (strings "matdef",
// "matlkup", "Texture %s not in material list", source path
// ...\mdksrc\share\loadmats.c) — CODE-CORROBORATED naming; the file
// format fields above are still named by offset.

#ifndef MDK_CORE_MTI_DIRECTORY_H
#define MDK_CORE_MTI_DIRECTORY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// CODE-CORROBORATED layout constants (file offsets).
inline constexpr std::uint64_t kMtiCountOffset = 0x14;
inline constexpr std::uint64_t kMtiRecordBase = 0x18;
inline constexpr std::uint64_t kMtiRecordStride = 0x18;  // 24 bytes
inline constexpr std::size_t kMtiNameFieldSize = 8;
// All stored payload offsets index the "content blob": file offset 4.
inline constexpr std::uint64_t kMtiContentBlobBase = 4;
// +0x08 discriminator: this exact value marks an index record
// (CODE-CORROBORATED — the original compares against 0xffffffff).
inline constexpr std::uint32_t kMtiIndexRecordFlag = 0xffffffffu;
// +0x08 bits selecting the extended payload header
// (CODE-CORROBORATED — the original tests 0x00010000 | 0x00020000).
inline constexpr std::uint32_t kMtiExtendedHeaderMask = 0x00030000u;

struct MtiEntry {
  std::array<std::byte, kMtiNameFieldSize> nameField{};  // record +0x00
  std::uint32_t fieldAt0x08 = 0;  // +0x08 — class/flag word (above)
  std::uint32_t fieldAt0x0C = 0;  // +0x0c — index / raw param
  std::uint32_t fieldAt0x10 = 0;  // +0x10 — raw param (payload recs)
  std::uint32_t fieldAt0x14 = 0;  // +0x14 — blob offset (payload recs)

  // Payload header (payload records only — zero/absent for index
  // records). Positions within the payload per the header variant.
  std::optional<std::uint16_t> headerCount;  // u16 @payload+0 (extended)
  std::uint16_t headerFieldA = 0;  // u16 @payload+0 / +4
  std::uint16_t headerFieldB = 0;  // u16 @payload+2 / +6
  std::uint64_t payloadDataFileOffset = 0;  // file offset past header

  // Printable run up to first NUL; non-printable bytes become '\xNN'.
  // Metadata display only — `nameField` remains the lossless source.
  std::string name() const;

  // CODE-CORROBORATED record class: +0x08 == 0xffffffff. The original
  // reads only +0x0c from such records; +0x10/+0x14 are ignored and
  // preserved raw here.
  bool isIndexRecord() const { return fieldAt0x08 == kMtiIndexRecordFlag; }

  // CODE-CORROBORATED: bits 0x00030000 of +0x08 select the 8-byte
  // (extended) payload header over the 4-byte one.
  bool hasExtendedHeader() const {
    return (fieldAt0x08 & kMtiExtendedHeaderMask) != 0;
  }
  std::uint64_t payloadHeaderBytes() const {
    return hasExtendedHeader() ? 8 : 4;
  }

  // File position of the payload: content blob base (4) + stored
  // offset. CODE-CORROBORATED via the original's img+stored
  // dereference. Meaningful for payload records only.
  std::uint64_t payloadFileOffset() const {
    return kMtiContentBlobBase + fieldAt0x14;
  }
};

enum class MtiDirectoryStatus {
  kOk,
  kNotTaggedEnvelope,    // top-level envelope missing/mismatched —
                         // this file is not a tagged-envelope family
  kTruncatedHeader,      // file too small to contain the count field
  kDirectoryOutOfBounds, // count * stride does not fit inside the file
  kEntryOutOfBounds,     // a payload record's offset/header escapes
                         // [dirEnd, trailerEnd]
};

struct MtiDirectory {
  MtiDirectoryStatus status = MtiDirectoryStatus::kNotTaggedEnvelope;
  std::string detail;               // failure context / which entry
  std::size_t badEntryIndex = static_cast<std::size_t>(-1);

  std::uint32_t count = 0;          // raw u32 @0x14
  std::uint64_t directoryEnd = 0;   // file offset past the last record
  std::vector<MtiEntry> entries;

  // Envelope-adjacent observations (reported, not load-bearing):
  std::uint32_t secondaryLength = 0;          // raw u32 @0x10
  bool secondaryEqualsTrailerOffset = false;  // == size - 12
  bool trailerPresent = false;                // bytes[size-12..size)
                                              // == name field
};

// Parse a .MTI directory from the whole file. `file` must be the
// complete file bytes (payload data past each header is never
// dereferenced — only offsets and the header u16s are read). Never
// fails; status distinguishes "not this format" from "malformed".
MtiDirectory inspectMtiDirectory(std::span<const std::byte> file);

std::string_view mtiDirectoryStatusName(MtiDirectoryStatus s);

} // namespace mdk

#endif // MDK_CORE_MTI_DIRECTORY_H
