#include "frontend_presenter.h"

#include "core/abort_console.h"
#include "core/display_menu.h"
#include "core/frontend_machines.h"
#include "core/frontend_palette.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/keyboard_menu.h"
#include "core/mouse_menu.h"
#include "core/options_menu.h"
#include "core/save_name_entry.h"
#include "core/save_slot_list.h"
#include "core/sound_menu.h"

#include <cstring>
#include <cstdio>

namespace mdkbridge {

namespace {

// PRESENTATION CHOICE — save-list selection band: SYS_PAL index 34
// is the darkest gray ramp entry (59,67,67) — visible against the
// black dialog background without obscuring FONTSML text. The
// original's exact highlight treatment is UNKNOWN.
constexpr std::uint8_t kHighlightIndex = 34;
// Save-name cursor fill (SYS_PAL white).
constexpr std::uint8_t kCursorIndex = 19;

// OBSERVED save-list row layout (docs/reverse-engineering): rows
// render at y = 0x67 + row*0x10, hit band 0x66 < y < 0x137, window
// of 13 rows.
constexpr int kSaveRowY0 = 0x67;
constexpr int kSaveRowStep = 0x10;
constexpr int kSaveVisibleRows = 13;
// PRESENTATION CHOICE — text column x for the stems (the original
// column x is UNKNOWN; 40 clears the thumbnail/detail pane).
constexpr int kSaveRowX = 40;
constexpr int kSaveRowWidth = 380;
// THMB preview placement (PRESENTATION CHOICE — upper-right pane).
constexpr int kThmbX = 480;
constexpr int kThmbY = 120;

// OBSERVED abort/save-confirm band (abort_console.h): the mouse hit
// band is (y - 0x95) / 0x24 — items anchor at y 0x95 and 0x95+0x24.
constexpr int kAbortItemY0 = 0x95;
constexpr int kAbortItemStep = 0x24;
// PRESENTATION CHOICE — dialog title row above the OBSERVED band.
constexpr int kDialogTitleY = 0x5f;

void fillRect(mdk::IndexedFramebuffer& fb, int x, int y, int w, int h,
              std::uint8_t index) {
  const int x0 = x < 0 ? 0 : x;
  const int y0 = y < 0 ? 0 : y;
  const int x1 = (x + w > fb.width()) ? fb.width() : x + w;
  const int y1 = (y + h > fb.height()) ? fb.height() : y + h;
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  for (int yy = y0; yy < y1; ++yy) {
    std::memset(fb.pixels() + yy * fb.stride() + x0, index,
                static_cast<std::size_t>(x1 - x0));
  }
}

// Split a record's embedded newlines into lines, each centered via
// the shared measure pattern (missingAdvance per font).
void drawCenteredLines(const mdk::FtiFont& font, std::string_view text,
                       mdk::IndexedFramebuffer& fb, int y, int lineStep,
                       int missingAdvance, float scale = 1.0f) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    const std::string_view line =
        (nl == std::string_view::npos)
            ? text.substr(pos)
            : text.substr(pos, nl - pos);
    const int w = mdk::measureFtiText(font, line, missingAdvance);
    const int x = static_cast<int>(
        (fb.width() - w * static_cast<double>(scale)) * 0.5);
    mdk::drawFtiTextScaled(font, line, fb, x, y, scale,
                           missingAdvance);
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
    y += lineStep;
  }
}

// The shared two-item confirm row (abort console / save-name autosave
// confirm): records ABORT2/ABORT3 at the OBSERVED band rows, selected
// item at full scale, the other at the unselected 0.65 the root/
// options family uses.
void drawConfirmItems(const mdk::FtiFont& font, std::string_view yes,
                      std::string_view no, int sel,
                      mdk::IndexedFramebuffer& fb) {
  const std::string_view items[2] = {yes, no};
  int maxW = 0;
  for (const auto t : items) {
    const int w =
        mdk::measureFtiText(font, t, mdk::kFtiFontBigMissingAdvance);
    if (w > maxW) {
      maxW = w;
    }
  }
  const int xArg = maxW / 2;
  for (int i = 0; i < 2; ++i) {
    const float scale =
        (i == sel) ? mdk::kFrontendScaleSelected
                   : mdk::kFrontendScaleUnselected;
    const int w = mdk::measureFtiText(font, items[i],
                                    mdk::kFtiFontBigMissingAdvance);
    const int x = static_cast<int>(
        static_cast<double>(xArg) -
        static_cast<double>(w) * static_cast<double>(scale) * 0.5);
    mdk::drawFtiTextScaled(font, items[i], fb, x,
                           kAbortItemY0 + i * kAbortItemStep, scale,
                           mdk::kFtiFontBigMissingAdvance);
  }
}

}  // namespace

void FrontendPresenter::dialogPalette(int brightness) {
  frame_.fb.clear(0);
  for (int i = 0; i < mdk::Palette::size(); ++i) {
    if (i < 64) {
      const auto* head = lastRes_->sysPalHead.data();
      frame_.palette.set(
          i, {static_cast<std::uint8_t>(head[i * 3]),
              static_cast<std::uint8_t>(head[i * 3 + 1]),
              static_cast<std::uint8_t>(head[i * 3 + 2]), 255});
    } else {
      frame_.palette.set(i, {0, 0, 0, 255});
    }
  }
  mdk::applyFrontendBrightness(frame_.palette, brightness);
}

void FrontendPresenter::drawCenteredBig(const mdk::FtiFont& font,
                                        std::string_view text, int y,
                                        float scale) {
  drawCenteredLines(font, text, frame_.fb, y, 20,
                    mdk::kFtiFontBigMissingAdvance, scale);
}

void FrontendPresenter::saveListScreen(
    mdk::FrontendShell& sh, const mdk::FrontendResources& res,
    const mdk::FrontendHostServices* host) {
  const mdk::SaveSlotListController* list = sh.saveList();
  // Title (record SVOPT1, two lines — OBSERVED text).
  drawCenteredLines(res.fontBig, res.svOpt1, frame_.fb, 0x14, 20,
                    mdk::kFtiFontBigMissingAdvance);

  if (!list || list->count() == 0) {
    // SVOPT3 — OBSERVED "No Saved Games Found".
    drawCenteredBig(res.fontBig, res.svOpt3, 0x100);
    return;
  }

  // 13-row window (OBSERVED) from topRow; stems FONTSML.
  const int rows = list->count() - list->topRow() < kSaveVisibleRows
                       ? list->count() - list->topRow()
                       : kSaveVisibleRows;
  for (int r = 0; r < rows; ++r) {
    const int idx = list->topRow() + r;
    const int y = kSaveRowY0 + r * kSaveRowStep;
    if (idx == list->selection()) {
      fillRect(frame_.fb, kSaveRowX - 8, y - 3, kSaveRowWidth,
               kSaveRowStep, kHighlightIndex);
    }
    mdk::drawFtiText(res.fontSml, list->stems()[idx], frame_.fb,
                     kSaveRowX, y, mdk::kFtiFontSmlMissingAdvance);
  }

  // Selected-slot detail pane. Summary comes from the controller;
  // the THMB payload is host-only (inspectSlotDetail).
  const mdk::SaveSlotSummary* sel = list->selected();
  if (!sel) {
    return;
  }
  char meta[64];
  if (!sel->valid) {
    // SVBAD — OBSERVED "Invalid/Corrupt File".
    mdk::drawFtiText(res.fontSml, res.svBad, frame_.fb, kSaveRowX + 300,
                     kSaveRowY0, mdk::kFtiFontSmlMissingAdvance);
    return;
  }
  std::snprintf(meta, sizeof(meta), "LEVEL %d  MODE %d", sel->levelId,
                sel->modeField);
  mdk::drawFtiText(res.fontSml, meta, frame_.fb, kThmbX - 40,
                   kThmbY + 54, mdk::kFtiFontSmlMissingAdvance);
  std::snprintf(meta, sizeof(meta), "HEALTH %d  DEATHS %d",
                sel->health, sel->deathCount);
  mdk::drawFtiText(res.fontSml, meta, frame_.fb, kThmbX - 40,
                   kThmbY + 66, mdk::kFtiFontSmlMissingAdvance);

  if (!sel->fullSave) {
    // Header-only saves: the original falls back to LOAD<level>.LBB
    // for the preview image. That format is NOT decoded (UNKNOWN) —
    // per the phase stop rule the pane presents metadata only, no
    // invented imagery.
    mdk::drawFtiText(res.fontSml, "HEADER ONLY", frame_.fb,
                     kThmbX - 40, kThmbY + 20,
                     mdk::kFtiFontSmlMissingAdvance);
    return;
  }

  // Existing THMB decode for presentation: 768-byte palette head +
  // 64x45 indexed pixels (OBSERVED record shape).
  if (host) {
    const auto detail = host->inspectSlotDetail(sel->name);
    if (detail && detail->thumbnail.size() == kFrontendThmbBytes) {
      std::memcpy(frame_.thmbPalette.data(), detail->thumbnail.data(),
                  kFrontendThmbPaletteBytes);
      std::memcpy(frame_.thmbPixels.data(),
                  detail->thumbnail.data() + kFrontendThmbPaletteBytes,
                  frame_.thmbPixels.size());
      frame_.hasThmb = true;
      frame_.thmbX = kThmbX;
      frame_.thmbY = kThmbY;
    }
  }
}

void FrontendPresenter::saveNameScreen(mdk::FrontendShell& sh,
                                       const mdk::FrontendResources& res) {
  const mdk::SaveNameEntryController* name = sh.saveName();
  if (!name) {
    return;
  }
  if (name->confirmPhase()) {
    // Autosave arm (DAT_0054bd9c == 0): SV_ASK + the shared
    // ABORT2/ABORT3 confirm row on the OBSERVED abort band.
    drawCenteredBig(res.fontBig, res.svAsk, kDialogTitleY);
    drawConfirmItems(res.fontBig, res.abort2, res.abort3,
                     name->confirmSelection(), frame_.fb);
    return;
  }

  // Typing phase: SV_TITLE + the name buffer + blink cursor.
  drawCenteredBig(res.fontBig, res.svTitle, kDialogTitleY);
  const std::string& buf = name->name();
  const int y = kAbortItemY0 + 0x20;
  const int w = mdk::measureFtiText(res.fontBig, buf,
                                  mdk::kFtiFontBigMissingAdvance);
  const int x = (frame_.fb.width() - w) / 2;
  mdk::drawFtiText(res.fontBig, buf, frame_.fb, x, y,
                   mdk::kFtiFontBigMissingAdvance);
  // Cursor (OBSERVED blink mask (tick & 8) — the shell tick is not
  // exposed; the presenter ticks its own presentation counter).
  if ((tick_ & 8) != 0) {
    const int cx = x + mdk::measureFtiText(
                           res.fontBig,
                           std::string_view(buf).substr(
                               0, static_cast<std::size_t>(
                                      name->cursor())),
                           mdk::kFtiFontBigMissingAdvance);
    fillRect(frame_.fb, cx + 2, y + 14, 8, 2, kCursorIndex);
  }
  if (name->writeFailed()) {
    // SV_FAIL — OBSERVED "Can't save to\n%s\nCD Drive?" with the
    // stem substituted (the original formats the path; the stem is
    // the closest presentation-safe binding).
    char fail[128];
    std::snprintf(fail, sizeof(fail), res.svFail.c_str(),
                  buf.c_str());
    drawCenteredLines(res.fontSml, fail, frame_.fb, y + 40, 14,
                      mdk::kFtiFontSmlMissingAdvance);
  }
}

void FrontendPresenter::abortScreen(mdk::FrontendShell& sh,
                                    const mdk::FrontendResources& res) {
  const mdk::AbortConsoleController* ab = sh.abortConsole();
  // OBSERVED draw (abort console evidence): ABORT1 title + the
  // ABORT2/ABORT3 items on the (y - 0x95)/0x24 band + ARROW.
  drawCenteredBig(res.fontBig, res.abort1, kDialogTitleY);
  drawConfirmItems(res.fontBig, res.abort2, res.abort3,
                   ab ? ab->selection() : 1, frame_.fb);
}

void FrontendPresenter::helpScreen(const mdk::FrontendResources& res) {
  // OBSERVED records HELP_TOP / HELP_01..18 / HELP_BOT. HELP_* rows
  // carry a two-column "key<TAB>action" shape — split on '\t'.
  // Column geometry is PRESENTATION CHOICE (exact x's UNKNOWN).
  drawCenteredBig(res.fontBig, res.helpTop, 0x0c);
  constexpr int kLeftX = 40;
  constexpr int kRightX = 150;
  constexpr int kRowStep = 15;
  int y = 0x2e;
  for (std::size_t i = 0; i < res.helpLines.size(); ++i) {
    const std::string_view row = res.helpLines[i];
    const std::size_t tab = row.find('\t');
    const std::string_view key =
        row.substr(0, tab == std::string_view::npos ? row.size()
                                                    : tab);
    const std::string_view action =
        tab == std::string_view::npos ? std::string_view{}
                                      : row.substr(tab + 1);
    mdk::drawFtiText(res.fontSml, key, frame_.fb, kLeftX, y,
                     mdk::kFtiFontSmlMissingAdvance);
    if (!action.empty()) {
      mdk::drawFtiText(res.fontSml, action, frame_.fb, kRightX, y,
                       mdk::kFtiFontSmlMissingAdvance);
    }
    y += kRowStep;
  }
  drawCenteredBig(res.fontBig, res.helpBot, 0x154);
}

void FrontendPresenter::pauseScreen(
    const mdk::FrontendResources& res) {
  drawCenteredBig(res.fontBig, res.paused, 0xaa);
}

void FrontendPresenter::noiseFrame() {
  // Mode-1 transition placeholder (FUN_00418e04's noise draw is
  // OBSERVED as noise; the exact generator is UNKNOWN). Deterministic
  // LCG fill — presentation-only, never touches gameplay/core RNG.
  for (int y = 0; y < frame_.fb.height(); ++y) {
    std::uint8_t* row = frame_.fb.pixels() + y * frame_.fb.stride();
    for (int x = 0; x < frame_.fb.width(); ++x) {
      noiseSeed_ = noiseSeed_ * 1103515245u + 12345u;
      row[x] = static_cast<std::uint8_t>((noiseSeed_ >> 16) & 0x3f);
    }
  }
}

bool FrontendPresenter::compose(mdk::FrontendShell& shell,
                                const mdk::FrontendResources& res,
                                const mdk::FrontendHostServices* host,
                                bool transitionPlaying,
                                std::string* err) {
  ++tick_;
  frame_.hasThmb = false;
  optViews_.assign(res.optStrings.begin(), res.optStrings.end());
  lastRes_ = &res;  // for dialogPalette's SYS_PAL head

  // Entry/returning transition: host-side playback window between
  // TransitionArmed and the ack — the mode-1 noise placeholder.
  if (transitionPlaying) {
    dialogPalette(shell.flow().brightness());
    noiseFrame();
    lastRes_ = nullptr;
    return true;
  }

  const int sub = shell.subMode();
  const bool attractActive =
      shell.flow().root().attractState() > 0 &&
      shell.flow().root().attractSlideActive();

  bool ok = true;
  switch (sub) {
  case mdk::kSubSaveList:
    dialogPalette(shell.flow().brightness());
    saveListScreen(shell, res, host);
    break;
  case mdk::kSubSaveName:
    dialogPalette(shell.flow().brightness());
    saveNameScreen(shell, res);
    break;
  case mdk::kSubAbort:
    dialogPalette(shell.flow().brightness());
    abortScreen(shell, res);
    break;
  case mdk::kSubHelp:
    dialogPalette(shell.flow().brightness());
    helpScreen(res);
    break;
  case mdk::kSubSound: {
    mdk::SoundMenuLabels labels;
    labels.title = res.sndTitle;
    labels.info = res.sndInfo;
    labels.effects = res.sndEffects;
    labels.music = res.sndMusic;
    labels.end100 = res.sndEnd100;
    labels.end0 = res.sndEnd0;
    labels.done = res.sndDone;
    ok = mdk::renderSoundMenuDynamic(
        frame_.fb, frame_.palette, res.fontBig, res.fontSml,
        *res.arrow.frame(0), labels, res.sysPalHead,
        shell.flow().sound(), shell.flow().brightness(), err);
    break;
  }
  case mdk::kSubMouse: {
    mdk::MouseMenuLabels labels;
    labels.test = res.mouseTest;
    labels.enabled = res.mouseEnabled;
    labels.disabled = res.mouseDisabled;
    labels.reversed = res.mouseReversed;
    labels.normal = res.mouseNormal;
    labels.quit = res.mouseQuit;
    labels.buttons = res.mouseButtons;
    for (int i = 0; i < mdk::kMouseGridRows; ++i) {
      labels.actions[i] = res.mouseActions[i];
    }
    for (int i = 0; i < 9; ++i) {
      labels.axisNames[i] = res.mouseAxisNames[i];
    }
    for (int i = 0; i < mdk::kMouseAxisCount; ++i) {
      labels.axisCaptions[i] = res.mouseAxisCaps[i];
    }
    ok = mdk::renderMouseMenuDynamic(
        frame_.fb, frame_.palette, res.fontSml, *res.arrow.frame(0),
        labels, res.sysPalHead, shell.flow().mouse(),
        shell.flow().brightness(), err);
    break;
  }
  case mdk::kSubKeyboard: {
    mdk::KeyboardMenuLabels labels;
    for (int i = 0; i < mdk::kKeyboardBindingRows; ++i) {
      labels.rows[i] = res.kbRows[i];
    }
    labels.reset = res.kbReset;
    labels.quit = res.kbQuit;
    labels.doit = res.kbDoit;
    labels.langTag = res.kbLangTag ? res.kbLangTag : 'E';
    ok = mdk::renderKeyboardMenuDynamic(
        frame_.fb, frame_.palette, res.fontSml, *res.arrow.frame(0),
        labels, res.sysPalHead, shell.flow().keyboard(),
        shell.flow().brightness(), err);
    break;
  }
  case mdk::kSubDisplay: {
    mdk::DisplayMenuLabels labels;
    labels.brightnessFmt = res.dspBrightness;
    labels.detailHigh = res.dspDetailHigh;
    labels.detailLow = res.dspDetailLow;
    labels.quit = res.dspQuit;
    ok = mdk::renderDisplayMenuDynamic(
        frame_.fb, frame_.palette, res.fontBig, *res.arrow.frame(0),
        labels, res.sysPalHead, shell.flow().display(), err);
    break;
  }
  case mdk::kSubOptions: {
    mdk::OptionsMenuLabels labels;
    for (int i = 0; i < mdk::kOptionsItemCount; ++i) {
      labels.items[i] = res.omStrings[i];
    }
    // Row 6 record = OM_SK_<skill> — the same 0x421254 branch.
    labels.items[mdk::kOptionsSkillRow] = res.omSkill
        [mdk::optionsSkillRecordIndex(shell.flow().options().skill())];
    ok = mdk::renderOptionsMenuDynamic(
        frame_.fb, frame_.palette, res.fontBig, *res.arrow.frame(0),
        labels, res.sysPalHead, shell.flow().options(),
        shell.flow().brightness(), err);
    break;
  }
  default: {
    // Root + attract + legacy/unknown subs: the authoritative menu.
    if (shell.paused()) {
      // In-frontend pause presentation — PAUSED record over the
      // menu backdrop (PRESENTATION CHOICE of stacking order).
      dialogPalette(shell.flow().brightness());
      pauseScreen(res);
      break;
    }
    ok = mdk::renderFrontendMenuDynamic(
        frame_.fb, frame_.palette, res.backdrop, res.fontBig,
        *res.arrow.frame(0), optViews_, shell.flow().root(),
        shell.flow().brightness(), err);
    if (attractActive) {
      // Attract slide overlay is DEFERRED (MDKS_* GIF decoding is
      // the Phase 18B.2B seam) — the menu stays visible.
    }
    break;
  }
  }
  lastRes_ = nullptr;
  return ok;
}

}  // namespace mdkbridge
