// .DTI interior structure parser (Phase 3G) — metadata only.
//
// EVIDENCE (bytes: all 6 .DTI files in BUILD_A — TRAVERSE/LEVEL3..8;
// statics: Ghidra disassembly of the original traversal loader and
// consumers — see docs/DATA_FORMATS.md):
//
//   The file is the common tagged envelope (logical name "*.DAT").
//   The interior begins at file offset 0x14 with a five-entry table
//   of contents; every TOC entry is an image-relative u32 offset
//   (image base = file offset 4 — the original loader reads the u32
//   length at 0x00 then loads the remaining bytes into memory, so
//   image offset N is file offset N+4; FUN_00425c8c). Target file
//   offset of a stored value v is therefore 4 + v.
//
//   Section map (all CODE-CORROBORATED by direct dereferences
//   img + img[0x10..0x20] in the traversal loader FUN_00433d40 and
//   img + img[0x14] in the s1 lookup FUN_00423bf0):
//
//     img+0x10 / file 0x14   u32 toc[5]  — offsets of sections 0..4;
//                            strictly increasing, tiled back-to-back;
//                            section 4 runs to the name trailer.
//
//     s0  [toc0, toc1)      parameter block; OBSERVED size 0x74
//                           (29 u32s) in all 6 files. The loader reads
//                           all 29 words in FUN_00433d40:
//                           [0]    -> initial arena index (multiplied
//                                   by the 0x466 runtime record stride
//                                   into DAT_00540c48)
//                           [1..4] -> DAT_00540bfc/c00/c04/c2c — the
//                                   view state FUN_0043490c also writes
//                                   from s1 records (position-like
//                                   floats in the corpus)
//                           [5],[6]-> fill bytes, replicated x4
//                           [7],[8]-> backdrop sample base offsets
//                           [9]    -> grid columns-4 (row stride is
//                                   +4; the value itself is the wrap
//                                   modulus in FUN_0046ec60)
//                           [10]   -> grid rows
//                           [0xb]  -> secondary fill byte A; when
//                                   positive (signed) the file carries
//                                   a second s4 plane
//                           [0xc]  -> secondary fill byte B
//                           [13..28] -> four u32 groups transposed
//                                   into a 16-byte matrix consumed by
//                                   FUN_00406d84 x4 (exact role
//                                   UNKNOWN)
//
//     s1  [toc1, toc2)      {u32 count, count x 24-byte records}
//                           record = {u32 word0, u32 key, f32 f[4]};
//                           stride 24 is CODE-CORROBORATED
//                           (FUN_00423bf0 advances piVar2 += 6 ints,
//                           matches word[1] == key argument, then calls
//                           FUN_0043490c(words[3], words[4], words[5],
//                           words[6]) — word[6] is the FIRST u32 of the
//                           next record: the original reads one field
//                           past each record (and past the last record
//                           into s2's count). OBSERVED quirk, kept.
//                           Corpus: count == 10 in all 6 files; key
//                           semantics UNKNOWN (consumers are debug
//                           command handlers in FUN_00423ca0 whose
//                           command strings are obfuscated).
//
//     s2  [toc2, toc3)      {u32 count, count x 16-byte records}
//                           record = {char name[8], u32 imageOff,
//                           f32 scalar}; the loader expands each into
//                           a 0x466-stride runtime record and stores
//                           the CMI table-3 lookup result at +0x220
//                           (FUN_00458550, once per record — the proven
//                           CMI<->DTI link). Records are the objects
//                           the original diagnostics call "arena"
//                           ("arena %s not found", FUN_00432ec4;
//                           "BSPShow %s not found", FUN_00432e2c;
//                           "Alien has NOT changed arenas"). A name
//                           starting with 'c'/'C' skips a flag write at
//                           load (OBSERVED quirk; the corridor arenas
//                           are the C-prefixed ones — "Cooridors" is
//                           the original spelling in diagnostics).
//                           imageOff points at the record's payload:
//                           {u32 count, count x 36-byte sub-records}.
//                           Payloads tile the remainder of s2
//                           contiguously in the corpus and end exactly
//                           at toc[3].
//
//     s2 sub-record         36 bytes = 9 u32 fields; field[0] is the
//                           type dispatch (CODE-CORROBORATED):
//                             type 2: "HotGen" — bytes [0x18,0x24)
//                                     hold a name matched against the
//                                     CMI enemy table at load
//                                     ("HotGen %s not found"); the
//                                     match index is OR-ed into the
//                                     high half of field[1]
//                             type 4: "HotPick" — same name field /
//                                     enemy table; index overwrites
//                                     field[1] ("HotPick %s not found")
//                             type 6: "connect" — field[1] is a
//                                     connect-ID (>999 in file form),
//                                     field[2] a side code (0..7,
//                                     paired 0<->1 2<->3 4<->5 6<->7),
//                                     fields[3..8] six floats compared
//                                     verbatim between the two
//                                     endpoints; FUN_00434e54 pairs
//                                     endpoints across arenas and
//                                     rewrites field[1] to the
//                                     partner's arena index
//                                     ("connect_%d", "Mismatched
//                                     connect coords/type",
//                                     "Unmatched connect")
//                           Types 1, 3, 5, 7, 8, 9 are OBSERVED in the
//                           corpus (histogram in docs); their field
//                           semantics are UNKNOWN — the parser keeps
//                           them as raw u32 fields.
//
//     s3  [toc3, toc4)      {u32 count, u8 rgb[768]} — OBSERVED span
//                           772 bytes in all 6 files; count ->
//                           DAT_00540dcc; the first three payload bytes
//                           are zeroed at load (first RGB triplet);
//                           FUN_004346e8 copies 0x90 u32s from +0xc0;
//                           FUN_0046d490 expands count*3 bytes into
//                           4-byte entries of the runtime palette
//                           array DAT_0054d7b8 ("init_palette" exists
//                           in the binary). CORROBORATED as the
//                           256-entry RGB table.
//
//     s4  [toc4, trailer)   byte grid: planeSize =
//                           (s0[9]+4) * s0[10] bytes per plane, one
//                           plane plus a second when s0[0xb] > 0
//                           (FUN_0047a770 samples the second plane at
//                           s4 + planeSize when _DAT_0054ec98 is set;
//                           plane count x planeSize equals the section
//                           span in all 6 files). Sampled column-wise
//                           into the framebuffer with horizontal wrap
//                           (FUN_0046ec60 / FUN_0047a770) — CORROBORATED
//                           usage as the scrolling indexed-color
//                           backdrop; byte values are palette indices.
//
//     bytes[size-12..)      trailer: 12-byte repeat of the name field
//                           (shared tagged-envelope observation)
//
//   "DTI"/"DAT" itself is NOT proven to mean any semantic name;
//   keep neutral section naming. Where original strings supply
//   vocabulary ("arena", "connect", "HotGen", "HotPick", palette
//   usage) that vocabulary is scoped to the exact records/functions
//   the evidence covers — nothing more.
//
// Scope rule: this parser enumerates the five proven sections and
// their record boundaries as metadata ONLY. It validates TOC order,
// section bounds, counts, strides and payload targets, preserves raw
// fields, and never interprets payload bytes beyond the proven
// record/field boundaries.
//
#ifndef MDK_CORE_DTI_STRUCTURE_H
#define MDK_CORE_DTI_STRUCTURE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// CODE-CORROBORATED layout constants (file offsets).
inline constexpr std::uint64_t kDtiImageBaseOffset = 0x04;  // img = file+4
inline constexpr std::uint64_t kDtiTocFileOffset = 0x14;    // img 0x10
inline constexpr std::size_t kDtiTocEntries = 5;
inline constexpr std::size_t kDtiParamWords = 29;     // s0: 29 u32s
inline constexpr std::size_t kDtiParamBlockSize = 0x74;  // 29 * 4
inline constexpr std::size_t kDtiKeyedRecordStride = 24;  // s1 records
inline constexpr std::size_t kDtiArenaRecordStride = 16;  // s2 records
inline constexpr std::size_t kDtiSubRecordStride = 36;    // s2 payloads
inline constexpr std::size_t kDtiPaletteBytes = 768;      // s3 payload

// Section indices in the TOC (neutral names; roles per the comment
// above).
enum DtiSection : std::size_t {
  kDtiSecParams = 0,   // s0
  kDtiSecKeyed = 1,    // s1
  kDtiSecArenas = 2,   // s2
  kDtiSecPalette = 3,  // s3
  kDtiSecGrid = 4,     // s4
};

// File-offset span [start, end).
struct DtiSpan {
  std::uint64_t fileStart = 0;
  std::uint64_t fileEnd = 0;
  std::uint64_t size() const { return fileEnd - fileStart; }
};

// s1 record: {u32 word0, u32 key, f32 f[4]} — 24 bytes. Kept raw;
// `floatAt(i)` renders words[2+i] as the f32 the corpus contains.
struct DtiKeyedRecord {
  std::uint64_t fileOffset = 0;
  std::uint32_t word0 = 0;               // OBSERVED 1..count-1,0
  std::uint32_t key = 0;                 // word[1] — the match field
  std::array<std::uint32_t, 4> raw{};    // words[2..5] (f32 bit patterns)
  float floatAt(std::size_t i) const;    // raw[i] reinterpreted
};

// One 36-byte s2 payload sub-record: {u32 type, u32 fields[8]}.
// Fields are raw u32s; which are floats/names is type-dependent
// (see header comment for the proven cases).
struct DtiSubRecord {
  std::uint64_t fileOffset = 0;
  std::uint32_t type = 0;
  std::array<std::uint32_t, 8> fields{};
  // Bytes [0x18, 0x24) as a printable run — the HotGen/HotPick name
  // field for types 2/4 (all-ASCII in the corpus), raw for others.
  std::string name18() const;
  float fieldAsFloat(std::size_t i) const;  // fields[i] reinterpreted
};

// s2 record: {char name[8], u32 imageOff, f32 scalar} + its payload
// {u32 count, count x 36-byte sub-records}.
struct DtiArenaRecord {
  std::uint64_t fileOffset = 0;
  std::array<std::byte, 8> nameBytes{};   // raw name field
  bool nameEndsWithTerminator = false;    // NUL inside the 8 bytes
  std::uint32_t payloadImageOffset = 0;   // image-relative (4 + v)
  std::uint32_t scalarBits = 0;           // f32 -> runtime +0x462
  // Resolved payload location/contents (bounds-checked):
  std::uint64_t payloadFileOffset = 0;    // 4 + payloadImageOffset
  std::uint32_t subRecordCount = 0;
  std::vector<DtiSubRecord> subRecords;

  // Printable run up to the first NUL; non-printable -> '\xNN'.
  std::string name() const;
  float scalar() const;
};

enum class DtiStructureStatus {
  kOk,
  kNotTaggedEnvelope,   // top-level envelope missing/mismatched
  kTruncatedHeader,     // file too small for the five-entry TOC
  kSectionOutOfBounds,  // TOC entry out of file / not strictly
                        // increasing / section smaller than its
                        // proven minimum (s0 0x74, s3 4+768, s4
                        // planes)
  kRecordOutOfBounds,   // a count x stride array escapes its section
  kOffsetOutOfBounds,   // an s2 payload image-offset lands outside
                        // the payload region (native hardening; the
                        // original dereferences it unconditionally)
};

struct DtiStructure {
  DtiStructureStatus status = DtiStructureStatus::kNotTaggedEnvelope;
  std::string detail;
  std::size_t badSection = static_cast<std::size_t>(-1);
  std::size_t badRecord = static_cast<std::size_t>(-1);

  // TOC: image-relative offsets as stored, and the resolved file
  // spans per section (valid only when status == kOk).
  std::array<std::uint32_t, kDtiTocEntries> tocImageOffsets{};
  std::array<DtiSpan, kDtiTocEntries> sections;

  // s0 — the 29 proven-read words, preserved raw.
  std::array<std::uint32_t, kDtiParamWords> params{};

  // s1 — keyed records (proven stride 24; semantics UNKNOWN).
  std::vector<DtiKeyedRecord> keyedRecords;
  std::uint64_t s1TrailingBytes = 0;  // span - (4 + count*24); OBSERVED 0

  // s2 — arena records + their enumerated payloads.
  std::vector<DtiArenaRecord> arenas;
  std::uint64_t s2PayloadRegionStart = 0;  // file offset after the
                                           // 16-byte record array

  // s3 — palette table: count + the fixed 768-byte RGB region.
  std::uint32_t paletteCount = 0;
  DtiSpan paletteBytes{};                 // [start, start+768)
  std::uint64_t s3TrailingBytes = 0;

  // s4 — backdrop grid derived from s0: bytes per plane, plane count
  // (1 or 2; second plane present when params[0x0b] > 0 signed).
  std::uint64_t gridPlaneSize = 0;
  std::uint32_t gridPlaneCount = 0;
  std::uint64_t s4TrailingBytes = 0;      // span - planes*planeSize;
                                          // OBSERVED 0

  // Envelope-adjacent observations (reported, not load-bearing):
  std::uint32_t secondaryLength = 0;
  bool secondaryEqualsTrailerOffset = false;
  bool trailerPresent = false;
};

// Parse a .DTI interior from the whole file. `file` must be the
// complete file bytes (sub-record fields are reported raw — payloads
// are never interpreted). Never fails; status distinguishes "not
// this format" from "malformed".
DtiStructure inspectDtiStructure(std::span<const std::byte> file);

std::string_view dtiStructureStatusName(DtiStructureStatus s);

} // namespace mdk

#endif // MDK_CORE_DTI_STRUCTURE_H
