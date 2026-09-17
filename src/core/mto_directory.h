// .MTO interior directory parser (Phase 3E) — metadata only.
//
// EVIDENCE (bytes: all 6 .MTO files in BUILD_A, 60/60 blocks; statics:
// Ghidra disassembly of the original overlay loader and consumers —
// see docs/DATA_FORMATS.md):
//
//   The file is the common tagged envelope. Its interior is an
//   "overlay" directory — the original subsystem name is
//   CODE-CORROBORATED by strings "overlay" (alloc tag in
//   FUN_0041a910), "No overlay data for %s" (FUN_0041a9d8 lookup
//   failure), "Too many overlay sounds" / "Failed to resolve overlay
//   alien %s" (consumers), and the filename constructor
//   "%s\LEVEL%d\LEVEL%dO.MTO" (FUN_0041b7b4).
//
//     u32le @0x14      overlay count N   (CODE-CORROBORATED: the
//                                      original loader FUN_0041a84c
//                                      fseeks to 0x14 and freads this
//                                      u32; N==10 in all 6 BUILD_A
//                                      files)
//     N x 12 bytes     directory records at file offset 0x18
//                                      (CODE-CORROBORATED: allocation
//                                      element size 0x0c, fread of
//                                      count*12 bytes):
//                        +0x00  name[8]  (CODE-CORROBORATED: the lookup
//                                       FUN_0041a9d8 compares via
//                                       FUN_0042fa80 with bound 8)
//                        +0x08  u32      FILE-ABSOLUTE offset of the
//                                       overlay block (CODE-CORROBORATED:
//                                       FUN_0041a9d8 fseeks SEEK_SET
//                                       to this value, then freads a
//                                       u32 there)
//     bytes[size-12..) trailer: 12-byte repeat of the name field
//                      (shared tagged-envelope observation)
//
//   OVERLAY BLOCK (at the record's file offset `off`; spans
//   [off, off+blockLen); OBSERVED to tile contiguously with 4-byte
//   alignment, last block ending at the trailer):
//
//     off+0x00 u32     blockLen — byte count INCLUDING this u32. The
//                      original streams blockLen bytes starting at
//                      off+4 into a scratch buffer (FUN_0041a9d8 /
//                      FUN_0041aad0; chunks <= 0x8000), i.e. the last
//                      4 streamed bytes are slack into the align4 gap.
//     off+0x04 u32     offset field A — target = off+8+value
//                      (CODE-CORROBORATED pointer math buf+buf[0]+4
//                      in FUN_00432534/FUN_00403498/FUN_004387ec)
//     off+0x08 u32     offset field B — target = off+4+value
//                      (FUN_004321dc stores buf+buf[1])
//     off+0x0c u32     offset field C — target = off+4+value
//                      (FUN_004321dc feeds buf+buf[2] to FUN_00419ee0)
//     off+0x10         EMBEDDED TAGGED FILE "<entry>.MAT" — a complete
//                      envelope-format file whose interior is an MTI
//                      directory (CODE-CORROBORATED: FUN_00432534 calls
//                      the MTI parser FUN_0041a1e0 with img=buf+0x10,
//                      i.e. the embedded name field; the original then
//                      performs material lookups against it):
//       inner+0x00 u32 innerLen = innerSize-4     (== fieldAt0x10)
//       inner+0x04 name[12] "<entry>.MAT"         (img base)
//       inner+0x10 u32 secondary = innerSize-12  (== fieldAt0x20;
//                                               inner trailer offset)
//       inner+0x14 u32 count2                    (img+0x10 — the MTI
//                                               record count position)
//       inner+0x18 count2 x 24-byte MTI records  (img+0x14):
//                        +0x00 name[8]
//                        +0x08 u32 class/flag word (0xffffffff = index
//                             record; none observed in BUILD_A MTOs —
//                             all 483 records are payload records)
//                        +0x0c u32 raw
//                        +0x10 u32 raw
//                        +0x14 u32 img-relative payload offset
//                             (CODE-CORROBORATED: the MTI parser
//                             dereferences img+stored)
//       payloads         tile [dirEnd, innerSize-12) img-relative
//       inner+innerSize-12 name[12] trailer (== name, OBSERVED 60/60)
//
//     innerEnd+0x00 u32 regionA byte count (self-exclusive; OBSERVED:
//                      fieldAt0x04 == fieldAt0x10+0x10 in all 60 blocks
//                      puts the region-A structure at innerEnd+4):
//       tA+0x00 u32    countA → recA[countA] x 12B {name[8], u32 off}
//                             (name lookup, FUN_004387ec; off is
//                             relative to tA)
//       tA+0x04 u32    countB → recB[countB] x 12B {name[8], u32 off}
//                             ("overlay alien" lookup, FUN_00403498 →
//                             FUN_00428400 (setupob.c); off is
//                             tA-relative to a {u32 count, data} pair)
//       tA+0x08 u32    countC → recC[countC] x 24B
//                             ("overlay sounds" — original aborts when
//                             countC > 16; FUN_004287cc reads
//                             u16@+0x0c, u16@+0x0e, tA-relative u32
//                             @+0x10 → FUN_00402e2c sound path)
//       then           payload blobs referenced by the offsets
//       region A extent: [tA, align4(tA+sizeA)) — OBSERVED 60/60 to
//                      reach the field-B target
//     region B         [tB, tC): OBSERVED exactly 0x150 bytes in all 60
//                      blocks; FUN_004321dc copies DAT_00540dcc*3 bytes
//                      from buf+fieldAt0x08 (palette-triple shape —
//                      STRONG, size source is external state)
//     region C         [tC, off+blockLen): FUN_00419ee0 counted-array
//                      structure (CODE-CORROBORATED walk):
//                        u32 c1; rec10[c1] (name[10]-shaped fields);
//                        +2 pad iff c1 odd; u32 c2; rec44[c2];
//                        u32 c3; rec36[c3]; u32 c4; rec12[c4];
//                        u32; then extra data (rec44 +0x1c/+0x20 fields
//                        are offsets relative to that extra base —
//                        converted to pointers in place)
//
//   Record internals of region C and the payload blobs of region A are
//   NOT decoded here — bounds only.
//
// Scope rule: this parser enumerates the directory and the proven
// interior skeleton as metadata ONLY. It validates offsets/sizes
// against file bounds, reads the payload header u16s the original
// itself reads, and never interprets payload data.
//
#ifndef MDK_CORE_MTO_DIRECTORY_H
#define MDK_CORE_MTO_DIRECTORY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// CODE-CORROBORATED layout constants (file offsets).
inline constexpr std::uint64_t kMtoCountOffset = 0x14;
inline constexpr std::uint64_t kMtoRecordBase = 0x18;
inline constexpr std::uint64_t kMtoRecordStride = 0x0c;  // 12 bytes
inline constexpr std::size_t kMtoNameFieldSize = 8;

// Block interior (block base = the record's file-absolute offset).
inline constexpr std::uint64_t kMtoBlockHeaderSize = 0x10;  // len + 3 ofs
inline constexpr std::uint64_t kMtoInnerFileOffset = 0x10;  // inner start
// Inner ".MAT" file field positions (relative to inner base).
inline constexpr std::uint64_t kMtoInnerCountOffset = 0x14;
inline constexpr std::uint64_t kMtoInnerRecordBase = 0x18;
inline constexpr std::uint64_t kMtoInnerRecordStride = 0x18;  // 24 bytes
// Payload offsets inside the inner file are relative to the embedded
// name field (inner+4), i.e. block+0x14 — the MTI parser's img base.
inline constexpr std::uint64_t kMtoInnerBlobOffset = 4;
// Minimum embedded file: u32 + name[12] + u32 + u32 + trailer[12].
inline constexpr std::uint64_t kMtoInnerMinSize = 0x24;
// Region A interior strides (CODE-CORROBORATED walks).
inline constexpr std::uint64_t kMtoRegionABStride = 0x0c;  // arrays A & B
inline constexpr std::uint64_t kMtoRegionACStride = 0x18;  // array C (24)
// Region C counted-array strides (FUN_00419ee0).
inline constexpr std::uint64_t kMtoRegionCStride1 = 10;
inline constexpr std::uint64_t kMtoRegionCStride2 = 44;
inline constexpr std::uint64_t kMtoRegionCStride3 = 36;
inline constexpr std::uint64_t kMtoRegionCStride4 = 12;

// +0x08 discriminator shared with the MTI record form.
inline constexpr std::uint32_t kMtoIndexRecordFlag = 0xffffffffu;
inline constexpr std::uint32_t kMtoExtendedHeaderMask = 0x00030000u;

// Inner ".MAT" directory record — the MTI 24-byte record form
// (CODE-CORROBORATED: the embedded file is parsed by the original MTI
// parser with img = the embedded name field).
struct MtoInnerRecord {
  std::array<std::byte, kMtoNameFieldSize> nameField{};  // rec +0x00
  std::uint32_t fieldAt0x08 = 0;  // +0x08 — class/flag word
  std::uint32_t fieldAt0x0C = 0;  // +0x0c — index / raw param
  std::uint32_t fieldAt0x10 = 0;  // +0x10 — raw param (payload recs)
  std::uint32_t fieldAt0x14 = 0;  // +0x14 — img-relative payload offset

  std::optional<std::uint16_t> headerCount;   // ext header u16 @+0
  std::uint16_t headerFieldA = 0;             // u16 @+0 / @+4
  std::uint16_t headerFieldB = 0;             // u16 @+2 / @+6
  std::uint64_t payloadDataFileOffset = 0;    // file offset past header

  std::string name() const;
  bool isIndexRecord() const { return fieldAt0x08 == kMtoIndexRecordFlag; }
  bool hasExtendedHeader() const {
    return (fieldAt0x08 & kMtoExtendedHeaderMask) != 0;
  }
  std::uint64_t payloadHeaderBytes() const {
    return hasExtendedHeader() ? 8 : 4;
  }
};

// Region-A arrays A and B share the {name[8], u32 offset} record form.
// Offsets are relative to the region-A structure base (tA); array B
// offsets address a {u32 count, data} pair (CODE-CORROBORATED).
struct MtoNameOffsetRecord {
  std::array<std::byte, kMtoNameFieldSize> nameField{};
  std::uint32_t fieldAt0x08 = 0;
  std::string name() const;
};

// Region-A array C — "overlay sounds" (original bound: count <= 16).
// Record internals beyond the u16s/offset are UNKNOWN; preserved raw.
struct MtoSoundRecord {
  std::uint32_t fieldAt0x00 = 0;
  std::uint32_t fieldAt0x04 = 0;
  std::uint32_t fieldAt0x08 = 0;
  std::uint16_t fieldAt0x0C = 0;  // u16 read by the original
  std::uint16_t fieldAt0x0E = 0;  // u16 read by the original
  std::uint32_t fieldAt0x10 = 0;  // tA-relative offset (original deref)
  std::uint32_t fieldAt0x14 = 0;
};

// Region-C array-1 record: a 10-byte name-shaped field (stride proven
// by the original walk; content observed name-like, semantics UNKNOWN).
struct MtoRegionCName {
  std::array<std::byte, 10> nameField{};
  std::string name() const;
};

// One overlay block — the interior of a table-1 record's target.
struct MtoBlock {
  std::uint64_t fileOffset = 0;     // block base (table1 +0x08 value)
  std::uint32_t blockLength = 0;    // u32 @off — self-inclusive

  // Raw offset fields (bases in the header comment); reported raw.
  std::uint32_t fieldAt0x04 = 0;    // -> region A struct target
  std::uint32_t fieldAt0x08 = 0;    // -> region B target
  std::uint32_t fieldAt0x0C = 0;    // -> region C target

  // Embedded tagged ".MAT" file at off+0x10.
  std::uint32_t innerLength = 0;             // innerSize-4
  std::array<std::byte, 12> innerNameField{}; // "<entry>.MAT"
  std::uint32_t innerSecondaryLength = 0;    // innerSize-12 (observed)
  std::uint32_t innerCount = 0;              // MTI record count
  std::vector<MtoInnerRecord> innerRecords;
  std::uint64_t innerEndOffset = 0;          // file offset past inner
  bool innerTrailerPresent = false;          // trailer == name
  bool innerSecondaryEqualsTrailerOffset = false;
  std::string innerName() const;

  // Region A: {u32 size @innerEnd, struct @tA=innerEnd+4}.
  std::uint32_t regionASize = 0;
  std::uint64_t regionAOffset = 0;   // tA
  std::uint64_t regionAEndOffset = 0;  // align4(tA+sizeA)
  std::uint32_t regionACountA = 0;
  std::uint32_t regionACountB = 0;
  std::uint32_t regionACountC = 0;
  std::vector<MtoNameOffsetRecord> regionAArrayA;   // array A
  std::vector<MtoNameOffsetRecord> regionAArrayB;   // array B (aliens)
  std::vector<MtoSoundRecord> regionAArrayC;        // array C (sounds)

  // Region B: [tB, tC) — OBSERVED 0x150 bytes in BUILD_A.
  std::uint64_t regionBOffset = 0;
  std::uint64_t regionBSize = 0;

  // Region C: FUN_00419ee0 counted-array walk to the block end.
  std::uint64_t regionCOffset = 0;
  std::uint32_t regionCCount1 = 0;   // rec10 array
  std::uint32_t regionCCount2 = 0;   // rec44 array
  std::uint32_t regionCCount3 = 0;   // rec36 array
  std::uint32_t regionCCount4 = 0;   // rec12 array
  std::vector<MtoRegionCName> regionCNames;  // array-1 fields
  std::uint64_t regionCExtraOffset = 0;      // post-array4 base
};

// Table-1 directory record: {name[8], u32 blockFileOffset}.
struct MtoEntry {
  std::array<std::byte, kMtoNameFieldSize> nameField{};
  std::uint32_t blockFileOffset = 0;
  std::string name() const;
};

enum class MtoDirectoryStatus {
  kOk,
  kNotTaggedEnvelope,    // top-level envelope missing/mismatched
  kTruncatedHeader,      // file too small to contain the count field
  kDirectoryOutOfBounds, // count * stride does not fit inside the file
  kBlockOutOfBounds,     // a record offset or block length escapes
                        // [directoryEnd, trailerEnd]
  kInteriorMalformed,    // a proven interior invariant is violated
};

struct MtoDirectory {
  MtoDirectoryStatus status = MtoDirectoryStatus::kNotTaggedEnvelope;
  std::string detail;
  std::size_t badEntryIndex = static_cast<std::size_t>(-1);

  std::uint32_t count = 0;
  std::uint64_t directoryEnd = 0;   // file offset past table 1
  std::vector<MtoEntry> entries;
  std::vector<MtoBlock> blocks;     // interior per entry (same order)

  // Envelope-adjacent observations (reported, not load-bearing):
  std::uint32_t secondaryLength = 0;
  bool secondaryEqualsTrailerOffset = false;
  bool trailerPresent = false;
};

// Parse an .MTO overlay directory from the whole file. `file` must be
// the complete file bytes (payload data is never dereferenced — only
// offsets, counts and the payload header u16s are read). Never fails;
// status distinguishes "not this format" from "malformed".
MtoDirectory inspectMtoDirectory(std::span<const std::byte> file);

std::string_view mtoDirectoryStatusName(MtoDirectoryStatus s);

} // namespace mdk

#endif // MDK_CORE_MTO_DIRECTORY_H
