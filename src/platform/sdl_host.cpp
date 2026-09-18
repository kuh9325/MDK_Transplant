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

void SdlHost::pushFrontendSelfTestStep(std::uint64_t frameIndex,
                                       bool rootOnly) {
  const std::uint64_t lastStep = rootOnly ? 3 : 63;
  if (!window_ || frameIndex > lastStep) {
    return;
  }
  const SDL_WindowID id = SDL_GetWindowID(window_);
  SDL_Event e{};
  auto keyTap = [&](SDL_Scancode sc, SDL_Keycode kc) {
    e = SDL_Event{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = sc;
    e.key.key = kc;
    e.key.down = true;
    SDL_PushEvent(&e);
    e = SDL_Event{};
    e.type = SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = sc;
    e.key.key = kc;
    e.key.down = false;
    SDL_PushEvent(&e);
  };
  auto motion = [&](float yrel) {
    e = SDL_Event{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.which = kSelftestMouseID;
    e.motion.xrel = 0.0f;
    e.motion.yrel = yrel;
    if (!SDL_PushEvent(&e)) {
      log::warn(kTag, "frontend selftest: motion push rejected: %s",
                SDL_GetError());
    }
  };
  auto button = [&](bool down) {
    e = SDL_Event{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                  : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = id;
    e.button.which = kSelftestMouseID;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = down;
    SDL_PushEvent(&e);
  };
  switch (frameIndex) {
  case 0:  // DOWN tap: root sel 0 -> 1.
    keyTap(SDL_SCANCODE_DOWN, SDLK_DOWN);
    break;
  case 1:  // Arrow (300,180) -> y=139: inside root item-3's band.
    motion(-41.0f);
    break;
  case 2:  // Button down: hit-test then activate -> OpenOptions.
    button(true);
    break;
  case 3:  // Release: re-arms the latch (first options frame, sel 8).
    button(false);
    break;
  case 4:  // Arrow y=139 -> 259: options band 6 (Skill) — Phase 4G.
    motion(120.0f);
    break;
  case 5:  // RIGHT tap: skill +1 (canonical Normal -> Hard), dirty.
    keyTap(SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    break;
  case 6:  // Enter tap: activate cycles skill +1 (Hard -> Easy).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 7:  // LEFT tap: skill -1 (Easy -> Hard).
    keyTap(SDL_SCANCODE_LEFT, SDLK_LEFT);
    break;
  case 8:  // Idle frame: LEFT must be sampled released before the
         // next tap — the repeat deadline (tick+30) only resets on
         // a not-held frame, like the original.
    break;
  case 9:  // LEFT tap: skill -1 (Hard -> Normal); dirty stays set.
    keyTap(SDL_SCANCODE_LEFT, SDLK_LEFT);
    break;
  case 10: // RIGHT tap: skill +1 (Normal -> Hard) — persisted value.
    keyTap(SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    break;
  case 11: // ESC tap: Back -> FUN_00420d68 -> dirty persist fires
          // (persist #1: Skill = final value only).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 12: // Enter tap: root sel 3 -> re-enter options (skill held).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  // ---- Phase 4H display leg (frames 13..24) ----
  case 13: // Arrow y=259 -> 289: options band 7 (the Display row).
    motion(30.0f);
    break;
  case 14: // Enter tap: activate row 7 -> FUN_0041d020 -> display
          // child entered (entry sel 2 = DSP_QUIT).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 15: // Arrow y=289 -> 29: display band 0 (Brightness row).
    motion(-260.0f);
    break;
  case 16: // RIGHT tap: brightness 0 -> 1 (repeat query), dirty.
    keyTap(SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    break;
  case 17: // Enter tap: activate row 0 -> brightness 1 -> 2.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 18: // Arrow y=29 -> 49: display band 1 (detail row).
    motion(20.0f);
    break;
  case 19: // Enter tap: activate row 1 -> ForcePCorrect TRUE, dirty.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 20: // Arrow y=49 -> 89: display band 2 (DSP_QUIT row).
    motion(40.0f);
    break;
  case 21: // Enter tap: activate row 2 -> FUN_0041d144 -> options
          // resumes with selection 7.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 22: // ESC tap: options Back -> FUN_00420d68 -> dirty persist
          // fires (persist #2: Skill + Brightness + ForcePCorrect).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 23: // Enter tap: root sel 3 -> options entry 3 — proves the
          // triple survives process-lifetime (persisted values, not
          // defaults).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 24: // ESC tap: options Back -> root; dirty was cleared by
          // persist #2, no mutation since -> no persist call.
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  // ---- Phase 4I sound leg (frames 25..35) ----
  case 25: // Enter tap: root sel 3 -> options entry 4 (mouse
          // 300,89 -> options sel 8).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 26: // Arrow y=89 -> 90: options band 1 — the Sound row
          // (trunc((90-23)/36) = 1).
    motion(1.0f);
    break;
  case 27: // Enter tap: activate row 1 -> FUN_0042322c -> sound
          // child entered (entry sel 0 — DAT_0054bdbc is BSS-zero;
          // MAINSONG stops, OPTSONG starts — semantic events).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 28: // RIGHT tap: row 0 -> SoundFX +10 (70 -> 80), dirty —
          // OPTBUTT + volume-apply events.
    keyTap(SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    break;
  case 29: // DOWN tap: selection 0 -> 1 (OPTBUTT).
    keyTap(SDL_SCANCODE_DOWN, SDLK_DOWN);
    break;
  case 30: // LEFT tap: row 1 -> SoundMusic -10 (100 -> 90), dirty.
    keyTap(SDL_SCANCODE_LEFT, SDLK_LEFT);
    break;
  case 31: // DOWN tap: selection 1 -> 2 (the Done row).
    keyTap(SDL_SCANCODE_DOWN, SDLK_DOWN);
    break;
  case 32: // Enter tap: activate row 2 -> OPTBUTT then
          // FUN_00423280 (OPTSONG stops, MAINSONG restarts) ->
          // options resumes with selection 1.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 33: // ESC tap: options Back -> FUN_00420d68 -> dirty persist
          // fires (persist #3: all five settings).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 34: // Enter tap: root sel 3 -> options entry 5 — proves the
          // five-tuple survives process-lifetime.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 35: // ESC tap: options Back -> root; dirty cleared by
          // persist #3 -> no persist call.
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  // ---- Phase 4J mouse leg (frames 36..48) ----
  case 36: // Enter tap: root sel 3 -> options entry 6 (mouse
          // 300,90 -> options sel 8).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 37: // Arrow y=90 -> 140: options band 3 — the Mouse row
          // (trunc((140-23)/36) = 3).
    motion(50.0f);
    break;
  case 38: // Enter tap: activate row 3 -> FUN_00421664 -> mouse
          // child entered (DAT_0054bd40/38 reset -> sel 0 col 0).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 39: // DOWN tap: mouse selection 0 -> 1 (the MouseOn row).
    keyTap(SDL_SCANCODE_DOWN, SDLK_DOWN);
    break;
  case 40: // Enter tap: activate row 1 -> MouseOn toggle
          // (TRUE -> FALSE), dirty.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 41: // DOWN tap: selection 1 -> 2 (the MouseYReversed row).
    keyTap(SDL_SCANCODE_DOWN, SDLK_DOWN);
    break;
  case 42: // RIGHT tap: row 2 -> MouseYReversed raw bits 0 -> 1
          // (the float-slot denormal quirk), dirty.
    keyTap(SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    break;
  case 43: // Arrow y=140 -> 45 with x=300 >= 250: grid band 0
          // (trunc((45-33)/16) = 0 -> sel 7), column clamps to 0.
    motion(-95.0f);
    break;
  case 44: // Enter tap: activate grid row 0 -> FUN_00421774 toggles
          // bit 0 of MouseWButtMapA through the exclusivity table
          // (factory mask 1 -> 0), dirty.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 45: // ESC tap: silent exit — DAT_00541493 = 0x0b + RET,
          // options resumes with selection 3 (the Mouse row).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 46: // ESC tap: options Back -> FUN_00420d68 -> dirty persist
          // fires (persist #4: all settings incl. the W-set mouse
          // mutations).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 47: // Enter tap: root sel 3 -> options entry 7 — proves the
          // mutated mouse tuple survives process-lifetime.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 48: // ESC tap: options Back -> root; dirty cleared by
          // persist #4 -> no persist call.
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  // ---- Phase 4K keyboard leg (frames 49..63) ----
  case 49: // Enter tap: root sel 3 -> options entry 8 (mouse
          // 300,45 -> options sel 8).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 50: // Arrow y=45 -> 180: options band 4 — the Keyboard row
          // (trunc((180-23)/36) = 4).
    motion(135.0f);
    break;
  case 51: // Enter tap: activate row 4 -> FUN_0041f030 -> keyboard
          // child entered (DAT_0054bcac = 0x14 — sel 20, the
          // KM_QUIT row).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 52: // Arrow (300,180) -> (300,8): y in [2,18) -> sel 19 —
          // the KM_RESET row.
    motion(-172.0f);
    break;
  case 53: // Enter tap: activate sel 19 -> FUN_00425db0 — the
          // 29-dword mirror copy resets all bindings AND the ten
          // hidden hotkey slots, then dirty = ECX = 1.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 54: // Arrow (300,8) -> (300,304): band trunc((304-50)/30) =
          // 8, x<320 -> sel 8 — the KM_SNIPE row.
    motion(296.0f);
    break;
  case 55: // Enter tap: activate binding row 8 -> DAT_0054bca8 = 1
          // — capture begins (KM_DOIT prompt frame).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 56: // X tap: down+up inside one poll interval — the latch
          // still lands bit 45 in the edge bitmap -> capture
          // commits KeySniper = 45 ('X'), dirty stays latched.
    keyTap(SDL_SCANCODE_X, SDLK_X);
    break;
  case 57: // Enter tap: capture on row 8 again.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 58: // ESC tap INSIDE capture -> DAT_0054bca8 = 0 — cancel
          // only (NOT the normal-state exit; the binding is
          // untouched).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 59: // Arrow (300,304) -> (300,20): y in [18,34) -> sel 20 —
          // the KM_QUIT row.
    motion(-284.0f);
    break;
  case 60: // Enter tap: activate sel 20 -> mode 0x0b + RET —
          // options resumes with selection 4 (the Keyboard row).
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 61: // ESC tap: options Back -> FUN_00420d68 -> dirty persist
          // fires (persist #5: all settings incl. KeySniper = 45).
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  case 62: // Enter tap: root sel 3 -> options entry 9 — proves the
          // rebound key survives process-lifetime.
    keyTap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    break;
  case 63: // ESC tap: options Back -> root; dirty cleared by
          // persist #5 -> no persist call.
    keyTap(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    break;
  default:
    break;
  }
}

void SdlHost::pushGameplaySelfTestStep(std::uint64_t frameIndex) {
  constexpr std::uint64_t lastStep = 16;
  if (!window_ || frameIndex > lastStep) {
    return;
  }
  const SDL_WindowID id = SDL_GetWindowID(window_);
  SDL_Event e{};
  auto key = [&](SDL_Scancode sc, SDL_Keycode kc, bool down) {
    e = SDL_Event{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = sc;
    e.key.key = kc;
    e.key.down = down;
    SDL_PushEvent(&e);
  };
  auto keyTap = [&](SDL_Scancode sc, SDL_Keycode kc) {
    key(sc, kc, true);
    key(sc, kc, false);
  };
  auto motion = [&](float xrel, float yrel) {
    e = SDL_Event{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.which = kSelftestMouseID;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    SDL_PushEvent(&e);
  };
  auto wheel = [&](float y, std::int32_t intY) {
    e = SDL_Event{};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = id;
    e.wheel.which = kSelftestMouseID;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.y = y;
    e.wheel.integer_y = intY;
    SDL_PushEvent(&e);
  };
  auto button = [&](std::uint8_t btn, bool down) {
    e = SDL_Event{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                  : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = id;
    e.button.which = kSelftestMouseID;
    e.button.button = btn;
    e.button.down = down;
    SDL_PushEvent(&e);
  };
  switch (frameIndex) {
  case 0:  // LEFT-arrow down: the KeyLeft level drives turn -1.
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    break;
  case 1:  // held — the level repeats the turn without a new edge.
    break;
  case 2:  // release: the level clears.
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    break;
  case 3:  // SPACE down: KeySniper's edge pulses (factory binding).
    key(SDL_SCANCODE_SPACE, SDLK_SPACE, true);
    break;
  case 4:  // held — the edge does not repeat.
    break;
  case 5:  // release.
    key(SDL_SCANCODE_SPACE, SDLK_SPACE, false);
    break;
  case 6:  // '1' tap -> internal code 2 -> hidden weapon slot 0.
    keyTap(SDL_SCANCODE_1, SDLK_1);
    break;
  case 7:  // '5' tap -> internal code 6 -> hidden weapon slot 4.
    keyTap(SDL_SCANCODE_5, SDLK_5);
    break;
  case 8:  // X (KeySide) + LEFT: the SideStep modifier reroutes the
         // turn level into strafe -1.
    key(SDL_SCANCODE_X, SDLK_X, true);
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    break;
  case 9:  // release both.
    key(SDL_SCANCODE_X, SDLK_X, false);
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    break;
  case 10: // dx +320: the W-set axis-0 letter ('A' factory) ->
         // normalized turn 20/33.333 = 0.6.
    motion(320.0f, 0.0f);
    break;
  case 11: // wheel +1 -> dz 120: the axis-2 letter charges the zoom
         // accumulator (factory 'G').
    wheel(1.0f, 1);
    break;
  case 12: // button A down: mask bit decode (factory Fire).
    button(SDL_BUTTON_LEFT, true);
    break;
  case 13: // A up + C down: the sniper button's synthetic edge.
    button(SDL_BUTTON_LEFT, false);
    button(SDL_BUTTON_MIDDLE, true);
    break;
  case 14: // C held — the synthetic edge does not repeat.
    break;
  case 15: // C up — re-arm the snipe latch.
    button(SDL_BUTTON_MIDDLE, false);
    break;
  case 16: // settle — all controls idle.
    break;
  default:
    break;
  }
}

void SdlHost::pushMotionSelfTestStep(std::uint64_t frameIndex) {
  constexpr std::uint64_t lastStep = 23;
  if (!window_ || frameIndex > lastStep) {
    return;
  }
  const SDL_WindowID id = SDL_GetWindowID(window_);
  SDL_Event e{};
  auto key = [&](SDL_Scancode sc, SDL_Keycode kc, bool down) {
    e = SDL_Event{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = sc;
    e.key.key = kc;
    e.key.down = down;
    SDL_PushEvent(&e);
  };
  auto motion = [&](float xrel, float yrel) {
    e = SDL_Event{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.which = kSelftestMouseID;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    SDL_PushEvent(&e);
  };
  switch (frameIndex) {
  case 0:  // LEFT down: the turn level begins (consumed this frame,
         // integrated next frame — the original's one-frame order).
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    break;
  case 1:
  case 2:  // held.
    break;
  case 3:  // release.
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    break;
  case 4:  // idle.
    break;
  case 5:  // UP down: move-forward level.
    key(SDL_SCANCODE_UP, SDLK_UP, true);
    break;
  case 6:
  case 7:  // held.
    break;
  case 8:  // UP+LEFT down: move + turn together.
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    break;
  case 9:  // held.
    break;
  case 10: // release both.
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    key(SDL_SCANCODE_UP, SDLK_UP, false);
    break;
  case 11: // X+LEFT down: the SideStep modifier reroutes the turn
         // level into strafe -1.
    key(SDL_SCANCODE_X, SDLK_X, true);
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    break;
  case 12: // held.
    break;
  case 13: // release both.
    key(SDL_SCANCODE_X, SDLK_X, false);
    key(SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    break;
  case 14: // LSHIFT+UP down: turbo move.
    key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, true);
    key(SDL_SCANCODE_UP, SDLK_UP, true);
    break;
  case 15:
  case 16: // held.
    break;
  case 17: // release both.
    key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, false);
    key(SDL_SCANCODE_UP, SDLK_UP, false);
    break;
  case 18: // 'W' down: unbound under factory settings; a custom
         // KeyUp='W' makes it drive forward.
    key(SDL_SCANCODE_W, SDLK_W, true);
    break;
  case 19: // held.
    break;
  case 20: // release.
    key(SDL_SCANCODE_W, SDLK_W, false);
    break;
  case 21: // dx +320: the axis-0 letter (factory 'A' -> mouse turn).
    motion(320.0f, 0.0f);
    break;
  case 22:
  case 23: // idle: the mouse impulse integrates then decays.
    break;
  default:
    break;
  }
}

void SdlHost::pushVerticalSelfTestStep(std::uint64_t frameIndex) {
  constexpr std::uint64_t lastStep = 55;
  if (!window_ || frameIndex > lastStep) {
    return;
  }
  const SDL_WindowID id = SDL_GetWindowID(window_);
  auto key = [&](bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.which = kSelftestKeyboardID;
    e.key.scancode = SDL_SCANCODE_LALT;
    e.key.key = SDLK_LALT;
    e.key.down = down;
    SDL_PushEvent(&e);
  };
  switch (frameIndex) {
  case 0:  // LALT down: factory KeyJump=56; the level is consumed this
         // frame and drives the jump machine next frame (latency).
    key(true);
    break;
  case 31: // LALT up: release mid-float — the sustain-end event and
         // normal gravity land next frame.
    key(false);
    break;
  default: // held (1..30) / idle (32..55).
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
