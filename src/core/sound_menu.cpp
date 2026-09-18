#include "core/sound_menu.h"

#include "core/framebuffer.h"
#include "core/frontend_menu.h" // FrontendMenuInput
#include "core/frontend_palette.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"

namespace mdk {

SoundMenuController::SoundMenuController(const FrontendMachineState& s,
                                         int selection, int soundFx,
                                         int soundMusic,
                                         bool settingsDirty)
    : m_(s), selection_(selection), soundFx_(soundFx),
      soundMusic_(soundMusic), settingsDirty_(settingsDirty) {
  // FUN_0042322c (OBSERVED): DAT_00541493 = 2 handled by the flow;
  // FUN_0041d774 stops/releases the ambient menu song, the
  // MDKSOUND.SNI blob loads and OPTSONG resolves, then
  // FUN_00402388(OPTSONG, 0) starts the screen's song.
  events_.push_back(SoundAudioEvent::AmbientSongStop);
  events_.push_back(SoundAudioEvent::SongStart);
}

void SoundMenuController::update(const FrontendMenuInput& in) {
  endedEarly_ = false;

  // FUN_004187e0 accumulate + DAT_00541518 += DAT_0049b6e8 — the
  // shared main-loop prologue, identical to the options screen.
  frontendMouseAccumulate(m_.mouseX, in.mouseDx, kFrontendMouseMaxX);
  frontendMouseAccumulate(m_.mouseY, in.mouseDy, kFrontendMouseMaxY);
  m_.tick += m_.timing.frameStep;

  // 1. prev query (FUN_004237b4): a fired query plays OPTBUTT first
  // (FUN_00402388(DAT_0054bdc4, 1) at 0x423410), then dec wraps
  // <0 -> DAT_0054bdb8-1 (0x423419).
  if (frontendRepeatQuery(m_.tick, in.prevHeld, m_.prevDeadline)) {
    events_.push_back(SoundAudioEvent::Button);
    selection_ -= 1;
    if (selection_ < 0) {
      selection_ = kSoundItemCount - 1;
    }
  }

  // 2. next query (FUN_00423838): OPTBUTT, then inc wraps
  // >=3 -> 0 (0x423440).
  if (frontendRepeatQuery(m_.tick, in.nextHeld, m_.nextDeadline)) {
    events_.push_back(SoundAudioEvent::Button);
    selection_ += 1;
    if (selection_ >= kSoundItemCount) {
      selection_ = 0;
    }
  }

  // 3. Mouse hit-test — the same three-global gate as Options
  // (DAT_0054b644/648/640); the controller clamps to (590,350)
  // inside the gate. band = trunc((mouseY - 61) / 46) — x86 IDIV
  // semantics; a valid band (0..2) is assigned unconditionally.
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    if (m_.mouseX > kFrontendHitClampX) m_.mouseX = kFrontendHitClampX;
    if (m_.mouseY > kFrontendHitClampY) m_.mouseY = kFrontendHitClampY;
    const int band =
        (m_.mouseY - kSoundHitBandBase) / kSoundHitBandSize;
    if (band > -1 && band < kSoundItemCount) {
      selection_ = band;
    }
  }

  // 4. DIK_ESCAPE (DAT_0054b570) -> FUN_00423280 + RET (0x423647):
  // stop OPTSONG, release the SNI, restart MAINSONG, mode 0x0b.
  // The ONLY exit path that does not play OPTBUTT — the original
  // jumps straight to the exit with no FUN_00402388.
  if (in.cancelEdge) {
    events_.push_back(SoundAudioEvent::SongStop);
    events_.push_back(SoundAudioEvent::AmbientSongStart);
    action_ = SoundAction::Back;
    endedEarly_ = true;
    return;
  }

  // 5. LEFT query (FUN_004238bc): OPTBUTT, then row 0 ->
  // DAT_00541308 -10 (0x4234dd), row 1 -> DAT_0054130c -10
  // (0x4234f8), row 2 falls through (0x4234d0 `jnz`). A mutation
  // clamps <0 -> 0, latches DAT_00541486, then calls FUN_004024c4
  // — even when the clamp kept the boundary value.
  if (frontendRepeatQuery(m_.tick, in.leftHeld, m_.leftDeadline)) {
    events_.push_back(SoundAudioEvent::Button);
    if (selection_ == 0) {
      soundFx_ -= kSoundVolumeStep;
      if (soundFx_ < kSoundVolumeMin) {
        soundFx_ = kSoundVolumeMin;
      }
      settingsDirty_ = true;
      events_.push_back(SoundAudioEvent::VolumesApplied);
    } else if (selection_ == 1) {
      soundMusic_ -= kSoundVolumeStep;
      if (soundMusic_ < kSoundVolumeMin) {
        soundMusic_ = kSoundVolumeMin;
      }
      settingsDirty_ = true;
      events_.push_back(SoundAudioEvent::VolumesApplied);
    }
  }

  // 6. RIGHT query (FUN_00423940): OPTBUTT, then the same rows +10
  // clamping >100 -> 100 (0x42352d/0x423548).
  if (frontendRepeatQuery(m_.tick, in.rightHeld, m_.rightDeadline)) {
    events_.push_back(SoundAudioEvent::Button);
    if (selection_ == 0) {
      soundFx_ += kSoundVolumeStep;
      if (soundFx_ > kSoundVolumeMax) {
        soundFx_ = kSoundVolumeMax;
      }
      settingsDirty_ = true;
      events_.push_back(SoundAudioEvent::VolumesApplied);
    } else if (selection_ == 1) {
      soundMusic_ += kSoundVolumeStep;
      if (soundMusic_ > kSoundVolumeMax) {
        soundMusic_ = kSoundVolumeMax;
      }
      settingsDirty_ = true;
      events_.push_back(SoundAudioEvent::VolumesApplied);
    }
  }

  // 7. Activate query (FUN_00423764): Enter edge, or any-button
  // down-edge while the latch is armed. OPTBUTT plays BEFORE the
  // row check (0x423571); sel==0 -> draw, sel!=1 -> FUN_00423280
  // + RET — rows 0/1 fall through with no mutation, row 2 exits.
  if (in.mouseButtons == 0) {
    m_.buttonLatch = true;
  }
  bool fire = in.confirmEdge;
  if (!fire && m_.buttonLatch && in.mouseButtons != 0) {
    fire = true;
  }
  if (fire) {
    m_.buttonLatch = false;
    events_.push_back(SoundAudioEvent::Button);
    if (selection_ != 0 && selection_ != 1) {
      events_.push_back(SoundAudioEvent::SongStop);
      events_.push_back(SoundAudioEvent::AmbientSongStart);
      action_ = SoundAction::Back;
      endedEarly_ = true;
      return;
    }
  }

  // 8. Draw: handled by the renderer (clear -> centered title/info
  // -> volume rows -> Done -> ARROW -> FUN_0042fe78 timing in
  // endFrame()).
}

float SoundMenuController::itemScale(int keyX, int keyY,
                                     bool selFlag) {
  return frontendRampScale(m_.ramp, keyX, keyY, selFlag,
                           m_.timing.smoothed);
}

void SoundMenuController::endFrame(double dtMs) {
  frontendTimingUpdate(m_.timing, dtMs);
}

SoundAction SoundMenuController::consumeAction() {
  const SoundAction a = action_;
  action_ = SoundAction::None;
  return a;
}

std::vector<SoundAudioEvent> SoundMenuController::drainAudioEvents() {
  std::vector<SoundAudioEvent> out = std::move(events_);
  events_.clear();
  return out;
}

namespace {

int rowY(int row) {
  return kSoundRowY0 + kSoundRowStep * row;
}

// FUN_00414d2c (OBSERVED): centered text — FONTBIG while the
// measure stays under 600, else the FONTSML centered path
// (FUN_00414f1c, advance 4).
void drawCenteredLine(const FtiFont& fontBig, const FtiFont& fontSml,
                      std::string_view text, IndexedFramebuffer& fb,
                      int y) {
  const int w =
      measureFtiText(fontBig, text, kFtiFontBigMissingAdvance);
  if (w >= kSoundCenterWidth) {
    const int ws =
        measureFtiText(fontSml, text, kFtiFontSmlMissingAdvance);
    drawFtiText(fontSml, text, fb, (kSoundCenterWidth - ws) / 2, y,
                kFtiFontSmlMissingAdvance);
    return;
  }
  drawFtiText(fontBig, text, fb, (kSoundCenterWidth - w) / 2, y,
              kFtiFontBigMissingAdvance);
}

// FUN_00416aa8 (OBSERVED): solid inclusive rectfill — covers every
// pixel x0..x1, y0..y1.
void rectfill(IndexedFramebuffer& fb, int x0, int y0, int x1, int y1,
              std::uint8_t color) {
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      fb.put(x, y, color);
    }
  }
}

// One volume row at rowY (FUN_004232b0, OBSERVED): the label is
// ramp-keyed (4, rowY) and drawn FONTBIG-scaled left-aligned at
// x=4 (FUN_00423b10 -> FUN_00414f64); then the inclusive bar
// x=210..210+trunc(vol*280/100), y=rowY-12..rowY-1, color 4; then
// the FONTSML endpoint labels "100%" @498 and "0%" @175.
void drawVolumeRow(const FtiFont& fontBig, const FtiFont& fontSml,
                   IndexedFramebuffer& fb, const SoundMenuLabels& labels,
                   int row, std::string_view label, int volume,
                   float scale) {
  const int y = rowY(row);
  drawFtiTextScaled(fontBig, label, fb, kSoundLabelX, y, scale,
                    kFtiFontBigMissingAdvance);
  const int w = (volume * kSoundBarWidth) / kSoundVolumeMax;
  rectfill(fb, kSoundBarX0, y - kSoundBarTop, kSoundBarX0 + w,
           y - kSoundBarBottom, kSoundBarColor);
  drawFtiText(fontSml, labels.end100, fb, kSoundEnd100X, y,
              kFtiFontSmlMissingAdvance);
  drawFtiText(fontSml, labels.end0, fb, kSoundEnd0X, y,
              kFtiFontSmlMissingAdvance);
}

// Shared body: `scaleFor(p, row)` yields the row's scale in draw
// order — the static spec reports 1.0/0.65 by selection; the
// dynamic path runs the live FUN_00423a24 ramp keyed (4,y) or
// (-1,y) exactly where the original calls it.
bool drawSoundFrame(IndexedFramebuffer& fb, Palette& palette,
                    const FtiFont& fontBig, const FtiFont& fontSml,
                    const FtiSpriteFrame& arrow,
                    const SoundMenuLabels& labels,
                    std::span<const std::byte> sysPalHead,
                    int soundFx, int soundMusic,
                    int brightness, int arrowX, int arrowY,
                    float (*scaleFor)(void*, int), void* ctx,
                    std::string* err) {
  if (fb.width() != kSoundCenterWidth || fb.height() != 360) {
    if (err) *err = "sound frame: framebuffer is not 600x360";
    return false;
  }
  if (sysPalHead.size() < 192) {
    if (err) *err = "sound frame: SYS_PAL head needs 192 bytes";
    return false;
  }
  if (labels.title.empty() || labels.info.empty() ||
      labels.effects.empty() || labels.music.empty() ||
      labels.end100.empty() || labels.end0.empty() ||
      labels.done.empty()) {
    if (err) *err = "sound frame: empty label string";
    return false;
  }

  // The screen performs no palette upload of its own — the bound
  // palette is the options screen's SYS_PAL composition, which the
  // port binds per frame (head + zeroed tail + the DAT_0054147e
  // lift, OBSERVED to apply to every bound entry incl. index 0).
  for (int i = 0; i < 64; ++i) {
    palette.set(i, {static_cast<std::uint8_t>(sysPalHead[i * 3 + 0]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 1]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 2]),
                    255});
  }
  for (int i = 64; i < Palette::size(); ++i) {
    palette.set(i, {0, 0, 0, 255});
  }
  applyFrontendBrightness(palette, brightness);

  fb.clear(0);

  drawCenteredLine(fontBig, fontSml, labels.title, fb, kSoundTitleY);
  drawCenteredLine(fontBig, fontSml, labels.info, fb, kSoundInfoY);

  drawVolumeRow(fontBig, fontSml, fb, labels, 0, labels.effects,
                soundFx, scaleFor(ctx, 0));
  drawVolumeRow(fontBig, fontSml, fb, labels, 1, labels.music,
                soundMusic, scaleFor(ctx, 1));

  // Row 2 — SND_DONE via FUN_00423384: ramp key (-1,179),
  // FONTBIG scaled, x = trunc((600 - w*scale) * 0.5).
  const float doneScale = scaleFor(ctx, kSoundDoneRow);
  const int w =
      measureFtiText(fontBig, labels.done, kFtiFontBigMissingAdvance);
  const int x = static_cast<int>(
      (kSoundCenterWidth - w * doneScale) * 0.5);
  drawFtiTextScaled(fontBig, labels.done, fb, x, rowY(kSoundDoneRow),
                    doneScale, kFtiFontBigMissingAdvance);

  blitFtiSpriteFrame(arrow, fb, arrowX, arrowY);
  return true;
}

float specScale(void* ctx, int row) {
  const int selection = *static_cast<int*>(ctx);
  return row == selection ? 1.0f : 0.65f;
}

struct DynamicCtx {
  SoundMenuController* ctl;
};

float dynamicScale(void* ctx, int row) {
  auto* c = static_cast<DynamicCtx*>(ctx);
  const int y = rowY(row);
  const int keyX =
      row == kSoundDoneRow ? kSoundDoneKeyX : kSoundVolRowKeyX;
  return c->ctl->itemScale(keyX, y, row == c->ctl->selection());
}

} // namespace

bool renderSoundMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                          const FtiFont& fontBig,
                          const FtiFont& fontSml,
                          const FtiSpriteFrame& arrow,
                          const SoundMenuLabels& labels,
                          std::span<const std::byte> sysPalHead,
                          const SoundMenuSpec& spec, std::string* err) {
  int selection = spec.selection;
  return drawSoundFrame(fb, palette, fontBig, fontSml, arrow, labels,
                        sysPalHead, spec.soundFx, spec.soundMusic,
                        spec.brightness, spec.arrowX, spec.arrowY,
                        &specScale, &selection, err);
}

bool renderSoundMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontBig,
                            const FtiFont& fontSml,
                            const FtiSpriteFrame& arrow,
                            const SoundMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            SoundMenuController& ctl, int brightness,
                            std::string* err) {
  DynamicCtx ctx{&ctl};
  return drawSoundFrame(fb, palette, fontBig, fontSml, arrow, labels,
                        sysPalHead, ctl.soundFx(), ctl.soundMusic(),
                        brightness, ctl.mouseX(), ctl.mouseY(),
                        &dynamicScale, &ctx, err);
}

} // namespace mdk
