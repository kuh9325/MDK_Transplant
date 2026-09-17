// Presentation backend interface.
//
// ORIGINAL ENGINE OBSERVATION: the original renderer module is swappable
// at link time (software/D3D/Glide/SGL/Verite variants share the engine
// image) and the software build presents an indexed working buffer through
// DirectDraw surfaces (Phase 2B). This interface is the native analogue of
// that seam: it presents an indexed framebuffer, nothing more.
//
// NATIVE PORT PROJECT DECISION: one backend for now — Metal. The
// interface stays small enough that a second backend remains possible.

#ifndef MDK_RENDERER_PRESENTER_H
#define MDK_RENDERER_PRESENTER_H

#include <memory>
#include <string>

namespace mdk {

class IndexedFramebuffer;
class Palette;

class Presenter {
public:
  virtual ~Presenter() = default;

  // Draw the indexed framebuffer (expanded through `palette`) to the
  // window's drawable, preserving the presentation aspect. Called once
  // per frame.
  virtual bool present(const IndexedFramebuffer& fb,
                       const Palette& palette) = 0;

  // Notify that the drawable pixel size changed (resize / DPI move).
  virtual void drawableSizeChanged(int w, int h) = 0;

  virtual const char* name() const = 0;
};

// Factory for the Metal presenter. `sdlWindow` is an opaque SDL_Window*
// (kept opaque so this header needs no SDL types). Returns nullptr and
// fills `error` on failure.
std::unique_ptr<Presenter> createMetalPresenter(void* sdlWindow,
                                                std::string* error);

} // namespace mdk

#endif // MDK_RENDERER_PRESENTER_H
