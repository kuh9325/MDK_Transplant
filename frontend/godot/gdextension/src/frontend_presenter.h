// Godot frontend presenter — composes the authoritative FrontendShell
// state into a 600x360 indexed frame + palette using the SAME core
// renderers the native SDL app uses. GDScript receives RGBA bytes;
// no menu semantics live here.
//
// Evidence levels per layout element are marked at each draw site:
// OBSERVED records/coords where the evidence doc pins them,
// PRESENTATION CHOICE where the original's exact dialog geometry is
// not yet captured (marked UNKNOWN at the site).

#ifndef MDK_BRIDGE_FRONTEND_PRESENTER_H
#define MDK_BRIDGE_FRONTEND_PRESENTER_H

#include "core/framebuffer.h"
#include "core/frontend_host.h"
#include "core/frontend_resources.h"
#include "core/frontend_shell.h"
#include "core/indexed_image.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mdkbridge {

// OBSERVED THMB record shape: 768-byte palette + 64x45 indexed pixels
// (docs/reverse-engineering/save format evidence).
inline constexpr int kFrontendThmbWidth = 64;
inline constexpr int kFrontendThmbHeight = 45;
inline constexpr int kFrontendThmbPaletteBytes = 768;
inline constexpr int kFrontendThmbBytes =
    kFrontendThmbPaletteBytes +
    kFrontendThmbWidth * kFrontendThmbHeight;

struct FrontendComposedFrame {
  mdk::IndexedFramebuffer fb{600, 360};
  mdk::Palette palette;
};

class FrontendPresenter {
public:
  // Compose one frame for the shell's current state. `host` supplies
  // the save-slot detail probe (THMB bytes), the LBB detail images
  // and the MISC\MDKS_* slide bytes; it may be null in tests —
  // host-sourced imagery then degrades to its absent-resource path.
  //
  // `transitionSec` is the elapsed time of the armed returning-entry
  // transition (FUN_0041e554): >= 0 while it plays (the INTRO1A still
  // under the blended palette timeline), < 0 otherwise.
  bool compose(mdk::FrontendShell& shell,
               const mdk::FrontendResources& res,
               const mdk::FrontendHostServices* host,
               double transitionSec,
               std::string* err);

  const FrontendComposedFrame& frame() const { return frame_; }

private:
  // Every dialog-family screen (save list / name / abort / help /
  // pause) uploads the resident SYS_PAL head like the options
  // palettes do (OBSERVED palette-upload pattern; the dialog body
  // layout is UNKNOWN where noted).
  void dialogPalette(int brightness);
  void drawCenteredBig(const mdk::FtiFont& font, std::string_view text,
                       int y, float scale = 1.0f);

  // Returning-entry transition frame: INTRO1A pixels under the
  // six-phase blended palette (FUN_0041e554).
  void transitionFrame(const mdk::FrontendResources& res,
                       double seconds, int brightness);

  void saveListScreen(mdk::FrontendShell& sh,
                      const mdk::FrontendResources& res,
                      const mdk::FrontendHostServices* host);
  void saveNameScreen(mdk::FrontendShell& sh,
                      const mdk::FrontendResources& res);
  void abortScreen(mdk::FrontendShell& sh,
                   const mdk::FrontendResources& res);
  void helpScreen(const mdk::FrontendResources& res);
  void pauseScreen(const mdk::FrontendResources& res);

  // Lazily decoded attract slide, keyed by the attract state (the
  // slide number). nullopt = no decode attempted yet; an empty
  // engaged value caches a failure so a broken slide is not
  // re-decoded every frame (bounded).
  const mdk::IndexedImage* slideImage(
      const mdk::FrontendHostServices* host, int state);
  // Lazily inspected save-slot detail, keyed by stem (DAT_0054bd2c's
  // selection-change cadence — the file set is static mid-dialog).
  const mdk::FrontendSlotInspection* slotDetail(
      const mdk::FrontendHostServices* host, const std::string& stem);

  FrontendComposedFrame frame_;
  // Presentation-only counter: cursor blink phase.
  unsigned tick_ = 0;
  std::vector<std::string_view> optViews_;
  // Borrowed during compose() for dialogPalette's SYS_PAL head.
  const mdk::FrontendResources* lastRes_ = nullptr;

  int slideIndex_ = -1;
  std::optional<mdk::IndexedImage> slideImg_;
  std::string detailStem_;
  std::optional<mdk::FrontendSlotInspection> detail_;
};

}  // namespace mdkbridge

#endif  // MDK_BRIDGE_FRONTEND_PRESENTER_H
