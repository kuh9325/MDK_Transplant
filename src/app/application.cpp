#include "app/application.h"

#include "app/diagnostic_scene.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/clock.h"
#include "core/compat.h"
#include "core/data_root.h"
#include "core/display_menu.h"
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

// Phase 4E — translate the platform InputState into the controller's
// semantic per-frame input. Original reference points:
//   prevHeld/nextHeld : DIK_UP/DIK_DOWN with the original keymap's
//     "held OR pressed during this poll" semantics (mapA bits 103/108).
//   confirmEdge       : DIK_RETURN non-repeat press edge (bit 28).
//   attractEdge       : DIK_RIGHT non-repeat press edge (bit 106).
//   mouseDx/Dy        : integer device deltas — SDL's float pixel
//     deltas truncate toward zero (nearest integer-domain model).
//   mouseButtons      : 4-bit nibble bit i = button i+1 held.
static FrontendMenuInput frontendInputFromSdl(const InputState& input) {
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
  fi.mouseButtons = static_cast<std::uint8_t>(
      (input.mouseButtonDown(SDL_BUTTON_LEFT) ? 0x1 : 0) |
      (input.mouseButtonDown(SDL_BUTTON_RIGHT) ? 0x2 : 0) |
      (input.mouseButtonDown(SDL_BUTTON_MIDDLE) ? 0x4 : 0) |
      (input.mouseButtonDown(SDL_BUTTON_X1) ? 0x8 : 0));
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
          log::info(kTag,
                    "settings: loaded %s (skill=%d brightness=%d "
                    "pcorrect=%d fx=%d mus=%d ignored=%d,%d,%d,%d)",
                    cfg_.settingsFile->string().c_str(),
                    initialSettings.skill, initialSettings.brightness,
                    initialSettings.forcePCorrect ? 1 : 0,
                    initialSettings.soundFx, initialSettings.soundMusic,
                    loaded->ignoredSkillLines,
                    loaded->ignoredBrightnessLines,
                    loaded->ignoredSoundFxLines,
                    loaded->ignoredSoundMusicLines);
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
      // Phase 4I four-screen script: 36 steps (0..35) — the run
      // quits right after the last injected step.
      cfg_.frames = frontendFlow ? 36 : 10;
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
      const FrontendMenuInput fi = frontendInputFromSdl(input);
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
    log::info(kTag,
              "interactive front-end last frame: fb=%016llx "
              "palette=%016llx screen=%s sel=%d skill=%d "
              "brightness=%d rampAcc=%.2f",
              static_cast<unsigned long long>(digestIndexedFb(fb)),
              static_cast<unsigned long long>(digestPalette(palette)),
              inDisp ? "display" : inSnd ? "sound"
                     : inOpts ? "options" : "root",
              inDisp ? frontendFlow->display().selection()
              : inSnd ? frontendFlow->sound().selection()
              : inOpts ? frontendFlow->options().selection()
                     : (frontendCtl ? frontendCtl->selection()
                                    : frontendFlow->root().selection()),
              inOpts ? frontendFlow->options().skill() : -1,
              frontendFlow ? frontendFlow->brightness() : 0,
              inDisp ? frontendFlow->display().rampAccumulator()
              : inSnd ? frontendFlow->sound().rampAccumulator()
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
    // Five options entries: initial config, post-persist-#1,
    // post-display, post-sound-entry, and post-persist-#3 — the
    // settings survive process-lifetime.
    const bool entrySkillsOk =
        optionsEntrySkills.size() == 5 &&
        optionsEntrySkills[0] == settingsInitialSkill &&
        optionsEntrySkills[1] == expected &&
        optionsEntrySkills[2] == expected &&
        optionsEntrySkills[3] == expected &&
        optionsEntrySkills[4] == expected;
    // Exits 1, 2, 4 are dirty (skill mutations, then the display
    // child's, then the sound child's — all carried back through
    // the shared DAT_00541486); exits 3 and 5 are clean — the
    // preceding persists cleared the flag.
    const bool exitsOk =
        optionsExitDirty.size() == 5 && optionsExitDirty[0] &&
        optionsExitDirty[1] && !optionsExitDirty[2] &&
        optionsExitDirty[3] && !optionsExitDirty[4];
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
    // With --settings-file the persisted file must hold the final
    // five-tuple — re-read here for the verdict.
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
               disk->settings.soundMusic == expectedMus;
    }
    selftestOk_ = selftestOk_ && frontendEnteredOptions &&
                  frontendReturnedToRoot &&
                  frontendFlow->screen() == FrontendScreen::Root &&
                  frontendLastOptionsAction ==
                      OptionsAction::SkillCycleNext &&
                  frontendFlow->root().selection() == 3 &&
                  frontendFlow->root().mouseX() == 300 &&
                  frontendFlow->root().mouseY() == 90 &&
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
                  settingsPersistCalls == 3 &&
                  settingsPersistedSkill == expected &&
                  settingsPersistedBrightness == expectedBright &&
                  settingsPersistedForcePCorrect == expectedPcorrect &&
                  settingsPersistedSoundFx == expectedFx &&
                  settingsPersistedSoundMusic == expectedMus &&
                  frontendFlow->skill() == expected &&
                  !frontendFlow->settingsDirty() && fileOk;
    log::info(kTag,
              "frontend selftest (four-screen): %s (entries=%d "
              "entry-skills=%d,%d,%d,%d,%d exit-dirty=%d,%d,%d,%d,%d "
              "persists=%d persisted=%d,%d,%d,%d,%d skill=%d "
              "bright=%d pcorr=%d fx=%d mus=%d dirty=%d "
              "display-entry=%d resume-sel=%d display-frames=%d "
              "display-fb=%016llx display-pal=%016llx "
              "sound-entry=%d sound-resume=%d sound-frames=%d "
              "sound-fb=%016llx sound-pal=%016llx audio-events=%d "
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
              settingsPersistCalls, settingsPersistedSkill,
              settingsPersistedBrightness,
              settingsPersistedForcePCorrect,
              settingsPersistedSoundFx, settingsPersistedSoundMusic,
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
