// .SNI interior directory parser (Phase 3C) — metadata only.
//
// EVIDENCE (bytes: all 15 .SNI files in BUILD_A; statics: Ghidra
// disassembly of the original loaders — see docs/DATA_FORMATS.md):
//
//   The file is the common tagged envelope. Its interior is:
//
//     u32le @0x14      entry count N            (file offset; the
//                                              original loader seeks
//                                              +0x10 SEEK_CUR after the
//                                              u32@0, i.e. lands on 0x14)
//     N x 24 bytes     directory records, at file offset 0x18:
//                        +0x00  name[12]  NUL-padded, compare bounded
//                                       to 12 bytes (MOV EBX,0xc at the
//                                       original compare call site)
//                        +0x0c  u32       semantics UNKNOWN (small set
//                                       of flag-like values observed)
//                        +0x10  u32       stored payload offset,
//                                       RELATIVE TO THE CONTENT BLOB
//                                       (file position = stored + 4;
//                                       original seeks SEEK_SET to
//                                       stored+4 — FUN_00429014)
//                        +0x14  u32       payload byte count
//     payloads         [4+storedOff, +size) each; OBSERVED to tile
//                      [dirEnd, size-12) in directory order, starts
//                      4-byte aligned (gaps 0 or 2 bytes)
//     bytes[size-12..) trailer: 12-byte repeat of the name field
//                      (OBSERVED 46/46 tagged files; u32@0x10 ==
//                      size-12 == trailer file offset)
//
//   SENTINEL RECORDS (OBSERVED: 6/6 in BUILD_A are 'K_'-prefixed names
//   — K_SURF, K_SURFJ, K_SLIP, K_SLIDE, K_BSLIDE, K_FSLIDE — all in
//   *S.SNI level files, always the trailing entries): records whose
//   +0x0c and +0x14 fields are BOTH 0xffffffff carry a real in-bounds
//   offset at +0x10 but no byte count; the stored offset still points
//   at data past the last real payload. They are reported as position
//   markers: offset bounds-checked, size field never treated as a
//   byte count. Semantics UNKNOWN (the 'K_' correlation is an
//   observation, not a rule).
//
//   The original in-memory path (FUN_004259a8 blob loader) reads the
//   u32@0 then freads exactly `size-4` bytes; the image pointer is the
//   content blob at file offset 4 — consistent with blob-relative
//   stored offsets.
//
// Scope rule: this parser enumerates directory records as metadata
// ONLY. It validates offsets/sizes against file bounds and never
// interprets payload bytes (RIFF/WAVE contents are a later phase).

#ifndef MDK_CORE_SNI_DIRECTORY_H
#define MDK_CORE_SNI_DIRECTORY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// OBSERVED layout constants (file offsets).
inline constexpr std::uint64_t kSniCountOffset = 0x14;
inline constexpr std::uint64_t kSniRecordBase = 0x18;
inline constexpr std::uint64_t kSniRecordStride = 0x18; // 24 bytes
inline constexpr std::size_t kSniNameFieldSize = 12;
// All stored payload offsets index the "content blob": file offset 4.
inline constexpr std::uint64_t kSniContentBlobBase = 4;

struct SniEntry {
  std::array<std::byte, kSniNameFieldSize> nameField{}; // record +0x00
  std::uint32_t fieldAt0x0C = 0;   // record +0x0c — semantics UNKNOWN
  std::uint32_t blobOffset = 0;    // record +0x10 — blob-relative offset
  std::uint32_t payloadSize = 0;   // record +0x14 — byte count

  // Printable run up to first NUL; non-printable bytes become '\xNN'.
  // Metadata display only — `nameField` remains the lossless source.
  std::string name() const;

  // OBSERVED sentinel/marker record class: both +0x0c and +0x14 are
  // 0xffffffff. The stored offset is still a real position; the size
  // field carries no byte count.
  bool isSentinel() const {
    return fieldAt0x0C == 0xffffffffu && payloadSize == 0xffffffffu;
  }

  // File position of the payload (or marked position, for sentinels):
  // content blob base (4) + stored offset. OBSERVED via the
  // original's fseek(stored + 4, SEEK_SET).
  std::uint64_t payloadFileOffset() const {
    return kSniContentBlobBase + blobOffset;
  }
  std::uint64_t payloadFileEnd() const {
    return payloadFileOffset() + payloadSize;
  }
};

enum class SniDirectoryStatus {
  kOk,
  kNotTaggedEnvelope,   // top-level envelope missing/mismatched —
                        // this file is not a tagged-envelope family
  kTruncatedHeader,     // file too small to contain the count field
  kDirectoryOutOfBounds,// count * stride does not fit inside the file
  kEntryOutOfBounds,    // a payload range escapes [dirEnd, trailerEnd]
};

struct SniDirectory {
  SniDirectoryStatus status = SniDirectoryStatus::kNotTaggedEnvelope;
  std::string detail;              // failure context / which entry
  std::size_t badEntryIndex = static_cast<std::size_t>(-1);

  std::uint32_t count = 0;         // raw u32 @0x14
  std::uint64_t directoryEnd = 0;  // file offset past the last record
  std::vector<SniEntry> entries;

  // Envelope-adjacent observations (reported, not load-bearing):
  std::uint32_t secondaryLength = 0;             // raw u32 @0x10
  bool secondaryEqualsTrailerOffset = false;     // == size - 12
  bool trailerPresent = false;                   // bytes[size-12..size)
                                                 // == name field
};

// Parse a .SNI directory from the whole file. `file` must be the
// complete file bytes (payload contents are never dereferenced — only
// offsets/sizes are validated arithmetically). Never fails; status
// distinguishes "not this format" from "malformed".
SniDirectory inspectSniDirectory(std::span<const std::byte> file);

std::string_view sniDirectoryStatusName(SniDirectoryStatus s);

} // namespace mdk

#endif // MDK_CORE_SNI_DIRECTORY_H
