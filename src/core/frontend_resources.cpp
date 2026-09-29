// Phase 18B.2A — shared frontend resource loader. Moved verbatim
// from src/app/application.cpp so the Godot bridge presenter binds
// the same decoded records; the overlay/dialog strings were added
// to the same load path (same record family, same NUL-terminated
// shape — OBSERVED).
#include "core/frontend_resources.h"

#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/data_root.h"
#include "core/display_menu.h"
#include "core/frontend_menu.h"
#include "core/fti_directory.h"
#include "core/sni_directory.h"
#include "core/sound_menu.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace mdk {

static constexpr std::size_t kFrontendResMaxBytes =
    512ull * 1024 * 1024;

bool loadFrontendResources(DataRoot& root, FrontendResources& res,
                           std::string* err) {
  // Backdrop: MISC/OPTIONS.BNI record MDKOPT (Phase 4A decoder).
  const auto bni =
      root.readFile("MISC/OPTIONS.BNI", kFrontendResMaxBytes, err);
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
  const auto fti =
      root.readFile("MISC/MDKFONT.FTI", kFrontendResMaxBytes, err);
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

  // Phase 18B.2A — the overlay/dialog records (sub-modes 1/8/9/10 +
  // PAUSED/OPTSTRT/SVBAD/SV_FAIL). Same NUL-terminated shape.
  if (!loadCStr("OPTSTRT", res.optStrt) ||
      !loadCStr("SVOPT1", res.svOpt1) ||
      !loadCStr("SVOPT2", res.svOpt2) ||
      !loadCStr("SVOPT3", res.svOpt3) ||
      !loadCStr("SV_TITLE", res.svTitle) ||
      !loadCStr("SV_ASK", res.svAsk) ||
      !loadCStr("SVBAD", res.svBad) ||
      !loadCStr("SV_FAIL", res.svFail) ||
      !loadCStr("PAUSED", res.paused) ||
      !loadCStr("ABORT1", res.abort1) ||
      !loadCStr("ABORT2", res.abort2) ||
      !loadCStr("ABORT3", res.abort3) ||
      !loadCStr("HELP_TOP", res.helpTop) ||
      !loadCStr("HELP_BOT", res.helpBot)) {
    return false;
  }
  for (int i = 0; i < 18; ++i) {
    char name[10];
    std::snprintf(name, sizeof(name), "HELP_%02d", i + 1);
    if (!loadCStr(name, res.helpLines[i])) return false;
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
      root.readFile(kSoundSniFile, kFrontendResMaxBytes, err);
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

}  // namespace mdk
