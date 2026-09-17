#include "platform/sdl_host.h"

#include "core/log.h"
#include "input/input_state.h"

#include <SDL3/SDL.h>

namespace mdk {

static constexpr const char* kTag = "platform";

SdlHost::~SdlHost() { shutdown(); }

bool SdlHost::init() {
  if (initialized_) {
    return true;
  }
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    log::error(kTag, "SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  initialized_ = true;
  log::info(kTag, "SDL initialized (%s)", SDL_GetRevision());
  return true;
}

bool SdlHost::createWindow(int width, int height, const char* title) {
  const SDL_WindowFlags flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE |
                                SDL_WINDOW_HIGH_PIXEL_DENSITY;
  window_ = SDL_CreateWindow(title, width, height, flags);
  if (!window_) {
    log::error(kTag, "SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  int w = 0, h = 0;
  windowSizeInPixels(&w, &h);
  log::info(kTag, "window %dx%d points, drawable %dx%d pixels", width, height,
            w, h);
  return true;
}

void SdlHost::pumpEvents(InputState& input) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (onQuit) {
        onQuit();
      }
      break;
    case SDL_EVENT_KEY_DOWN:
      input.key(e.key.scancode, true, e.key.repeat);
      break;
    case SDL_EVENT_KEY_UP:
      input.key(e.key.scancode, false, false);
      break;
    case SDL_EVENT_MOUSE_MOTION:
      input.mouseMotion(e.motion.xrel, e.motion.yrel);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      input.mouseButton(e.button.button, true);
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      input.mouseButton(e.button.button, false);
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      input.mouseWheel(e.wheel.x, e.wheel.y, e.wheel.integer_x,
                       e.wheel.integer_y);
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED:
      if (onDrawableSizeChanged) {
        onDrawableSizeChanged(e.window.data1, e.window.data2);
      }
      break;
    default:
      break;
    }
  }
}

void SdlHost::setRelativeMouse(bool enabled) {
  if (!window_) {
    return;
  }
  if (!SDL_SetWindowRelativeMouseMode(window_, enabled)) {
    log::warn(kTag, "relative mouse mode request failed: %s", SDL_GetError());
    return;
  }
  log::info(kTag, "relative mouse mode %s", enabled ? "on" : "off");
}

bool SdlHost::relativeMouse() const {
  return window_ && SDL_GetWindowRelativeMouseMode(window_);
}

void SdlHost::injectSelfTestEvents() {
  if (!window_) {
    return;
  }
  const SDL_WindowID id = SDL_GetWindowID(window_);
  SDL_Event e{};

  e.type = SDL_EVENT_KEY_DOWN;
  e.key.windowID = id;
  e.key.scancode = SDL_SCANCODE_Q;
  e.key.key = SDLK_Q;
  e.key.down = true;
  SDL_PushEvent(&e);

  e = SDL_Event{};
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.motion.windowID = id;
  e.motion.x = 10.0f;
  e.motion.y = 10.0f;
  e.motion.xrel = 5.0f;
  e.motion.yrel = -3.0f;
  SDL_PushEvent(&e);

  e = SDL_Event{};
  e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  e.button.windowID = id;
  e.button.button = SDL_BUTTON_LEFT;
  e.button.down = true;
  SDL_PushEvent(&e);

  e = SDL_Event{};
  e.type = SDL_EVENT_MOUSE_WHEEL;
  e.wheel.windowID = id;
  e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  e.wheel.x = 0.0f;
  e.wheel.y = 1.0f;
  e.wheel.integer_x = 0;
  e.wheel.integer_y = 1;
  SDL_PushEvent(&e);
}

bool SdlHost::verifySelfTestInput(const InputState& input) const {
  return input.keyDown(SDL_SCANCODE_Q) && input.mouseDx() == 5.0f &&
         input.mouseDy() == -3.0f && input.mouseButtonDown(SDL_BUTTON_LEFT) &&
         input.wheelY() == 1.0f && input.wheelTicksY() == 1;
}

// Sentinel device ID stamped on injected selftest events so the
// isolation filter can distinguish them from real hardware input.
static constexpr SDL_MouseID kSelftestMouseID = 0xFEEDC0DE;
static constexpr SDL_KeyboardID kSelftestKeyboardID = 0xFEEDC0DE;

// Event filter for the scripted frontend selftest: keeps injected
// (sentinel-tagged) events, drops all real key/button/motion input so
// the physical devices cannot perturb the deterministic script.
static bool SDLCALL frontendSelftestFilter(void* /*userdata*/,
                                           SDL_Event* e) {
  switch (e->type) {
  case SDL_EVENT_MOUSE_MOTION:
    return e->motion.which == kSelftestMouseID;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
    return e->button.which == kSelftestMouseID;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
    return e->key.which == kSelftestKeyboardID;
  default:
    return true;
  }
}

void SdlHost::isolateHardwareInputForSelftest() {
  SDL_SetEventFilter(frontendSelftestFilter, nullptr);
  // The filter only covers events added from now on — discard real
  // hardware input already queued during window/setup.
  SDL_FlushEvent(SDL_EVENT_KEY_DOWN);
  SDL_FlushEvent(SDL_EVENT_KEY_UP);
  SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
  SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_DOWN);
  SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_UP);
  SDL_FlushEvent(SDL_EVENT_MOUSE_WHEEL);
}

void SdlHost::pushFrontendSelfTestStep(std::uint64_t frameIndex) {
  if (!window_ || frameIndex > 3) {
    return;
  }
  const SDL_WindowID id = SDL_GetWindowID(window_);
  SDL_Event e{};
  switch (frameIndex) {
  case 0:  // DOWN-arrow tap: press+release inside one frame.
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = SDL_SCANCODE_DOWN;
    e.key.key = SDLK_DOWN;
    e.key.down = true;
    SDL_PushEvent(&e);
    e = SDL_Event{};
    e.type = SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = SDL_SCANCODE_DOWN;
    e.key.key = SDLK_DOWN;
    e.key.down = false;
    SDL_PushEvent(&e);
    break;
  case 1:  // Arrow from (300,180) to y=139 — inside item-3's band.
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.which = kSelftestMouseID;
    e.motion.x = 300.0f;
    e.motion.y = 139.0f;
    e.motion.xrel = 0.0f;
    e.motion.yrel = -41.0f;
    if (!SDL_PushEvent(&e)) {
      log::warn(kTag, "frontend selftest: motion push rejected: %s",
                SDL_GetError());
    }
    break;
  case 2:  // Button down: hit-test then activate (same frame).
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = id;
    e.button.which = kSelftestMouseID;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    SDL_PushEvent(&e);
    break;
  case 3:  // Release: re-arms the original's button latch.
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = id;
    e.button.which = kSelftestMouseID;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = false;
    SDL_PushEvent(&e);
    break;
  default:
    break;
  }
}

void SdlHost::windowSizeInPixels(int* w, int* h) const {
  if (window_) {
    SDL_GetWindowSizeInPixels(window_, w, h);
  }
}

void SdlHost::setTitle(const std::string& title) {
  if (window_) {
    SDL_SetWindowTitle(window_, title.c_str());
  }
}

void SdlHost::shutdown() {
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (initialized_) {
    SDL_Quit();
    initialized_ = false;
    log::info(kTag, "SDL shutdown");
  }
}

} // namespace mdk
