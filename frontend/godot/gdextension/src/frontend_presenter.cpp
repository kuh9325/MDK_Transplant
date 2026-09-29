#include "frontend_presenter.h"

#include "gif_decode.h"

#include "core/abort_console.h"
#include "core/display_menu.h"
#include "core/frontend_machines.h"
#include "core/frontend_palette.h"
#include "core/frontend_transition.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/keyboard_menu.h"
#include "core/mouse_menu.h"
#include "core/options_menu.h"
#include "core/save_name_entry.h"
#include "core/save_slot_list.h"
#include "core/sound_menu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace mdkbridge {

namespace {

// Save-name cursor fill (SYS_PAL white).
constexpr std::uint8_t kCursorIndex = 19;

// OBSERVED save-list layout (FUN_004206d0 draw block):
//   * rows: FONTSML stems at x=0x62, pen rows y = 0x67 + row*0x10,
//     13-row window (the hit band 0x66 < y < 0x137 covers them);
//   * title record SVOPT1 via the shared centered-multiline draw
//     (FUN_004239c4): literal "\\n" escape split, 36px step, first
//     line at y=0x1f;
//   * empty list: SVOPT3 the same way at y=0x64;
//   * selected row: the FUN_00414b28 blinking double bracket via
//     FUN_00414dd4's flag;
//   * detail pane (selected slot only): full save -> the THMB record
//     blitted at (0x1a2, 0x67); header-only -> LOAD_<level>.LBB at
//     (0x1c2 - w/2, 0x67); invalid -> SVBAD at (0xa, 0x162);
//   * the detail palette merge (FUN_00413b40): DAC[0..63] keep the
//     resident SYS_PAL head, DAC[64..255] take the preview palette.
constexpr int kSaveRowX = 0x62;
constexpr int kSaveRowY0 = 0x67;
constexpr int kSaveRowStep = 0x10;
constexpr int kSaveVisibleRows = 13;
constexpr int kSaveTitleY = 0x1f;
constexpr int kSaveEmptyY = 0x64;
constexpr int kSaveBadX = 0xa;
constexpr int kSaveBadY = 0x162;
constexpr int kSaveThmbX = 0x1a2;
constexpr int kSaveThmbY = 0x67;
constexpr int kSaveLbbCenterX = 0x1c2;
constexpr int kSaveLbbY = 0x67;
// FUN_004239c4's line step (OBSERVED).
constexpr int kTitleLineStep = 0x24;
// The merged palette band: entries 64..255 (OBSERVED FUN_00413b40
// range) = bytes 192..767 of a 768-triplet palette block.
constexpr int kMergeFirstEntry = 64;
constexpr int kMergeByteBase = kMergeFirstEntry * 3;  // 192

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
// the shared measure pattern (missingAdvance per font). Records use
// the ORIGINAL's two-byte escape "\\n" (FUN_004239c4), not a real
// newline byte.
void drawCenteredLines(const mdk::FtiFont& font, std::string_view text,
                       mdk::IndexedFramebuffer& fb, int y, int lineStep,
                       int missingAdvance, float scale = 1.0f) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find("\\n", pos);
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
    pos = nl + 2;
    y += lineStep;
  }
}

// FUN_00414d2c (OBSERVED): one centered line — FONTBIG, falling back
// to FONTSML when the FONTBIG measure reaches the 600px frame width.
void drawCenteredFontLine(const mdk::FtiFont& fontBig,
                          const mdk::FtiFont& fontSml,
                          std::string_view line,
                          mdk::IndexedFramebuffer& fb, int y) {
  const int wBig = mdk::measureFtiText(fontBig, line,
                                       mdk::kFtiFontBigMissingAdvance);
  if (wBig >= fb.width()) {
    const int w = mdk::measureFtiText(fontSml, line,
                                      mdk::kFtiFontSmlMissingAdvance);
    mdk::drawFtiText(fontSml, line, fb, (fb.width() - w) / 2, y,
                     mdk::kFtiFontSmlMissingAdvance);
    return;
  }
  mdk::drawFtiText(fontBig, line, fb, (fb.width() - wBig) / 2, y,
                   mdk::kFtiFontBigMissingAdvance);
}

// FUN_004239c4 (OBSERVED): a centered title/message record — split on
// the literal "\\n" escape, each line through FUN_00414d2c, 36px step.
void drawCenteredRecord(const mdk::FtiFont& fontBig,
                        const mdk::FtiFont& fontSml,
                        std::string_view text,
                        mdk::IndexedFramebuffer& fb, int y) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find("\\n", pos);
    drawCenteredFontLine(
        fontBig, fontSml,
        (nl == std::string_view::npos)
            ? text.substr(pos)
            : text.substr(pos, nl - pos),
        fb, y);
    if (nl == std::string_view::npos) break;
    pos = nl + 2;
    y += kTitleLineStep;
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

const mdk::FrontendSlotInspection* FrontendPresenter::slotDetail(
    const mdk::FrontendHostServices* host, const std::string& stem) {
  // DAT_0054bd2c's cadence (OBSERVED): re-inspect when the selected
  // row changes; the inspected result is reused while the selection
  // stays put. An engaged-but-absent detail_ caches the miss.
  if (!host) return nullptr;
  if (stem != detailStem_) {
    detailStem_ = stem;
    detail_ = host->inspectSlotDetail(stem);
  }
  return detail_ ? &*detail_ : nullptr;
}

const mdk::IndexedImage* FrontendPresenter::slideImage(
    const mdk::FrontendHostServices* host, int state) {
  if (state != slideIndex_) {
    slideIndex_ = state;
    slideImg_.reset();
    if (host) {
      if (const auto bytes = host->slideData(state)) {
        auto img = decodeGifImage(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes->data()),
                bytes->size()));
        // The OBSERVED 600x360 logical-screen gate (FUN_00416e98);
        // the core probe already filtered on it, so this is the
        // same verdict from the decode side. The slide also must
        // carry its own palette (the draw block uploads it).
        if (img && img->hasPalette && img->width == 600 &&
            img->height == 360) {
          slideImg_ = std::move(*img);
        }
      }
    }
  }
  return slideImg_ ? &*slideImg_ : nullptr;
}

void FrontendPresenter::saveListScreen(
    mdk::FrontendShell& sh, const mdk::FrontendResources& res,
    const mdk::FrontendHostServices* host) {
  mdk::SaveSlotListController* list = sh.saveList();
  const mdk::SaveSlotSummary* sel = list ? list->selected() : nullptr;

  // FUN_00415658 — the save list draws on a cleared (pen-0) buffer.
  frame_.fb.clear(0);

  // Palette (OBSERVED FUN_00413b40 order): the resident SYS_PAL head
  // fills entries 0..63; the selected slot's detail palette fills
  // 64..255 — the THMB head for full saves, the LOAD_<level>.LBB
  // palette for header-only ones. No preview -> the band stays the
  // dialog's (black fill).
  const mdk::FrontendSlotInspection* detail = nullptr;
  const mdk::IndexedImage* lbb = nullptr;
  const std::uint8_t* thmbPal = nullptr;
  const std::uint8_t* thmbPx = nullptr;
  if (sel && sel->valid && host) {
    if (sel->fullSave) {
      detail = slotDetail(host, sel->name);
      if (detail &&
          detail->thumbnail.size() == kFrontendThmbBytes) {
        thmbPal = reinterpret_cast<const std::uint8_t*>(
            detail->thumbnail.data());
        thmbPx = thmbPal + kFrontendThmbPaletteBytes;
      }
    } else {
      lbb = host->saveListLbbImage(sel->levelId);
    }
  }

  const auto* head = res.sysPalHead.data();
  for (int i = 0; i < mdk::Palette::size(); ++i) {
    mdk::Palette::Color c{0, 0, 0, 255};
    if (i < kMergeFirstEntry) {
      c = {static_cast<std::uint8_t>(head[i * 3]),
           static_cast<std::uint8_t>(head[i * 3 + 1]),
           static_cast<std::uint8_t>(head[i * 3 + 2]), 255};
    } else if (thmbPal) {
      const int b = kMergeByteBase + (i - kMergeFirstEntry) * 3;
      c = {thmbPal[b], thmbPal[b + 1], thmbPal[b + 2], 255};
    } else if (lbb && lbb->hasPalette) {
      c = {lbb->palette[i].r, lbb->palette[i].g, lbb->palette[i].b,
           255};
    }
    frame_.palette.set(i, c);
  }
  mdk::applyFrontendBrightness(frame_.palette,
                               sh.flow().brightness());

  // OBSERVED draw order: the detail imagery / SVBAD runs before the
  // title and rows (FUN_004206d0's draw block).
  if (sel && !sel->valid) {
    // SVBAD "Invalid/Corrupt File" at (0xa, 0x162).
    mdk::drawFtiText(res.fontSml, res.svBad, frame_.fb, kSaveBadX,
                     kSaveBadY, mdk::kFtiFontSmlMissingAdvance);
  } else if (thmbPx) {
    // Full save: the THMB 64x45 pixels at (0x1a2, 0x67).
    for (int r = 0; r < kFrontendThmbHeight; ++r) {
      for (int c = 0; c < kFrontendThmbWidth; ++c) {
        frame_.fb.put(kSaveThmbX + c, kSaveThmbY + r,
                      thmbPx[r * kFrontendThmbWidth + c]);
      }
    }
  } else if (lbb) {
    // Header-only: LOAD_<level>.LBB at (0x1c2 - w/2, 0x67).
    const int x0 = kSaveLbbCenterX - lbb->width / 2;
    for (int r = 0; r < lbb->height; ++r) {
      const std::uint8_t* src =
          lbb->pixels.data() + r * lbb->stride;
      for (int c = 0; c < lbb->width; ++c) {
        frame_.fb.put(x0 + c, kSaveLbbY + r, src[c]);
      }
    }
  }

  if (!list || list->count() == 0) {
    // Empty: SVOPT3 "No Saved Games Found" centered at y=0x64.
    drawCenteredRecord(res.fontBig, res.fontSml, res.svOpt3,
                       frame_.fb, kSaveEmptyY);
  } else {
    // Title: SVOPT1 "Select Saved Game\nESC to Quit" centered at
    // y=0x1f (36px line step — FUN_004239c4).
    drawCenteredRecord(res.fontBig, res.fontSml, res.svOpt1,
                       frame_.fb, kSaveTitleY);
    // 13-row window (OBSERVED) from topRow; the selected row runs
    // the FUN_00414b28 shimmer through FUN_00414dd4's flag. The
    // shared marker accumulator advances inside the bracket draw
    // (fild/fadd/fistp in FUN_00414b28) — so it steps only on frames
    // where a flagged row is actually drawn.
    const int rows = std::min(list->count() - list->topRow(),
                              kSaveVisibleRows);
    const int sel = list->selection();
    const bool flaggedVisible =
        sel >= list->topRow() && sel < list->topRow() + rows;
    const bool phase = flaggedVisible ? sh.advanceMarkerBlink() : false;
    for (int r = 0; r < rows; ++r) {
      const int idx = list->topRow() + r;
      mdk::drawFtiTextFlagged(
          res.fontSml, list->stems()[static_cast<std::size_t>(idx)],
          frame_.fb, kSaveRowX, kSaveRowY0 + r * kSaveRowStep,
          idx == list->selection(), phase);
    }
  }

  // FUN_004236c0 — the ARROW cursor at the shared mouse position
  // (the save list reads the same DAT_0054b634/638 globals).
  mdk::blitFtiSpriteFrame(*res.arrow.frame(0), frame_.fb,
                          sh.sharedMachine().mouseX,
                          sh.sharedMachine().mouseY);
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
    // the closest presentation-safe binding). The record's literal
    // "\n" escapes split through the shared centered-multiline draw.
    char fail[128];
    std::snprintf(fail, sizeof(fail), res.svFail.c_str(),
                  buf.c_str());
    drawCenteredRecord(res.fontBig, res.fontSml, fail, frame_.fb,
                       y + 40);
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

void FrontendPresenter::transitionFrame(
    const mdk::FrontendResources& res, double seconds,
    int brightness) {
  frame_.fb.clear(0);
  if (!res.transition) {
    // INTRO1A absent from OPTIONS.BNI — a bounded deviation: the
    // original dereferences the unbound record. The frame is black
    // and the caller completes immediately (frontend_transition_
    // seconds() reports 0 for a missing record).
    for (int i = 0; i < mdk::Palette::size(); ++i) {
      frame_.palette.set(i, {0, 0, 0, 255});
    }
    return;
  }
  // FUN_0041e554 (OBSERVED): the INTRO1A pixels sit in the
  // framebuffer for the whole run; only the palette moves through
  // the six-phase timeline.
  const mdk::IndexedImage& img = res.transition->image;
  const std::size_t n =
      std::min<std::size_t>(frame_.fb.pixelCount(),
                            img.pixels.size());
  std::memcpy(frame_.fb.pixels(), img.pixels.data(), n);
  std::uint8_t pal[mdk::compat::kPaletteEntries * 3];
  mdk::frontendTransitionPalette(*res.transition, seconds, pal);
  for (int i = 0; i < mdk::Palette::size(); ++i) {
    frame_.palette.set(
        i, {pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], 255});
  }
  // The blends upload through FUN_0046d208 — the brightness lift
  // applies to the staged result the same as every other palette.
  mdk::applyFrontendBrightness(frame_.palette, brightness);
}

bool FrontendPresenter::compose(mdk::FrontendShell& shell,
                                const mdk::FrontendResources& res,
                                const mdk::FrontendHostServices* host,
                                double transitionSec,
                                std::string* err) {
  ++tick_;
  optViews_.assign(res.optStrings.begin(), res.optStrings.end());
  lastRes_ = &res;  // for dialogPalette's SYS_PAL head

  // Entry/returning transition: host-side playback window between
  // TransitionArmed and the ack — the INTRO1A still under the
  // blended palette timeline (FUN_0041e554).
  if (transitionSec >= 0.0) {
    transitionFrame(res, transitionSec, shell.flow().brightness());
    lastRes_ = nullptr;
    return true;
  }

  const int sub = shell.subMode();
  if (sub != mdk::kSubSaveList) {
    // The inspection cache only lives for the save list's own
    // selection-change cadence — a re-armed list re-inspects (the
    // file may have been rewritten since).
    detailStem_.clear();
    detail_.reset();
  }
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
    // Attract slide bound (OBSERVED FUN_0041dc90 draw block): the
    // slide pixels replace the framebuffer and the slide's own
    // palette uploads; the menu item strings draw over it only when
    // attractState != 1; the ARROW always draws.
    mdk::FrontendMenuController& ctl = shell.flow().root();
    if (attractActive) {
      if (const mdk::IndexedImage* slide =
              slideImage(host, ctl.attractState())) {
        const std::size_t n =
            std::min<std::size_t>(frame_.fb.pixelCount(),
                                  slide->pixels.size());
        std::memcpy(frame_.fb.pixels(), slide->pixels.data(), n);
        for (int i = 0; i < mdk::Palette::size(); ++i) {
          frame_.palette.set(
              i, {slide->palette[i].r, slide->palette[i].g,
                  slide->palette[i].b, 255});
        }
        mdk::applyFrontendBrightness(frame_.palette,
                                     shell.flow().brightness());
        if (!ctl.menuStringsHidden()) {
          ok = mdk::drawFrontendMenuItems(frame_.fb, res.fontBig,
                                          optViews_, ctl, err);
        }
        mdk::blitFtiSpriteFrame(*res.arrow.frame(0), frame_.fb,
                                ctl.mouseX(), ctl.mouseY());
        break;
      }
      // The core probe gated on the 600x360 logical screen, yet the
      // full decode failed — the file changed or hit a bound the
      // header probe cannot see. Bounded deviation: the menu frame
      // stands (the original cannot reach this — its gate IS the
      // decode).
    }
    ok = mdk::renderFrontendMenuDynamic(
        frame_.fb, frame_.palette, res.backdrop, res.fontBig,
        *res.arrow.frame(0), optViews_, ctl,
        shell.flow().brightness(), err);
    break;
  }
  }
  lastRes_ = nullptr;
  return ok;
}

}  // namespace mdkbridge
