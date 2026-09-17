// Phase 3A unit tests for platform-neutral logic. No SDL, no Metal —
// those paths are exercised by the runtime smoke test instead.

#include "core/compat.h"
#include "core/data_root.h"
#include "core/framebuffer.h"
#include "core/mode_dispatch.h"
#include "core/viewport.h"
#include "input/input_state.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checks;                                                              \
    if (!(cond)) {                                                         \
      ++failures;                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                      \
  } while (0)

bool near(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) < eps;
}

void test_framebuffer() {
  mdk::IndexedFramebuffer fb(8, 4);
  CHECK(fb.width() == 8 && fb.height() == 4);
  CHECK(fb.pixelCount() == 32);

  fb.clear(7);
  CHECK(fb.at(0, 0) == 7 && fb.at(7, 3) == 7);

  fb.put(3, 2, 42);
  CHECK(fb.at(3, 2) == 42);
  CHECK(fb.at(3, 1) == 7); // neighbor untouched

  // Out-of-bounds writes/reads are safe no-ops / zero.
  fb.put(-1, 0, 9);
  fb.put(0, 99, 9);
  CHECK(fb.at(-5, -5) == 0);
  CHECK(fb.at(8, 4) == 0);
}

void test_palette_expand() {
  mdk::Palette pal;
  pal.set(0, {0, 0, 0, 255});
  pal.set(1, {10, 20, 30, 255});
  pal.set(255, {200, 100, 50, 255});

  mdk::IndexedFramebuffer fb(2, 2);
  fb.clear(0);
  fb.put(0, 0, 1);
  fb.put(1, 1, 255);

  std::vector<std::uint8_t> out(fb.pixelCount() * 4);
  mdk::expandToBGRA(fb, pal, out.data());

  // BGRA byte order: B,G,R,A
  CHECK(out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 255);
  CHECK(out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 255);
  const std::size_t last = 3 * 4;
  CHECK(out[last] == 50 && out[last + 1] == 100 && out[last + 2] == 200 &&
        out[last + 3] == 255);
}

void test_viewport() {
  using mdk::aspectFit;
  using mdk::presentationRect;

  // Exact 4:3 window: canvas fills it.
  mdk::RectD r = aspectFit(640, 480, 4.0 / 3.0);
  CHECK(near(r.x, 0) && near(r.y, 0) && near(r.w, 640) && near(r.h, 480));

  // Wide window: pillarbox (full height, centered horizontally).
  r = aspectFit(800, 480, 4.0 / 3.0);
  CHECK(near(r.w, 640) && near(r.h, 480) && near(r.x, 80) && near(r.y, 0));

  // Tall window: letterbox (full width, centered vertically).
  r = aspectFit(640, 600, 4.0 / 3.0);
  CHECK(near(r.w, 640) && near(r.h, 480) && near(r.x, 0) && near(r.y, 60));

  // Degenerate input.
  r = aspectFit(0, 480, 4.0 / 3.0);
  CHECK(r.w == 0 && r.h == 0);

  // presentationRect: 600x360 centered inside the 640x480 canvas.
  // On an exact 4:3 window: x=20, y=60, w=600, h=360.
  r = presentationRect(640, 480);
  CHECK(near(r.x, 20) && near(r.y, 60) && near(r.w, 600) && near(r.h, 360));

  // On a 2x Retina drawable the rect scales linearly.
  r = presentationRect(1280, 960);
  CHECK(near(r.x, 40) && near(r.y, 120) && near(r.w, 1200) &&
        near(r.h, 720));

  // On a wide drawable: pillarboxed canvas (x=160..1440), inner rect
  // centered within it (+40 inside the canvas).
  r = presentationRect(1600, 960);
  CHECK(near(r.w, 1200) && near(r.h, 720));
  CHECK(near(r.x, 200) && near(r.y, 120));
}

void test_data_root() {
  namespace fs = std::filesystem;
  const fs::path tmp =
      fs::temp_directory_path() / "mdk_native_test_dataroot";
  fs::create_directories(tmp);

  std::string err;
  auto root = mdk::DataRoot::open(tmp, &err);
  CHECK(root.has_value());
  CHECK(root->resolve("MISC").filename() == "MISC");

  auto missing = mdk::DataRoot::open(tmp / "no_such_dir", &err);
  CHECK(!missing.has_value());
  CHECK(!err.empty());

  const fs::path filePath = tmp / "a_file.txt";
  { FILE* f = std::fopen(filePath.c_str(), "w"); std::fclose(f); }
  auto notDir = mdk::DataRoot::open(filePath, &err);
  CHECK(!notDir.has_value());

  fs::remove_all(tmp);
}

void test_mode_dispatch() {
  mdk::ModeDispatcher d;
  CHECK(d.primary() == mdk::mode::nativeBoot);
  CHECK(!d.quitRequested());

  int bootFrames = 0, shellFrames = 0;
  d.on(mdk::mode::nativeBoot,
       [&](const mdk::FrameContext&) {
         ++bootFrames;
         d.setPrimary(mdk::mode::nativeShell);
       });
  d.on(mdk::mode::nativeShell,
       [&](const mdk::FrameContext& ctx) {
         ++shellFrames;
         CHECK(ctx.frameIndex >= 1); // shell never runs on frame 0
         if (shellFrames == 3) {
           d.requestQuit();
         }
       });

  mdk::FrameContext ctx;
  for (ctx.frameIndex = 0; ctx.frameIndex < 10 && !d.quitRequested();
       ++ctx.frameIndex) {
    d.dispatch(ctx);
  }
  CHECK(bootFrames == 1);
  CHECK(shellFrames == 3);
  CHECK(d.quitRequested());
  CHECK(d.sub() == mdk::mode::subModeNone);

  // Original observed mode ids remain untouched evidence values.
  CHECK(mdk::mode::observed::frontend == 0);
  CHECK(mdk::mode::observed::traversal == 3);
  CHECK(mdk::mode::observed::cinematic == 8);
}

void test_input_state() {
  mdk::InputState in;
  in.beginFrame();

  in.key(41, true, false);   // down
  in.key(41, true, true);    // repeat
  CHECK(in.keyDown(41));
  CHECK(in.keyEvents().size() == 2);
  in.key(41, false, false);  // up
  CHECK(!in.keyDown(41));
  CHECK(in.keyEvents().size() == 3);

  in.mouseMotion(5.0f, -3.0f);
  in.mouseMotion(1.5f, 0.5f);
  CHECK(near(in.mouseDx(), 6.5) && near(in.mouseDy(), -2.5));

  in.mouseButton(1, true);
  CHECK(in.mouseButtonDown(1));
  in.mouseButton(1, false);
  CHECK(!in.mouseButtonDown(1));
  CHECK(in.buttonEvents().size() == 2);

  in.mouseWheel(0.25f, 1.0f, 0, 1);
  in.mouseWheel(0.0f, -0.5f, 0, -1);
  CHECK(near(in.wheelX(), 0.25) && near(in.wheelY(), 0.5));
  CHECK(in.wheelTicksX() == 0 && in.wheelTicksY() == 0);

  // beginFrame clears per-frame deltas but keeps held state.
  in.key(30, true, false);
  in.beginFrame();
  CHECK(in.keyEvents().empty());
  CHECK(in.mouseDx() == 0 && in.wheelY() == 0);
  CHECK(in.keyDown(30));
}

} // namespace

int main() {
  test_framebuffer();
  test_palette_expand();
  test_viewport();
  test_data_root();
  test_mode_dispatch();
  test_input_state();
  std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
