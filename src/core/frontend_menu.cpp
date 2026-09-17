#include "core/frontend_menu.h"

#include "core/compat.h"
#include "core/framebuffer.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/indexed_image.h"

#include <cstring>

namespace mdk {

FrontendMenuSpec frontendMenuSpec(bool savesExist) {
  FrontendMenuSpec spec;
  // FUN_0041d85c: DAT_0049aa78 = !savesExist -> saves: sel 0
  // ("Continue"), no saves: sel 1 ("New Game").
  const int selected = savesExist ? 0 : 1;
  if (savesExist) {
    for (int i = 0; i < kFrontendOptCount; ++i) {
      spec.items.push_back(
          {i, kFrontendItemY0 + kFrontendItemStep * i, i == selected});
    }
  } else {
    // No-saves branch: OPT1..OPT4 at y = 31,67,103,139.
    for (int i = 1; i < kFrontendOptCount; ++i) {
      spec.items.push_back(
          {i, kFrontendItemY0 + kFrontendItemStep * (i - 1),
           i == selected});
    }
  }
  return spec;
}

bool renderFrontendMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                             const IndexedImage& backdrop,
                             const FtiFont& fontBig,
                             const FtiSpriteFrame& arrow,
                             std::span<const std::string_view> optStrings,
                             const FrontendMenuSpec& spec,
                             std::string* err) {
  auto fail = [&](const char* msg) {
    if (err) {
      *err = msg;
    }
    return false;
  };

  if (backdrop.width != fb.width() || backdrop.height != fb.height() ||
      backdrop.stride != backdrop.width) {
    return fail("frontend menu: backdrop is not the 600x360 work size");
  }
  if (optStrings.size() < kFrontendOptCount) {
    return fail("frontend menu: OPT0..OPT4 strings not resolved");
  }

  // Step 1 (OBSERVED): memcpy(fb, MDKOPT pixels, 0x34bc0) — the image
  // is exactly the work-buffer size so this is a flat copy.
  std::memcpy(fb.pixels(), backdrop.pixels.data(),
              static_cast<std::size_t>(fb.width()) * fb.height());

  // Palette upload (FUN_00413b40 -> FUN_0046d208 head+tail: the full
  // 256-entry embedded palette).
  if (backdrop.hasPalette) {
    for (int i = 0; i < palette.size(); ++i) {
      const auto& c = backdrop.palette[static_cast<std::size_t>(i)];
      palette.set(i, {c.r, c.g, c.b, 255});
    }
  }

  // Step 2: items. x_arg = maxW/2 — integer signed division per the
  // original SAR/SUB/SAR pattern; maxW = largest UNSCALED measure
  // over the drawn items (FUN_00414be8, missing-glyph advance 6).
  int maxW = 0;
  for (const auto& item : spec.items) {
    const auto& text = optStrings[item.optIndex];
    if (text.empty()) {
      return fail("frontend menu: empty OPT string");
    }
    const int w = measureFtiText(fontBig, text,
                                 kFtiFontBigMissingAdvance);
    if (w > maxW) {
      maxW = w;
    }
  }
  const int xArg = maxW / 2;  // integer, trunc toward zero (SAR)

  for (const auto& item : spec.items) {
    const auto& text = optStrings[item.optIndex];
    const float scale = item.selected ? kFrontendScaleSelected
                                      : kFrontendScaleUnselected;
    const int w = measureFtiText(fontBig, text,
                                 kFtiFontBigMissingAdvance);
    // FUN_00423b38: x = trunc(x_arg - w*scale*0.5) — x87 truncation
    // toward zero on the f32 scale value.
    const int x = static_cast<int>(
        static_cast<double>(xArg) -
        static_cast<double>(w) * static_cast<double>(scale) * 0.5);
    drawFtiTextScaled(fontBig, text, fb, x, item.y, scale,
                      kFtiFontBigMissingAdvance);
  }

  // Step 3: ARROW at the raw mouse position (FUN_004236c0 —
  // hotspot applied inside the blit).
  blitFtiSpriteFrame(arrow, fb, spec.arrowX, spec.arrowY);
  return true;
}

} // namespace mdk
