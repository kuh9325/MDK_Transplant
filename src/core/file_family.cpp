#include "core/file_family.h"

#include <cctype>

namespace mdk {

namespace {

// Trailing extension, lowercased. Returns empty if the path has no
// '.' after the last path separator. Accepts '/' and '\'.
std::string_view extensionOf(std::string_view relPath) {
  const std::size_t sep = relPath.find_last_of("/\\");
  const std::size_t dot = relPath.find_last_of('.');
  if (dot == std::string_view::npos ||
      (sep != std::string_view::npos && dot < sep))
    return {};
  return relPath.substr(dot + 1);
}

bool extEquals(std::string_view ext, const char *want) {
  // want is always lowercase ASCII.
  std::size_t i = 0;
  for (; want[i] != '\0'; ++i) {
    if (i >= ext.size())
      return false;
    const unsigned char c = static_cast<unsigned char>(ext[i]);
    if (static_cast<char>(std::tolower(c)) != want[i])
      return false;
  }
  return i == ext.size();
}

} // namespace

MdkFileFamily fileFamilyForPath(std::string_view relPath) {
  const std::string_view ext = extensionOf(relPath);
  if (ext.empty())
    return MdkFileFamily::kUnknown;

  // Proprietary MDK data families (all OBSERVED in BUILD_A).
  if (extEquals(ext, "mto")) return MdkFileFamily::kMto;
  if (extEquals(ext, "sni")) return MdkFileFamily::kSni;
  if (extEquals(ext, "mti")) return MdkFileFamily::kMti;
  if (extEquals(ext, "cmi")) return MdkFileFamily::kCmi;
  if (extEquals(ext, "dti")) return MdkFileFamily::kDti;
  if (extEquals(ext, "fti")) return MdkFileFamily::kFti;
  if (extEquals(ext, "bni")) return MdkFileFamily::kBni;
  if (extEquals(ext, "lbb")) return MdkFileFamily::kLbb;
  if (extEquals(ext, "sav")) return MdkFileFamily::kSav;

  // Documented external formats shipped with the data set.
  if (extEquals(ext, "flc")) return MdkFileFamily::kFlic;
  if (extEquals(ext, "mve")) return MdkFileFamily::kMve;
  if (extEquals(ext, "gif")) return MdkFileFamily::kGif;
  if (extEquals(ext, "frc")) return MdkFileFamily::kFrc;

  // Other extensions observed in BUILD_A — executables, drivers,
  // text/config and misc support files. Recognized so the dispatch
  // stays explicit, but none are MDK data families. (This list matches
  // the Phase 3B parserFamilyForPath kOtherFormat set exactly.)
  static const char *const kOtherKnown[] = {
      "bat", "cfg", "com", "conf", "db",  "dll", "exe", "ico", "inf",
      "ini", "ovl", "pdf", "sys", "txt",  "vxd", "wmv", "386",
  };
  for (const char *want : kOtherKnown)
    if (extEquals(ext, want))
      return MdkFileFamily::kOtherKnown;

  return MdkFileFamily::kUnknown;
}

std::string_view fileFamilyName(MdkFileFamily f) {
  switch (f) {
  case MdkFileFamily::kMto:        return "MTO";
  case MdkFileFamily::kSni:        return "SNI";
  case MdkFileFamily::kMti:        return "MTI";
  case MdkFileFamily::kCmi:        return "CMI";
  case MdkFileFamily::kDti:        return "DTI";
  case MdkFileFamily::kFti:        return "FTI";
  case MdkFileFamily::kBni:        return "BNI";
  case MdkFileFamily::kLbb:        return "LBB";
  case MdkFileFamily::kSav:        return "SAV";
  case MdkFileFamily::kFlic:       return "FLIC";
  case MdkFileFamily::kMve:        return "MVE";
  case MdkFileFamily::kGif:        return "GIF";
  case MdkFileFamily::kFrc:        return "FRC";
  case MdkFileFamily::kOtherKnown: return "other-known";
  case MdkFileFamily::kUnknown:    return "unknown";
  }
  return "unknown";
}

FamilySupport fileFamilySupport(MdkFileFamily f) {
  switch (f) {
  case MdkFileFamily::kSni:
  case MdkFileFamily::kMti:
  case MdkFileFamily::kMto:
  case MdkFileFamily::kCmi:
    return FamilySupport::kDirectoryMetadata;
  case MdkFileFamily::kDti:
  case MdkFileFamily::kFti:
  case MdkFileFamily::kBni:
    return FamilySupport::kEnvelopeOnly;
  case MdkFileFamily::kFlic:
  case MdkFileFamily::kMve:
  case MdkFileFamily::kGif:
  case MdkFileFamily::kFrc:
    return FamilySupport::kStandardExternalFormat;
  case MdkFileFamily::kLbb:
  case MdkFileFamily::kSav:
  case MdkFileFamily::kOtherKnown:
  case MdkFileFamily::kUnknown:
    return FamilySupport::kUnsupported;
  }
  return FamilySupport::kUnsupported;
}

std::string_view familySupportName(FamilySupport s) {
  switch (s) {
  case FamilySupport::kUnsupported:
    return "unsupported";
  case FamilySupport::kEnvelopeOnly:
    return "envelope-only";
  case FamilySupport::kDirectoryMetadata:
    return "directory-metadata";
  case FamilySupport::kStandardExternalFormat:
    return "standard-external-format";
  }
  return "unsupported";
}

} // namespace mdk
