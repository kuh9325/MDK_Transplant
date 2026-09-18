#include "core/frontend_settings.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mdk {

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

std::string_view trim(std::string_view v) {
  while (!v.empty() && isSpace(v.front())) v.remove_prefix(1);
  while (!v.empty() && isSpace(v.back())) v.remove_suffix(1);
  return v;
}

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

// The type-1 value read (OBSERVED): atof-family leading float
// parse. `out` is left untouched on failure (caller counts the
// line as ignored — NATIVE hardening; the original's atof yields
// 0.0 on unparseable input, an edge deliberately not reproduced).
bool parseLeadingFloat(std::string_view text, float* out) {
  const std::string v = std::string(trim(text));
  if (v.empty()) return false;
  const char* begin = v.c_str();
  char* end = nullptr;
  const float f = std::strtof(begin, &end);
  if (end == begin) return false;
  *out = f;
  return true;
}

// Writer-side emit helpers — FUN_004260ac's `%g` float format
// (OBSERVED; MSVC %g and C11 %g agree for the proven values) and
// the fold-compare the writer uses for type-3 strings (OBSERVED:
// "abg" compares EQUAL to the "ABG" mirror and is not emitted).
void emitFloat(std::string& out, const char* key, float v) {
  char line[64];
  std::snprintf(line, sizeof(line), "%s = %g\r\n", key,
                static_cast<double>(v));
  out += line;
}
void emitInt(std::string& out, const char* key, std::uint32_t bits) {
  char line[64];
  std::snprintf(line, sizeof(line), "%s = %d\r\n", key,
                static_cast<std::int32_t>(bits));
  out += line;
}
void emitString(std::string& out, const char* key,
                const std::string& v) {
  out += key;
  out += " = ";
  out += v;
  out += "\r\n";
}

} // namespace

std::string serializeFrontendSettings(const FrontendSettings& s) {
  // FUN_004260ac (OBSERVED): header + blank line, then one
  // `name = value` line per NON-default table value, in table
  // order — SoundFX (entry 8, int), SoundMusic (entry 9, int),
  // the entries-49-68 Mouse block, Skill (entry 88, int),
  // Brightness (entry 89, int), ForcePCorrect (entry 90, type-2
  // bool `%s = TRUE`; the mirror compare means a non-default bool
  // is always TRUE).
  // CRLF terminators — the original's "wt" mode text file.
  std::string out;
  out += kFrontendSettingsHeader;
  out += "\r\n\r\n";
  char line[40];
  if (s.soundFx != kFrontendSoundFxDefault) {
    std::snprintf(line, sizeof(line), "SoundFX = %d\r\n", s.soundFx);
    out += line;
  }
  if (s.soundMusic != kFrontendSoundMusicDefault) {
    std::snprintf(line, sizeof(line), "SoundMusic = %d\r\n",
                  s.soundMusic);
    out += line;
  }
  // Entries 49-52: type-3 strings — emit iff the fold compare
  // against the mirror string differs (OBSERVED at 0x4261ab).
  if (!keyEquals(s.mouseWAxesMap, kFrontendMouseAxesMapDefault)) {
    emitString(out, "MouseWAxesMap", s.mouseWAxesMap);
  }
  if (!keyEquals(s.mouseDAxesMap, kFrontendMouseAxesMapDefault)) {
    emitString(out, "MouseDAxesMap", s.mouseDAxesMap);
  }
  if (!keyEquals(s.mouseWButtMap, kFrontendMouseButtMapDefault)) {
    emitString(out, "MouseWButtMap", s.mouseWButtMap);
  }
  if (!keyEquals(s.mouseDButtMap, kFrontendMouseButtMapDefault)) {
    emitString(out, "MouseDButtMap", s.mouseDButtMap);
  }
  // Entries 53-60: type-0 dword slots — emit iff bits differ.
  const std::uint32_t wButt[4] = {s.mouseWButtMapA, s.mouseWButtMapB,
                                  s.mouseWButtMapC, s.mouseWButtMapD};
  const std::uint32_t dButt[4] = {s.mouseDButtMapA, s.mouseDButtMapB,
                                  s.mouseDButtMapC, s.mouseDButtMapD};
  const char* const wButtKey[4] = {"MouseWButtMapA", "MouseWButtMapB",
                                   "MouseWButtMapC", "MouseWButtMapD"};
  const char* const dButtKey[4] = {"MouseDButtMapA", "MouseDButtMapB",
                                   "MouseDButtMapC", "MouseDButtMapD"};
  for (int i = 0; i < 4; ++i) {
    if (wButt[i] != kFrontendMouseButtDefaults[i]) {
      emitInt(out, wButtKey[i], wButt[i]);
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (dButt[i] != kFrontendMouseButtDefaults[i]) {
      emitInt(out, dButtKey[i], dButt[i]);
    }
  }
  // Entries 61-66: type-1 floats — emit iff the float compare
  // against the mirror differs (OBSERVED at 0x42620d).
  if (s.mouseWXScale != kFrontendMouseXYScaleDefault)
    emitFloat(out, "MouseWXScale", s.mouseWXScale);
  if (s.mouseWYScale != kFrontendMouseXYScaleDefault)
    emitFloat(out, "MouseWYScale", s.mouseWYScale);
  if (s.mouseWZScale != kFrontendMouseZScaleDefault)
    emitFloat(out, "MouseWZScale", s.mouseWZScale);
  if (s.mouseDXScale != kFrontendMouseXYScaleDefault)
    emitFloat(out, "MouseDXScale", s.mouseDXScale);
  if (s.mouseDYScale != kFrontendMouseXYScaleDefault)
    emitFloat(out, "MouseDYScale", s.mouseDYScale);
  if (s.mouseDZScale != kFrontendMouseZScaleDefault)
    emitFloat(out, "MouseDZScale", s.mouseDZScale);
  // Entry 67: type-2 bool — mirror TRUE, so only FALSE emits.
  if (!s.mouseOn) {
    out += "MouseOn = FALSE\r\n";
  }
  // Entry 68: type-1 float slot written raw int32 by the screen —
  // emit iff the float compare differs from 0.0f (OBSERVED quirk:
  // a toggled-on value emits `1.4013e-45`, the denormal whose bits
  // are 1; the original never emits `MouseYReversed = 1`).
  if (std::bit_cast<float>(s.mouseYReversed) != 0.0f) {
    emitFloat(out, "MouseYReversed",
              std::bit_cast<float>(s.mouseYReversed));
  }
  // Entries 69-87: the keyboard block — type-0 dword slots in
  // ORIGINAL internal key codes, emit iff != the 0x49b1f2 mirror
  // (BUILD_A emits none — all bindings sit at factory).
  const struct {
    const char* key;
    int value;
    int factory;
  } keyEntries[19] = {
      {"KeyLeft", s.keyLeft, 105},       {"KeyRight", s.keyRight, 106},
      {"KeyUp", s.keyUp, 103},           {"KeyDown", s.keyDown, 108},
      {"KeyJump", s.keyJump, 56},        {"KeySide", s.keySide, 45},
      {"KeyFire", s.keyFire, 29},        {"KeySniper", s.keySniper, 57},
      {"KeyTurbo", s.keyTurbo, 42},      {"KeySturbo", s.keySturbo, 58},
      {"KeyLookUp", s.keyLookUp, 30},    {"KeyLookDown", s.keyLookDown, 44},
      {"KeyZoomIn", s.keyZoomIn, 30},    {"KeyZoomOut", s.keyZoomOut, 44},
      {"KeyItemNext", s.keyItemNext, 27},{"KeyItemPrev", s.keyItemPrev, 26},
      {"KeyItemUse", s.keyItemUse, 28},  {"KeySideL", s.keySideL, 51},
      {"KeySideR", s.keySideR, 52}};
  for (const auto& e : keyEntries) {
    if (e.value != e.factory) {
      emitInt(out, e.key, static_cast<std::uint32_t>(e.value));
    }
  }
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
    if (keyEquals(key, "SoundFX")) {
      int v = 0;
      if (!parseLeadingInt(value, &v) ||
          v < kFrontendSoundVolumeMin || v > kFrontendSoundVolumeMax) {
        ++r.ignoredSoundFxLines;   // NATIVE hardening — see header
        continue;
      }
      r.settings.soundFx = v;
    } else if (keyEquals(key, "SoundMusic")) {
      int v = 0;
      if (!parseLeadingInt(value, &v) ||
          v < kFrontendSoundVolumeMin || v > kFrontendSoundVolumeMax) {
        ++r.ignoredSoundMusicLines;   // NATIVE hardening — see header
        continue;
      }
      r.settings.soundMusic = v;
    } else if (keyEquals(key, "Skill")) {
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
    } else if (keyEquals(key, "MouseWAxesMap")) {
      // Type-3 (OBSERVED): string copy of the value — NATIVE
      // hardening bounds it via std::string (the original's strcpy
      // into char[4] is unbounded; UNKNOWN edge, not reproduced).
      r.settings.mouseWAxesMap = std::string(trim(value));
    } else if (keyEquals(key, "MouseDAxesMap")) {
      r.settings.mouseDAxesMap = std::string(trim(value));
    } else if (keyEquals(key, "MouseWButtMap")) {
      r.settings.mouseWButtMap = std::string(trim(value));
    } else if (keyEquals(key, "MouseDButtMap")) {
      r.settings.mouseDButtMap = std::string(trim(value));
    } else if (keyEquals(key, "MouseWButtMapA") ||
               keyEquals(key, "MouseWButtMapB") ||
               keyEquals(key, "MouseWButtMapC") ||
               keyEquals(key, "MouseWButtMapD") ||
               keyEquals(key, "MouseDButtMapA") ||
               keyEquals(key, "MouseDButtMapB") ||
               keyEquals(key, "MouseDButtMapC") ||
               keyEquals(key, "MouseDButtMapD")) {
      // Type-0 (OBSERVED): leading int stored as raw dword bits —
      // BUILD_A writes `MouseWButtMapD = 32768` (bit 15). NATIVE
      // hardening: a failed parse is ignored and counted.
      int v = 0;
      if (!parseLeadingInt(value, &v)) {
        ++r.ignoredMouseLines;
        continue;
      }
      std::uint32_t* slot = nullptr;
      if (keyEquals(key, "MouseWButtMapA")) slot = &r.settings.mouseWButtMapA;
      else if (keyEquals(key, "MouseWButtMapB")) slot = &r.settings.mouseWButtMapB;
      else if (keyEquals(key, "MouseWButtMapC")) slot = &r.settings.mouseWButtMapC;
      else if (keyEquals(key, "MouseWButtMapD")) slot = &r.settings.mouseWButtMapD;
      else if (keyEquals(key, "MouseDButtMapA")) slot = &r.settings.mouseDButtMapA;
      else if (keyEquals(key, "MouseDButtMapB")) slot = &r.settings.mouseDButtMapB;
      else if (keyEquals(key, "MouseDButtMapC")) slot = &r.settings.mouseDButtMapC;
      else slot = &r.settings.mouseDButtMapD;
      *slot = static_cast<std::uint32_t>(v);
    } else if (keyEquals(key, "MouseWXScale") ||
               keyEquals(key, "MouseWYScale") ||
               keyEquals(key, "MouseWZScale") ||
               keyEquals(key, "MouseDXScale") ||
               keyEquals(key, "MouseDYScale") ||
               keyEquals(key, "MouseDZScale")) {
      // Type-1 (OBSERVED): atof-family leading float parse. NATIVE
      // hardening: a failed parse is ignored and counted (the
      // original's atof yields 0.0 — UNKNOWN edge, not reproduced).
      float v = 0.0f;
      if (!parseLeadingFloat(value, &v)) {
        ++r.ignoredMouseLines;
        continue;
      }
      if (keyEquals(key, "MouseWXScale")) r.settings.mouseWXScale = v;
      else if (keyEquals(key, "MouseWYScale")) r.settings.mouseWYScale = v;
      else if (keyEquals(key, "MouseWZScale")) r.settings.mouseWZScale = v;
      else if (keyEquals(key, "MouseDXScale")) r.settings.mouseDXScale = v;
      else if (keyEquals(key, "MouseDYScale")) r.settings.mouseDYScale = v;
      else r.settings.mouseDZScale = v;
    } else if (keyEquals(key, "MouseOn")) {
      // Type-2 (OBSERVED): toupper(first value char) == 'T',
      // unconditional — same contract as ForcePCorrect.
      std::size_t j = 0;
      while (j < value.size() && isSpace(value[j])) ++j;
      r.settings.mouseOn =
          j < value.size() &&
          (value[j] == 'T' || value[j] == 't');
    } else if (keyEquals(key, "MouseYReversed")) {
      // Type-1 float slot (OBSERVED): atof into the float slot —
      // BOTH file forms round-trip: `= 1` (hand edit) -> float
      // 1.0f -> bits 0x3f800000 (nonzero = ON); `= 1.4013e-45`
      // (writer's denormal for raw bits 1) -> bits 1 (ON). NATIVE
      // hardening: a failed parse is ignored and counted.
      float v = 0.0f;
      if (!parseLeadingFloat(value, &v)) {
        ++r.ignoredMouseLines;
        continue;
      }
      r.settings.mouseYReversed = std::bit_cast<std::uint32_t>(v);
    } else if (keyEquals(key, "KeyLeft") || keyEquals(key, "KeyRight") ||
               keyEquals(key, "KeyUp") || keyEquals(key, "KeyDown") ||
               keyEquals(key, "KeyJump") || keyEquals(key, "KeySide") ||
               keyEquals(key, "KeyFire") || keyEquals(key, "KeySniper") ||
               keyEquals(key, "KeyTurbo") || keyEquals(key, "KeySturbo") ||
               keyEquals(key, "KeyLookUp") || keyEquals(key, "KeyLookDown") ||
               keyEquals(key, "KeyZoomIn") || keyEquals(key, "KeyZoomOut") ||
               keyEquals(key, "KeyItemNext") || keyEquals(key, "KeyItemPrev") ||
               keyEquals(key, "KeyItemUse") || keyEquals(key, "KeySideL") ||
               keyEquals(key, "KeySideR")) {
      // Type-0 (OBSERVED): leading int stored as the ORIGINAL
      // internal key code 0..127 — NOT translated to/from SDL
      // scancodes (the domain is preserved end to end). NATIVE
      // hardening: failed parse or out-of-domain values are ignored
      // and counted (the original stores whatever strtol yields —
      // that edge is UNKNOWN and deliberately not reproduced).
      int v = 0;
      if (!parseLeadingInt(value, &v) || v < 0 ||
          v > kFrontendKeyCodeMax) {
        ++r.ignoredKeyLines;
        continue;
      }
      int* slot = nullptr;
      if (keyEquals(key, "KeyLeft")) slot = &r.settings.keyLeft;
      else if (keyEquals(key, "KeyRight")) slot = &r.settings.keyRight;
      else if (keyEquals(key, "KeyUp")) slot = &r.settings.keyUp;
      else if (keyEquals(key, "KeyDown")) slot = &r.settings.keyDown;
      else if (keyEquals(key, "KeyJump")) slot = &r.settings.keyJump;
      else if (keyEquals(key, "KeySide")) slot = &r.settings.keySide;
      else if (keyEquals(key, "KeyFire")) slot = &r.settings.keyFire;
      else if (keyEquals(key, "KeySniper")) slot = &r.settings.keySniper;
      else if (keyEquals(key, "KeyTurbo")) slot = &r.settings.keyTurbo;
      else if (keyEquals(key, "KeySturbo")) slot = &r.settings.keySturbo;
      else if (keyEquals(key, "KeyLookUp")) slot = &r.settings.keyLookUp;
      else if (keyEquals(key, "KeyLookDown")) slot = &r.settings.keyLookDown;
      else if (keyEquals(key, "KeyZoomIn")) slot = &r.settings.keyZoomIn;
      else if (keyEquals(key, "KeyZoomOut")) slot = &r.settings.keyZoomOut;
      else if (keyEquals(key, "KeyItemNext")) slot = &r.settings.keyItemNext;
      else if (keyEquals(key, "KeyItemPrev")) slot = &r.settings.keyItemPrev;
      else if (keyEquals(key, "KeyItemUse")) slot = &r.settings.keyItemUse;
      else if (keyEquals(key, "KeySideL")) slot = &r.settings.keySideL;
      else slot = &r.settings.keySideR;
      *slot = v;
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
