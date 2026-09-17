#include "app/application.h"

#include "app/diagnostic_scene.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/clock.h"
#include "core/compat.h"
#include "core/data_root.h"
#include "core/file_family.h"
#include "core/framebuffer.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/indexed_image.h"
#include "core/log.h"
#include "core/mode_dispatch.h"
#include "core/stream_context.h"
#include "input/input_state.h"
#include "platform/sdl_host.h"
#include "renderer/presenter.h"

#include <SDL3/SDL_scancode.h> // SDL_SCANCODE_ESCAPE for the shell quit key

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mdk {

static constexpr const char* kTag = "app";

// Read cap for --preview-resource source files — far above the
// largest BNI bundle in BUILD_A (~2.5 MB) while staying a sane bound.
static constexpr std::size_t kPreviewMaxBytes = 512ull * 1024 * 1024;

// Load + decode the --preview-resource target: DataRoot -> BNI
// directory -> named record -> bitmap decoder. Paletted records use
// the embedded palette (Phase 4A); the one proven indexed-only
// context (STREAM/STREAM.BNI BG) resolves its external palette per
// the original binding (Phase 4B). Fills `err` and returns nullopt
// on any failure.
static std::optional<IndexedImage> loadPreviewImage(
    DataRoot& root, const std::string& relFile, const std::string& name,
    std::string* err) {
  if (fileFamilyForPath(relFile) != MdkFileFamily::kBni) {
    *err = "preview supports BNI resources only (Phase 4A/4B decoder "
           "coverage): " + relFile;
    return std::nullopt;
  }
  const auto file = root.readFile(relFile, kPreviewMaxBytes, err);
  if (!file) {
    return std::nullopt;
  }
  const auto dir = inspectBniDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  if (dir.status != BniDirectoryStatus::kOk) {
    *err = "BNI directory: " +
           std::string(bniDirectoryStatusName(dir.status)) + " — " +
           dir.detail;
    return std::nullopt;
  }
  const BniRecord* rec = findBniRecord(dir, name);
  if (!rec) {
    *err = "record not found: " + name;
    return std::nullopt;
  }
  const std::span<const std::byte> payload(
      file->data() + rec->payloadFileOffset, rec->payloadSize());
  const auto probe = probeBniImage(payload);
  if (probe.shape == BniImageShape::kIndexedOnly) {
    // Indexed-only records need the consumer's external palette.
    // The only binding proven so far is the STREAM backdrop
    // (FUN_0042b270): SYS_PAL head + PAL record tail.
    if (!isStreamBackdropRequest(relFile, rec->name())) {
      *err = "indexed-only record has no proven external-palette "
             "binding in this context (Phase 4B proves " +
             std::string(kStreamBniFile) + " " +
             std::string(kStreamImageRecord) + " only)";
      return std::nullopt;
    }
    const auto fti = root.readFile(std::string(kStreamSystemFile),
                                   kPreviewMaxBytes, err);
    if (!fti) {
      return std::nullopt;
    }
    auto img = decodeStreamBackdrop(
        std::span<const std::byte>(file->data(), file->size()),
        std::span<const std::byte>(fti->data(), fti->size()), err);
    if (img) {
      log::info(kTag, "preview: %s %s — %dx%d indexed, %llu pixel "
                "bytes, STREAM context palette (SYS_PAL[0:64] + "
                "PAL[64:256]), digest=%016llx",
                relFile.c_str(), rec->name().c_str(), img->width,
                img->height,
                static_cast<unsigned long long>(img->pixels.size()),
                static_cast<unsigned long long>(imageDigest(*img)));
    }
    return img;
  }
  auto img = decodeBniPalettedImage(payload, err);
  if (img) {
    log::info(kTag, "preview: %s %s — %dx%d indexed, %llu pixel bytes, "
              "256-entry embedded palette, digest=%016llx",
              relFile.c_str(), rec->name().c_str(), img->width,
              img->height,
              static_cast<unsigned long long>(img->pixels.size()),
              static_cast<unsigned long long>(imageDigest(*img)));
  }
  return img;
}

// Phase 4C font preview: resolve an FTI record, decode it with the
// proven FONTSML/FONTBIG glyph decoder, bind the resident SYS_PAL
// head palette from the same FTI file, and draw the glyphs into the
// indexed framebuffer. Without `text` every mapped glyph is drawn as
// a diagnostic atlas (evidence-neutral grid layout); with `text` the
// byte string is drawn once using the proven advance rule (glyph
// width; the record's proven missing-glyph advance for unmapped
// bytes). Fills `err` and returns false on any failure.
static bool loadFontPreview(DataRoot& root, const std::string& relFile,
                            const std::string& recName,
                            const std::optional<std::string>& text,
                            IndexedFramebuffer& fb, Palette& palette,
                            std::string* err) {
  if (fileFamilyForPath(relFile) != MdkFileFamily::kFti) {
    *err = "--preview-font supports .FTI font records only (Phase 4C "
           "decoder coverage): " + relFile;
    return false;
  }
  const auto file = root.readFile(relFile, kPreviewMaxBytes, err);
  if (!file) {
    return false;
  }
  const auto dir = inspectFtiDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  if (dir.status != FtiDirectoryStatus::kOk) {
    *err = "FTI directory: " +
           std::string(ftiDirectoryStatusName(dir.status)) + " — " +
           dir.detail;
    return false;
  }
  const FtiRecord* rec = findFtiRecord(dir, recName);
  if (!rec) {
    *err = "record not found: " + recName;
    return false;
  }
  const std::span<const std::byte> payload(
      file->data() + rec->payloadFileOffset, rec->payloadSize());
  std::string derr;
  const auto font = decodeFtiFont(payload, &derr);
  if (!font) {
    *err = "font decode: " + derr;
    return false;
  }

  // Palette binding (CORROBORATED): glyph bytes are final palette
  // indices and every index used by the real fonts is <= 62 — inside
  // the resident 64-entry system palette head SYS_PAL which the
  // original loads from this same FTI (FUN_0040163c). Entries 64-255
  // are never referenced by the glyphs; they stay zeroed.
  const FtiRecord* palRec = findFtiRecord(dir, "SYS_PAL");
  if (!palRec || palRec->payloadSize() < 192) {
    *err = "SYS_PAL record missing/short in " + relFile +
           " — no proven palette binding for this font";
    return false;
  }
  const std::byte* sp = file->data() + palRec->payloadFileOffset;
  for (int i = 0; i < 64; ++i) {
    palette.set(i, {static_cast<std::uint8_t>(sp[i * 3 + 0]),
                    static_cast<std::uint8_t>(sp[i * 3 + 1]),
                    static_cast<std::uint8_t>(sp[i * 3 + 2]), 255});
  }

  // Per-consumer missing-glyph advance (a constant in the original
  // draw code, not the payload): 4 for the FONTSML path
  // (FUN_00414dd4), 6 for FONTBIG (FUN_00414c34).
  int missingAdvance = 0;
  const std::string canonName = rec->name();  // stored (uppercase) name
  if (canonName == "FONTSML") {
    missingAdvance = kFtiFontSmlMissingAdvance;
  } else if (canonName == "FONTBIG") {
    missingAdvance = kFtiFontBigMissingAdvance;
  }

  fb.clear(0);  // index 0 = SYS_PAL[0] = black

  if (text) {
    if (missingAdvance == 0) {
      *err = "text preview needs a proven missing-glyph advance — only "
             "FONTSML (4) and FONTBIG (6) have one established";
      return false;
    }
    // Baseline placement: put the pen row below the tallest ascent in
    // the string (diagnostic placement — the original callers choose
    // the pen; only the per-glyph rule is proven).
    int maxTop = 0;
    for (const char ch : *text) {
      if (const FtiGlyph* g =
              font->glyphFor(static_cast<std::uint8_t>(ch))) {
        maxTop = std::max(maxTop, int(g->top));
      }
    }
    drawFtiText(*font, *text, fb, 8, maxTop + 8, missingAdvance);
    log::info(kTag,
              "font preview: %s %s — %zu mapped glyphs, text %zu bytes, "
              "digest=%016llx",
              relFile.c_str(), rec->name().c_str(), font->mappedCount,
              text->size(),
              static_cast<unsigned long long>(ftiFontDigest(*font)));
    return true;
  }

  // Atlas: every mapped glyph in table order, one diagnostic cell
  // each. Cell height covers the largest top+bottom extent so every
  // glyph draws fully inside its cell; padding is dropped if the
  // atlas would not fit the work buffer (diagnostic layout only).
  int maxW = 0, maxTop = 0, maxBot = 0;
  for (const auto& g : font->glyphs) {
    if (g) {
      maxW = std::max(maxW, int(g->width));
      maxTop = std::max(maxTop, int(g->top));
      maxBot = std::max(maxBot, int(g->bottom));
    }
  }
  int pad = 2;
  for (;;) {
    const int cellW = maxW + pad;
    const int cellH = maxTop + maxBot + 1 + pad;
    const int cols = std::max(1, fb.width() / cellW);
    const int rows =
        (int(font->mappedCount) + cols - 1) / cols;
    if (rows * cellH <= fb.height() || pad == 0) {
      int drawn = 0;
      for (const auto& g : font->glyphs) {
        if (!g) {
          continue;
        }
        const int col = drawn % cols, row = drawn / cols;
        drawFtiGlyph(*g, fb, col * cellW + pad / 2,
                     row * cellH + pad / 2 + maxTop);
        ++drawn;
      }
      if (rows * cellH > fb.height()) {
        log::warn(kTag, "font atlas exceeds %dpx height — lower rows "
                  "clipped", fb.height());
      }
      break;
    }
    pad -= 2;
  }
  log::info(kTag,
            "font preview: %s %s — %zu mapped glyphs (codes %d-%d), "
            "max pixel index %u, digest=%016llx",
            relFile.c_str(), rec->name().c_str(), font->mappedCount,
            font->firstMapped, font->lastMapped, font->maxPixelIndex,
            static_cast<unsigned long long>(ftiFontDigest(*font)));
  return true;
}

Application::Application(AppConfig cfg) : cfg_(std::move(cfg)) {}

int Application::run() {
  SdlHost host;
  if (!host.init()) {
    return 1;
  }
  if (!host.createWindow(cfg_.windowWidth, cfg_.windowHeight, "MDK-Native")) {
    return 1;
  }

  std::string err;
  auto presenter = createMetalPresenter(host.window(), &err);
  if (!presenter) {
    log::error(kTag, "presenter init failed: %s", err.c_str());
    return 1;
  }
  log::info(kTag, "presenter: %s", presenter->name());

  ModeDispatcher dispatcher;
  host.onQuit = [&] { dispatcher.requestQuit(); };
  host.onDrawableSizeChanged = [&](int w, int h) {
    log::info(kTag, "drawable resized to %dx%d", w, h);
    presenter->drawableSizeChanged(w, h);
  };

  std::optional<DataRoot> dataRoot;
  if (cfg_.dataPath) {
    std::string derr;
    dataRoot = DataRoot::open(*cfg_.dataPath, &derr);
    if (!dataRoot) {
      log::error(kTag, "%s", derr.c_str());
      return 2;
    }
    log::info(kTag, "data root (read-only): %s",
              dataRoot->path().string().c_str());
    log::info(kTag, "resolver ready");
  } else {
    log::info(kTag, "no --data-path; diagnostic shell does not need data");
  }

  if (cfg_.relativeMouse) {
    host.setRelativeMouse(true);
  }

  IndexedFramebuffer fb(compat::kWorkWidth, compat::kWorkHeight);
  Palette palette;
  DiagnosticScene scene;

  // Phase 4A preview mode: one proven original visual resource
  // decoded into the indexed framebuffer, then presented unchanged
  // every frame. The synthetic diagnostic scene stays the default
  // when no preview is requested.
  const bool previewMode =
      cfg_.previewFile.has_value() || cfg_.fontPreviewFile.has_value();
  if (cfg_.previewFile) {
    if (!dataRoot) {
      log::error(kTag, "--preview-resource requires --data-path");
      return 2;
    }
    std::string perr;
    auto img = loadPreviewImage(*dataRoot, *cfg_.previewFile,
                                cfg_.previewRecord.value_or(""), &perr);
    if (!img) {
      log::error(kTag, "preview failed: %s", perr.c_str());
      return 2;
    }
    fb.clear(0);
    blitIndexedImage(*img, fb, palette);
  } else if (cfg_.fontPreviewFile) {
    if (!dataRoot) {
      log::error(kTag, "--preview-font requires --data-path");
      return 2;
    }
    std::string perr;
    if (!loadFontPreview(*dataRoot, *cfg_.fontPreviewFile,
                         cfg_.fontPreviewRecord.value_or(""),
                         cfg_.fontPreviewText, fb, palette, &perr)) {
      log::error(kTag, "font preview failed: %s", perr.c_str());
      return 2;
    }
  } else {
    scene.buildPalette(palette);
  }

  InputState input;
  Clock clock;

  // Mode handlers: boot -> shell -> (quit flag) demonstrates the
  // dispatcher shape without implementing original modes.
  dispatcher.on(mode::nativeBoot, [&](const FrameContext&) {
    log::info(kTag, "mode boot -> shell");
    dispatcher.setPrimary(mode::nativeShell);
  });
  dispatcher.on(mode::nativeShell, [&](const FrameContext& ctx) {
    if (input.keyDown(SDL_SCANCODE_ESCAPE)) {
      dispatcher.requestQuit();
      return;
    }
    scene.update(ctx.frameIndex, input);
  });

  if (cfg_.selftest) {
    host.injectSelfTestEvents();
    if (cfg_.frames == 0) {
      cfg_.frames = 10;
    }
  }

  while (!dispatcher.quitRequested()) {
    input.beginFrame();
    host.pumpEvents(input);
    const FrameTick t = clock.tick();

    if (cfg_.selftest && t.index == 0) {
      selftestOk_ = host.verifySelfTestInput(input);
      log::info(kTag, "input selftest: %s",
                selftestOk_ ? "PASS" : "FAIL");
    }

    dispatcher.dispatch({t.index, t.dtSeconds, t.elapsedSeconds});

    // Preview frames are static: the decoded image was blitted once
    // before the loop; the diagnostic scene owns rendering otherwise.
    if (!previewMode) {
      scene.render(fb, palette);
    }
    if (!presenter->present(fb, palette)) {
      log::warn(kTag, "present failed (frame %llu)",
                static_cast<unsigned long long>(t.index));
    }

    if (t.index % 30 == 0) {
      char title[96];
      std::snprintf(title, sizeof(title), "MDK-Native — frame %llu",
                    static_cast<unsigned long long>(t.index));
      host.setTitle(title);
    }
    if (cfg_.frames != 0 && t.index + 1 >= cfg_.frames) {
      dispatcher.requestQuit();
    }
  }

  if (cfg_.dumpPpm) {
    // Debug verification hook: write the last presented frame (post
    // palette expansion — exactly what was uploaded to Metal) as P6 PPM.
    std::vector<std::uint8_t> bgra(fb.pixelCount() * 4);
    expandToBGRA(fb, palette, bgra.data());
    if (FILE* f = std::fopen(cfg_.dumpPpm->c_str(), "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", fb.width(), fb.height());
      for (std::size_t i = 0; i < fb.pixelCount(); ++i) {
        std::fputc(bgra[i * 4 + 2], f); // R
        std::fputc(bgra[i * 4 + 1], f); // G
        std::fputc(bgra[i * 4 + 0], f); // B
      }
      std::fclose(f);
      log::info(kTag, "dumped frame %llu to %s",
                static_cast<unsigned long long>(clock.frameCount() - 1),
                cfg_.dumpPpm->c_str());
    } else {
      log::warn(kTag, "could not write %s", cfg_.dumpPpm->c_str());
    }
  }

  presenter.reset(); // Metal view must die before the window
  host.shutdown();
  log::info(kTag, "shutdown complete after %llu frames",
            static_cast<unsigned long long>(clock.frameCount()));
  return selftestOk_ ? 0 : 3;
}

bool parseArgs(int argc, char** argv, AppConfig& cfg, std::string& error,
               bool& showHelp) {
  showHelp = false;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto needValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        error = std::string(name) + " requires a value";
        return nullptr;
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
      showHelp = true;
      return true;
    } else if (!std::strcmp(a, "--data-path")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.dataPath = v;
    } else if (!std::strcmp(a, "--dump-ppm")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.dumpPpm = v;
    } else if (!std::strcmp(a, "--frames")) {
      const char* v = needValue(a);
      if (!v) return false;
      cfg.frames = std::strtoull(v, nullptr, 10);
    } else if (!std::strcmp(a, "--preview-resource")) {
      const char* f = needValue(a);
      if (!f) return false;
      const char* r = needValue(a);
      if (!r) return false;
      cfg.previewFile = f;
      cfg.previewRecord = r;
    } else if (!std::strcmp(a, "--preview-font")) {
      const char* f = needValue(a);
      if (!f) return false;
      const char* r = needValue(a);
      if (!r) return false;
      cfg.fontPreviewFile = f;
      cfg.fontPreviewRecord = r;
      // Optional trailing TEXT argument (must not look like a flag).
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        cfg.fontPreviewText = argv[++i];
      }
    } else if (!std::strcmp(a, "--selftest")) {
      cfg.selftest = true;
    } else if (!std::strcmp(a, "--no-relative-mouse")) {
      cfg.relativeMouse = false;
    } else {
      error = std::string("unknown argument: ") + a;
      return false;
    }
  }
  return true;
}

} // namespace mdk
