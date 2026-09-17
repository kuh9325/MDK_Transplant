// .FTI interior directory parser (Phase 3H) — metadata only.
//
// EVIDENCE (bytes: all 5 .FTI files in BUILD_A, 1575 records; statics:
// Ghidra disassembly/decompilation of the original loader and lookup —
// see docs/DATA_FORMATS.md):
//
//   The file is the LENGTH-ONLY envelope: u32 @0 == fileSize - 4, no
//   12-byte logical-name field and no name trailer (bytes [4,8) are a
//   small u32 count — the container classifier reports
//   kLengthEnvelope). The original loads the whole image (file bytes
//   [4, size)) via the shared whole-blob loader FUN_00425b34 with
//   destination slot DAT_0049ff50 ("MISC\MDKFONT.FTI" default,
//   "MISC\FONT%c.FTI" / "MISC\FONTG.FTI" variants — disasm of
//   FUN_00401abc: MOV EDX,0x49ff50; CALL FUN_00425b34).
//
//   Interior (CODE-CORROBORATED by the resource lookup FUN_00414890 —
//   41 static callers engine-wide):
//
//     img+0x00 u32   record count N    (iVar3 = *DAT_0049ff50)
//     img+0x04       N x 12-byte records (piVar4 = DAT_0049ff50 + 1;
//                    stride 12: piVar4 += 3 u32s per step):
//       +0x00 name[8]  — compared as an EXACT two-u32 equality
//                      (local_24 == rec[0] && local_20 == rec[1];
//                      the query is zero-padded into a 12-byte stack
//                      buffer). A record name may use all 8 bytes —
//                      OBSERVED 81/315 records per file carry no NUL;
//                      bytes after a NUL still participate in the
//                      original compare.
//       +0x08 u32      image-relative offset — the original returns
//                      image + value (DAT_0049ff50 + piVar4[2]).
//     payload region [dirEnd, size): OBSERVED 5/5 files — stored
//                      offsets are unique and sorted ascending in
//                      record order; the smallest resolves exactly to
//                      the directory end; each payload spans to the
//                      next stored offset, the last to EOF. No stored
//                      sizes — payload end is inferred from the next
//                      offset (whether trailing slack exists inside a
//                      payload is UNKNOWN per-payload).
//     not-found path: "Error finding %s" fatal (FUN_00408fac).
//
//   Original vocabulary: DAT_0049ff50's subsystem emits "Font table
//   not initialized!" when the image is absent (FUN_0047da70 path),
//   and the resident records include FONTSML/FONTBIG — but payloads
//   are heterogeneous (RIFF/WAVE, palette-like tables, image and
//   animation structures). "FTI" expansion and any per-payload type
//   remain UNKNOWN; keep neutral record/payload naming.
//
// Scope rule: this parser enumerates the directory and per-record
// payload bounds as metadata ONLY. It validates count, record stride
// and (hardening) stored offsets against file bounds, preserves raw
// names/offsets, and never interprets payload data.
//
#ifndef MDK_CORE_FTI_DIRECTORY_H
#define MDK_CORE_FTI_DIRECTORY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// CODE-CORROBORATED layout constants (file offsets).
inline constexpr std::uint64_t kFtiImageBaseOffset = 0x04;   // img = file+4
inline constexpr std::uint64_t kFtiCountOffset = 0x04;       // u32 @file 0x04
inline constexpr std::uint64_t kFtiRecordBase = 0x08;        // records @file 0x08
inline constexpr std::uint64_t kFtiRecordStride = 0x0c;      // 12 bytes
inline constexpr std::size_t kFtiNameFieldSize = 8;

// One directory record: {name[8], u32 imageOffset}.
struct FtiRecord {
  std::uint64_t recordFileOffset = 0;                  // rec position
  std::array<std::byte, kFtiNameFieldSize> nameField{}; // rec +0x00
  std::uint32_t imageOffset = 0;                       // rec +0x08 (raw)
  std::uint64_t payloadFileOffset = 0;                 // 4 + imageOffset
  std::uint64_t payloadEnd = 0;                        // next distinct
                                                      // stored offset
                                                      // (+4) or EOF
  bool nameHasTerminator = false;  // a NUL exists within nameField
                                   // (not required by the original's
                                   // exact two-u32 compare)

  // Printable run up to the first NUL; non-printable bytes become
  // '\xNN'. For display only — nameField is the lossless form and the
  // original compares all 8 bytes.
  std::string name() const;
  std::uint64_t payloadSize() const { return payloadEnd - payloadFileOffset; }
};

enum class FtiDirectoryStatus {
  kOk,
  kNotLengthEnvelope,    // u32 @0 != size-4, or bytes[4,8) form a
                         // plausible tagged-name field (wrong family)
  kTruncatedHeader,      // file too small for the count at 0x04
  kDirectoryOutOfBounds, // count * stride escapes the file
  kOffsetOutOfBounds,    // a stored offset escapes the payload region
                         // (native hardening; the original dereferences
                         // it unconditionally)
};

struct FtiDirectory {
  FtiDirectoryStatus status = FtiDirectoryStatus::kNotLengthEnvelope;
  std::string detail;
  std::size_t badRecordIndex = static_cast<std::size_t>(-1);

  std::uint32_t count = 0;
  std::uint64_t directoryEnd = 0;   // file offset past the last record
                                    // == payload region start
  std::vector<FtiRecord> records;

  // Corpus-wide OBSERVED properties (reported, not load-bearing):
  bool offsetsSortedAscending = false;  // record order is ascending
  bool offsetsUnique = false;           // no two records share an offset
  bool firstPayloadAtDirectoryEnd = false;  // min offset == dirEnd
};

// Parse an .FTI directory from the whole file. `file` must be the
// complete file bytes (payload data is never dereferenced — only the
// count, record fields and bounds are read). Never fails; status
// distinguishes "not this format" from "malformed".
FtiDirectory inspectFtiDirectory(std::span<const std::byte> file);

std::string_view ftiDirectoryStatusName(FtiDirectoryStatus s);

} // namespace mdk

#endif // MDK_CORE_FTI_DIRECTORY_H
