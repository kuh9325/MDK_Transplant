#include "core/container.h"

#include "core/binary_reader.h"

#include <cstdio>

namespace mdk {

namespace {

bool isPrintableAscii(std::byte b) {
  const auto v = static_cast<unsigned char>(b);
  return v >= 0x20 && v <= 0x7e;
}

char foldAsciiChar(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Plausibility rule for the 12-byte logical-name field (NATIVE
// DECISION derived from the OBSERVED shape): a run of >=1 printable
// ASCII bytes, then only NUL padding. A full 12-byte field may have no
// NUL at all (OBSERVED: "MDKSOUND.SND").
bool plausibleNameField(const std::array<std::byte, 12>& f,
                        std::size_t* nameLen) {
  std::size_t n = 0;
  while (n < f.size() && f[n] != std::byte{0}) {
    if (!isPrintableAscii(f[n])) {
      return false;
    }
    ++n;
  }
  if (n == 0) {
    return false;
  }
  for (std::size_t i = n; i < f.size(); ++i) {
    if (f[i] != std::byte{0}) {
      return false;
    }
  }
  *nameLen = n;
  return true;
}

} // namespace

ContainerInfo inspectContainer(std::span<const std::byte> head,
                               std::uint64_t totalSize) {
  ContainerInfo info;
  info.totalSize = totalSize;

  BinaryReader r(head);
  if (const auto d = r.peekU32le(0)) {
    info.hasDeclaredLength = true;
    info.declaredLength = *d;
    info.lengthValid = totalSize >= 4 &&
                       static_cast<std::uint64_t>(*d) == totalSize - 4;
  }
  if (!info.lengthValid) {
    return info;
  }

  info.shape = ContainerShape::kLengthEnvelope;
  if (head.size() >= 8) {
    info.hasTag = true;
    for (std::size_t i = 0; i < 4; ++i) {
      info.tag[i] = head[4 + i];
    }
  }
  if (head.size() < kContainerHeaderSize || totalSize < kContainerHeaderSize) {
    return info;
  }
  info.hasNameField = true;
  for (std::size_t i = 0; i < info.nameField.size(); ++i) {
    info.nameField[i] = head[4 + i];
  }

  std::size_t nameLen = 0;
  if (plausibleNameField(info.nameField, &nameLen)) {
    info.shape = ContainerShape::kTaggedName;
    info.logicalName.reserve(nameLen);
    for (std::size_t i = 0; i < nameLen; ++i) {
      info.logicalName.push_back(static_cast<char>(info.nameField[i]));
    }
  }
  return info;
}

bool tagIsPrintable(std::span<const std::byte, 4> tag) {
  for (std::byte b : tag) {
    if (!isPrintableAscii(b)) {
      return false;
    }
  }
  return true;
}

std::string tagToString(std::span<const std::byte, 4> tag) {
  std::string out;
  char buf[5];
  for (std::byte b : tag) {
    if (isPrintableAscii(b)) {
      out.push_back(static_cast<char>(b));
    } else {
      std::snprintf(buf, sizeof(buf), "\\x%02x",
                    static_cast<unsigned char>(b));
      out += buf;
    }
  }
  return out;
}

bool nameStemMatches(const ContainerInfo& info, std::string_view stem) {
  if (!info.hasNameField || stem.empty() ||
      stem.size() > info.nameField.size()) {
    return false;
  }
  for (std::size_t i = 0; i < stem.size(); ++i) {
    const auto a = foldAsciiChar(static_cast<char>(info.nameField[i]));
    if (a != foldAsciiChar(stem[i])) {
      return false;
    }
  }
  if (stem.size() == info.nameField.size()) {
    return true;
  }
  const auto next = static_cast<char>(info.nameField[stem.size()]);
  return next == '.' || next == '\0';
}

ParserFamily parserFamilyForPath(std::string_view relPath) {
  const auto slash = relPath.find_last_of("/\\");
  const auto dot = relPath.find_last_of('.');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash)) {
    return ParserFamily::kUnknown;
  }
  std::string ext(relPath.substr(dot + 1));
  for (char& c : ext) {
    c = foldAsciiChar(c);
  }

  // OBSERVED in BUILD_A: tag-envelope families.
  if (ext == "mto" || ext == "sni" || ext == "mti" || ext == "cmi" ||
      ext == "dti") {
    return ParserFamily::kTagEnvelope;
  }
  // OBSERVED in BUILD_A: u32 length envelope, non-tag second field.
  if (ext == "fti" || ext == "bni") {
    return ParserFamily::kLengthEnvelope;
  }
  // OBSERVED in BUILD_A: not this container — raw proprietary (.LBB,
  // .SAV), standard external formats, executables, and text.
  if (ext == "lbb" || ext == "sav" || ext == "flc" || ext == "mve" ||
      ext == "gif" || ext == "frc" || ext == "cfg" || ext == "ini" ||
      ext == "txt" || ext == "inf" || ext == "conf" || ext == "exe" ||
      ext == "dll" || ext == "com" || ext == "vxd" || ext == "386" ||
      ext == "ico" || ext == "pdf" || ext == "wmv" || ext == "db" ||
      ext == "bat" || ext == "sys" || ext == "ovl") {
    return ParserFamily::kOtherFormat;
  }
  return ParserFamily::kUnknown;
}

std::string_view parserFamilyName(ParserFamily f) {
  switch (f) {
    case ParserFamily::kTagEnvelope: return "tag-envelope";
    case ParserFamily::kLengthEnvelope: return "length-envelope-only";
    case ParserFamily::kOtherFormat: return "other/non-container";
    case ParserFamily::kUnknown: return "unknown-extension";
  }
  return "?";
}

} // namespace mdk
