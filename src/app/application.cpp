#include "app/application.h"

#include "app/diagnostic_scene.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/clock.h"
#include "core/compat.h"
#include "core/data_root.h"
#include "core/display_menu.h"
#include "core/mouse_menu.h"
#include "core/file_family.h"
#include "core/framebuffer.h"
#include "core/frontend_flow.h"
#include "core/frontend_menu.h"
#include "core/frontend_settings.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/indexed_image.h"
#include "core/log.h"
#include "core/mode_dispatch.h"
#include "core/options_menu.h"
#include "core/sni_directory.h"
#include "core/sound_menu.h"
#include "core/stream_context.h"
#include "input/input_state.h"
#include "platform/sdl_host.h"
#include "renderer/presenter.h"

#include <SDL3/SDL_mouse.h>    // SDL_BUTTON_* for the button nibble map
#include <SDL3/SDL_scancode.h> // SDL_SCANCODE_* key translation

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace mdk {

static constexpr const char* kTag = "app";

// Read cap for --preview-resource source files — far above the
// largest BNI bundle in BUILD_A (~2.5 MB) while staying a sane bound.
static constexpr std::size_t kPreviewMaxBytes = 512ull * 1024 * 1024;

// Load + decode the --preview-resource target: DataRoot -> BNI
// directory -> named record -> bitmap decoder. Paletted records use
// the embedded palette (Phase 4A); the one proven indexed-only
// context (STREAM/STREAM.BNI BG) resolves its external palette per
// the original binding (Phase 4B). Fills `err` and returns nullopt
// on any failure.
static std::optional<IndexedImage> loadPreviewImage(
    DataRoot& root, const std::string& relFile, const std::string& name,
    std::string* err) {
  if (fileFamilyForPath(relFile) != MdkFileFamily::kBni) {
    *err = "preview supports BNI resources only (Phase 4A/4B decoder "
           "coverage): " + relFile;
    return std::nullopt;
  }
  const auto file = root.readFile(relFile, kPreviewMaxBytes, err);
  if (!file) {
    return std::nullopt;
  }
  const auto dir = inspectBniDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  if (dir.status != BniDirectoryStatus::kOk) {
    *err = "BNI directory: " +
           std::string(bniDirectoryStatusName(dir.status)) + " — " +
           dir.detail;
    return std::nullopt;
  }
  const BniRecord* rec = findBniRecord(dir, name);
  if (!rec) {
    *err = "record not found: " + name;
    return std::nullopt;
  }
  const std::span<const std::byte> payload(
      file->data() + rec->payloadFileOffset, rec->payloadSize());
  const auto probe = probeBniImage(payload);
  if (probe.shape == BniImageShape::kIndexedOnly) {
    // Indexed-only records need the consumer's external palette.
    // The only binding proven so far is the STREAM backdrop
    // (FUN_0042b270): SYS_PAL head + PAL record tail.
    if (!isStreamBackdropRequest(relFile, rec->name())) {
      *err = "indexed-only record has no proven external-palette "
             "binding in this context (Phase 4B proves " +
             std::string(kStreamBniFile) + " " +
             std::string(kStreamImageRecord) + " only)";
      return std::nullopt;
    }
    const auto fti = root.readFile(std::string(kStreamSystemFile),
                                   kPreviewMaxBytes, err);
    if (!fti) {
      return std::nullopt;
    }
    auto img = decodeStreamBackdrop(
        std::span<const std::byte>(file->data(), file->size()),
        std::span<const std::byte>(fti->data(), fti->size()), err);
    if (img) {
      log::info(kTag, "preview: %s %s — %dx%d indexed, %llu pixel "
                "bytes, STREAM context palette (SYS_PAL[0:64] + "
                "PAL[64:256]), digest=%016llx",
                relFile.c_str(), rec->name().c_str(), img->width,
                img->height,
                static_cast<unsigned long long>(img->pixels.size()),
                static_cast<unsigned long long>(imageDigest(*img)));
    }
    return img;
  }
  auto img = decodeBniPalettedImage(payload, err);
  if (img) {
    log::info(kTag, "preview: %s %s — %dx%d indexed, %llu pixel bytes, "
              "256-entry embedded palette, digest=%016llx",
              relFile.c_str(), rec->name().c_str(), img->width,
              img->height,
              static_cast<unsigned long long>(img->pixels.size()),
              static_cast<unsigned long long>(imageDigest(*img)));
  }
  return img;
}

// Phase 4C font preview: resolve an FTI record, decode it with the
// proven FONTSML/FONTBIG glyph decoder, bind the resident SYS_PAL
// head palette from the same FTI file, and draw the glyphs into the
// indexed framebuffer. Without `text` every mapped glyph is drawn as
// a diagnostic atlas (evidence-neutral grid layout); with `text` the
// byte string is drawn once using the proven advance rule (glyph
// width; the record's proven missing-glyph advance for unmapped
// bytes). Fills `err` and returns false on any failure.
static bool loadFontPreview(DataRoot& root, const std::string& relFile,
                            const std::string& recName,
                            const std::optional<std::string>& text,
                            IndexedFramebuffer& fb, Palette& palette,
                            std::string* err) {
  if (fileFamilyForPath(relFile) != MdkFileFamily::kFti) {
    *err = "--preview-font supports .FTI font records only (Phase 4C "
           "decoder coverage): " + relFile;
    return false;
  }
  const auto file = root.readFile(relFile, kPreviewMaxBytes, err);
  if (!file) {
    return false;
  }
  const auto dir = inspectFtiDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  if (dir.status != FtiDirectoryStatus::kOk) {
    *err = "FTI directory: " +
           std::string(ftiDirectoryStatusName(dir.status)) + " — " +
           dir.detail;
    return false;
  }
  const FtiRecord* rec = findFtiRecord(dir, recName);
  if (!rec) {
    *err = "record not found: " + recName;
    return false;
  }
  const std::span<const std::byte> payload(
      file->data() + rec->payloadFileOffset, rec->payloadSize());
  std::string derr;
  const auto font = decodeFtiFont(payload, &derr);
  if (!font) {
    *err = "font decode: " + derr;
    return false;
  }

  // Palette binding (CORROBORATED): glyph bytes are final palette
  // indices and every index used by the real fonts is <= 62 — inside
  // the resident 64-entry system palette head SYS_PAL which the
  // original loads from this same FTI (FUN_0040163c). Entries 64-255
  // are never referenced by the glyphs; they stay zeroed.
  const FtiRecord* palRec = findFtiRecord(dir, "SYS_PAL");
  if (!palRec || palRec->payloadSize() < 192) {
    *err = "SYS_PAL record missing/short in " + relFile +
           " — no proven palette binding for this font";
    return false;
  }
  const std::byte* sp = file->data() + palRec->payloadFileOffset;
  for (int i = 0; i < 64; ++i) {
    palette.set(i, {static_cast<std::uint8_t>(sp[i * 3 + 0]),
                    static_cast<std::uint8_t>(sp[i * 3 + 1]),
                    static_cast<std::uint8_t>(sp[i * 3 + 2]), 255});
  }

  // Per-consumer missing-glyph advance (a constant in the original
  // draw code, not the payload): 4 for the FONTSML path
  // (FUN_00414dd4), 6 for FONTBIG (FUN_00414c34).
  int missingAdvance = 0;
  const std::string canonName = rec->name();  // stored (uppercase) name
  if (canonName == "FONTSML") {
    missingAdvance = kFtiFontSmlMissingAdvance;
  } else if (canonName == "FONTBIG") {
    missingAdvance = kFtiFontBigMissingAdvance;
  }

  fb.clear(0);  // index 0 = SYS_PAL[0] = black

  if (text) {
    if (missingAdvance == 0) {
      *err = "text preview needs a proven missing-glyph advance — only "
             "FONTSML (4) and FONTBIG (6) have one established";
      return false;
    }
    // Baseline placement: put the pen row below the tallest ascent in
    // the string (diagnostic placement — the original callers choose
    // the pen; only the per-glyph rule is proven).
    int maxTop = 0;
    for (const char ch : *text) {
      if (const FtiGlyph* g =
              font->glyphFor(static_cast<std::uint8_t>(ch))) {
        maxTop = std::max(maxTop, int(g->top));
      }
    }
    drawFtiText(*font, *text, fb, 8, maxTop + 8, missingAdvance);
    log::info(kTag,
              "font preview: %s %s — %zu mapped glyphs, text %zu bytes, "
              "digest=%016llx",
              relFile.c_str(), rec->name().c_str(), font->mappedCount,
              text->size(),
              static_cast<unsigned long long>(ftiFontDigest(*font)));
    return true;
  }

  // Atlas: every mapped glyph in table order, one diagnostic cell
  // each. Cell height covers the largest top+bottom extent so every
  // glyph draws fully inside its cell; padding is dropped if the
  // atlas would not fit the work buffer (diagnostic layout only).
  int maxW = 0, maxTop = 0, maxBot = 0;
  for (const auto& g : font->glyphs) {
    if (g) {
      maxW = std::max(maxW, int(g->width));
      maxTop = std::max(maxTop, int(g->top));
      maxBot = std::max(maxBot, int(g->bottom));
    }
  }
  int pad = 2;
  for (;;) {
    const int cellW = maxW + pad;
    const int cellH = maxTop + maxBot + 1 + pad;
    const int cols = std::max(1, fb.width() / cellW);
    const int rows =
        (int(font->mappedCount) + cols - 1) / cols;
    if (rows * cellH <= fb.height() || pad == 0) {
      int drawn = 0;
      for (const auto& g : font->glyphs) {
        if (!g) {
          continue;
        }
        const int col = drawn % cols, row = drawn / cols;
        drawFtiGlyph(*g, fb, col * cellW + pad / 2,
                     row * cellH + pad / 2 + maxTop);
        ++drawn;
      }
      if (rows * cellH > fb.height()) {
        log::warn(kTag, "font atlas exceeds %dpx height — lower rows "
                  "clipped", fb.height());
      }
      break;
    }
    pad -= 2;
  }
  log::info(kTag,
            "font preview: %s %s — %zu mapped glyphs (codes %d-%d), "
            "max pixel index %u, digest=%016llx",
            relFile.c_str(), rec->name().c_str(), font->mappedCount,
            font->firstMapped, font->lastMapped, font->maxPixelIndex,
            static_cast<unsigned long long>(ftiFontDigest(*font)));
  return true;
}

// Phase 4D sprite preview: resolve an FTI record, decode it with the
// proven sprite-table decoder (ARROW format), bind SYS_PAL from the
// same file when present (arrow pixels are palette indices), and draw
// the sprite over a synthetic two-tone checkerboard so transparency is
// inspectable. Diagnostic only — placement is not original. Fills
// `err` and returns false on any failure.
static bool loadSpritePreview(DataRoot& root, const std::string& relFile,
                              const std::string& recName,
                              IndexedFramebuffer& fb, Palette& palette,
                              std::string* err) {
  if (fileFamilyForPath(relFile) != MdkFileFamily::kFti) {
    *err = "--preview-sprite supports .FTI sprite records only "
           "(Phase 4D decoder coverage): " + relFile;
    return false;
  }
  const auto file = root.readFile(relFile, kPreviewMaxBytes, err);
  if (!file) {
    return false;
  }
  const auto dir = inspectFtiDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  if (dir.status != FtiDirectoryStatus::kOk) {
    *err = "FTI directory: " +
           std::string(ftiDirectoryStatusName(dir.status)) + " — " +
           dir.detail;
    return false;
  }
  const FtiRecord* rec = findFtiRecord(dir, recName);
  if (!rec) {
    *err = "record not found: " + recName;
    return false;
  }
  const std::span<const std::byte> payload(
      file->data() + rec->payloadFileOffset, rec->payloadSize());
  std::string derr;
  const auto sprite = decodeFtiSprite(payload, &derr);
  if (!sprite) {
    *err = "sprite decode (" + recName + "): " + derr;
    return false;
  }
  const FtiSpriteFrame* frame = sprite->frame(0);
  if (!frame) {
    *err = "sprite record " + recName + " has no frame 0";
    return false;
  }

  // Palette binding (CORROBORATED): sprite bytes are final palette
  // indices; SYS_PAL is the resident palette for this file's records.
  // Entries 64-255 are never referenced by ARROW (max index 1).
  const FtiRecord* palRec = findFtiRecord(dir, "SYS_PAL");
  if (palRec && palRec->payloadSize() >= 192) {
    const std::byte* sp = file->data() + palRec->payloadFileOffset;
    for (int i = 0; i < 64; ++i) {
      palette.set(i, {static_cast<std::uint8_t>(sp[i * 3 + 0]),
                      static_cast<std::uint8_t>(sp[i * 3 + 1]),
                      static_cast<std::uint8_t>(sp[i * 3 + 2]), 255});
    }
  }

  // Synthetic checkerboard (diagnostic only): SYS_PAL[10] orange /
  // SYS_PAL[13] blue in 8px cells — the white index-1 arrow stands
  // out on both; transparent gaps show the pattern through.
  for (int y = 0; y < fb.height(); ++y) {
    for (int x = 0; x < fb.width(); ++x) {
      const bool cell = ((x / 8) + (y / 8)) & 1;
      fb.put(x, y, cell ? 10 : 13);
    }
  }
  // One draw at the proven reset-mouse position plus one offset draw
  // so the hotspot/origin behavior is visible.
  blitFtiSpriteFrame(*frame, fb, kFrontendMouseResetX,
                     kFrontendMouseResetY);
  blitFtiSpriteFrame(*frame, fb, 120, 60);
  log::info(kTag,
            "sprite preview: %s %s — %u frame(s), frame0 %ux%u hot "
            "(%d,%d), %zu stream bytes, %llu opaque px, max idx %u, "
            "digest=%016llx",
            relFile.c_str(), rec->name().c_str(),
            static_cast<unsigned>(sprite->frames.size()), frame->width,
            frame->height, frame->hotspotX, frame->hotspotY,
            frame->stream.size(),
            static_cast<unsigned long long>(frame->opaqueWrites),
            frame->maxPixelIndex,
            static_cast<unsigned long long>(ftiSpriteDigest(*sprite)));
  return true;
}

// The decoded original resources the front-end root menu needs —
// shared by the static Phase 4D preview and the Phase 4E interactive
// controller. All bindings resolve through the proven original paths.
struct FrontendResources {
  IndexedImage backdrop;            // MISC/OPTIONS.BNI record MDKOPT
  FtiFont fontBig;                  // MISC/MDKFONT.FTI record FONTBIG
  FtiFont fontSml;                  // record FONTSML (sound end labels)
  FtiSprite arrow;                  // record ARROW (frame 0 used)
  std::vector<std::string> optStrings;  // OPT0..OPT4 C strings
  // Phase 4F options sub-menu (FUN_00420eac): the OM_* row labels
  // (records ARE NUL-terminated strings — OBSERVED) and the resident
  // system-palette head the options palette upload uses.
  std::array<std::string, kOptionsItemCount> omStrings;
  std::array<std::string, 3> omSkill;    // OM_SK_0/1/2 skill variants
  // Phase 4H display child (FUN_0041d1e0): the DSP_* row records —
  // DSP_BRGT is a printf format ("Brightness %d"), the rest plain
  // strings (OBSERVED NUL-terminated, same as OM_*).
  std::string dspBrightness;             // DSP_BRGT
  std::string dspDetailHigh;             // DSP_DETH
  std::string dspDetailLow;              // DSP_DETL
  std::string dspQuit;                   // DSP_QUIT
  // Phase 4I sound child (FUN_004233d8): the SND_* records — same
  // NUL-terminated string shape. SND_SET ("Setup Device") exists in
  // the FTI but is never resolved by the proven frame (vestigial —
  // not loaded).
  std::string sndTitle;                  // SND_TITL
  std::string sndInfo;                   // SND_INFO
  std::string sndEffects;                // SND_FX
  std::string sndMusic;                  // SND_MUSI
  std::string sndDone;                   // SND_DONE
  std::string sndEnd100;                 // SND_100
  std::string sndEnd0;                   // SND_0
  // Phase 4J mouse child (FUN_004217e8): the JOY_*/M_* records —
  // same NUL-terminated string shape, all drawn FONTSML.
  std::string mouseTest;                 // JOY_TEST
  std::string mouseEnabled;              // M_ENA
  std::string mouseDisabled;             // M_DIS
  std::string mouseReversed;             // M_REV
  std::string mouseNormal;               // M_NORM
  std::string mouseQuit;                 // JOY_QUIT
  std::string mouseButtons;              // JOY_B (grid header)
  std::array<std::string, kMouseGridRows> mouseActions;   // JOY_BA..BP
  std::array<std::string, 9> mouseAxisNames; // JOY_A0 + JOY_AA..AH
  std::array<std::string, kMouseAxisCount> mouseAxisCaps; // JOY_AX0..2
  // Phase 4K keyboard child (FUN_0041f18c): the KM_* records —
  // same NUL-terminated string shape, all drawn FONTSML — plus the
  // LANG record's first byte (the glyph-table selector; a missing
  // LANG soft-resolves to English exactly like FUN_00414930's 0).
  std::array<std::string, kKeyboardBindingRows> kbRows;   // KM_* rows
  std::string kbReset;                 // KM_RESET
  std::string kbQuit;                  // KM_QUIT
  std::string kbDoit;                  // KM_DOIT
  char kbLangTag = 0;                  // LANG record first byte
  std::array<std::byte, 192> sysPalHead{};  // SYS_PAL record head
  bool savesExist = false;          // FUN_00428290 SAVES/*.SAV probe
};

// Load and decode every front-end resource. Fills `err` -> false on
// failure (missing files/records, decode errors).
static bool loadFrontendResources(DataRoot& root, FrontendResources& res,
                                  std::string* err) {
  // Backdrop: MISC/OPTIONS.BNI record MDKOPT (Phase 4A decoder).
  const auto bni = root.readFile("MISC/OPTIONS.BNI", kPreviewMaxBytes, err);
  if (!bni) {
    *err = "front-end: cannot read MISC/OPTIONS.BNI — " + *err;
    return false;
  }
  const auto bdir = inspectBniDirectory(
      std::span<const std::byte>(bni->data(), bni->size()));
  if (bdir.status != BniDirectoryStatus::kOk) {
    *err = "front-end: OPTIONS.BNI directory — " +
           std::string(bniDirectoryStatusName(bdir.status)) + " — " +
           bdir.detail;
    return false;
  }
  const BniRecord* mdkopt = findBniRecord(bdir, "MDKOPT");
  if (!mdkopt) {
    *err = "front-end: record MDKOPT not found in OPTIONS.BNI";
    return false;
  }
  const std::span<const std::byte> optPayload(
      bni->data() + mdkopt->payloadFileOffset, mdkopt->payloadSize());
  std::string derr;
  const auto backdrop = decodeBniPalettedImage(optPayload, &derr);
  if (!backdrop) {
    *err = "front-end: MDKOPT decode — " + derr;
    return false;
  }

  // FTI resources: FONTBIG font, ARROW sprite, OPT0..OPT4 strings.
  const auto fti = root.readFile("MISC/MDKFONT.FTI", kPreviewMaxBytes, err);
  if (!fti) {
    *err = "front-end: cannot read MISC/MDKFONT.FTI — " + *err;
    return false;
  }
  const auto fdir = inspectFtiDirectory(
      std::span<const std::byte>(fti->data(), fti->size()));
  if (fdir.status != FtiDirectoryStatus::kOk) {
    *err = "front-end: MDKFONT.FTI directory — " +
           std::string(ftiDirectoryStatusName(fdir.status)) + " — " +
           fdir.detail;
    return false;
  }
  auto ftiPayload = [&](const char* name, const FtiRecord*& recOut,
                        std::span<const std::byte>& out) -> bool {
    recOut = findFtiRecord(fdir, name);
    if (!recOut) {
      *err = std::string("front-end: record ") + name +
             " not found in MDKFONT.FTI";
      return false;
    }
    out = std::span<const std::byte>(
        fti->data() + recOut->payloadFileOffset,
        recOut->payloadSize());
    return true;
  };

  const FtiRecord* rec = nullptr;
  std::span<const std::byte> payload;
  if (!ftiPayload("FONTBIG", rec, payload)) return false;
  const auto fontBig = decodeFtiFont(payload, &derr);
  if (!fontBig) {
    *err = "front-end: FONTBIG decode — " + derr;
    return false;
  }
  if (!ftiPayload("ARROW", rec, payload)) return false;
  const auto arrow = decodeFtiSprite(payload, &derr);
  if (!arrow) {
    *err = "front-end: ARROW decode — " + derr;
    return false;
  }
  if (!arrow->frame(0)) {
    *err = "front-end: ARROW has no frame 0";
    return false;
  }
  // Phase 4I: the sound screen's volume endpoint labels ("0%"/"100%")
  // are FONTSML (FUN_00414dd4) and its title/info rows fall back to
  // FONTSML when the FONTBIG measure reaches 600 (FUN_00414d2c ->
  // FUN_00414f1c).
  if (!ftiPayload("FONTSML", rec, payload)) return false;
  const auto fontSml = decodeFtiFont(payload, &derr);
  if (!fontSml) {
    *err = "front-end: FONTSML decode — " + derr;
    return false;
  }

  // OPTi payloads are the NUL-terminated label strings themselves
  // (OBSERVED — the records ARE C strings drawn verbatim).
  std::vector<std::string> optStrings(kFrontendOptCount);
  for (int i = 0; i < kFrontendOptCount; ++i) {
    char name[8];
    std::snprintf(name, sizeof(name), "OPT%d", i);
    if (!ftiPayload(name, rec, payload)) return false;
    const std::span<const std::byte> span = payload;
    const void* nul = std::memchr(span.data(), 0, span.size());
    if (!nul) {
      *err = std::string("front-end: ") + name +
             " payload is not a NUL-terminated string";
      return false;
    }
    optStrings[i].assign(
        reinterpret_cast<const char*>(span.data()),
        static_cast<const char*>(nul) -
            reinterpret_cast<const char*>(span.data()));
  }

  // Phase 4F: OM_* records are the same NUL-terminated string shape
  // (OBSERVED — the payloads ARE C strings drawn verbatim). Row 6's
  // record is dynamic: OM_SK_0/1/2 by DAT_0054147a.
  auto loadCStr = [&](const char* name, std::string& out) -> bool {
    if (!ftiPayload(name, rec, payload)) return false;
    const void* nul2 =
        std::memchr(payload.data(), 0, payload.size());
    if (!nul2) {
      *err = std::string("front-end: ") + name +
             " payload is not a NUL-terminated string";
      return false;
    }
    out.assign(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<const char*>(nul2) -
            reinterpret_cast<const char*>(payload.data()));
    return true;
  };
  for (int i = 0; i < kOptionsItemCount; ++i) {
    if (!loadCStr(kOptionsRecordNames[i], res.omStrings[i])) {
      return false;
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (!loadCStr(kOptionsSkillRecords[i], res.omSkill[i])) {
      return false;
    }
  }

  // Phase 4H: DSP_* records — same NUL-terminated string shape;
  // DSP_BRGT's text is the row-0 printf format ("Brightness %d").
  if (!loadCStr(kDisplayBrightnessRecord, res.dspBrightness) ||
      !loadCStr(kDisplayDetailHighRecord, res.dspDetailHigh) ||
      !loadCStr(kDisplayDetailLowRecord, res.dspDetailLow) ||
      !loadCStr(kDisplayQuitRecord, res.dspQuit)) {
    return false;
  }

  // Phase 4I: SND_* records — same NUL-terminated string shape.
  // Only the seven records the proven frame resolves; SND_SET stays
  // unloaded (never referenced by FUN_004233d8's draw block).
  if (!loadCStr(kSoundTitleRecord, res.sndTitle) ||
      !loadCStr(kSoundInfoRecord, res.sndInfo) ||
      !loadCStr(kSoundFxRecord, res.sndEffects) ||
      !loadCStr(kSoundMusicRecord, res.sndMusic) ||
      !loadCStr(kSoundDoneRecord, res.sndDone) ||
      !loadCStr(kSoundEnd100Record, res.sndEnd100) ||
      !loadCStr(kSoundEnd0Record, res.sndEnd0)) {
    return false;
  }

  // Phase 4J: JOY_*/M_* records — same NUL-terminated string shape.
  // Grid rows resolve "JOY_B%c" ('A'+r -> JOY_BA..BP), axis actions
  // "JOY_A%c" (JOY_A0 for '0'/invalid, JOY_AA..AH), captions
  // "JOY_AX%d" (0..2) — the same resolution the frame handler's
  // sprintf calls perform (OBSERVED literals).
  if (!loadCStr(kMouseTestRecord, res.mouseTest) ||
      !loadCStr(kMouseEnabledRecord, res.mouseEnabled) ||
      !loadCStr(kMouseDisabledRecord, res.mouseDisabled) ||
      !loadCStr(kMouseReversedRecord, res.mouseReversed) ||
      !loadCStr(kMouseNormalRecord, res.mouseNormal) ||
      !loadCStr(kMouseQuitRecord, res.mouseQuit) ||
      !loadCStr(kMouseButtonsRecord, res.mouseButtons)) {
    return false;
  }
  for (int r = 0; r < kMouseGridRows; ++r) {
    char name[8];
    std::snprintf(name, sizeof(name), "JOY_B%c", 'A' + r);
    if (!loadCStr(name, res.mouseActions[r])) return false;
  }
  if (!loadCStr("JOY_A0", res.mouseAxisNames[0])) return false;
  for (int i = 0; i < 8; ++i) {
    char name[8];
    std::snprintf(name, sizeof(name), "JOY_A%c", 'A' + i);
    if (!loadCStr(name, res.mouseAxisNames[i + 1])) return false;
  }
  for (int i = 0; i < kMouseAxisCount; ++i) {
    char name[8];
    std::snprintf(name, sizeof(name), "JOY_AX%d", i);
    if (!loadCStr(name, res.mouseAxisCaps[i])) return false;
  }

  // Phase 4K: the KM_* records — same NUL-terminated string shape,
  // resolved in the proven draw order (the record-pointer table in
  // FUN_0041f18c), NOT settings-table order.
  for (int r = 0; r < kKeyboardBindingRows; ++r) {
    if (!loadCStr(kKeyboardRowRecords[r], res.kbRows[r])) {
      return false;
    }
  }
  if (!loadCStr(kKeyboardResetRecord, res.kbReset) ||
      !loadCStr(kKeyboardQuitRecord, res.kbQuit) ||
      !loadCStr(kKeyboardDoitRecord, res.kbDoit)) {
    return false;
  }
  // LANG — FUN_00414930's soft resolve: a missing record yields 0,
  // and FUN_0041f068 reads only the payload's first byte ('F' ->
  // French, 'G' -> German, else English).
  if (const FtiRecord* lang =
          findFtiRecord(fdir, kKeyboardLangRecord)) {
    if (lang->payloadSize() > 0) {
      res.kbLangTag = static_cast<char>(
          fti->data()[lang->payloadFileOffset]);
    }
  }

  // SYS_PAL head — the resident system palette whose head fills
  // DAT_00540820[0:192]; the options screen uploads that buffer
  // (FUN_0046d208) on entry.
  const FtiRecord* palRec = findFtiRecord(fdir, "SYS_PAL");
  if (!palRec || palRec->payloadSize() < 192) {
    *err = "front-end: SYS_PAL record missing/short in MDKFONT.FTI";
    return false;
  }
  std::memcpy(res.sysPalHead.data(),
              fti->data() + palRec->payloadFileOffset, 192);

  // Phase 4I: the sound entry (FUN_0042322c) loads MISC\MDKSOUND.SNI
  // via FUN_00428828 and resolves OPTSONG/OPTBUTT inside it
  // (FUN_00402fe8). The port models the audio triggers semantically
  // — no payload decode — but the resolve contract is real: the
  // directory must parse and both records must be present.
  const auto sni =
      root.readFile(kSoundSniFile, kPreviewMaxBytes, err);
  if (!sni) {
    *err = "front-end: cannot read MISC/MDKSOUND.SNI — " + *err;
    return false;
  }
  const auto sdir = inspectSniDirectory(
      std::span<const std::byte>(sni->data(), sni->size()));
  if (sdir.status != SniDirectoryStatus::kOk) {
    *err = "front-end: MDKSOUND.SNI directory — " +
           std::string(sniDirectoryStatusName(sdir.status)) + " — " +
           sdir.detail;
    return false;
  }
  auto sniRecord = [&](const char* name) {
    for (const auto& e : sdir.entries) {
      if (e.name() == name) return true;
    }
    return false;
  };
  if (!sniRecord(kSoundSongRecord)) {
    *err = "front-end: record OPTSONG not found in MDKSOUND.SNI";
    return false;
  }
  if (!sniRecord(kSoundButtonRecord)) {
    *err = "front-end: record OPTBUTT not found in MDKSOUND.SNI";
    return false;
  }

  // FUN_00428290 checks the SAVES directory for <name>.SAV files;
  // BUILD_A has 1.SAV + 2.SAV -> the five-item "Continue" menu.
  bool savesExist = false;
  if (const auto savesDir = root.resolve("SAVES")) {
    std::error_code ec;
    for (const auto& e :
         std::filesystem::directory_iterator(*savesDir, ec)) {
      const auto ext = e.path().extension().string();
      if (e.is_regular_file() &&
          (ext == ".SAV" || ext == ".sav")) {
        savesExist = true;
        break;
      }
    }
  }

  res.backdrop = std::move(*backdrop);
  res.fontBig = std::move(*fontBig);
  res.fontSml = std::move(*fontSml);
  res.arrow = std::move(*arrow);
  res.optStrings = std::move(optStrings);
  res.savesExist = savesExist;
  return true;
}

// Deterministic digests of a composed indexed frame — the indexed
// pixel payload and the RGBA palette, matching the Phase 4D static
// preview's digest domains.
static std::uint64_t digestIndexedFb(const IndexedFramebuffer& fb) {
  return fnv1a64(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(fb.pixels()), fb.pixelCount()));
}

static std::uint64_t digestPalette(const Palette& palette) {
  std::vector<std::byte> palBytes(palette.size() * 4);
  for (int i = 0; i < palette.size(); ++i) {
    const auto c = palette.get(i);
    palBytes[i * 4 + 0] = static_cast<std::byte>(c.r);
    palBytes[i * 4 + 1] = static_cast<std::byte>(c.g);
    palBytes[i * 4 + 2] = static_cast<std::byte>(c.b);
    palBytes[i * 4 + 3] = static_cast<std::byte>(c.a);
  }
  return fnv1a64(palBytes);
}

// Phase 4D options/front-end preview: compose the one proven static
// front-end frame (FUN_0041dc90 stable entry state) — MDKOPT backdrop
// + OPT0..OPT4 FONTBIG scaled centered labels + ARROW at the reset
// mouse position. Fills `err` -> false on failure.
static bool loadOptionsPreview(DataRoot& root, IndexedFramebuffer& fb,
                               Palette& palette, std::string* err) {
  FrontendResources res;
  if (!loadFrontendResources(root, res, err)) {
    return false;
  }
  const FrontendMenuSpec spec = frontendMenuSpec(res.savesExist);
  std::vector<std::string_view> views(res.optStrings.begin(),
                                      res.optStrings.end());
  std::string derr;
  if (!renderFrontendMenuFrame(fb, palette, res.backdrop, res.fontBig,
                               *res.arrow.frame(0), views, spec, &derr)) {
    *err = "options preview: compose — " + derr;
    return false;
  }

  // Deterministic digests (decoded representations, not the PPM).
  const std::uint64_t fbDigest = digestIndexedFb(fb);
  const std::uint64_t palDigest = digestPalette(palette);
  log::info(kTag,
            "options preview: MDKOPT %dx%d digest=%016llx | FONTBIG "
            "digest=%016llx | ARROW digest=%016llx | saves=%d sel=%d | "
            "composed fb=%016llx palette=%016llx",
            res.backdrop.width, res.backdrop.height,
            static_cast<unsigned long long>(imageDigest(res.backdrop)),
            static_cast<unsigned long long>(ftiFontDigest(res.fontBig)),
            static_cast<unsigned long long>(ftiSpriteDigest(res.arrow)),
            res.savesExist ? 1 : 0, res.savesExist ? 0 : 1,
            static_cast<unsigned long long>(fbDigest),
            static_cast<unsigned long long>(palDigest));
  return true;
}

// Phase 4F options sub-menu preview: compose the proven static
// FUN_00420eac frame — cleared framebuffer + OM_* labels (selection 8
// at scale 1.0, the rest 0.65, canonical skill 1 -> "Skill - Normal")
// + ARROW at the (unchanged) logical mouse position, under the
// resident system palette (SYS_PAL head + zeroed tail — see
// options_menu.h).
// Fills `err` -> false on failure.
static bool loadOptionsSubmenuPreview(DataRoot& root,
                                      IndexedFramebuffer& fb,
                                      Palette& palette,
                                      std::string* err) {
  FrontendResources res;
  if (!loadFrontendResources(root, res, err)) {
    return false;
  }
  const OptionsMenuSpec spec;  // canonical entry state
  OptionsMenuLabels labels;
  for (int i = 0; i < kOptionsItemCount; ++i) {
    labels.items[i] = res.omStrings[i];
  }
  labels.items[kOptionsSkillRow] =
      res.omSkill[optionsSkillRecordIndex(spec.skill)];
  std::string derr;
  if (!renderOptionsMenuFrame(fb, palette, res.fontBig,
                              *res.arrow.frame(0), labels,
                              res.sysPalHead, spec, &derr)) {
    *err = "options sub-menu preview: compose — " + derr;
    return false;
  }

  const std::uint64_t fbDigest = digestIndexedFb(fb);
  const std::uint64_t palDigest = digestPalette(palette);
  log::info(kTag,
            "options sub-menu preview: OM_* %dx%d sel=%d skill=%d "
            "hidden=%d | FONTBIG digest=%016llx | ARROW digest=%016llx "
            "| composed fb=%016llx palette=%016llx",
            fb.width(), fb.height(), spec.selection, spec.skill,
            spec.devHidden ? 1 : 0,
            static_cast<unsigned long long>(ftiFontDigest(res.fontBig)),
            static_cast<unsigned long long>(ftiSpriteDigest(res.arrow)),
            static_cast<unsigned long long>(fbDigest),
            static_cast<unsigned long long>(palDigest));
  return true;
}

// Phase 4H display child preview: compose the proven static
// FUN_0041d1e0 entry frame — cleared framebuffer + the three DSP_*
// rows (entry selection 2 = DSP_QUIT at scale 1.0, the rest 0.65)
// + the 4x48 swatch grid + ARROW at the (unchanged) logical mouse
// position, under the composed display palette (SYS_PAL head +
// gray/red/green/blue ramps — see display_menu.h).
// Fills `err` -> false on failure.
static bool loadDisplaySubmenuPreview(DataRoot& root,
                                      IndexedFramebuffer& fb,
                                      Palette& palette,
                                      std::string* err) {
  FrontendResources res;
  if (!loadFrontendResources(root, res, err)) {
    return false;
  }
  const DisplayMenuSpec spec;  // canonical entry state
  const DisplayMenuLabels labels{res.dspBrightness, res.dspDetailHigh,
                                 res.dspDetailLow, res.dspQuit};
  std::string derr;
  if (!renderDisplayMenuFrame(fb, palette, res.fontBig,
                              *res.arrow.frame(0), labels,
                              res.sysPalHead, spec, &derr)) {
    *err = "display sub-menu preview: compose — " + derr;
    return false;
  }

  const std::uint64_t fbDigest = digestIndexedFb(fb);
  const std::uint64_t palDigest = digestPalette(palette);
  log::info(kTag,
            "display sub-menu preview: DSP_* %dx%d sel=%d "
            "brightness=%d pcorrect=%d | FONTBIG digest=%016llx | "
            "ARROW digest=%016llx | composed fb=%016llx "
            "palette=%016llx",
            fb.width(), fb.height(), spec.selection, spec.brightness,
            spec.forcePCorrect ? 1 : 0,
            static_cast<unsigned long long>(ftiFontDigest(res.fontBig)),
            static_cast<unsigned long long>(ftiSpriteDigest(res.arrow)),
            static_cast<unsigned long long>(fbDigest),
            static_cast<unsigned long long>(palDigest));
  return true;
}

// Phase 4I sound child preview: compose the proven static
// FUN_004233d8 entry frame — cleared framebuffer + SND_TITL/SND_INFO
// centered rows + the two volume rows (left-aligned scaled labels,
// inclusive bars at the factory volumes 70/100, FONTSML "0%"/"100%"
// endpoints) + SND_DONE centered + ARROW at the (unchanged) logical
// mouse position, under the inherited options palette (SYS_PAL head
// + zeroed tail — the screen uploads no palette of its own).
// First-entry selection 0 — DAT_0054bdbc is BSS-zeroed and never
// written at entry. Fills `err` -> false on failure.
static bool loadSoundSubmenuPreview(DataRoot& root,
                                    IndexedFramebuffer& fb,
                                    Palette& palette,
                                    std::string* err) {
  FrontendResources res;
  if (!loadFrontendResources(root, res, err)) {
    return false;
  }
  const SoundMenuSpec spec;  // canonical entry state
  const SoundMenuLabels labels{res.sndTitle, res.sndInfo,
                               res.sndEffects, res.sndMusic,
                               res.sndEnd100, res.sndEnd0,
                               res.sndDone};
  std::string derr;
  if (!renderSoundMenuFrame(fb, palette, res.fontBig, res.fontSml,
                            *res.arrow.frame(0), labels,
                            res.sysPalHead, spec, &derr)) {
    *err = "sound sub-menu preview: compose — " + derr;
    return false;
  }

  const std::uint64_t fbDigest = digestIndexedFb(fb);
  const std::uint64_t palDigest = digestPalette(palette);
  log::info(kTag,
            "sound sub-menu preview: SND_* %dx%d sel=%d fx=%d mus=%d "
            "| FONTBIG digest=%016llx | FONTSML digest=%016llx | "
            "ARROW digest=%016llx | composed fb=%016llx "
            "palette=%016llx",
            fb.width(), fb.height(), spec.selection, spec.soundFx,
            spec.soundMusic,
            static_cast<unsigned long long>(ftiFontDigest(res.fontBig)),
            static_cast<unsigned long long>(ftiFontDigest(res.fontSml)),
            static_cast<unsigned long long>(ftiSpriteDigest(res.arrow)),
            static_cast<unsigned long long>(fbDigest),
            static_cast<unsigned long long>(palDigest));
  return true;
}

// Phase 4J mouse child preview: compose the proven static
// FUN_004217e8 entry frame — cleared framebuffer + the four left
// rows (JOY_TEST/M_ENA/M_NORM/JOY_QUIT at factory MouseOn=TRUE,
// MouseYReversed=0) + the JOY_B header and 4x16 button-map cell
// grid + three axis bars (centered markers at zero deltas) + the
// test indicator + ARROW at the (unchanged) logical mouse — all
// FONTSML under the inherited options palette (SYS_PAL head +
// zeroed tail; the screen uploads no palette of its own). Entry
// resets selection and grid column to 0 (FUN_00421664).
// Fills `err` -> false on failure.
static bool loadMouseSubmenuPreview(DataRoot& root,
                                    IndexedFramebuffer& fb,
                                    Palette& palette,
                                    std::string* err) {
  FrontendResources res;
  if (!loadFrontendResources(root, res, err)) {
    return false;
  }
  const MouseMenuSpec spec;  // canonical entry state
  MouseMenuLabels labels;
  labels.test = res.mouseTest;
  labels.enabled = res.mouseEnabled;
  labels.disabled = res.mouseDisabled;
  labels.reversed = res.mouseReversed;
  labels.normal = res.mouseNormal;
  labels.quit = res.mouseQuit;
  labels.buttons = res.mouseButtons;
  for (int r = 0; r < kMouseGridRows; ++r) {
    labels.actions[r] = res.mouseActions[r];
  }
  for (int i = 0; i < 9; ++i) {
    labels.axisNames[i] = res.mouseAxisNames[i];
  }
  for (int i = 0; i < kMouseAxisCount; ++i) {
    labels.axisCaptions[i] = res.mouseAxisCaps[i];
  }
  std::string derr;
  if (!renderMouseMenuFrame(fb, palette, res.fontSml,
                            *res.arrow.frame(0), labels,
                            res.sysPalHead, spec, &derr)) {
    *err = "mouse sub-menu preview: compose — " + derr;
    return false;
  }

  const std::uint64_t fbDigest = digestIndexedFb(fb);
  const std::uint64_t palDigest = digestPalette(palette);
  log::info(kTag,
            "mouse sub-menu preview: JOY_* %dx%d sel=%d col=%d "
            "on=%d yrev=%d | FONTSML digest=%016llx | ARROW "
            "digest=%016llx | composed fb=%016llx palette=%016llx",
            fb.width(), fb.height(), spec.selection, spec.column,
            spec.mouseOn ? 1 : 0, spec.mouseYReversedBits != 0 ? 1 : 0,
            static_cast<unsigned long long>(ftiFontDigest(res.fontSml)),
            static_cast<unsigned long long>(ftiSpriteDigest(res.arrow)),
            static_cast<unsigned long long>(fbDigest),
            static_cast<unsigned long long>(palDigest));
  return true;
}

// Phase 4K keyboard child preview: compose the proven static
// FUN_0041f030/FUN_0041f18c entry frame — cleared framebuffer + the
// two-column 19-row KM_* grid (factory bindings, LANG-selected
// glyphs) + centered KM_RESET/KM_QUIT (entry selection 20 — the
// KM_QUIT row flagged) + ARROW at the carried logical mouse — all
// FONTSML under the inherited options palette (SYS_PAL head +
// zeroed tail; the screen uploads no palette of its own). No
// KM_DOIT — capture is clear at entry. Fills `err` -> false on
// failure.
static bool loadKeyboardSubmenuPreview(DataRoot& root,
                                       IndexedFramebuffer& fb,
                                       Palette& palette,
                                       std::string* err) {
  FrontendResources res;
  if (!loadFrontendResources(root, res, err)) {
    return false;
  }
  KeyboardMenuSpec spec;  // canonical entry state + resolved LANG
  spec.langTag = res.kbLangTag;
  KeyboardMenuLabels labels;
  for (int r = 0; r < kKeyboardBindingRows; ++r) {
    labels.rows[r] = res.kbRows[r];
  }
  labels.reset = res.kbReset;
  labels.quit = res.kbQuit;
  labels.doit = res.kbDoit;
  labels.langTag = res.kbLangTag;
  std::string derr;
  if (!renderKeyboardMenuFrame(fb, palette, res.fontSml,
                               *res.arrow.frame(0), labels,
                               res.sysPalHead, spec, &derr)) {
    *err = "keyboard sub-menu preview: compose — " + derr;
    return false;
  }

  const std::uint64_t fbDigest = digestIndexedFb(fb);
  const std::uint64_t palDigest = digestPalette(palette);
  log::info(kTag,
            "keyboard sub-menu preview: KM_* %dx%d sel=%d cap=%d "
            "lang=%c | FONTSML digest=%016llx | ARROW "
            "digest=%016llx | composed fb=%016llx palette=%016llx",
            fb.width(), fb.height(), spec.selection,
            spec.capture ? 1 : 0,
            spec.langTag ? spec.langTag : '0',
            static_cast<unsigned long long>(ftiFontDigest(res.fontSml)),
            static_cast<unsigned long long>(ftiSpriteDigest(res.arrow)),
            static_cast<unsigned long long>(fbDigest),
            static_cast<unsigned long long>(palDigest));
  return true;
}

// Phase 4K platform seam — SDL scancode -> the DirectInput offset the
// original's keyboard device would have produced (FUN_0046b688's
// dwOfs). Keys with no DIK equivalent return -1: the original's
// device never reports them, so they produce no bitmap bit. This
// lives in the application layer on purpose — mdk_core stays
// platform-neutral; the DIK -> internal-code rule it maps into is
// the original-domain helper in keyboard_menu.h. Note the faithful
// aliasing: F13-F15 map to DIK 0x64-0x66 (base domain), whose
// internal codes 100/101/102 alias SYSRQ/RALT/HOME exactly as the
// original's own table produces.
static int dikFromSdlScancode(SDL_Scancode sc) {
  switch (sc) {
  // Letters -> DIK layout order.
  case SDL_SCANCODE_A: return 0x1e;
  case SDL_SCANCODE_B: return 0x30;
  case SDL_SCANCODE_C: return 0x2e;
  case SDL_SCANCODE_D: return 0x20;
  case SDL_SCANCODE_E: return 0x12;
  case SDL_SCANCODE_F: return 0x21;
  case SDL_SCANCODE_G: return 0x22;
  case SDL_SCANCODE_H: return 0x23;
  case SDL_SCANCODE_I: return 0x17;
  case SDL_SCANCODE_J: return 0x24;
  case SDL_SCANCODE_K: return 0x25;
  case SDL_SCANCODE_L: return 0x26;
  case SDL_SCANCODE_M: return 0x32;
  case SDL_SCANCODE_N: return 0x31;
  case SDL_SCANCODE_O: return 0x18;
  case SDL_SCANCODE_P: return 0x19;
  case SDL_SCANCODE_Q: return 0x10;
  case SDL_SCANCODE_R: return 0x13;
  case SDL_SCANCODE_S: return 0x1f;
  case SDL_SCANCODE_T: return 0x14;
  case SDL_SCANCODE_U: return 0x16;
  case SDL_SCANCODE_V: return 0x2f;
  case SDL_SCANCODE_W: return 0x11;
  case SDL_SCANCODE_X: return 0x2d;
  case SDL_SCANCODE_Y: return 0x15;
  case SDL_SCANCODE_Z: return 0x2c;
  // Digit row '1'..'0' -> DIK 0x02..0x0b.
  case SDL_SCANCODE_1: return 0x02;
  case SDL_SCANCODE_2: return 0x03;
  case SDL_SCANCODE_3: return 0x04;
  case SDL_SCANCODE_4: return 0x05;
  case SDL_SCANCODE_5: return 0x06;
  case SDL_SCANCODE_6: return 0x07;
  case SDL_SCANCODE_7: return 0x08;
  case SDL_SCANCODE_8: return 0x09;
  case SDL_SCANCODE_9: return 0x0a;
  case SDL_SCANCODE_0: return 0x0b;
  // Punctuation / whitespace.
  case SDL_SCANCODE_RETURN: return 0x1c;
  case SDL_SCANCODE_ESCAPE: return 0x01;
  case SDL_SCANCODE_BACKSPACE: return 0x0e;
  case SDL_SCANCODE_TAB: return 0x0f;
  case SDL_SCANCODE_SPACE: return 0x39;
  case SDL_SCANCODE_MINUS: return 0x0c;
  case SDL_SCANCODE_EQUALS: return 0x0d;
  case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
  case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
  case SDL_SCANCODE_BACKSLASH: return 0x2b;
  case SDL_SCANCODE_NONUSHASH: return 0x2b;  // same 0x2b position
  case SDL_SCANCODE_SEMICOLON: return 0x27;
  case SDL_SCANCODE_APOSTROPHE: return 0x28;
  case SDL_SCANCODE_GRAVE: return 0x29;
  case SDL_SCANCODE_COMMA: return 0x33;
  case SDL_SCANCODE_PERIOD: return 0x34;
  case SDL_SCANCODE_SLASH: return 0x35;
  case SDL_SCANCODE_CAPSLOCK: return 0x3a;   // DIK_CAPITAL
  // Function keys F1..F12.
  case SDL_SCANCODE_F1: return 0x3b;
  case SDL_SCANCODE_F2: return 0x3c;
  case SDL_SCANCODE_F3: return 0x3d;
  case SDL_SCANCODE_F4: return 0x3e;
  case SDL_SCANCODE_F5: return 0x3f;
  case SDL_SCANCODE_F6: return 0x40;
  case SDL_SCANCODE_F7: return 0x41;
  case SDL_SCANCODE_F8: return 0x42;
  case SDL_SCANCODE_F9: return 0x43;
  case SDL_SCANCODE_F10: return 0x44;
  case SDL_SCANCODE_F11: return 0x57;
  case SDL_SCANCODE_F12: return 0x58;
  case SDL_SCANCODE_F13: return 0x64;  // aliases internal 100 (SYSRQ)
  case SDL_SCANCODE_F14: return 0x65;  // aliases internal 101 (RALT)
  case SDL_SCANCODE_F15: return 0x66;  // aliases internal 102 (HOME)
  // (F16-F24 have no DIK — unmappable.)
  // Nav cluster — extended DIKs through the 0x49bbf0 table.
  case SDL_SCANCODE_PRINTSCREEN: return 0xb7;  // SYSRQ -> 100
  case SDL_SCANCODE_SCROLLLOCK: return 0x46;   // DIK_SCROLL (base)
  case SDL_SCANCODE_PAUSE: return 0xc5;        // table -> 0x7f
  case SDL_SCANCODE_INSERT: return 0xd2;       // -> 110
  case SDL_SCANCODE_HOME: return 0xc7;         // -> 102
  case SDL_SCANCODE_PAGEUP: return 0xc9;       // -> 104
  case SDL_SCANCODE_DELETE: return 0xd3;       // -> 111
  case SDL_SCANCODE_END: return 0xcf;          // -> 107
  case SDL_SCANCODE_PAGEDOWN: return 0xd1;     // -> 109
  case SDL_SCANCODE_RIGHT: return 0xcd;        // -> 106
  case SDL_SCANCODE_LEFT: return 0xcb;         // -> 105
  case SDL_SCANCODE_DOWN: return 0xd0;         // -> 108
  case SDL_SCANCODE_UP: return 0xc8;           // -> 103
  // Keypad.
  case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; // DIK_NUMLOCK
  case SDL_SCANCODE_KP_DIVIDE: return 0xb5;    // -> 99
  case SDL_SCANCODE_KP_MULTIPLY: return 0x37;  // base
  case SDL_SCANCODE_KP_MINUS: return 0x4a;     // DIK_SUBTRACT
  case SDL_SCANCODE_KP_PLUS: return 0x4e;      // DIK_ADD
  case SDL_SCANCODE_KP_ENTER: return 0x9c;     // -> 96
  case SDL_SCANCODE_KP_1: return 0x4f;
  case SDL_SCANCODE_KP_2: return 0x50;
  case SDL_SCANCODE_KP_3: return 0x51;
  case SDL_SCANCODE_KP_4: return 0x4b;
  case SDL_SCANCODE_KP_5: return 0x4c;
  case SDL_SCANCODE_KP_6: return 0x4d;
  case SDL_SCANCODE_KP_7: return 0x47;
  case SDL_SCANCODE_KP_8: return 0x48;
  case SDL_SCANCODE_KP_9: return 0x49;
  case SDL_SCANCODE_KP_0: return 0x52;
  case SDL_SCANCODE_KP_PERIOD: return 0x53;    // DIK_DECIMAL
  case SDL_SCANCODE_NONUSBACKSLASH: return 0x56; // DIK_OEM_102
  case SDL_SCANCODE_APPLICATION: return 0xdd;  // DIK_APPS -> 0x7f
  case SDL_SCANCODE_POWER: return 0xde;        // -> 0x7f
  case SDL_SCANCODE_KP_EQUALS: return 0x8d;    // -> 0x7f
  case SDL_SCANCODE_KP_COMMA: return 0xb3;     // -> 0x7f
  // Modifiers.
  case SDL_SCANCODE_LCTRL: return 0x1d;        // DIK_LCONTROL
  case SDL_SCANCODE_LSHIFT: return 0x2a;
  case SDL_SCANCODE_LALT: return 0x38;         // DIK_LMENU
  case SDL_SCANCODE_LGUI: return 0xdb;         // DIK_LWIN -> 0x7f
  case SDL_SCANCODE_RCTRL: return 0x9d;        // -> 97, poll folds 29
  case SDL_SCANCODE_RSHIFT: return 0x36;       // -> 54, poll folds 42
  case SDL_SCANCODE_RALT: return 0xb8;         // -> 101, poll folds 56
  case SDL_SCANCODE_RGUI: return 0xdc;         // DIK_RWIN -> 0x7f
  // Media/system keys with standard DIK equivalents (all -> 0x7f).
  case SDL_SCANCODE_MUTE: return 0xa0;
  case SDL_SCANCODE_VOLUMEUP: return 0xb0;
  case SDL_SCANCODE_VOLUMEDOWN: return 0xae;
  default: return -1;
  }
}

// Phase 4K raw-key edge machine — the FUN_0046b688 / FUN_00419370
// pair reconstructed in the original's internal key-code domain
// (NOT SDL scancodes, NOT persisted values):
//   level : the pure level bitmap (+0x10 in the original) — press
//           sets, release clears.
//   latch : the sticky bitmap (+0x00) — press sets; cleared only by
//           the frame-end reseed from `level`.
//   prev  : the previous poll's pre-reseed latch.
// Per poll the original computes edge = latch & ~prev, then
// prev = latch, then latch = level — so a press+release wholly
// inside one poll interval still lands in the edge bitmap (the
// latch saw it) while a held key never repeats as a fresh edge.
// Process lifetime: the bitmaps are device state, not screen state
// — they persist across screen transitions exactly like the
// original's globals.
struct FrontendRawKeyState {
  KeyboardEdgeBitmap level{};
  KeyboardEdgeBitmap latch{};
  KeyboardEdgeBitmap prev{};
};

// One frame of device events -> the four internal edge dwords
// (DAT_0049a8e8..f4 analogue). Repeat events are not device edges —
// the original's device reports a press once.
static void frontendRawKeyPoll(FrontendRawKeyState& st,
                               const InputState& input,
                               KeyboardEdgeBitmap& edgeOut) {
  for (const KeyEvent& e : input.keyEvents()) {
    if (e.repeat) {
      continue;
    }
    const int dik =
        dikFromSdlScancode(static_cast<SDL_Scancode>(e.scancode));
    if (dik < 0) {
      continue;
    }
    const int code = internalKeyFromDik(dik);
    const std::uint32_t bit = 1u << (code & 31);
    if (e.down) {
      st.latch[code >> 5] |= bit;   // press sets BOTH bitmaps
      st.level[code >> 5] |= bit;
    } else {
      st.level[code >> 5] &= ~bit;  // release clears only the level
    }
  }
  for (int w = 0; w < 4; ++w) {
    edgeOut[w] = st.latch[w] & ~st.prev[w];
    st.prev[w] = st.latch[w];
    st.latch[w] = st.level[w];      // the frame-end reseed
  }
}

// Phase 4E — translate the platform InputState into the controller's
// semantic per-frame input. Original reference points:
//   prevHeld/nextHeld : DIK_UP/DIK_DOWN with the original keymap's
//     "held OR pressed during this poll" semantics (mapA bits 103/108).
//   confirmEdge       : DIK_RETURN non-repeat press edge (bit 28).
//   attractEdge       : DIK_RIGHT non-repeat press edge (bit 106).
//   mouseDx/Dy        : integer device deltas — SDL's float pixel
//     deltas truncate toward zero (nearest integer-domain model).
//   mouseButtons      : 4-bit nibble bit i = button i+1 held.
//   rawKeyEdge        : FUN_0046b688/FUN_00419370 edge bitmap in the
//     original's internal 0..127 key-code domain (Phase 4K) — see
//     FrontendRawKeyState above.
static FrontendMenuInput frontendInputFromSdl(
    const InputState& input, FrontendRawKeyState& rawKeys) {
  FrontendMenuInput fi;
  bool prevPress = false, nextPress = false;
  bool leftPress = false, rightPress = false;
  for (const KeyEvent& e : input.keyEvents()) {
    if (!e.down || e.repeat) {
      continue;
    }
    switch (e.scancode) {
    case SDL_SCANCODE_UP: prevPress = true; break;
    case SDL_SCANCODE_DOWN: nextPress = true; break;
    case SDL_SCANCODE_LEFT: leftPress = true; break;
    case SDL_SCANCODE_RIGHT: rightPress = true; fi.attractEdge = true; break;
    case SDL_SCANCODE_RETURN: fi.confirmEdge = true; break;
    case SDL_SCANCODE_ESCAPE: fi.cancelEdge = true; break;
    default: break;
    }
  }
  fi.prevHeld = input.keyDown(SDL_SCANCODE_UP) || prevPress;
  fi.nextHeld = input.keyDown(SDL_SCANCODE_DOWN) || nextPress;
  fi.leftHeld = input.keyDown(SDL_SCANCODE_LEFT) || leftPress;
  fi.rightHeld = input.keyDown(SDL_SCANCODE_RIGHT) || rightPress;
  fi.mouseDx = static_cast<int>(input.mouseDx());
  fi.mouseDy = static_cast<int>(input.mouseDy());
  // DIMOUSESTATE.lZ — the wheel axis the Mouse child's Z indicator
  // reads (DAT_0054b64c). DirectInput reports ±WHEEL_DELTA (120)
  // per detent; SDL gives ±1 integer tick per notch — scale by 120
  // to keep the original's device-delta domain (Phase 4J).
  fi.mouseDz = input.wheelTicksY() * 120;
  fi.mouseButtons = static_cast<std::uint8_t>(
      (input.mouseButtonDown(SDL_BUTTON_LEFT) ? 0x1 : 0) |
      (input.mouseButtonDown(SDL_BUTTON_RIGHT) ? 0x2 : 0) |
      (input.mouseButtonDown(SDL_BUTTON_MIDDLE) ? 0x4 : 0) |
      (input.mouseButtonDown(SDL_BUTTON_X1) ? 0x8 : 0));
  // Phase 4K: the raw internal-domain edge bitmap (only the
  // Keyboard child's capture path consumes it — everything else
  // reads the semantic fields above, unchanged).
  frontendRawKeyPoll(rawKeys, input, fi.rawKeyEdge);
  return fi;
}

static const char* frontendActionName(FrontendAction a) {
  switch (a) {
  case FrontendAction::ContinueGame: return "ContinueGame";
  case FrontendAction::NewGame: return "NewGame";
  case FrontendAction::SavedGame: return "SavedGame";
  case FrontendAction::OpenOptions: return "OpenOptions";
  case FrontendAction::Quit: return "Quit";
  case FrontendAction::EnterAttract: return "EnterAttract";
  default: return "None";
  }
}

static const char* optionsActionName(OptionsAction a) {
  switch (a) {
  case OptionsAction::Help: return "Help";
  case OptionsAction::Sound: return "Sound";
  case OptionsAction::Joystick: return "Joystick";
  case OptionsAction::Mouse: return "Mouse";
  case OptionsAction::Keyboard: return "Keyboard";
  case OptionsAction::Performance: return "Performance";
  case OptionsAction::SkillCyclePrev: return "SkillCyclePrev";
  case OptionsAction::SkillCycleNext: return "SkillCycleNext";
  case OptionsAction::Display: return "Display";
  case OptionsAction::Back: return "Back";
  default: return "None";
  }
}

static const char* displayActionName(DisplayAction a) {
  switch (a) {
  case DisplayAction::Back: return "Back";
  default: return "None";
  }
}

static const char* soundActionName(SoundAction a) {
  switch (a) {
  case SoundAction::Back: return "Back";
  default: return "None";
  }
}

static const char* mouseActionName(MouseAction a) {
  switch (a) {
  case MouseAction::Back: return "Back";
  default: return "None";
  }
}

static const char* keyboardActionName(KeyboardAction a) {
  switch (a) {
  case KeyboardAction::Back: return "Back";
  default: return "None";
  }
}

// Phase 4I semantic audio events — logged for observability; no
// audio backend consumes them (DirectSound playback deferred).
static const char* soundAudioEventName(SoundAudioEvent e) {
  switch (e) {
  case SoundAudioEvent::AmbientSongStop: return "AmbientSongStop";
  case SoundAudioEvent::SongStart: return "SongStart(OPTSONG)";
  case SoundAudioEvent::Button: return "Button(OPTBUTT)";
  case SoundAudioEvent::VolumesApplied: return "VolumesApplied";
  case SoundAudioEvent::SongStop: return "SongStop(OPTSONG)";
  case SoundAudioEvent::AmbientSongStart:
    return "AmbientSongStart(MAINSONG)";
  default: return "?";
  }
}

Application::Application(AppConfig cfg) : cfg_(std::move(cfg)) {}

int Application::run() {
  SdlHost host;
  if (!host.init()) {
    return 1;
  }
  if (!host.createWindow(cfg_.windowWidth, cfg_.windowHeight, "MDK-Native")) {
    return 1;
  }

  std::string err;
  auto presenter = createMetalPresenter(host.window(), &err);
  if (!presenter) {
    log::error(kTag, "presenter init failed: %s", err.c_str());
    return 1;
  }
  log::info(kTag, "presenter: %s", presenter->name());

  ModeDispatcher dispatcher;
  host.onQuit = [&] { dispatcher.requestQuit(); };
  host.onDrawableSizeChanged = [&](int w, int h) {
    log::info(kTag, "drawable resized to %dx%d", w, h);
    presenter->drawableSizeChanged(w, h);
  };

  std::optional<DataRoot> dataRoot;
  if (cfg_.dataPath) {
    std::string derr;
    dataRoot = DataRoot::open(*cfg_.dataPath, &derr);
    if (!dataRoot) {
      log::error(kTag, "%s", derr.c_str());
      return 2;
    }
    log::info(kTag, "data root (read-only): %s",
              dataRoot->path().string().c_str());
    log::info(kTag, "resolver ready");
  } else {
    log::info(kTag, "no --data-path; diagnostic shell does not need data");
  }

  if (cfg_.relativeMouse) {
    host.setRelativeMouse(true);
  }

  IndexedFramebuffer fb(compat::kWorkWidth, compat::kWorkHeight);
  Palette palette;
  DiagnosticScene scene;

  // Phase 4E/4F interactive front-end state (set up only for
  // --interactive-frontend). The resources and controller persist for
  // the whole run; `frontendViews` aliases res.optStrings. Phase 4F
  // drives the two-screen flow controller; --frontend-root-only keeps
  // the Phase 4E root controller for the regression snapshot.
  std::optional<FrontendResources> frontendRes;
  std::optional<FrontendMenuController> frontendCtl;
  std::optional<FrontendFlowController> frontendFlow;
  std::vector<std::string_view> frontendViews;
  // Phase 4K: the raw-key edge machine (level/latch/prev bitmaps —
  // device state, not screen state; persists across transitions).
  FrontendRawKeyState frontendRawKeys;
  FrontendAction frontendLastAction = FrontendAction::None;
  OptionsAction frontendLastOptionsAction = OptionsAction::None;
  bool frontendEnteredOptions = false;
  bool frontendReturnedToRoot = false;
  // Phase 4G/4H observability for the scripted selftest verdict:
  // the skill value seen at each options entry (process-lifetime
  // retention), the dirty flag seen at each options exit, the
  // persistence-sink invocation count plus the last persisted
  // triple, and the Display child's entry/resume state.
  std::vector<int> optionsEntrySkills;
  std::vector<bool> optionsExitDirty;
  int settingsPersistCalls = 0;
  int settingsPersistedSkill = -1;
  int settingsPersistedBrightness = -1;
  int settingsPersistedForcePCorrect = -1;
  int settingsPersistedSoundFx = -1;
  int settingsPersistedSoundMusic = -1;
  int settingsInitialSkill = 1;  // post-config startup value
  int settingsInitialSoundFx = kSoundFxFactory;      // post-config
  int settingsInitialSoundMusic = kSoundMusicFactory;//   volumes
  int settingsInitialBrightness = 0;   // post-config
  int settingsInitialPcorrect = 0;     //   display settings
  bool displayEntered = false;
  int displayEntrySelection = -1;    // DAT_0054b834 seen at entry
  int optionsResumeSelection = -1;   // _DAT_0054bd34 after FUN_0041d144
  int displayFramesDrawn = 0;
  std::uint64_t displayLastFbDigest = 0;
  std::uint64_t displayLastPalDigest = 0;
  // Phase 4I sound child observability: entry state (DAT_0054bdbc is
  // a process global — first entry 0), the options selection on
  // resume, drawn-frame digests, and the semantic audio events the
  // proven FUN_00402388/FUN_004024c4/ambient-song triggers emit.
  bool soundEntered = false;
  int soundEntrySelection = -1;      // DAT_0054bdbc seen at entry
  int soundResumeSelection = -1;     // _DAT_0054bd34 after FUN_00423280
  int soundFramesDrawn = 0;
  std::uint64_t soundLastFbDigest = 0;
  std::uint64_t soundLastPalDigest = 0;
  std::vector<SoundAudioEvent> audioEventLog;
  // Phase 4J mouse child observability: entry state (DAT_0054bd40
  // IS reset at FUN_00421664 — always 0), the options selection on
  // resume (row 3), drawn-frame digests, and the loaded/persisted
  // W-set values for the verdict.
  bool mouseEntered = false;
  int mouseEntrySelection = -1;      // DAT_0054bd40 seen at entry
  int mouseResumeSelection = -1;     // _DAT_0054bd34 after the exit
  int mouseFramesDrawn = 0;
  std::uint64_t mouseLastFbDigest = 0;
  std::uint64_t mouseLastPalDigest = 0;
  bool settingsInitialMouseOn = true;   // post-config (factory TRUE)
  std::uint32_t settingsInitialMouseYRev = 0;  // raw float-slot bits
  std::string settingsInitialAxesMap = "ABG";  // W axes map
  std::uint32_t settingsInitialButtA = 1;      // MouseWButtMapA
  int settingsPersistedMouseOn = -1;
  std::uint32_t settingsPersistedMouseYRev = 0;
  std::string settingsPersistedAxesMap;
  std::uint32_t settingsPersistedButtA = 0;
  // Phase 4K keyboard child observability: entry state
  // (DAT_0054bcac = 0x14 — always the KM_QUIT row), the options
  // selection on resume (row 4), drawn-frame digests, the capture
  // flag for the script's KM_DOIT frame, and the loaded/persisted
  // Key* values the verdict checks (KeySniper = g7 is the row the
  // script rebinds).
  bool keyboardEntered = false;
  int keyboardEntrySelection = -1;   // DAT_0054bcac seen at entry
  int keyboardResumeSelection = -1;  // _DAT_0054bd34 after the exit
  int keyboardFramesDrawn = 0;
  bool keyboardCaptureSeen = false;  // capture==1 on a drawn frame
  std::uint64_t keyboardLastFbDigest = 0;
  std::uint64_t keyboardLastPalDigest = 0;
  int settingsInitialKeySniper = 57;  // post-config (factory SPACE)
  int settingsPersistedKeySniper = -1;
  int keyboardEntryKeySniper = -1;   // kg[7] seen at child entry —
                                     // proves the loaded binding
                                     // reached the screen's globals

  // Phase 4A preview mode: one proven original visual resource
  // decoded into the indexed framebuffer, then presented unchanged
  // every frame. The synthetic diagnostic scene stays the default
  // when no preview is requested.
  const bool previewMode = cfg_.previewFile.has_value() ||
                           cfg_.fontPreviewFile.has_value() ||
                           cfg_.spritePreviewFile.has_value() ||
                           cfg_.optionsPreview ||
                           cfg_.optionsSubmenuPreview ||
                           cfg_.displaySubmenuPreview ||
                           cfg_.soundSubmenuPreview ||
                           cfg_.mouseSubmenuPreview ||
                           cfg_.keyboardSubmenuPreview ||
                           cfg_.interactiveFrontend;
  if (cfg_.previewFile) {
    if (!dataRoot) {
      log::error(kTag, "--preview-resource requires --data-path");
      return 2;
    }
    std::string perr;
    auto img = loadPreviewImage(*dataRoot, *cfg_.previewFile,
                                cfg_.previewRecord.value_or(""), &perr);
    if (!img) {
      log::error(kTag, "preview failed: %s", perr.c_str());
      return 2;
    }
    fb.clear(0);
    blitIndexedImage(*img, fb, palette);
  } else if (cfg_.fontPreviewFile) {
    if (!dataRoot) {
      log::error(kTag, "--preview-font requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadFontPreview(*dataRoot, *cfg_.fontPreviewFile,
                         cfg_.fontPreviewRecord.value_or(""),
                         cfg_.fontPreviewText, fb, palette, &perr)) {
      log::error(kTag, "font preview failed: %s", perr.c_str());
      return 2;
    }
  } else if (cfg_.spritePreviewFile) {
    if (!dataRoot) {
      log::error(kTag, "--preview-sprite requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadSpritePreview(*dataRoot, *cfg_.spritePreviewFile,
                           cfg_.spritePreviewRecord.value_or(""),
                           fb, palette, &perr)) {
      log::error(kTag, "sprite preview failed: %s", perr.c_str());
      return 2;
    }
  } else if (cfg_.optionsPreview) {
    if (!dataRoot) {
      log::error(kTag, "--preview-options requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadOptionsPreview(*dataRoot, fb, palette, &perr)) {
      log::error(kTag, "options preview failed: %s", perr.c_str());
      return 2;
    }
  } else if (cfg_.optionsSubmenuPreview) {
    if (!dataRoot) {
      log::error(kTag,
                 "--preview-options-submenu requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadOptionsSubmenuPreview(*dataRoot, fb, palette, &perr)) {
      log::error(kTag, "options sub-menu preview failed: %s",
                 perr.c_str());
      return 2;
    }
  } else if (cfg_.displaySubmenuPreview) {
    if (!dataRoot) {
      log::error(kTag,
                 "--preview-display-submenu requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadDisplaySubmenuPreview(*dataRoot, fb, palette, &perr)) {
      log::error(kTag, "display sub-menu preview failed: %s",
                 perr.c_str());
      return 2;
    }
  } else if (cfg_.soundSubmenuPreview) {
    if (!dataRoot) {
      log::error(kTag,
                 "--preview-sound-submenu requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadSoundSubmenuPreview(*dataRoot, fb, palette, &perr)) {
      log::error(kTag, "sound sub-menu preview failed: %s",
                 perr.c_str());
      return 2;
    }
  } else if (cfg_.mouseSubmenuPreview) {
    if (!dataRoot) {
      log::error(kTag,
                 "--preview-mouse-submenu requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadMouseSubmenuPreview(*dataRoot, fb, palette, &perr)) {
      log::error(kTag, "mouse sub-menu preview failed: %s",
                 perr.c_str());
      return 2;
    }
  } else if (cfg_.keyboardSubmenuPreview) {
    if (!dataRoot) {
      log::error(kTag,
                 "--preview-keyboard-submenu requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadKeyboardSubmenuPreview(*dataRoot, fb, palette, &perr)) {
      log::error(kTag, "keyboard sub-menu preview failed: %s",
                 perr.c_str());
      return 2;
    }
  } else if (cfg_.interactiveFrontend) {
    if (!dataRoot) {
      log::error(kTag, "--interactive-frontend requires --data-path");
      return 2;
    }
    FrontendResources res;
    std::string perr;
    if (!loadFrontendResources(*dataRoot, res, &perr)) {
      log::error(kTag, "interactive front-end load failed: %s",
                 perr.c_str());
      return 2;
    }
    frontendViews.assign(res.optStrings.begin(), res.optStrings.end());
    if (cfg_.frontendRootOnly) {
      // Phase 4E regression path: the root controller alone, so
      // OpenOptions stays a deferred semantic action.
      frontendCtl.emplace(res.savesExist);
    } else {
      // Phase 4G native-owned persistence seam: --settings-file is
      // the ONLY settings location the port touches — always outside
      // the read-only DataRoot. Startup load mirrors FUN_00425de4
      // (defaults first, config overrides); a dirty options exit
      // mirrors FUN_004260ac through the flow's sink.
      FrontendSettings initialSettings;
      if (cfg_.settingsFile) {
        std::string serr;
        auto loaded =
            loadFrontendSettingsFile(*cfg_.settingsFile, &serr);
        if (loaded) {
          initialSettings = loaded->settings;
          settingsInitialSkill = initialSettings.skill;
          settingsInitialSoundFx = initialSettings.soundFx;
          settingsInitialSoundMusic = initialSettings.soundMusic;
          settingsInitialBrightness = initialSettings.brightness;
          settingsInitialPcorrect =
              initialSettings.forcePCorrect ? 1 : 0;
          settingsInitialMouseOn = initialSettings.mouseOn;
          settingsInitialMouseYRev = initialSettings.mouseYReversed;
          settingsInitialAxesMap = initialSettings.mouseWAxesMap;
          settingsInitialButtA = initialSettings.mouseWButtMapA;
          settingsInitialKeySniper = initialSettings.keySniper;
          log::info(kTag,
                    "settings: loaded %s (skill=%d brightness=%d "
                    "pcorrect=%d fx=%d mus=%d mouseOn=%d yrev=%u "
                    "axes=%s buttA=%u keySniper=%d "
                    "ignored=%d,%d,%d,%d,%d,%d)",
                    cfg_.settingsFile->string().c_str(),
                    initialSettings.skill, initialSettings.brightness,
                    initialSettings.forcePCorrect ? 1 : 0,
                    initialSettings.soundFx, initialSettings.soundMusic,
                    initialSettings.mouseOn ? 1 : 0,
                    initialSettings.mouseYReversed,
                    initialSettings.mouseWAxesMap.c_str(),
                    initialSettings.mouseWButtMapA,
                    initialSettings.keySniper,
                    loaded->ignoredSkillLines,
                    loaded->ignoredBrightnessLines,
                    loaded->ignoredSoundFxLines,
                    loaded->ignoredSoundMusicLines,
                    loaded->ignoredMouseLines,
                    loaded->ignoredKeyLines);
        } else {
          log::warn(kTag, "settings: %s — %s; factory defaults",
                    cfg_.settingsFile->string().c_str(),
                    serr.empty() ? "absent (fresh boot)"
                                 : serr.c_str());
        }
      }
      auto sink = [&](const FrontendSettings& s) {
        ++settingsPersistCalls;
        settingsPersistedSkill = s.skill;
        settingsPersistedBrightness = s.brightness;
        settingsPersistedForcePCorrect = s.forcePCorrect ? 1 : 0;
        settingsPersistedSoundFx = s.soundFx;
        settingsPersistedSoundMusic = s.soundMusic;
        settingsPersistedMouseOn = s.mouseOn ? 1 : 0;
        settingsPersistedMouseYRev = s.mouseYReversed;
        settingsPersistedAxesMap = s.mouseWAxesMap;
        settingsPersistedButtA = s.mouseWButtMapA;
        settingsPersistedKeySniper = s.keySniper;
        if (!cfg_.settingsFile) {
          // No writable location configured — the FUN_004260ac
          // silent-failure analogue: process-lifetime only.
          log::warn(kTag,
                    "settings persist skipped — no --settings-file "
                    "(skill=%d brightness=%d pcorrect=%d fx=%d "
                    "mus=%d)", s.skill,
                    s.brightness, s.forcePCorrect ? 1 : 0, s.soundFx,
                    s.soundMusic);
          return false;
        }
        std::string perr;
        if (!saveFrontendSettingsFile(*cfg_.settingsFile, s, &perr)) {
          log::warn(kTag, "settings persist failed: %s",
                    perr.c_str());
          return false;
        }
        log::info(kTag,
                  "settings persisted: %s (skill=%d brightness=%d "
                  "pcorrect=%d fx=%d mus=%d)",
                  cfg_.settingsFile->string().c_str(), s.skill,
                  s.brightness, s.forcePCorrect ? 1 : 0, s.soundFx,
                  s.soundMusic);
        return true;
      };
      frontendFlow.emplace(res.savesExist, initialSettings,
                           std::move(sink));
    }
    frontendRes.emplace(std::move(res));
    log::info(kTag,
              "interactive front-end: saves=%d sel=%d mouse=(%d,%d)%s — "
              "UP/DOWN select, RETURN or button activates (semantic "
              "actions only; downstream systems deferred)",
              res.savesExist ? 1 : 0,
              frontendCtl ? frontendCtl->selection()
                          : frontendFlow->root().selection(),
              frontendCtl ? frontendCtl->mouseX()
                          : frontendFlow->root().mouseX(),
              frontendCtl ? frontendCtl->mouseY()
                          : frontendFlow->root().mouseY(),
              frontendCtl ? " [root-only]" : "");
  } else {
    scene.buildPalette(palette);
  }

  InputState input;
  Clock clock;

  // Mode handlers: boot -> shell -> (quit flag) demonstrates the
  // dispatcher shape without implementing original modes.
  dispatcher.on(mode::nativeBoot, [&](const FrameContext&) {
    log::info(kTag, "mode boot -> shell");
    dispatcher.setPrimary(mode::nativeShell);
  });
  dispatcher.on(mode::nativeShell, [&](const FrameContext& ctx) {
    // While a reconstructed front-end sub-screen owns Esc
    // (FUN_00420eac cancel edge -> Back; FUN_0041d1e0 cancel edge ->
    // FUN_0041d144), the native diagnostic-shell Esc-to-quit
    // convenience is suspended; at the root screen it keeps its
    // Phase 4E behavior.
    const bool escRoutesToOptions =
        frontendFlow &&
        frontendFlow->screen() != FrontendScreen::Root;
    if (input.keyDown(SDL_SCANCODE_ESCAPE) && !escRoutesToOptions) {
      dispatcher.requestQuit();
      return;
    }
    scene.update(ctx.frameIndex, input);
  });

  if (cfg_.selftest) {
    if (frontendCtl || frontendFlow) {
      // Deterministic script: drop all real device input so only the
      // injected events reach the controller.
      host.isolateHardwareInputForSelftest();
    } else {
      host.injectSelfTestEvents();
    }
    if (cfg_.frames == 0) {
      // Phase 4K six-screen script: 64 steps (0..63) — the run
      // quits right after the last injected step.
      cfg_.frames = frontendFlow ? 64 : 10;
    }
  }

  while (!dispatcher.quitRequested()) {
    input.beginFrame();
    if (cfg_.selftest && (frontendCtl || frontendFlow)) {
      // Scripted interactive selftest: one step per frame (see
      // SdlHost::pushFrontendSelfTestStep).
      host.pushFrontendSelfTestStep(clock.frameCount(),
                                  cfg_.frontendRootOnly);
    }
    host.pumpEvents(input);
    const FrameTick t = clock.tick();

    if (cfg_.selftest && t.index == 0 && !frontendCtl && !frontendFlow) {
      selftestOk_ = host.verifySelfTestInput(input);
      log::info(kTag, "input selftest: %s",
                selftestOk_ ? "PASS" : "FAIL");
    }

    dispatcher.dispatch({t.index, t.dtSeconds, t.elapsedSeconds});

    if (frontendCtl || frontendFlow) {
      // Original frame order: poll -> controller update -> draw ->
      // timing update. The dynamic renderers drive the ramp via
      // per-item itemScale calls in draw order.
      const FrontendMenuInput fi =
          frontendInputFromSdl(input, frontendRawKeys);
      bool endedEarly = false;
      if (frontendCtl) {
        // Phase 4E root-only regression path.
        frontendCtl->update(fi);
        endedEarly = frontendCtl->frameEndedEarly();
      } else {
        frontendFlow->update(fi);
        endedEarly =
            frontendFlow->screen() == FrontendScreen::Display
                ? frontendFlow->display().frameEndedEarly()
            : frontendFlow->screen() == FrontendScreen::Sound
                ? frontendFlow->sound().frameEndedEarly()
            : frontendFlow->screen() == FrontendScreen::Mouse
                ? frontendFlow->mouse().frameEndedEarly()
            : frontendFlow->screen() == FrontendScreen::Keyboard
                ? frontendFlow->keyboard().frameEndedEarly()
            : frontendFlow->inOptions()
                ? frontendFlow->options().frameEndedEarly()
                : frontendFlow->root().frameEndedEarly();
      }
      // OBSERVED: every activation-dispatch branch RETs before the
      // draw block and the timing update — a dispatched frame draws
      // nothing and does not advance the timing machine. Skill cycles
      // and the attract trigger fall through to the draw.
      if (!endedEarly) {
        std::string rerr;
        bool rok = false;
        if (frontendFlow &&
            frontendFlow->screen() == FrontendScreen::Display) {
          // Phase 4H display frame: cleared buffer + DSP_* rows +
          // swatch grid + ARROW under the composed display palette
          // (FUN_0041d1e0 draw block).
          const DisplayMenuLabels labels{
              frontendRes->dspBrightness, frontendRes->dspDetailHigh,
              frontendRes->dspDetailLow, frontendRes->dspQuit};
          rok = renderDisplayMenuDynamic(
              fb, palette, frontendRes->fontBig,
              *frontendRes->arrow.frame(0), labels,
              frontendRes->sysPalHead, frontendFlow->display(), &rerr);
        } else if (frontendFlow &&
                   frontendFlow->screen() == FrontendScreen::Sound) {
          // Phase 4I sound frame: cleared buffer + centered
          // SND_TITL/SND_INFO + volume rows (scaled labels, inclusive
          // bars, FONTSML endpoints) + SND_DONE + ARROW under the
          // inherited options palette (FUN_004233d8 draw block).
          const SoundMenuLabels labels{
              frontendRes->sndTitle, frontendRes->sndInfo,
              frontendRes->sndEffects, frontendRes->sndMusic,
              frontendRes->sndEnd100, frontendRes->sndEnd0,
              frontendRes->sndDone};
          rok = renderSoundMenuDynamic(
              fb, palette, frontendRes->fontBig, frontendRes->fontSml,
              *frontendRes->arrow.frame(0), labels,
              frontendRes->sysPalHead, frontendFlow->sound(),
              frontendFlow->brightness(), &rerr);
        } else if (frontendFlow &&
                   frontendFlow->screen() == FrontendScreen::Mouse) {
          // Phase 4J mouse frame: cleared buffer + left rows + the
          // 4x16 button grid + axis bars/markers + test indicator +
          // ARROW — all FONTSML under the inherited options palette
          // (FUN_004217e8 draw block; the blink bracket advances
          // DAT_0049a770 once per flagged draw).
          MouseMenuLabels labels;
          labels.test = frontendRes->mouseTest;
          labels.enabled = frontendRes->mouseEnabled;
          labels.disabled = frontendRes->mouseDisabled;
          labels.reversed = frontendRes->mouseReversed;
          labels.normal = frontendRes->mouseNormal;
          labels.quit = frontendRes->mouseQuit;
          labels.buttons = frontendRes->mouseButtons;
          for (int r = 0; r < kMouseGridRows; ++r) {
            labels.actions[r] = frontendRes->mouseActions[r];
          }
          for (int i = 0; i < 9; ++i) {
            labels.axisNames[i] = frontendRes->mouseAxisNames[i];
          }
          for (int i = 0; i < kMouseAxisCount; ++i) {
            labels.axisCaptions[i] = frontendRes->mouseAxisCaps[i];
          }
          rok = renderMouseMenuDynamic(
              fb, palette, frontendRes->fontSml,
              *frontendRes->arrow.frame(0), labels,
              frontendRes->sysPalHead, frontendFlow->mouse(),
              frontendFlow->brightness(), &rerr);
        } else if (frontendFlow &&
                   frontendFlow->screen() == FrontendScreen::Keyboard) {
          // Phase 4K keyboard frame: cleared buffer + the two-column
          // KM_* row grid (labels + LANG-selected key glyphs) +
          // centered KM_RESET/KM_QUIT (+ KM_DOIT while capturing) +
          // ARROW — all FONTSML under the inherited options palette
          // (the FUN_0041f18c draw block; the blink bracket advances
          // DAT_0049a770 once per flagged draw).
          KeyboardMenuLabels labels;
          for (int r = 0; r < kKeyboardBindingRows; ++r) {
            labels.rows[r] = frontendRes->kbRows[r];
          }
          labels.reset = frontendRes->kbReset;
          labels.quit = frontendRes->kbQuit;
          labels.doit = frontendRes->kbDoit;
          labels.langTag = frontendRes->kbLangTag;
          rok = renderKeyboardMenuDynamic(
              fb, palette, frontendRes->fontSml,
              *frontendRes->arrow.frame(0), labels,
              frontendRes->sysPalHead, frontendFlow->keyboard(),
              frontendFlow->brightness(), &rerr);
        } else if (frontendFlow && frontendFlow->inOptions()) {
          // Phase 4F options frame: cleared buffer + OM_* labels +
          // ARROW under the system palette (FUN_00420eac draw block).
          OptionsMenuLabels labels;
          for (int i = 0; i < kOptionsItemCount; ++i) {
            labels.items[i] = frontendRes->omStrings[i];
          }
          // Row 6 record = OM_SK_<skill> — the same 0x421254 3-way
          // branch as the original draw block.
          labels.items[kOptionsSkillRow] = frontendRes->omSkill[
              optionsSkillRecordIndex(frontendFlow->options().skill())];
          rok = renderOptionsMenuDynamic(
              fb, palette, frontendRes->fontBig,
              *frontendRes->arrow.frame(0), labels,
              frontendRes->sysPalHead, frontendFlow->options(),
              frontendFlow->brightness(), &rerr);
        } else {
          rok = renderFrontendMenuDynamic(
              fb, palette, frontendRes->backdrop, frontendRes->fontBig,
              *frontendRes->arrow.frame(0), frontendViews,
              frontendCtl ? *frontendCtl : frontendFlow->root(),
              frontendFlow ? frontendFlow->brightness() : 0, &rerr);
        }
        if (!rok) {
          log::error(kTag, "interactive front-end render failed: %s",
                     rerr.c_str());
          selftestOk_ = false;
          dispatcher.requestQuit();
        }
        if (cfg_.selftest && frontendFlow &&
            frontendFlow->screen() == FrontendScreen::Display) {
          // Deterministic post-interaction digests — recorded every
          // drawn display frame; the last one is the child snapshot.
          displayLastFbDigest = digestIndexedFb(fb);
          displayLastPalDigest = digestPalette(palette);
          ++displayFramesDrawn;
        }
        if (cfg_.selftest && frontendFlow &&
            frontendFlow->screen() == FrontendScreen::Sound) {
          // Same for the sound child — the post-mutation snapshot.
          soundLastFbDigest = digestIndexedFb(fb);
          soundLastPalDigest = digestPalette(palette);
          ++soundFramesDrawn;
        }
        if (cfg_.selftest && frontendFlow &&
            frontendFlow->screen() == FrontendScreen::Mouse) {
          // Same for the mouse child — the post-mutation snapshot.
          mouseLastFbDigest = digestIndexedFb(fb);
          mouseLastPalDigest = digestPalette(palette);
          ++mouseFramesDrawn;
        }
        if (cfg_.selftest && frontendFlow &&
            frontendFlow->screen() == FrontendScreen::Keyboard) {
          // Same for the keyboard child — the post-mutation
          // snapshot; the capture flag and marker accumulator are
          // recorded for the verdict.
          keyboardLastFbDigest = digestIndexedFb(fb);
          keyboardLastPalDigest = digestPalette(palette);
          ++keyboardFramesDrawn;
          if (frontendFlow->keyboard().capture()) {
            keyboardCaptureSeen = true;
          }
        }
        // FUN_0042fe78/FUN_0042fb68 timing update — the tail of the
        // drawn frame only. --selftest feeds the original's paced
        // regime (100/3 ms per frame — rawDelta 4, step 1) so the
        // injected-input run is deterministic across machines; the
        // live path keeps real wall-clock deltas like the original.
        const double frontDtMs =
            cfg_.selftest ? (100.0 / 3.0) : (t.dtSeconds * 1000.0);
        if (frontendCtl) {
          frontendCtl->endFrame(frontDtMs);
        } else if (frontendFlow->screen() ==
                   FrontendScreen::Display) {
          frontendFlow->display().endFrame(frontDtMs);
        } else if (frontendFlow->screen() == FrontendScreen::Sound) {
          frontendFlow->sound().endFrame(frontDtMs);
        } else if (frontendFlow->screen() == FrontendScreen::Mouse) {
          frontendFlow->mouse().endFrame(frontDtMs);
        } else if (frontendFlow->screen() ==
                   FrontendScreen::Keyboard) {
          frontendFlow->keyboard().endFrame(frontDtMs);
        } else if (frontendFlow->inOptions()) {
          frontendFlow->options().endFrame(frontDtMs);
        } else {
          frontendFlow->root().endFrame(frontDtMs);
        }
      }
      // Phase 4I semantic audio drain — the sound child's proven
      // triggers (OPTSONG/OPTBUTT/volume-apply/ambient song) land on
      // the flow queue; logged for observability, no audio backend
      // consumes them in this phase.
      if (frontendFlow) {
        for (const SoundAudioEvent ev :
             frontendFlow->drainAudioEvents()) {
          audioEventLog.push_back(ev);
          log::info(kTag, "sound audio event: %s",
                    soundAudioEventName(ev));
        }
      }
      if (frontendCtl) {
        const FrontendAction a = frontendCtl->consumeAction();
        if (a != FrontendAction::None) {
          frontendLastAction = a;
          log::info(kTag, "frontend action: %s (sel=%d mouse=%d,%d)",
                    frontendActionName(a), frontendCtl->selection(),
                    frontendCtl->mouseX(), frontendCtl->mouseY());
          if (a == FrontendAction::Quit) {
            dispatcher.requestQuit();
          }
        }
      } else if (frontendFlow->screen() == FrontendScreen::Display) {
        const DisplayAction a = frontendFlow->consumeDisplayAction();
        if (a != DisplayAction::None) {
          log::info(kTag, "display action: %s (sel=%d mouse=%d,%d)",
                    displayActionName(a),
                    frontendFlow->display().selection(),
                    frontendFlow->display().mouseX(),
                    frontendFlow->display().mouseY());
        }
        if (frontendFlow->screen() == FrontendScreen::Options) {
          // Back/Esc consumed -> FUN_0041d144 -> options resumed.
          optionsResumeSelection = frontendFlow->options().selection();
          log::info(kTag,
                    "front-end flow: display -> options (resume "
                    "sel=%d mouse=%d,%d dirty=%d)",
                    optionsResumeSelection,
                    frontendFlow->options().mouseX(),
                    frontendFlow->options().mouseY(),
                    frontendFlow->options().settingsDirty() ? 1 : 0);
        }
      } else if (frontendFlow->screen() == FrontendScreen::Sound) {
        const SoundAction a = frontendFlow->consumeSoundAction();
        if (a != SoundAction::None) {
          log::info(kTag, "sound action: %s (sel=%d mouse=%d,%d)",
                    soundActionName(a),
                    frontendFlow->sound().selection(),
                    frontendFlow->sound().mouseX(),
                    frontendFlow->sound().mouseY());
        }
        if (frontendFlow->screen() == FrontendScreen::Options) {
          // Back/Esc/row-2 consumed -> FUN_00423280 -> options
          // resumed at the Sound row (sel 1).
          soundResumeSelection = frontendFlow->options().selection();
          log::info(kTag,
                    "front-end flow: sound -> options (resume "
                    "sel=%d mouse=%d,%d dirty=%d fx=%d mus=%d)",
                    soundResumeSelection,
                    frontendFlow->options().mouseX(),
                    frontendFlow->options().mouseY(),
                    frontendFlow->options().settingsDirty() ? 1 : 0,
                    frontendFlow->soundFx(),
                    frontendFlow->soundMusic());
        }
      } else if (frontendFlow->screen() == FrontendScreen::Mouse) {
        const MouseAction a = frontendFlow->consumeMouseAction();
        if (a != MouseAction::None) {
          log::info(kTag, "mouse action: %s (sel=%d col=%d mouse=%d,%d)",
                    mouseActionName(a),
                    frontendFlow->mouse().selection(),
                    frontendFlow->mouse().column(),
                    frontendFlow->mouse().mouseX(),
                    frontendFlow->mouse().mouseY());
        }
        if (frontendFlow->screen() == FrontendScreen::Options) {
          // Esc/row-3 consumed -> mode 0x0b -> options resumed at
          // the Mouse row (sel 3 — _DAT_0054bd34 untouched).
          mouseResumeSelection = frontendFlow->options().selection();
          log::info(kTag,
                    "front-end flow: mouse -> options (resume "
                    "sel=%d mouse=%d,%d dirty=%d on=%d yrev=%u "
                    "axes=%s buttA=%u)",
                    mouseResumeSelection,
                    frontendFlow->options().mouseX(),
                    frontendFlow->options().mouseY(),
                    frontendFlow->options().settingsDirty() ? 1 : 0,
                    frontendFlow->mouseOn() ? 1 : 0,
                    frontendFlow->mouseYReversedBits(),
                    frontendFlow->mouseAxesMap().c_str(),
                    frontendFlow->mouseButtMap()[0]);
        }
      } else if (frontendFlow->screen() == FrontendScreen::Keyboard) {
        const KeyboardAction a = frontendFlow->consumeKeyboardAction();
        if (a != KeyboardAction::None) {
          log::info(kTag,
                    "keyboard action: %s (sel=%d cap=%d mouse=%d,%d)",
                    keyboardActionName(a),
                    frontendFlow->keyboard().selection(),
                    frontendFlow->keyboard().capture() ? 1 : 0,
                    frontendFlow->keyboard().mouseX(),
                    frontendFlow->keyboard().mouseY());
        }
        if (frontendFlow->screen() == FrontendScreen::Options) {
          // Esc/KM_QUIT consumed -> mode 0x0b -> options resumed at
          // the Keyboard row (sel 4 — _DAT_0054bd34 untouched).
          keyboardResumeSelection =
              frontendFlow->options().selection();
          const auto& kg = frontendFlow->keyGlobals();
          log::info(kTag,
                    "front-end flow: keyboard -> options (resume "
                    "sel=%d mouse=%d,%d dirty=%d snipe=%d fire=%d "
                    "use=%d hidden14=%d)",
                    keyboardResumeSelection,
                    frontendFlow->options().mouseX(),
                    frontendFlow->options().mouseY(),
                    frontendFlow->options().settingsDirty() ? 1 : 0,
                    kg[7], kg[6], kg[26], kg[14]);
        }
      } else if (frontendFlow->inOptions()) {
        // Record the dirty flag consumed by FUN_00420d68 before the
        // transition eats it — the persist gate for this exit.
        if (frontendFlow->options().pendingAction() ==
            OptionsAction::Back) {
          optionsExitDirty.push_back(
              frontendFlow->options().settingsDirty());
        }
        const OptionsAction a = frontendFlow->consumeOptionsAction();
        if (a != OptionsAction::None) {
          frontendLastOptionsAction = a;
          log::info(kTag, "options action: %s (sel=%d mouse=%d,%d)",
                    optionsActionName(a),
                    frontendFlow->options().selection(),
                    frontendFlow->options().mouseX(),
                    frontendFlow->options().mouseY());
        }
        if (frontendFlow->screen() == FrontendScreen::Display) {
          // Display consumed -> FUN_0041d020 -> child entered.
          displayEntered = true;
          displayEntrySelection = frontendFlow->display().selection();
          log::info(kTag,
                    "front-end flow: options -> display (entry "
                    "sel=%d mouse=%d,%d brightness=%d pcorrect=%d)",
                    displayEntrySelection,
                    frontendFlow->display().mouseX(),
                    frontendFlow->display().mouseY(),
                    frontendFlow->display().brightness(),
                    frontendFlow->display().forcePCorrect() ? 1 : 0);
        } else if (frontendFlow->screen() == FrontendScreen::Sound) {
          // Sound consumed -> FUN_0042322c -> child entered.
          // DAT_0054bdbc is a process global — first entry 0, later
          // entries resume it (FUN_0042322c never writes it).
          soundEntered = true;
          soundEntrySelection = frontendFlow->sound().selection();
          log::info(kTag,
                    "front-end flow: options -> sound (entry "
                    "sel=%d mouse=%d,%d fx=%d mus=%d dirty=%d)",
                    soundEntrySelection,
                    frontendFlow->sound().mouseX(),
                    frontendFlow->sound().mouseY(),
                    frontendFlow->sound().soundFx(),
                    frontendFlow->sound().soundMusic(),
                    frontendFlow->sound().settingsDirty() ? 1 : 0);
        } else if (frontendFlow->screen() == FrontendScreen::Mouse) {
          // Mouse consumed -> FUN_00421664 -> child entered.
          // DAT_0054bd40/38 ARE reset at entry — always 0.
          mouseEntered = true;
          mouseEntrySelection = frontendFlow->mouse().selection();
          log::info(kTag,
                    "front-end flow: options -> mouse (entry "
                    "sel=%d col=%d mouse=%d,%d on=%d yrev=%u "
                    "axes=%s dirty=%d)",
                    mouseEntrySelection,
                    frontendFlow->mouse().column(),
                    frontendFlow->mouse().mouseX(),
                    frontendFlow->mouse().mouseY(),
                    frontendFlow->mouseOn() ? 1 : 0,
                    frontendFlow->mouseYReversedBits(),
                    frontendFlow->mouseAxesMap().c_str(),
                    frontendFlow->options().settingsDirty() ? 1 : 0);
        } else if (frontendFlow->screen() ==
                   FrontendScreen::Keyboard) {
          // Keyboard consumed -> FUN_0041f030 -> child entered.
          // DAT_0054bcac = 0x14 — the entry selection is always the
          // KM_QUIT row; the 29-dword block is the flow's process
          // global (mutations persist across re-entries).
          keyboardEntered = true;
          keyboardEntrySelection =
              frontendFlow->keyboard().selection();
          const auto& kg = frontendFlow->keyGlobals();
          keyboardEntryKeySniper = kg[7];
          log::info(kTag,
                    "front-end flow: options -> keyboard (entry "
                    "sel=%d mouse=%d,%d left=%d snipe=%d use=%d "
                    "hidden14=%d dirty=%d)",
                    keyboardEntrySelection,
                    frontendFlow->keyboard().mouseX(),
                    frontendFlow->keyboard().mouseY(),
                    kg[0], kg[7], kg[26], kg[14],
                    frontendFlow->options().settingsDirty() ? 1 : 0);
        } else if (frontendFlow->screen() == FrontendScreen::Root) {
          // Back/Esc consumed -> FUN_00420d68 -> root restored.
          frontendReturnedToRoot = true;
          log::info(kTag, "front-end flow: options -> root (sel=%d "
                    "mouse=%d,%d)", frontendFlow->root().selection(),
                    frontendFlow->root().mouseX(),
                    frontendFlow->root().mouseY());
        }
      } else {
        const FrontendAction a = frontendFlow->consumeRootAction();
        if (a != FrontendAction::None) {
          frontendLastAction = a;
          // Semantic event only — downstream systems deferred. Quit is
          // proven to close the native preview (maps the original's
          // DAT_0054148e quit-flag write).
          log::info(kTag, "frontend action: %s (sel=%d mouse=%d,%d)",
                    frontendActionName(a), frontendFlow->root().selection(),
                    frontendFlow->root().mouseX(),
                    frontendFlow->root().mouseY());
          if (a == FrontendAction::Quit) {
            dispatcher.requestQuit();
          }
        }
        if (frontendFlow->inOptions()) {
          // OpenOptions consumed -> FUN_00420cf0 -> options entered.
          frontendEnteredOptions = true;
          optionsEntrySkills.push_back(
              frontendFlow->options().skill());
          log::info(kTag, "front-end flow: root -> options "
                    "(entry sel=%d mouse=%d,%d skill=%d)",
                    frontendFlow->options().selection(),
                    frontendFlow->options().mouseX(),
                    frontendFlow->options().mouseY(),
                    frontendFlow->options().skill());
        }
      }
    } else if (!previewMode) {
      scene.render(fb, palette);
    }
    if (!presenter->present(fb, palette)) {
      log::warn(kTag, "present failed (frame %llu)",
                static_cast<unsigned long long>(t.index));
    }

    if (t.index % 30 == 0) {
      char title[96];
      std::snprintf(title, sizeof(title), "MDK-Native — frame %llu",
                    static_cast<unsigned long long>(t.index));
      host.setTitle(title);
    }
    if (cfg_.frames != 0 && t.index + 1 >= cfg_.frames) {
      dispatcher.requestQuit();
    }
  }

  // Deterministic dynamic-frame digests for the snapshot record —
  // same domains as the Phase 4D static preview.
  if (frontendCtl || frontendFlow) {
    const bool inOpts = frontendFlow && frontendFlow->inOptions();
    const bool inDisp =
        frontendFlow &&
        frontendFlow->screen() == FrontendScreen::Display;
    const bool inSnd =
        frontendFlow &&
        frontendFlow->screen() == FrontendScreen::Sound;
    const bool inMse =
        frontendFlow &&
        frontendFlow->screen() == FrontendScreen::Mouse;
    const bool inKbd =
        frontendFlow &&
        frontendFlow->screen() == FrontendScreen::Keyboard;
    log::info(kTag,
              "interactive front-end last frame: fb=%016llx "
              "palette=%016llx screen=%s sel=%d skill=%d "
              "brightness=%d rampAcc=%.2f",
              static_cast<unsigned long long>(digestIndexedFb(fb)),
              static_cast<unsigned long long>(digestPalette(palette)),
              inDisp ? "display" : inSnd ? "sound"
                     : inMse ? "mouse"
                     : inKbd ? "keyboard"
                     : inOpts ? "options" : "root",
              inDisp ? frontendFlow->display().selection()
              : inSnd ? frontendFlow->sound().selection()
              : inMse ? frontendFlow->mouse().selection()
              : inKbd ? frontendFlow->keyboard().selection()
              : inOpts ? frontendFlow->options().selection()
                     : (frontendCtl ? frontendCtl->selection()
                                    : frontendFlow->root().selection()),
              inOpts ? frontendFlow->options().skill() : -1,
              frontendFlow ? frontendFlow->brightness() : 0,
              inDisp ? frontendFlow->display().rampAccumulator()
              : inSnd ? frontendFlow->sound().rampAccumulator()
              : inMse ? 0.0f
              : inOpts ? frontendFlow->options().rampAccumulator()
                     : (frontendCtl
                            ? frontendCtl->rampAccumulator()
                            : frontendFlow->root().rampAccumulator()));
  }

  // Interactive selftest verdicts.
  if (cfg_.selftest && frontendCtl) {
    // Phase 4E root-only regression: the scripted sequence must leave
    // selection 3 (Options) chosen, OpenOptions emitted, and the
    // arrow at (300,139).
    selftestOk_ = selftestOk_ &&
                  frontendCtl->selection() == 3 &&
                  frontendLastAction == FrontendAction::OpenOptions &&
                  frontendCtl->mouseX() == 300 &&
                  frontendCtl->mouseY() == 139;
    log::info(kTag,
              "frontend selftest (root-only): %s (sel=%d mouse=%d,%d "
              "last=%s)",
              selftestOk_ ? "PASS" : "FAIL", frontendCtl->selection(),
              frontendCtl->mouseX(), frontendCtl->mouseY(),
              frontendActionName(frontendLastAction));
  }
  if (cfg_.selftest && frontendFlow) {
    // Phase 4I four-screen + persistence script:
    //   root nav -> options entry (sel 8) -> skill band -> RIGHT ->
    //   Enter -> LEFT -> LEFT -> RIGHT -> Esc (persist #1: Skill
    //   only) -> Enter (re-entry; skill retained process-lifetime)
    //   -> motion to the Display row -> Enter (FUN_0041d020, child
    //   entry sel 2) -> motion to band 0 -> RIGHT -> Enter
    //   (brightness 0->2) -> motion to band 1 -> Enter (ForcePCorrect
    //   -> TRUE) -> motion to band 2 -> Enter (FUN_0041d144 ->
    //   options resumes sel 7) -> Esc (FUN_00420d68 -> persist #2:
    //   Skill + Brightness + ForcePCorrect) -> Enter (entry 3 — the
    //   triple survives process-lifetime) -> Esc (clean exit, no
    //   persist) -> Enter (entry 4) -> motion to the Sound row ->
    //   Enter (FUN_0042322c, entry sel 0 — DAT_0054bdbc is BSS-zero)
    //   -> RIGHT (SoundFX +10) -> DOWN (sel 1) -> LEFT (SoundMusic
    //   -10) -> DOWN (sel 2) -> Enter (FUN_00423280 -> options
    //   resumes sel 1) -> Esc (FUN_00420d68 -> persist #3: all five
    //   settings) -> Enter (entry 5) -> Esc (clean exit) -> settled
    //   root frame.
    auto wrapUp = [](int s) { return s >= 2 ? 0 : s + 1; };
    auto wrapDn = [](int s) { return s <= 0 ? 2 : s - 1; };
    int expected = settingsInitialSkill;
    expected = wrapUp(expected);   // RIGHT
    expected = wrapUp(expected);   // Enter
    expected = wrapDn(expected);   // LEFT
    expected = wrapDn(expected);   // LEFT
    expected = wrapUp(expected);   // RIGHT — final persisted value
    // Volumes: one RIGHT on row 0 (+10 clamp 100), one LEFT on
    // row 1 (-10 clamp 0) — the final persisted pair.
    const int expectedFx =
        std::min(settingsInitialSoundFx + 10, kSoundVolumeMax);
    const int expectedMus =
        std::max(settingsInitialSoundMusic - 10, kSoundVolumeMin);
    // Display leg: RIGHT + Enter on row 0 (+1 wrap >=8->0 each),
    // one toggle on row 1 — computed from the loaded start.
    const int expectedBright = (settingsInitialBrightness + 2) % 8;
    const int expectedPcorrect = settingsInitialPcorrect ? 0 : 1;
    // Mouse leg (Phase 4J): one row-1 toggle, one row-2 toggle,
    // one grid-row-0 bit toggle on column 0 — computed from the
    // loaded W-set values (FUN_00421774 exclusivity for row 0:
    // bit set -> clear it; clear -> (mask & ~0x2) | 1).
    const int expectedMouseOn = settingsInitialMouseOn ? 0 : 1;
    const std::uint32_t expectedMouseYRev =
        settingsInitialMouseYRev != 0 ? 0u : 1u;
    const std::uint32_t expectedButtA =
        (settingsInitialButtA & 1u)
            ? (settingsInitialButtA & ~1u)
            : ((settingsInitialButtA & ~0x2u) | 1u);
    // Nine options entries: initial config, post-persist-#1,
    // post-display, post-sound-entry, post-persist-#3, post-mouse-
    // entry, post-persist-#4, post-keyboard-entry, and
    // post-persist-#5 — the settings survive process-lifetime.
    const bool entrySkillsOk =
        optionsEntrySkills.size() == 9 &&
        optionsEntrySkills[0] == settingsInitialSkill &&
        optionsEntrySkills[1] == expected &&
        optionsEntrySkills[2] == expected &&
        optionsEntrySkills[3] == expected &&
        optionsEntrySkills[4] == expected &&
        optionsEntrySkills[5] == expected &&
        optionsEntrySkills[6] == expected &&
        optionsEntrySkills[7] == expected &&
        optionsEntrySkills[8] == expected;
    // Exits 1, 2, 4, 6, 8 are dirty (skill mutations, then each
    // child's — all carried back through the shared DAT_00541486);
    // exits 3, 5, 7, 9 are clean — the preceding persists cleared
    // the flag.
    const bool exitsOk =
        optionsExitDirty.size() == 9 && optionsExitDirty[0] &&
        optionsExitDirty[1] && !optionsExitDirty[2] &&
        optionsExitDirty[3] && !optionsExitDirty[4] &&
        optionsExitDirty[5] && !optionsExitDirty[6] &&
        optionsExitDirty[7] && !optionsExitDirty[8];
    // The proven audio-trigger sequence for the whole run — entry
    // (ambient stop + OPTSONG start), per-query OPTBUTT + the two
    // FUN_004024c4 volume applies, exit (OPTSONG stop + ambient
    // restart). Esc on the sound screen emits no OPTBUTT — the
    // script exits via row-2 activate instead.
    const std::vector<SoundAudioEvent> expectedAudio{
        SoundAudioEvent::AmbientSongStop,
        SoundAudioEvent::SongStart,
        SoundAudioEvent::Button,           // RIGHT row 0
        SoundAudioEvent::VolumesApplied,
        SoundAudioEvent::Button,           // DOWN
        SoundAudioEvent::Button,           // LEFT row 1
        SoundAudioEvent::VolumesApplied,
        SoundAudioEvent::Button,           // DOWN
        SoundAudioEvent::Button,           // activate row 2
        SoundAudioEvent::SongStop,
        SoundAudioEvent::AmbientSongStart,
    };
    const bool audioOk = audioEventLog == expectedAudio;
    // Keyboard leg (Phase 4K): reset on row 19 (dirty latches even
    // though every visible value was already factory), then one
    // deterministic rebind — the injected SDL X tap lands internal
    // code 45 through the scancode->DIK->internal seam, exactly the
    // device path the original's capture poll sees.
    const int expectedKeySniper = 45;  // DIK 0x2d -> internal 45
    // With --settings-file the persisted file must hold the final
    // settings tuple — re-read here for the verdict.
    bool fileOk = true;
    if (cfg_.settingsFile) {
      std::string ferr;
      const auto disk =
          loadFrontendSettingsFile(*cfg_.settingsFile, &ferr);
      fileOk = disk && disk->settings.skill == expected &&
               disk->settings.brightness == expectedBright &&
               disk->settings.forcePCorrect ==
                   (expectedPcorrect != 0) &&
               disk->settings.soundFx == expectedFx &&
               disk->settings.soundMusic == expectedMus &&
               disk->settings.mouseOn == (expectedMouseOn != 0) &&
               disk->settings.mouseYReversed == expectedMouseYRev &&
               disk->settings.mouseWButtMapA == expectedButtA &&
               disk->settings.mouseWAxesMap ==
                   settingsInitialAxesMap &&
               disk->settings.keySniper == expectedKeySniper;
    }
    selftestOk_ = selftestOk_ && frontendEnteredOptions &&
                  frontendReturnedToRoot &&
                  frontendFlow->screen() == FrontendScreen::Root &&
                  frontendLastOptionsAction ==
                      OptionsAction::SkillCycleNext &&
                  frontendFlow->root().selection() == 3 &&
                  frontendFlow->root().mouseX() == 300 &&
                  frontendFlow->root().mouseY() == 20 &&
                  entrySkillsOk && exitsOk &&
                  displayEntered &&
                  displayEntrySelection == kDisplayEntrySelection &&
                  optionsResumeSelection == 7 &&
                  displayFramesDrawn > 0 &&
                  frontendFlow->brightness() == expectedBright &&
                  frontendFlow->forcePCorrect() ==
                      (expectedPcorrect != 0) &&
                  soundEntered &&
                  soundEntrySelection == 0 &&
                  soundResumeSelection == 1 &&
                  soundFramesDrawn == 4 &&
                  frontendFlow->soundFx() == expectedFx &&
                  frontendFlow->soundMusic() == expectedMus &&
                  audioOk &&
                  mouseEntered &&
                  mouseEntrySelection == 0 &&
                  mouseResumeSelection == 3 &&
                  mouseFramesDrawn == 6 &&
                  frontendFlow->mouseOn() == (expectedMouseOn != 0) &&
                  frontendFlow->mouseYReversedBits() ==
                      expectedMouseYRev &&
                  frontendFlow->mouseButtMap()[0] == expectedButtA &&
                  frontendFlow->mouseAxesMap() ==
                      settingsInitialAxesMap &&
                  keyboardEntered &&
                  keyboardEntrySelection == kKeyboardEntrySelection &&
                  keyboardResumeSelection == 4 &&
                  keyboardFramesDrawn == 8 &&
                  keyboardCaptureSeen &&
                  keyboardEntryKeySniper == settingsInitialKeySniper &&
                  frontendFlow->keyGlobals()[7] == expectedKeySniper &&
                  frontendFlow->keyGlobals()[14] == 2 &&
                  settingsPersistCalls == 5 &&
                  settingsPersistedSkill == expected &&
                  settingsPersistedBrightness == expectedBright &&
                  settingsPersistedForcePCorrect == expectedPcorrect &&
                  settingsPersistedSoundFx == expectedFx &&
                  settingsPersistedSoundMusic == expectedMus &&
                  settingsPersistedMouseOn == expectedMouseOn &&
                  settingsPersistedMouseYRev == expectedMouseYRev &&
                  settingsPersistedButtA == expectedButtA &&
                  settingsPersistedAxesMap == settingsInitialAxesMap &&
                  settingsPersistedKeySniper == expectedKeySniper &&
                  frontendFlow->skill() == expected &&
                  !frontendFlow->settingsDirty() && fileOk;
    log::info(kTag,
              "frontend selftest (six-screen): %s (entries=%d "
              "entry-skills=%d,%d,%d,%d,%d,%d,%d,%d,%d "
              "exit-dirty=%d,%d,%d,%d,%d,%d,%d,%d,%d "
              "persists=%d persisted=%d,%d,%d,%d,%d keySniper=%d "
              "skill=%d bright=%d pcorr=%d fx=%d mus=%d dirty=%d "
              "display-entry=%d resume-sel=%d display-frames=%d "
              "display-fb=%016llx display-pal=%016llx "
              "sound-entry=%d sound-resume=%d sound-frames=%d "
              "sound-fb=%016llx sound-pal=%016llx audio-events=%d "
              "mouse-entry=%d mouse-resume=%d mouse-frames=%d "
              "mouse-fb=%016llx mouse-pal=%016llx "
              "mouseOn=%d yrev=%u axes=%s buttA=%u "
              "kb-entry=%d kb-resume=%d kb-frames=%d kb-cap=%d "
              "kb-fb=%016llx kb-pal=%016llx keySnipe=%d hidden14=%d "
              "root sel=%d mouse=%d,%d last-options=%s)",
              selftestOk_ ? "PASS" : "FAIL",
              static_cast<int>(optionsEntrySkills.size()),
              optionsEntrySkills.size() > 0 ? optionsEntrySkills[0]
                                            : -1,
              optionsEntrySkills.size() > 1 ? optionsEntrySkills[1]
                                            : -1,
              optionsEntrySkills.size() > 2 ? optionsEntrySkills[2]
                                            : -1,
              optionsEntrySkills.size() > 3 ? optionsEntrySkills[3]
                                            : -1,
              optionsEntrySkills.size() > 4 ? optionsEntrySkills[4]
                                            : -1,
              optionsEntrySkills.size() > 5 ? optionsEntrySkills[5]
                                            : -1,
              optionsEntrySkills.size() > 6 ? optionsEntrySkills[6]
                                            : -1,
              optionsEntrySkills.size() > 7 ? optionsEntrySkills[7]
                                            : -1,
              optionsEntrySkills.size() > 8 ? optionsEntrySkills[8]
                                            : -1,
              optionsExitDirty.size() > 0 ? optionsExitDirty[0] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 1 ? optionsExitDirty[1] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 2 ? optionsExitDirty[2] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 3 ? optionsExitDirty[3] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 4 ? optionsExitDirty[4] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 5 ? optionsExitDirty[5] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 6 ? optionsExitDirty[6] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 7 ? optionsExitDirty[7] ? 1 : 0
                                          : -1,
              optionsExitDirty.size() > 8 ? optionsExitDirty[8] ? 1 : 0
                                          : -1,
              settingsPersistCalls, settingsPersistedSkill,
              settingsPersistedBrightness,
              settingsPersistedForcePCorrect,
              settingsPersistedSoundFx, settingsPersistedSoundMusic,
              settingsPersistedKeySniper,
              frontendFlow->skill(),
              frontendFlow->brightness(),
              frontendFlow->forcePCorrect() ? 1 : 0,
              frontendFlow->soundFx(), frontendFlow->soundMusic(),
              frontendFlow->settingsDirty() ? 1 : 0,
              displayEntrySelection, optionsResumeSelection,
              displayFramesDrawn,
              static_cast<unsigned long long>(displayLastFbDigest),
              static_cast<unsigned long long>(displayLastPalDigest),
              soundEntrySelection, soundResumeSelection,
              soundFramesDrawn,
              static_cast<unsigned long long>(soundLastFbDigest),
              static_cast<unsigned long long>(soundLastPalDigest),
              static_cast<int>(audioEventLog.size()),
              mouseEntrySelection, mouseResumeSelection,
              mouseFramesDrawn,
              static_cast<unsigned long long>(mouseLastFbDigest),
              static_cast<unsigned long long>(mouseLastPalDigest),
              frontendFlow->mouseOn() ? 1 : 0,
              frontendFlow->mouseYReversedBits(),
              frontendFlow->mouseAxesMap().c_str(),
              frontendFlow->mouseButtMap()[0],
              keyboardEntrySelection, keyboardResumeSelection,
              keyboardFramesDrawn, keyboardCaptureSeen ? 1 : 0,
              static_cast<unsigned long long>(keyboardLastFbDigest),
              static_cast<unsigned long long>(keyboardLastPalDigest),
              frontendFlow->keyGlobals()[7],
              frontendFlow->keyGlobals()[14],
              frontendFlow->root().selection(),
              frontendFlow->root().mouseX(),
              frontendFlow->root().mouseY(),
              optionsActionName(frontendLastOptionsAction));
  }

  if (cfg_.dumpPpm) {
    // Debug verification hook: write the last presented frame (post
    // palette expansion — exactly what was uploaded to Metal) as P6 PPM.
    std::vector<std::uint8_t> bgra(fb.pixelCount() * 4);
    expandToBGRA(fb, palette, bgra.data());
    if (FILE* f = std::fopen(cfg_.dumpPpm->c_str(), "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", fb.width(), fb.height());
      for (std::size_t i = 0; i < fb.pixelCount(); ++i) {
        std::fputc(bgra[i * 4 + 2], f); // R
        std::fputc(bgra[i * 4 + 1], f); // G
        std::fputc(bgra[i * 4 + 0], f); // B
      }
      std::fclose(f);
      log::info(kTag, "dumped frame %llu to %s",
                static_cast<unsigned long long>(clock.frameCount() - 1),
                cfg_.dumpPpm->c_str());
    } else {
      log::warn(kTag, "could not write %s", cfg_.dumpPpm->c_str());
    }
  }

  presenter.reset(); // Metal view must die before the window
  host.shutdown();
  log::info(kTag, "shutdown complete after %llu frames",
            static_cast<unsigned long long>(clock.frameCount()));
  return selftestOk_ ? 0 : 3;
}

bool parseArgs(int argc, char** argv, AppConfig& cfg, std::string& error,
               bool& showHelp) {
  showHelp = false;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto needValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        error = std::string(name) + " requires a value";
        return nullptr;
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
      showHelp = true;
      return true;
    } else if (!std::strcmp(a, "--data-path")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.dataPath = v;
    } else if (!std::strcmp(a, "--dump-ppm")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.dumpPpm = v;
    } else if (!std::strcmp(a, "--frames")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.frames = std::strtoull(v, nullptr, 10);
    } else if (!std::strcmp(a, "--preview-resource")) {
      const char* f = needValue(a);
      if (!f) return false;
      const char* r = needValue(a);
      if (!r) return false;
      cfg.previewFile = f;
      cfg.previewRecord = r;
    } else if (!std::strcmp(a, "--preview-font")) {
      const char* f = needValue(a);
      if (!f) return false;
      const char* r = needValue(a);
      if (!r) return false;
      cfg.fontPreviewFile = f;
      cfg.fontPreviewRecord = r;
      // Optional trailing TEXT argument (must not look like a flag).
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        cfg.fontPreviewText = argv[++i];
      }
    } else if (!std::strcmp(a, "--preview-sprite")) {
      const char* f = needValue(a);
      if (!f) return false;
      const char* r = needValue(a);
      if (!r) return false;
      cfg.spritePreviewFile = f;
      cfg.spritePreviewRecord = r;
    } else if (!std::strcmp(a, "--preview-options")) {
      cfg.optionsPreview = true;
    } else if (!std::strcmp(a, "--preview-options-submenu")) {
      cfg.optionsSubmenuPreview = true;
    } else if (!std::strcmp(a, "--preview-display-submenu")) {
      cfg.displaySubmenuPreview = true;
    } else if (!std::strcmp(a, "--preview-sound-submenu")) {
      cfg.soundSubmenuPreview = true;
    } else if (!std::strcmp(a, "--preview-mouse-submenu")) {
      cfg.mouseSubmenuPreview = true;
    } else if (!std::strcmp(a, "--preview-keyboard-submenu")) {
      cfg.keyboardSubmenuPreview = true;
    } else if (!std::strcmp(a, "--interactive-frontend")) {
      cfg.interactiveFrontend = true;
    } else if (!std::strcmp(a, "--frontend-root-only")) {
      cfg.frontendRootOnly = true;
    } else if (!std::strcmp(a, "--settings-file")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.settingsFile = v;
    } else if (!std::strcmp(a, "--selftest")) {
      cfg.selftest = true;
    } else if (!std::strcmp(a, "--no-relative-mouse")) {
      cfg.relativeMouse = false;
    } else {
      error = std::string("unknown argument: ") + a;
      return false;
    }
  }
  return true;
}

} // namespace mdk
