// Godot frontend presenter — composes the authoritative FrontendShell
// state into a 600x360 indexed frame + palette using the SAME core
// renderers the native SDL app uses. GDScript receives RGBA bytes and
// an optional THMB overlay; no menu semantics live here.
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

#include <array>
#include <cstdint>
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
  // Existing-save THMB preview. The thumbnail carries its OWN palette
  // so it cannot be baked into the shared indexed frame — the caller
  // expands and draws it as an overlay rect at (thmbX, thmbY).
  bool hasThmb = false;
  int thmbX = 0;
  int thmbY = 0;
  std::array<std::uint8_t,
             kFrontendThmbWidth * kFrontendThmbHeight>
      thmbPixels{};
  std::array<std::uint8_t, kFrontendThmbPaletteBytes> thmbPalette{};
};

class FrontendPresenter {
public:
  // Compose one frame for the shell's current state. `host` supplies
  // the save-slot detail probe (THMB bytes); it may be null in tests —
  // slot detail then degrades to summary-only text.
  // `transitionPlaying` mirrors the host's armed-but-not-acknowledged
  // entry transition (FrontendFx::TransitionArmed drained, awaiting
  // frontend_transition_complete) — the frame shows the mode-1 noise
  // placeholder then.
  bool compose(mdk::FrontendShell& shell,
               const mdk::FrontendResources& res,
               const mdk::FrontendHostServices* host,
               bool transitionPlaying,
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

  void saveListScreen(mdk::FrontendShell& sh,
                      const mdk::FrontendResources& res,
                      const mdk::FrontendHostServices* host);
  void saveNameScreen(mdk::FrontendShell& sh,
                      const mdk::FrontendResources& res);
  void abortScreen(mdk::FrontendShell& sh,
                   const mdk::FrontendResources& res);
  void helpScreen(const mdk::FrontendResources& res);
  void pauseScreen(const mdk::FrontendResources& res);
  void noiseFrame();

  FrontendComposedFrame frame_;
  // Presentation-only counters: cursor blink + noise fill. These are
  // NOT gameplay/frontend state — they never reach the shell.
  unsigned tick_ = 0;
  unsigned noiseSeed_ = 0;
  std::vector<std::string_view> optViews_;
  // Borrowed during compose() for dialogPalette's SYS_PAL head.
  const mdk::FrontendResources* lastRes_ = nullptr;
};

}  // namespace mdkbridge

#endif  // MDK_BRIDGE_FRONTEND_PRESENTER_H
