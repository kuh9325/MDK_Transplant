// mdk-stream-census — Phase 19B.2B1 native host diagnostic.
//
// Drives the real mode-5 StreamScene through the SAME StreamPresenter
// the GDExtension hosts (Godot-free by design) and reports the model
// submission census: resolution, element walk, per-tri A..G material
// reachability, deferred branch counts, misses, and invalid geometry.
//
// Usage:
//   mdk-stream-census --data-path DIR [--course N] [--skill S]
//                     [--seed HEX] [--shots DIR]
//   --course omitted -> runs 0..4 in sequence.
//   --shots DIR -> writes <course>_{early,mid,exit}.png (indexed->
//     RGBA through the applied DAC; ignores write failures).
//
// Asset binding replicates mdk_bridge.cpp::streamLoadAssets_ — the
// OBSERVED STREAM.BNI record names, palette compose, proto parse and
// HUD slot ids — plus the 19B.2B1 model resolver (the pool-index
// aux the core emits in kModelDraw).

#include "stream_presenter.h"
#include "stream_raster.h"

#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/data_root.h"
#include "core/dynamic_objects.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/indexed_image.h"
#include "core/stream_context.h"
#include "core/stream_scene.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

// ------------------------------------------------------------------
// Minimal PNG writer (RGBA8, zlib stored blocks — deterministic, no
// dependencies). Ignored diagnostic output only.
// ------------------------------------------------------------------
std::uint32_t crcTable[256];
bool crcInit = false;

void crcInitTab() {
  for (std::uint32_t i = 0; i != 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k != 8; ++k)
      c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crcTable[i] = c;
  }
  crcInit = true;
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
  std::uint32_t c = 0xffffffffu;
  for (std::size_t i = 0; i != n; ++i)
    c = crcTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(std::uint8_t(x >> 24));
  v.push_back(std::uint8_t(x >> 16));
  v.push_back(std::uint8_t(x >> 8));
  v.push_back(std::uint8_t(x));
}

void chunk(std::vector<std::uint8_t>& out, const char tag[4],
           const std::uint8_t* data, std::size_t n) {
  be32(out, static_cast<std::uint32_t>(n));
  const std::size_t base = out.size();
  for (int i = 0; i != 4; ++i) out.push_back(std::uint8_t(tag[i]));
  out.insert(out.end(), data, data + n);
  be32(out, crc32(out.data() + base, out.size() - base));
}

bool writePng(const std::filesystem::path& path, int w, int h,
              const std::uint8_t* rgba) {
  if (!crcInit) crcInitTab();
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(h) * (w * 4 + 1));
  for (int y = 0; y != h; ++y) {
    raw.push_back(0);                       // filter: none
    raw.insert(raw.end(), rgba + y * w * 4, rgba + (y + 1) * w * 4);
  }
  // zlib: header 0x78 0x01 + stored blocks + adler32.
  std::vector<std::uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  std::size_t pos = 0;
  while (pos < raw.size()) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(std::uint8_t(n));
    z.push_back(std::uint8_t(n >> 8));
    const std::uint16_t nn = static_cast<std::uint16_t>(~n & 0xffff);
    z.push_back(std::uint8_t(nn));
    z.push_back(std::uint8_t(nn >> 8));
    z.insert(z.end(), raw.data() + pos, raw.data() + pos + n);
    pos += n;
  }
  std::uint32_t adler = 1, b = 0;
  for (std::uint8_t x : raw) {
    adler = (adler + x) % 65521;
    b = (b + adler) % 65521;
  }
  be32(z, (b << 16) | adler);

  std::vector<std::uint8_t> png;
  const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a,
                               0x1a, 0x0a};
  png.insert(png.end(), sig, sig + 8);
  std::uint8_t ihdr[13];
  ihdr[0] = std::uint8_t(w >> 24); ihdr[1] = std::uint8_t(w >> 16);
  ihdr[2] = std::uint8_t(w >> 8);  ihdr[3] = std::uint8_t(w);
  ihdr[4] = std::uint8_t(h >> 24); ihdr[5] = std::uint8_t(h >> 16);
  ihdr[6] = std::uint8_t(h >> 8);  ihdr[7] = std::uint8_t(h);
  ihdr[8] = 8;   ihdr[9] = 6;      // RGBA8
  ihdr[10] = 0;  ihdr[11] = 0;     ihdr[12] = 0;
  chunk(png, "IHDR", ihdr, sizeof ihdr);
  chunk(png, "IDAT", z.data(), z.size());
  chunk(png, "IEND", nullptr, 0);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(png.data()),
          static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(f);
}

// ------------------------------------------------------------------
// Stream asset bind — replicates streamLoadAssets_ (the OBSERVED
// record names; BNI payloads re-headed flag=1 for parseGeometryRecord).
// ------------------------------------------------------------------
struct BoundAssets {
  mdk::StreamAssets assets;
  mdkbridge::StreamPresenter presenter;
  std::optional<mdk::RuntimeModel> protoKurt, protoBones, protoProf,
      protoEsc;
  std::vector<std::byte> bniBytes, ftiBytes, hudBytes;
};

bool bindStreamAssets(const mdk::DataRoot& root, int course,
                      BoundAssets& out, std::string& err) {
  auto bni = root.readFile("STREAM/STREAM.BNI", 1 << 28, &err);
  auto fti = root.readFile("MISC/MDKFONT.FTI", 1 << 28, &err);
  auto hud = root.readFile("TRAVERSE/TRAVSPRT.BNI", 1 << 28, &err);
  if (!bni || !fti || !hud) {
    err = "STREAM/MDKFONT/TRAVSPRT read: " + err;
    return false;
  }
  out.bniBytes = std::move(*bni);
  out.ftiBytes = std::move(*fti);
  out.hudBytes = std::move(*hud);
  const auto bdir = mdk::inspectBniDirectory(
      std::span<const std::byte>(out.bniBytes.data(),
                                 out.bniBytes.size()));
  const auto hdir = mdk::inspectBniDirectory(
      std::span<const std::byte>(out.hudBytes.data(),
                                 out.hudBytes.size()));
  const auto fdir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(out.ftiBytes.data(),
                                 out.ftiBytes.size()));
  if (bdir.status != mdk::BniDirectoryStatus::kOk ||
      hdir.status != mdk::BniDirectoryStatus::kOk ||
      fdir.status != mdk::FtiDirectoryStatus::kOk) {
    err = "directory inspect failed";
    return false;
  }
  auto tagOf = [&](const mdk::BniDirectory& d,
                   const char* name) -> int {
    const mdk::BniRecord* r = mdk::findBniRecord(d, name);
    return r ? static_cast<int>(r - d.records.data()) : -1;
  };
  auto payload = [&](const mdk::BniDirectory& d,
                     const std::vector<std::byte>& bytes,
                     const char* name) -> std::span<const std::byte> {
    const mdk::BniRecord* r = mdk::findBniRecord(d, name);
    if (!r) return {};
    return {bytes.data() + r->payloadFileOffset,
            static_cast<std::size_t>(r->payloadEnd -
                                     r->payloadFileOffset)};
  };

  // Composed scene palette: 192B SYS_PAL head + PAL[0xc0..0x300).
  std::array<std::uint8_t, 768> streamPal{};
  if (const mdk::FtiRecord* sp =
          mdk::findFtiRecord(fdir, mdk::kStreamSystemRecord)) {
    out.assets.paletteGlobal =
        reinterpret_cast<const std::uint8_t*>(
            out.ftiBytes.data() + sp->payloadFileOffset);
    std::memcpy(streamPal.data(), out.assets.paletteGlobal, 192);
  }
  const std::span<const std::byte> pal =
      payload(bdir, out.bniBytes, "PAL");
  if (!pal.empty()) {
    out.assets.palettePal =
        reinterpret_cast<const std::uint8_t*>(pal.data()) +
        mdk::kStreamPaletteTailOffset;
    std::memcpy(streamPal.data() + 192, out.assets.palettePal, 576);
  }
  const std::span<const std::byte> palSpan(
      reinterpret_cast<const std::byte*>(streamPal.data()),
      streamPal.size());
  out.presenter.bindRibbonPalette(streamPal.data());

  auto bindIndexed = [&](const mdk::BniDirectory& d,
                         const std::vector<std::byte>& bytes,
                         const char* name, int tag,
                         int* outW = nullptr,
                         int* outH = nullptr) -> bool {
    const std::span<const std::byte> p = payload(d, bytes, name);
    if (p.empty() || tag < 0) return false;
    auto img = mdk::decodeBniIndexedImage(p, palSpan, nullptr);
    if (!img) return false;
    if (outW) *outW = img->width;
    if (outH) *outH = img->height;
    out.presenter.bindImage(tag, std::move(*img));
    return true;
  };

  mdk::StreamAssets& a = out.assets;
  a.bgTag = tagOf(bdir, "BG");
  bindIndexed(bdir, out.bniBytes, "BG", a.bgTag);
  int planetW = 0, planetH = 0;
  a.planetTag[0] = tagOf(bdir, "PLANET");
  if (bindIndexed(bdir, out.bniBytes, "PLANET", a.planetTag[0],
                  &planetW, &planetH)) {
    a.planetTag[1] = planetW;
    a.planetTag[2] = planetH;
    a.planetTag[3] = planetW * planetH;
  }
  a.lightTag = tagOf(bdir, "LIGHT");
  bindIndexed(bdir, out.bniBytes, "LIGHT", a.lightTag);
  a.sndWind = tagOf(bdir, "WIND");
  a.sndHitside = tagOf(bdir, "HITSIDE");
  a.sndRescue = tagOf(bdir, "RESCUE");
  a.sndApple = tagOf(bdir, "APPLE");
  for (int i = 0; i != 7; ++i) {
    char nm[8];
    std::snprintf(nm, sizeof nm, "HURT%d", i + 1);
    a.sndHurt[i] = tagOf(bdir, nm);
  }
  a.hudIconTag = 2;   // engine slot 2 = SC_STAT
  a.hudDigitTag = 7;  // engine slot 7 = SNIP_TXT
  bindIndexed(hdir, out.hudBytes, "SC_STAT", a.hudIconTag,
              &a.hudIconW, &a.hudIconH);
  bindIndexed(hdir, out.hudBytes, "SNIP_TXT", a.hudDigitTag,
              &a.hudDigitW, &a.hudDigitH);

  auto bindProto = [&](const char* name,
                       std::optional<mdk::RuntimeModel>& pm) {
    const std::span<const std::byte> p = payload(bdir, out.bniBytes,
                                                 name);
    if (p.empty()) return;
    std::vector<std::uint8_t> headed(4 + p.size());
    const std::uint8_t fl[4] = {1, 0, 0, 0};
    std::memcpy(headed.data(), fl, 4);
    std::memcpy(headed.data() + 4, p.data(), p.size());
    pm = mdk::parseGeometryRecord(headed.data(),
                                  headed.data() + headed.size());
  };
  bindProto("KURT", out.protoKurt);
  bindProto("BONES", out.protoBones);
  bindProto("PROFSHIP", out.protoProf);
  const bool isFinal = course >= 4;
  bindProto(isFinal ? "GUNTA" : "SWH150", out.protoEsc);
  a.protoKurt = out.protoKurt ? &*out.protoKurt : nullptr;
  a.protoBones = out.protoBones ? &*out.protoBones : nullptr;
  a.protoProfship = out.protoProf ? &*out.protoProf : nullptr;
  a.protoEscort = out.protoEsc ? &*out.protoEsc : nullptr;

  auto payloadPtr = [&](const char* name) -> const std::uint8_t* {
    const std::span<const std::byte> p =
        payload(bdir, out.bniBytes, name);
    return p.empty() ? nullptr
                     : reinterpret_cast<const std::uint8_t*>(p.data());
  };
  a.animEscort = payloadPtr(isFinal ? "GUNTANIM" : "SWHANM");
  a.animBones = payloadPtr("BONESANIM");
  a.animKurt = payloadPtr("KURTANIM");
  a.animHvr = payloadPtr("FL_HVR");
  a.animWave = payloadPtr("FL_WAVE");
  a.animLimit = reinterpret_cast<const std::uint8_t*>(
      out.bniBytes.data() + out.bniBytes.size());

  std::string ferr;
  auto decodeFont = [&](const char* name)
      -> std::optional<mdk::FtiFont> {
    const mdk::FtiRecord* r = mdk::findFtiRecord(fdir, name);
    if (!r) return std::nullopt;
    return mdk::decodeFtiFont(
        std::span<const std::byte>(
            out.ftiBytes.data() + r->payloadFileOffset,
            static_cast<std::size_t>(r->payloadEnd -
                                     r->payloadFileOffset)),
        &ferr);
  };
  const auto fbBig = decodeFont("FONTBIG");
  const auto fbSml = decodeFont("FONTSML");
  if (fbBig && fbSml) out.presenter.bindFonts(*fbBig, *fbSml);
  return true;
}

const char* kMatName[7] = {"A.material", "B.flat", "C.fx47a770",
                           "D.flat",     "E.lut",  "F.fx46e940",
                           "G.lut"};

int runCourse(const mdk::DataRoot& root, int course, int skill,
              std::uint32_t seed, const std::string& shotDir) {
  BoundAssets bnd;
  std::string err;
  if (!bindStreamAssets(root, course, bnd, err)) {
    std::fprintf(stderr, "course %d: %s\n", course, err.c_str());
    return 1;
  }
  mdk::StreamScene sc;
  if (!sc.init(bnd.assets, course, skill, seed, 100)) {
    std::fprintf(stderr, "course %d: StreamScene::init failed\n",
                 course);
    return 1;
  }
  mdkbridge::StreamPresenter& pr = bnd.presenter;
  pr.bindModelResolver(
      [&sc](int i) -> const mdk::DynamicObject* {
        return (i >= 0 && i < mdk::kStreamPoolSize)
                   ? &sc.objectAt(i)
                   : nullptr;
      });

  mdk::StreamInput in{};
  in.nowMs = 1000;                       // the synthetic 46c650 clock
  int frames = 0;
  bool exited = false;
  // Shot ring: the first presented frame, a checkpoint nearest half
  // the presented total (chosen once the total is known), and the
  // last presented NON-exit frame (the terminal-fill frame is a flat
  // clear — excluded so "exit" shows the scene content). The applied
  // DAC palette is captured WITH the pixels — the palette mutates per
  // kPaletteSet and ends at the terminal fill.
  struct Shot {
    mdk::Palette pal;
    std::vector<std::uint8_t> px;
  };
  Shot early, mid, last;
  struct Ckpt {
    int seq;
    mdk::Palette pal;
    std::vector<std::uint8_t> px;
  };
  std::vector<Ckpt> ckpts;
  while (frames < 4000) {
    sc.step(in, 1.0f / 30.0f);
    in.nowMs += 33;                             // ~30.3fps wall ms
    ++frames;
    const int fillsBefore = pr.diag().terminalFills;
    for (const mdk::StreamEvent& ev : sc.events())
      pr.consume(ev, sc.paletteDac());
    sc.clearEvents();
    if (pr.framePending()) {
      pr.clearFramePending();
      const int seq = pr.diag().presented;
      const auto& fb = pr.framebuffer();
      Shot snap{pr.palette(),
                std::vector<std::uint8_t>(fb.pixels(),
                                          fb.pixels() +
                                              fb.pixelCount())};
      const bool exitFill =
          pr.diag().terminalFills != fillsBefore;
      if (seq == 1) early = snap;
      if (!exitFill) {
        last = snap;
        if (seq % 20 == 0) ckpts.push_back({seq, snap.pal, snap.px});
      }
    }
    if (sc.finished()) { exited = true; break; }
  }
  const auto& d = pr.diag();
  const mdk::StreamSnapshot s = sc.snapshot();
  std::printf(
      "course %d: frames=%d pres=%d exit=%d health=%d "
      "complete=%d fill=0x%02x\n",
      course, frames, d.presented, exited ? 1 : 0, s.health,
      s.complete, d.terminalFill < 0 ? -1 : d.terminalFill);
  std::printf(
      "  events: spr=%d sprdraw=%d sprmiss=%d hud=%d hmiss=%d tt=%d "
      "pal=%d rib=%d snd=%d fb=%016llx\n",
      d.sprites, d.spriteDrawn, d.spriteMisses, d.hudBlits,
      d.hudMisses, d.teletypeDraws, d.paletteSets,
      d.ribbon.commands, d.soundEvents,
      (unsigned long long)d.fbHash);
  std::printf(
      "  ribbon: cmd=%d rast=%d zero=%d clip=%d drop=%d unsup=%d "
      "lutmiss=%d px=%llu\n",
      d.ribbon.commands, d.ribbon.rasterized, d.ribbon.zeroPixels,
      d.ribbon.clipped, d.ribbon.clipDropped, d.ribbon.unsupported,
      d.ribbon.lutMisses, (unsigned long long)d.ribbon.pixels);
  for (const auto& [id, n] : d.ribbon.branch)
    std::printf("    rbranch %s n=%d\n",
                mdkbridge::streamTriBranchName(
                    static_cast<mdkbridge::StreamTriBranch>(id)),
                n);
  std::printf(
      "  model: cmd=%d res=%d miss=%d rec0=%d elemWalk=%d "
      "elemMask=%d triWalk=%d backface=%d overflow=%d invalid=%d "
      "flush=%d\n",
      d.model.commands, d.model.resolved, d.model.lookupMiss,
      d.model.classRec0, d.model.elementsWalked,
      d.model.elementsMasked, d.model.trisWalked, d.model.polysBackface,
      d.model.polysOverflow, d.model.invalidGeometry, d.model.flushes);
  std::printf("  model census A..G:");
  for (int i = 0; i != 7; ++i)
    std::printf(" %s=%d", kMatName[i], d.model.matCls[i]);
  std::printf("\n");
  std::printf(
      "  model raster: px=%llu clip=%d drop=%d unsup=%d lutmiss=%d "
      "fbdig=%016llx\n",
      (unsigned long long)d.model.raster.pixels,
      d.model.raster.clipped, d.model.raster.clipDropped,
      d.model.raster.unsupported, d.model.raster.lutMisses,
      (unsigned long long)d.model.fbDigest);
  for (const auto& [id, n] : d.model.raster.branch)
    std::printf("    dispatch %s n=%d\n",
                mdkbridge::streamTriBranchName(
                    static_cast<mdkbridge::StreamTriBranch>(id)),
                n);

  if (!shotDir.empty()) {
    const int midT = d.presented / 2;
    const Ckpt* pick = nullptr;
    for (const Ckpt& c : ckpts)
      if (pick == nullptr ||
          std::abs(c.seq - midT) < std::abs(pick->seq - midT))
        pick = &c;
    if (pick) mid = {pick->pal, pick->px};
    auto expand = [&](const Shot& s, std::vector<std::uint8_t>& rgba) {
      rgba.resize(s.px.size() * 4);
      for (std::size_t i = 0; i != s.px.size(); ++i) {
        const mdk::Palette::Color c = s.pal.get(s.px[i]);
        rgba[i * 4 + 0] = c.r;
        rgba[i * 4 + 1] = c.g;
        rgba[i * 4 + 2] = c.b;
        rgba[i * 4 + 3] = 255;
      }
    };
    std::error_code ec;
    std::filesystem::create_directories(shotDir, ec);
    auto save = [&](const char* tag, const Shot& s) {
      if (s.px.empty()) return;
      std::vector<std::uint8_t> rgba;
      expand(s, rgba);
      const auto path = std::filesystem::path(shotDir) /
          ("stream_c" + std::to_string(course) + "_" + tag + ".png");
      const bool okw = writePng(path, pr.framebuffer().width(),
                                pr.framebuffer().height(),
                                rgba.data());
      std::printf("  shot %-6s -> %s (%s)\n", tag, path.c_str(),
                  okw ? "ok" : "WRITE FAIL");
    };
    save("early", early);
    save("mid", mid);
    save("exit", last);
  }
  return 0;
}

int usage() {
  std::fprintf(stderr,
               "usage: mdk-stream-census --data-path DIR [--course N]\n"
               "       [--skill S] [--seed HEX] [--shots DIR]\n");
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  std::string dataPath;
  int course = -1, skill = 1;
  std::uint32_t seed = 0xC0FFEE;
  std::string shotDir;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
    if (!std::strcmp(a, "--data-path") && v) dataPath = v, ++i;
    else if (!std::strcmp(a, "--course") && v) course = std::atoi(v), ++i;
    else if (!std::strcmp(a, "--skill") && v) skill = std::atoi(v), ++i;
    else if (!std::strcmp(a, "--seed") && v)
      seed = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 0)), ++i;
    else if (!std::strcmp(a, "--shots") && v) shotDir = v, ++i;
    else return usage();
  }
  if (dataPath.empty()) return usage();
  std::string err;
  const auto root = mdk::DataRoot::open(dataPath, &err);
  if (!root) {
    std::fprintf(stderr, "data root: %s\n", err.c_str());
    return 2;
  }
  int rc = 0;
  if (course >= 0) rc = runCourse(*root, course, skill, seed, shotDir);
  else
    for (int c = 0; c != 5; ++c) rc |= runCourse(*root, c, skill, seed,
                                                 shotDir);
  return rc;
}
