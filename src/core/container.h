// Common container-envelope parser (Phase 3B).
//
// ORIGINAL ENGINE OBSERVATION (BUILD_A byte-level survey, all files in
// the listed families — see docs/DATA_ACCESS.md):
//
//   Tag family (.MTO/.SNI/.MTI/.CMI/.DTI — 46/46 files):
//     u32le @0        == fileSize - 4   (declared length: covers every
//                                        byte after its own field —
//                                        tag + payload; excludes itself)
//     bytes [4,16)    12-byte logical-name field "<stem>.<ext>", e.g.
//                     "LEVEL7O.MAT" (.MTO), "LEVEL7O.SND" (.SNI),
//                     "LEVEL7.CMD" (.CMI), "LEVEL7.DAT" (.DTI),
//                     "MDKSOUND.SND". NUL-padded; NOT NUL-terminated
//                     when the name fills the field.
//                     bytes [4,8) are the tag the original compares
//                     case-insensitively against the requested file's
//                     stem (mdkfopen-path helper FUN_0042fae8).
//     u32le @16       == fileSize - 12  (OBSERVED in all 46; semantics
//                                        UNKNOWN — likely delimits an
//                                        inner data section; not yet
//                                        interpreted)
//
//   Length-only family (.FTI/.BNI — 11/11 files):
//     u32le @0 == fileSize - 4, but bytes [4,8) are a small non-ASCII
//     u32 (the record count — Phase 3H) followed by fixed-stride
//     {name, image-offset} records. A different
//     structure — it must NOT be parsed as the tag envelope.
//
//   .LBB (6/6): no envelope — does not satisfy u32@0 == size-4.
//
// Standard formats (.FLC/.MVE/.GIF/.FRC/…) are never routed here.
//
// Scope rule (per phase brief): the parser implements ONLY this
// top-level envelope. Interior/nested structure remains UNKNOWN and is
// deliberately not decoded.

#ifndef MDK_CORE_CONTAINER_H
#define MDK_CORE_CONTAINER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mdk {

// Size of the fixed logical-name field at offset 4 (OBSERVED).
inline constexpr std::size_t kContainerNameFieldSize = 12;
// Header bytes needed to fully classify: u32 + name field.
inline constexpr std::size_t kContainerHeaderSize = 16;

enum class ContainerShape {
  kNone,           // no u32 length envelope (or truncated header)
  kLengthEnvelope, // u32@0 == size-4; following field is not a plausible
                   // ASCII tag/name (e.g. .FTI/.BNI count field)
  kTaggedName,     // u32@0 == size-4 and bytes[4,16) form a plausible
                   // NUL-padded ASCII logical name
};

// Everything the top-level envelope evidence supports. Non-owning view
// semantics: `head` must reference the start of the file and contain at
// least min(totalSize, kContainerHeaderSize) bytes.
struct ContainerInfo {
  std::uint64_t totalSize = 0;
  bool hasDeclaredLength = false;    // totalSize >= 4
  std::uint32_t declaredLength = 0;  // raw u32 @0
  bool lengthValid = false;          // declaredLength == totalSize - 4
  ContainerShape shape = ContainerShape::kNone;

  bool hasTag = false;                    // totalSize >= 8
  std::array<std::byte, 4> tag{};         // raw bytes [4,8)
  bool hasNameField = false;              // head covers [4,16)
  std::array<std::byte, 12> nameField{};  // raw bytes [4,16)
  std::string logicalName;                // decoded when kTaggedName
};

// Classify the start of a file. `head` = first
// min(totalSize, kContainerHeaderSize) bytes of the file (or the whole
// file when smaller); `totalSize` = real file size. Never fails.
ContainerInfo inspectContainer(std::span<const std::byte> head,
                               std::uint64_t totalSize);

// True when `tag` is entirely printable ASCII (what distinguishes the
// tag family from .FTI/.BNI at bytes[4,8) in BUILD_A).
bool tagIsPrintable(std::span<const std::byte, 4> tag);

// Lossless tag rendering: printable chars verbatim, else \xNN.
// For logging/metadata only — never parsed back.
std::string tagToString(std::span<const std::byte, 4> tag);

// Original-style stem check (OBSERVED behavior, FUN_0042fae8): the
// name field's leading bytes must equal `stem` under ASCII case-fold,
// bounded by stem length, and the byte after the stem must be '.', NUL,
// or the field end. The extra terminator condition is a NATIVE
// DECISION to avoid pure-prefix false positives.
bool nameStemMatches(const ContainerInfo& info, std::string_view stem);

// Parser applicability by file extension (case-insensitive). Explicit
// classification — never guess semantics from extension alone at parse
// time; this only selects which evidence-backed interpretation applies.
enum class ParserFamily {
  kTagEnvelope,       // .MTO .SNI .MTI .CMI .DTI
  kLengthEnvelope,    // .FTI .BNI — u32 envelope only, different payload
  kOtherFormat,       // .LBB (raw, proprietary) + standard/external
                      // formats (.FLC .MVE .GIF .FRC) + text/binary
                      // non-containers (.SAV .CFG .INI .TXT .INF .EXE
                      // .DLL .COM .VXD .386 .ICO .PDF .WMV .DB .CONF …)
  kUnknown,           // extension not seen in BUILD_A / unclassified
};
ParserFamily parserFamilyForPath(std::string_view relPath);
std::string_view parserFamilyName(ParserFamily f);

} // namespace mdk

#endif // MDK_CORE_CONTAINER_H
