// File-family dispatch (Phase 3C).
//
// Explicit, extension-driven classification of files found under the
// data root. This layer answers two questions only:
//
//   1. which observed file family does a relative path belong to
//      (case-insensitive, extension-based — never guessed from bytes);
//   2. what level of parser support exists for that family today.
//
// Family names are deliberately conservative: they mirror the observed
// disk extension and assert NOTHING about interior payload semantics.
// (AGENTS.md rule: no semantic naming before the semantics are proven.)
//
// Evidence levels per docs/reverse-engineering/EVIDENCE_POLICY.md.

#ifndef MDK_CORE_FILE_FAMILY_H
#define MDK_CORE_FILE_FAMILY_H

#include <string_view>

namespace mdk {

// OBSERVED in BUILD_A (the user-supplied install tree). One enumerator
// per observed extension for the proprietary families; standard/external
// formats get their own entries where the format is externally
// documented (FLIC, MVE, GIF, RIFF-FORC). Everything else observed in
// BUILD_A (executables, drivers, text/config, misc binary) shares
// kOtherKnown — they are not MDK data families.
enum class MdkFileFamily {
  kMto,   // .MTO — tagged-name envelope; interior overlay directory PROVEN
  kSni,   // .SNI — tagged-name envelope; interior directory PROVEN
  kMti,   // .MTI — tagged-name envelope; interior directory PROVEN
          //      (name[8] + 4 fields, 24-byte stride — Phase 3D)
  kCmi,   // .CMI — tagged-name envelope; interior counted
          //      variable-length tables PROVEN (Phase 3F)
  kDti,   // .DTI — tagged-name envelope; interior five-section table
          //      of contents PROVEN (Phase 3G)
  kFti,   // .FTI — length envelope only (non-ASCII field at +4)
  kBni,   // .BNI — length envelope only (non-ASCII field at +4)
  kLbb,   // .LBB — no u32 envelope observed (raw structure)
  kSav,   // .SAV — proprietary save-game format (packet skeleton
          //      observed in loader strings; no envelope)
  kFlic,  // .FLC — Autodesk FLIC (externally documented format)
  kMve,   // .MVE — Interplay MVE (externally documented format)
  kGif,   // .GIF — GIF87a/89a (externally documented format)
  kFrc,   // .FRC — RIFF "FORC" DirectInput force-effect data
  kOtherKnown, // extension seen in BUILD_A but not an MDK data family
               // (.EXE/.DLL/.386/.VXD/.CFG/.INI/.TXT/.ICO/.PDF/...)
  kUnknown,    // extension not observed in BUILD_A
};

// How far the native tree can currently take a family.
enum class FamilySupport {
  kUnsupported,             // recognized but unhandled (or unknown ext)
  kEnvelopeOnly,            // top-level u32 envelope validated; interior
                            // structure not yet evidence-backed
  kDirectoryMetadata,       // an interior directory/table is proven and
                            // enumerable as bounds-checked metadata
  kStandardExternalFormat,  // documented external format — deliberately
                            // not routed through MDK container parsing
};

// Case-insensitive dispatch on the trailing extension of a relative
// path. Both '/' and '\' separators are accepted (matching DataRoot).
// Files without an extension, or with an extension never observed in
// BUILD_A, report kUnknown. Never inspects file contents.
MdkFileFamily fileFamilyForPath(std::string_view relPath);

// Short stable name for logging/tool output ("MTO", "SNI", ...).
std::string_view fileFamilyName(MdkFileFamily f);

// Current parser support for a family. kDirectoryMetadata implies a
// concrete parser exists (SNI Phase 3C; MTI Phase 3D; MTO Phase 3E;
// CMI Phase 3F; DTI Phase 3G).
FamilySupport fileFamilySupport(MdkFileFamily f);
std::string_view familySupportName(FamilySupport s);

} // namespace mdk

#endif // MDK_CORE_FILE_FAMILY_H
