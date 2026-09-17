// .BNI interior directory parser (Phase 3H) — metadata only.
//
// EVIDENCE (bytes: all 6 .BNI files in BUILD_A, 178 records; statics:
// Ghidra disassembly/decompilation of the original loaders and lookup —
// see docs/DATA_FORMATS.md):
//
//   The file is the LENGTH-ONLY envelope: u32 @0 == fileSize - 4, no
//   12-byte logical-name field and no name trailer (bytes [4,8) are a
//   small u32 count — the container classifier reports
//   kLengthEnvelope). The original loads the whole image (file bytes
//   [4, size)) via the shared whole-blob loaders into a single global
//   slot DAT_004a1e38 — three wrappers each do
//   "MOV EDX,0x4a1e38; CALL loader": FUN_004038f0 (FUN_00425a80),
//   FUN_0040390c (FUN_00425b34), FUN_00403928 (FUN_00425bfc);
//   FUN_00403944 clears the slot. One BNI image is live at a time
//   (per-context bundles: FALL3D / TRAVSPRT / STREAM / OPTIONS /
//   STATS / FINISH).
//
//   Interior (CODE-CORROBORATED by the resource lookup FUN_00403958 —
//   instruction-level disasm):
//
//     img+0x00 u32   record count N    (MOV EBX,[img])
//     img+0x04       N x 16-byte records (ADD ECX,0x10 per step):
//       +0x00 name[12] — compared by the shared unbounded C-string
//                      comparator FUN_0042fa50, so a well-formed name
//                      must contain a NUL within the field (OBSERVED
//                      178/178 records: all NUL-terminated within 12
//                      bytes; longest name 9 chars — "BONESANIM").
//                      A non-terminated field would read into the
//                      offset word — reported as an anomaly.
//       +0x0c u32      image-relative offset — the original returns
//                      image + value (MOV EAX,[img]; ADD EAX,[rec+0xc]).
//     payload region [dirEnd, size): OBSERVED 6/6 files — stored
//                      offsets are unique and sorted ascending in
//                      record order; the smallest resolves exactly to
//                      the directory end; each payload spans to the
//                      next stored offset, the last to EOF. No stored
//                      sizes — payload end is inferred from the next
//                      offset.
//     not-found path: "Error finding %s" fatal (FUN_004039a4 ->
//                      FUN_00408fac). Lookup variants FUN_004039d8 /
//                      FUN_004039ec / FUN_00403a00 return payload+4 /
//                      read u16 fields at the payload head —
//                      consumers treat payload heads as structures;
//                      payload interiors are UNKNOWN.
//
//   "BNI" expansion is UNKNOWN; payloads are heterogeneous
//   (RIFF/WAVE, palette-like tables, image/animation structures) —
//   keep neutral record/payload naming.
//
// Scope rule: this parser enumerates the directory and per-record
// payload bounds as metadata ONLY. It validates count, record stride
// and (hardening) stored offsets against file bounds, preserves raw
// names/offsets, and never interprets payload data.
//
#ifndef MDK_CORE_BNI_DIRECTORY_H
#define MDK_CORE_BNI_DIRECTORY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// CODE-CORROBORATED layout constants (file offsets).
inline constexpr std::uint64_t kBniImageBaseOffset = 0x04;   // img = file+4
inline constexpr std::uint64_t kBniCountOffset = 0x04;       // u32 @file 0x04
inline constexpr std::uint64_t kBniRecordBase = 0x08;        // records @file 0x08
inline constexpr std::uint64_t kBniRecordStride = 0x10;      // 16 bytes
inline constexpr std::size_t kBniNameFieldSize = 12;

// One directory record: {name[12], u32 imageOffset}.
struct BniRecord {
  std::uint64_t recordFileOffset = 0;                  // rec position
  std::array<std::byte, kBniNameFieldSize> nameField{}; // rec +0x00
  std::uint32_t imageOffset = 0;                       // rec +0x0c (raw)
  std::uint64_t payloadFileOffset = 0;                 // 4 + imageOffset
  std::uint64_t payloadEnd = 0;                        // next distinct
                                                      // stored offset
                                                      // (+4) or EOF
  bool nameHasTerminator = false;  // a NUL exists within nameField —
                                   // required by the original's
                                   // unbounded compare

  // Printable run up to the first NUL; non-printable bytes become
  // '\xNN'. For display only — nameField is the lossless form.
  std::string name() const;
  std::uint64_t payloadSize() const { return payloadEnd - payloadFileOffset; }
};

enum class BniDirectoryStatus {
  kOk,
  kNotLengthEnvelope,    // u32 @0 != size-4, or bytes[4,8) form a
                         // plausible tagged-name field (wrong family)
  kTruncatedHeader,      // file too small for the count at 0x04
  kDirectoryOutOfBounds, // count * stride escapes the file
  kOffsetOutOfBounds,    // a stored offset escapes the payload region
                         // (native hardening; the original dereferences
                         // it unconditionally)
};

struct BniDirectory {
  BniDirectoryStatus status = BniDirectoryStatus::kNotLengthEnvelope;
  std::string detail;
  std::size_t badRecordIndex = static_cast<std::size_t>(-1);

  std::uint32_t count = 0;
  std::uint64_t directoryEnd = 0;   // file offset past the last record
                                    // == payload region start
  std::vector<BniRecord> records;

  // Corpus-wide OBSERVED properties (reported, not load-bearing):
  bool offsetsSortedAscending = false;  // record order is ascending
  bool offsetsUnique = false;           // no two records share an offset
  bool firstPayloadAtDirectoryEnd = false;  // min offset == dirEnd
};

// Parse a .BNI directory from the whole file. `file` must be the
// complete file bytes (payload data is never dereferenced — only the
// count, record fields and bounds are read). Never fails; status
// distinguishes "not this format" from "malformed".
BniDirectory inspectBniDirectory(std::span<const std::byte> file);

// Locate a record by name — the directory-level counterpart of the
// original lookup (FUN_00403958: record scan + name compare). The
// original's comparator is an unbounded case-sensitive C-string
// compare; this helper is ASCII case-INSENSITIVE (a native tooling
// convenience — all 178 observed names are uppercase anyway) and
// bounded: the stored name is the field up to its first NUL (all 12
// bytes if unterminated). Returns nullptr when not found.
const BniRecord* findBniRecord(const BniDirectory& dir,
                               std::string_view name);

std::string_view bniDirectoryStatusName(BniDirectoryStatus s);

} // namespace mdk

#endif // MDK_CORE_BNI_DIRECTORY_H
