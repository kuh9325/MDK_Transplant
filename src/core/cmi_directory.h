// .CMI interior directory parser (Phase 3F) — metadata only.
//
// EVIDENCE (bytes: all 6 .CMI files in BUILD_A, 884 records; statics:
// Ghidra disassembly of the original loader, table walkers, lookups
// and load/save relocators — see docs/DATA_FORMATS.md):
//
//   The file is the common tagged envelope (logical name "*.CMD").
//   The interior is a chain of FOUR counted variable-length tables
//   followed by a data region that ends at the name trailer:
//
//     u32le @0x14      table[0] count   (CODE-CORROBORATED: the loaded
//                                      image is file bytes [4, size);
//                                      FUN_0045840c reads this u32 at
//                                      image+0x10 then walks records
//                                      from image+0x14)
//     table[t]         records begin at count+4; each record is
//                        +0x00  u8     nameLength
//                        +0x01  byte[nameLength]  name bytes — OBSERVED
//                               884/884 to end with a NUL inside the
//                               counted length (the length INCLUDES
//                               the terminator); the original compares
//                               the name as a C string via FUN_0042fa50
//                        +1+len u32    value — image-relative offset
//                               (image base = file offset 4, so the
//                               target file offset is 4 + value;
//                               CODE-CORROBORATED: FUN_0045849c /
//                               FUN_00458550 / FUN_004286c8 all compute
//                               blob+value and dereference it; value 0
//                               is the null form for the table-1
//                               consumer — TEST EDX,EDX in
//                               FUN_004286c8)
//                      record stride = nameLength + 5 (the walk adds
//                      exactly that in FUN_0045840c/43c/46c/49c/58550)
//     table[t] end == table[t+1] count field; the four table headers
//     are located by three nested walkers (FUN_0045840c ends table 0,
//     FUN_0045843c ends table 1, FUN_0045846c ends table 2 — each
//     returns the next count field).
//     data region      [table[3] end, trailer): OBSERVED target of
//                      every nonzero record value in the corpus (884
//                      records: 762 nonzero values, all land here).
//                      Structures reachable from table[3] begin with
//                      two length-prefixed NUL-terminated strings
//                      followed by a further image-relative u32
//                      (CODE-CORROBORATED reads in FUN_0045849c /
//                      FUN_00458550; e.g. {len,str} {len,str} {u32}).
//                      Interior organization is otherwise UNKNOWN —
//                      bounded only.
//     bytes[size-12..) trailer: 12-byte repeat of the name field
//                      (shared tagged-envelope observation)
//
//   Table roles where the original code proves them:
//     table[1] feeds the destination array the original's own
//       diagnostic calls the "enemy table" ("Overflowed enemy table",
//       FUN_004286c8 error path; 0x88-stride destination, cap 0x50).
//     table[2] names are looked up against a sprintf-built name during
//       object init (FUN_004566f0); the target pointer is consumed by
//       FUN_004388d8 (tr_alcmd.c-region code).
//     table[3] is searched by name (FUN_0045849c / FUN_00458550) once
//       per arena record (FUN_00433d40 loop) and from FUN_00431fbc /
//       FUN_0043394c; values point at the two-strings structures above.
//     table[0] values OBSERVED to land in the data region; its
//       accessor FUN_004583fc has no static caller in this build —
//       consumer path UNKNOWN.
//   "CMI"/"CMD" itself is NOT yet proven to mean any semantic name;
//   keep neutral table/record naming.
//
// Scope rule: this parser enumerates the four proven tables and the
// data-region bounds as metadata ONLY. It validates counts, record
// strides and (hardening) nonzero value targets against file bounds,
// preserves raw names/values, and never interprets data-region
// payloads.
//
#ifndef MDK_CORE_CMI_DIRECTORY_H
#define MDK_CORE_CMI_DIRECTORY_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// CODE-CORROBORATED layout constants (file offsets).
inline constexpr std::uint64_t kCmiImageBaseOffset = 0x04;  // img = file+4
inline constexpr std::uint64_t kCmiInteriorOffset = 0x14;   // table[0] count
inline constexpr std::uint64_t kCmiRecordMinStride = 5;     // u8 len + u32
inline constexpr std::size_t kCmiTableCount = 4;  // walker chain depth

// One variable-length table record: {u8 nameLength, name bytes, u32
// value}. `nameBytes` preserves the counted bytes losslessly (the
// terminator, when present, is included — OBSERVED convention).
struct CmiRecord {
  std::uint64_t fileOffset = 0;         // record start (file offset)
  std::uint8_t nameLength = 0;          // raw u8 at +0x00
  std::vector<std::byte> nameBytes;     // `nameLength` bytes at +0x01
  std::uint32_t value = 0;              // u32 at +0x01+nameLength
  bool nameEndsWithTerminator = false;  // last name byte == NUL
  bool valueReachesDataRegion = false;  // nonzero value lands in the
                                        // data region (OBSERVED corpus
                                        // property; reported, not
                                        // load-bearing)

  // Printable run up to the first NUL; non-printable bytes become
  // '\xNN'. For display only — nameBytes is the lossless form.
  std::string name() const;
  // File offset the value points at under the proven image-relative
  // convention (image base = file+4): value==0 -> null (the table-1
  // consumer's tested null form), else 4 + value.
  std::optional<std::uint64_t> valueFileOffset() const;
};

// One counted variable-length table.
struct CmiTable {
  std::uint64_t countFileOffset = 0;    // file offset of the u32 count
  std::uint32_t count = 0;
  std::uint64_t recordsFileOffset = 0;  // count + 4
  std::uint64_t endFileOffset = 0;      // past last record (== next
                                        // count, or data-region start
                                        // for table[3])
  std::vector<CmiRecord> records;
};

enum class CmiDirectoryStatus {
  kOk,
  kNotTaggedEnvelope,   // top-level envelope missing/mismatched
  kTruncatedHeader,     // file too small for the first count at 0x14
  kTableOutOfBounds,    // a table's count field or its records escape
                        // [0x14, trailerStart)
  kValueOutOfBounds,    // a nonzero record value's image-relative
                        // target lands outside the file (native
                        // hardening; the original dereferences it
                        // unconditionally)
};

struct CmiDirectory {
  CmiDirectoryStatus status = CmiDirectoryStatus::kNotTaggedEnvelope;
  std::string detail;
  std::size_t badTableIndex = static_cast<std::size_t>(-1);
  std::size_t badRecordIndex = static_cast<std::size_t>(-1);

  std::vector<CmiTable> tables;  // exactly kCmiTableCount on success

  // Data region: [dataRegionOffset, dataRegionEnd). Bounded only —
  // interior structure is not enumerated by this parser.
  std::uint64_t dataRegionOffset = 0;
  std::uint64_t dataRegionEnd = 0;

  // Envelope-adjacent observations (reported, not load-bearing):
  std::uint32_t secondaryLength = 0;
  bool secondaryEqualsTrailerOffset = false;
  bool trailerPresent = false;
};

// Parse a .CMI interior from the whole file. `file` must be the
// complete file bytes (data-region payloads are never dereferenced —
// only counts, record fields and bounds are read). Never fails;
// status distinguishes "not this format" from "malformed".
CmiDirectory inspectCmiDirectory(std::span<const std::byte> file);

std::string_view cmiDirectoryStatusName(CmiDirectoryStatus s);

} // namespace mdk

#endif // MDK_CORE_CMI_DIRECTORY_H
