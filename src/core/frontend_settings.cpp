#include "core/frontend_settings.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mdk {

std::string serializeFrontendSettings(const FrontendSettings& s) {
  // FUN_004260ac (OBSERVED): header + blank line, then one
  // `name = value` line per NON-default table value, in table
  // order — Skill (entry 88, int), Brightness (entry 89, int),
  // ForcePCorrect (entry 90, type-2 bool `%s = TRUE`; the mirror
  // compare means a non-default bool is always TRUE).
  // CRLF terminators — the original's "wt" mode text file.
  std::string out;
  out += kFrontendSettingsHeader;
  out += "\r\n\r\n";
  char line[40];
  if (s.skill != kFrontendSkillDefault) {
    std::snprintf(line, sizeof(line), "Skill = %d\r\n", s.skill);
    out += line;
  }
  if (s.brightness != kFrontendBrightnessDefault) {
    std::snprintf(line, sizeof(line), "Brightness = %d\r\n",
                  s.brightness);
    out += line;
  }
  if (s.forcePCorrect) {
    out += "ForcePCorrect = TRUE\r\n";
  }
  return out;
}

namespace {

// FUN_0042fab4 (OBSERVED): ASCII fold via `and 0xdf` —
// case-insensitive compare of a parsed key against a table name.
bool keyEquals(std::string_view key, std::string_view name) {
  if (key.size() != name.size()) {
    return false;
  }
  for (std::size_t i = 0; i < key.size(); ++i) {
    const auto fold = [](char c) {
      return static_cast<unsigned char>(c) & 0xdf;
    };
    if (fold(key[i]) != fold(name[i])) {
      return false;
    }
  }
  return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// The original's value read: strtol-style leading integer parse
// (whitespace skipped, optional sign, digits; scanning stops at
// the first non-digit). Returns false when no digits are present.
bool parseLeadingInt(std::string_view text, int* out) {
  std::size_t i = 0;
  while (i < text.size() && isSpace(text[i])) ++i;
  bool neg = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    neg = text[i] == '-';
    ++i;
  }
  long v = 0;
  bool any = false;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    v = v * 10 + (text[i] - '0');
    if (v > 0x7fffffffL) v = 0x7fffffffL;  // saturate like strtol
    any = true;
    ++i;
  }
  if (!any) return false;
  *out = static_cast<int>(neg ? -v : v);
  return true;
}

} // namespace

FrontendSettingsParse parseFrontendSettings(std::string_view text) {
  FrontendSettingsParse r;   // factory defaults first (FUN_00425de4)
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t eol = text.find('\n', pos);
    const std::size_t end =
        eol == std::string_view::npos ? text.size() : eol;
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;

    // Original's line shape: optional leading ws, then a key run,
    // `=`, then a value run up to `;` or EOL, trailing ws trimmed.
    // Lines that don't reduce to `name = value` are skipped.
    std::size_t i = 0;
    while (i < line.size() && isSpace(line[i])) ++i;
    if (i >= line.size() || line[i] == ';') {
      continue;  // blank / comment
    }
    const std::size_t eq = line.find('=', i);
    if (eq == std::string_view::npos) {
      continue;
    }
    std::size_t kend = eq;
    while (kend > i && isSpace(line[kend - 1])) --kend;
    const std::string_view key = line.substr(i, kend - i);
    // Value: up to `;` or EOL (the original terminates there).
    std::string_view value = line.substr(eq + 1);
    const std::size_t semi = value.find(';');
    if (semi != std::string_view::npos) {
      value = value.substr(0, semi);
    }
    if (keyEquals(key, "Skill")) {
      int v = 0;
      if (!parseLeadingInt(value, &v) || v < kFrontendSkillMin ||
          v > kFrontendSkillMax) {
        ++r.ignoredSkillLines;   // NATIVE hardening — see header
        continue;
      }
      r.settings.skill = v;
    } else if (keyEquals(key, "Brightness")) {
      int v = 0;
      if (!parseLeadingInt(value, &v) ||
          v < kFrontendBrightnessMin || v > kFrontendBrightnessMax) {
        ++r.ignoredBrightnessLines;   // NATIVE hardening — see header
        continue;
      }
      r.settings.brightness = v;
    } else if (keyEquals(key, "ForcePCorrect")) {
      // Type-2 parse (OBSERVED): toupper(first non-space value char)
      // == 'T' -> 1 else 0 — applied unconditionally.
      std::size_t j = 0;
      while (j < value.size() && isSpace(value[j])) ++j;
      r.settings.forcePCorrect =
          j < value.size() &&
          (value[j] == 'T' || value[j] == 't');
    }
    // Unknown keys are skipped — the original's apply loop only
    // touches table entries it knows.
  }
  return r;
}

std::optional<FrontendSettingsParse> loadFrontendSettingsFile(
    const std::filesystem::path& path, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // Absent file is the fresh-boot case — factory defaults stand.
    std::error_code ec;
    if (err && std::filesystem::exists(path, ec)) {
      *err = "cannot open settings file: " + path.string();
    }
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) {
    if (err) {
      *err = "read failed: " + path.string();
    }
    return std::nullopt;
  }
  return parseFrontendSettings(ss.str());
}

bool saveFrontendSettingsFile(const std::filesystem::path& path,
                              const FrontendSettings& s,
                              std::string* err) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) {
      *err = "cannot open settings file for write: " + path.string();
    }
    return false;
  }
  out << serializeFrontendSettings(s);
  out.flush();
  if (!out) {
    if (err) {
      *err = "write failed: " + path.string();
    }
    return false;
  }
  return true;
}

} // namespace mdk
