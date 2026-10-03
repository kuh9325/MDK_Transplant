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

#include "stream_audio.h"
#include "stream_presenter.h"
#include "stream_raster.h"

#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/data_root.h"
#include "core/dynamic_objects.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/indexed_image.h"
#include "core/mti_directory.h"
#include "core/sni_wave.h"
#include "core/stream_context.h"
#include "core/stream_scene.h"
#include "core/traversal_audio_mixer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
  mdk::BniDirectory bdir;          // aliases bniBytes (19B.3B1
                                   // sound payloads + registrations)
  // 19B.2C — STREAM.MTI bytes + the decoded bank A; model material
  // tables point into bankA (pixels alias mtiBytes). bankANames is
  // parallel to bankA — the decoded record's 8-byte name for the
  // closure identity report.
  std::vector<std::byte> mtiBytes;
  std::vector<mdk::ArenaRenderMaterial> bankA;
  std::vector<std::string> bankANames;
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
  out.bdir = bdir;
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

  // 19B.2C — STREAM.MTI = the model material bank A (mirrors the
  // bridge's streamLoadAssets_ wiring); resolve each proto's model
  // name table against it.
  auto mti = root.readFile("STREAM/STREAM.MTI", 1 << 28, &err);
  if (mti) {
    out.mtiBytes = std::move(*mti);
    const std::span<const std::byte> mtiSpan(
        out.mtiBytes.data(), out.mtiBytes.size());
    const auto mdir = mdk::inspectMtiDirectory(mtiSpan);
    if (mdir.status == mdk::MtiDirectoryStatus::kOk) {
      out.bankA.reserve(mdir.entries.size());
      for (const mdk::MtiEntry& e : mdir.entries) {
        mdk::ArenaRenderMaterial rec;
        if (mdk::arenaRenderMaterialDecode(
                mtiSpan, e.payloadFileOffset(), e.fieldAt0x08,
                e.fieldAt0x0C, e.fieldAt0x10, e.nameField, &rec)) {
          out.bankA.push_back(std::move(rec));
          out.bankANames.push_back(e.name());
        }
      }
    }
    const std::span<const mdk::ArenaRenderMaterial> bank(
        out.bankA.data(), out.bankA.size());
    for (auto* mp : {&out.protoKurt, &out.protoBones, &out.protoProf,
                     &out.protoEsc})
      if (*mp) mdk::resolveModelMaterials(**mp, bank);
  }

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

  // 19B.3B1 — the audio host half: the OBSERVED 039c8/02e2c
  // registration table, the shared 63-voice pool, and a BNI-backed
  // resolver mirroring the bridge's streamSndEntry_ (vol/loop from
  // the registrations; rate/frames from decodeSniWave — memoized).
  mdkbridge::StreamAudioHost audio;
  mdkbridge::streamAudioBindRegs(audio, bnd.bdir);
  mdk::TraversalAudioMixer mixer;
  std::map<std::string, mdk::TraversalAudioSoundDef> sndDefs;
  int sndDecodeMisses = 0;
  const auto sndRes = [&](const std::string& name,
                          mdk::TraversalAudioSoundDef& def) -> bool {
    if (const auto it = sndDefs.find(name); it != sndDefs.end()) {
      def = it->second;
      return true;
    }
    const mdk::BniRecord* r = mdk::findBniRecord(bnd.bdir, name);
    if (!r) return false;
    mdk::SniWave wv;
    if (mdk::decodeSniWave(
            std::span<const std::byte>(
                bnd.bniBytes.data() + r->payloadFileOffset,
                static_cast<std::size_t>(r->payloadEnd -
                                         r->payloadFileOffset)),
            &wv, nullptr) != mdk::SniWaveStatus::kOk) {
      ++sndDecodeMisses;
      return false;
    }
    const mdkbridge::StreamSndReg* reg = audio.soundReg(
        static_cast<int>(r - bnd.bdir.records.data()));
    def.volume = reg ? reg->volume : 0x7fff;
    def.loop = reg ? reg->loop : false;
    def.rateHz = wv.rateHz;
    def.frames = static_cast<std::uint32_t>(wv.frames);
    sndDefs.emplace(name, def);
    return true;
  };

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
    const int listenerBefore = sc.seams().listener;
    sc.step(in, 1.0f / 30.0f);
    in.nowMs += 33;                             // ~30.3fps wall ms
    ++frames;
    const int fillsBefore = pr.diag().terminalFills;
    // 19B.3B1 — the 0x4026f8 host pass: camView_ listener feed
    // (frame scalar pinned 1.0 — pacing deferred) gated on the
    // core's seam count, event translate in emission order, then
    // the sweep tick.
    if (sc.seams().listener != listenerBefore)
      audio.updateListener(sc.camView(), 1.0f);
    for (const mdk::StreamEvent& ev : sc.events()) {
      audio.consume(ev, mixer, sndRes);
      pr.consume(ev, sc.paletteDac());
    }
    sc.clearEvents();
    audio.tick(mixer, 1.0 / 30.0);
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
  // 19B.3B1 — the teardown tail: FUN_004020b4(eda84) emits the WIND
  // stop AFTER the last step drain; consume it, then the bank-free
  // arm (FUN_0042c824) releases anything still playing.
  sc.teardown();
  for (const mdk::StreamEvent& ev : sc.events()) {
    audio.consume(ev, mixer, sndRes);
    pr.consume(ev, sc.paletteDac());
  }
  sc.clearEvents();
  mixer.stopAll();
  const mdkbridge::StreamAudioDiag& ad = audio.diag();
  std::printf(
      "  audio: ev=%d plays=%d(ensure=%d,restart=%d,loop=%d,pos=%d) "
      "stops=%d lstn=%d active=%d cap=%d miss=%d/%d/%d\n",
      ad.events, ad.plays, ad.ensurePlays, ad.restartPlays,
      ad.loopPlays, ad.positional, ad.stops, ad.listenerUpdates,
      mixer.activeCount(), mixer.poolExhaustedCount(),
      ad.unknownTags, ad.resolveMisses, sndDecodeMisses);
  for (const auto& [nm, n] : ad.playNames) {
    const auto st = ad.stopNames.find(nm);
    std::printf("    asnd %s plays=%d stops=%d\n", nm.c_str(), n,
                st == ad.stopNames.end() ? 0 : st->second);
  }
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
      "persp=%d affine=%d matflat=%d fbdig=%016llx\n",
      (unsigned long long)d.model.raster.pixels,
      d.model.raster.clipped, d.model.raster.clipDropped,
      d.model.raster.unsupported, d.model.raster.lutMisses,
      d.model.raster.matPersp, d.model.raster.matAffine,
      d.model.raster.matFlat,
      (unsigned long long)d.model.fbDigest);
  std::printf(
      "  material classes: indexRec=%d lookupMiss=%d invalidRec=%d "
      "texMiss=%d texMeta=%d clipFan=%d degenerate=%d zero=%d "
      "matPx=%llu keyedSkip=%llu\n",
      d.model.raster.matIndexRec, d.model.raster.matLookupMiss,
      d.model.raster.matInvalidRec, d.model.raster.texLookupMiss,
      d.model.raster.texInvalidMeta, d.model.raster.matClipFan,
      d.model.raster.matDegenerate, d.model.raster.matZero,
      (unsigned long long)d.model.raster.matPixels,
      (unsigned long long)d.model.raster.texTransparent);
  for (const auto& [id, n] : d.model.raster.branch)
    std::printf("    dispatch %s n=%d\n",
                mdkbridge::streamTriBranchName(
                    static_cast<mdkbridge::StreamTriBranch>(id)),
                n);

  // Index-record identity report — the closure gate requires each
  // flat-0xff index-record fallback named by record, model and slot.
  if (!d.model.raster.matIndexRecHits.empty()) {
    // bankA pointer -> record name (parallel vectors).
    std::map<const mdk::ArenaRenderMaterial*, std::string> recName;
    for (std::size_t i = 0; i != bnd.bankA.size(); ++i)
      recName[&bnd.bankA[i]] = bnd.bankANames[i];
    // bankA pointer -> [(proto name, model slot name)].
    std::map<const mdk::ArenaRenderMaterial*,
             std::vector<std::pair<std::string, std::string>>> slots;
    auto addSlots = [&](const char* proto,
                        const std::optional<mdk::RuntimeModel>& pm) {
      if (!pm) return;
      for (std::size_t i = 0; i != pm->materials.size(); ++i) {
        if (pm->materials[i] == nullptr) continue;
        std::string slot;
        if (i < pm->names.size()) {
          const auto& nf = pm->names[i].name;
          slot.assign(nf.data(),
                      strnlen(nf.data(), nf.size()));
        }
        slots[pm->materials[i]].push_back({proto, slot});
      }
    };
    addSlots("KURT", bnd.protoKurt);
    addSlots("BONES", bnd.protoBones);
    addSlots("PROFSHIP", bnd.protoProf);
    addSlots(course >= 4 ? "GUNTA" : "SWH150", bnd.protoEsc);
    for (const auto& [mp, n] : d.model.raster.matIndexRecHits) {
      const auto rn = recName.find(mp);
      std::printf("    indexRec hit n=%d rec=%s", n,
                  rn != recName.end() ? rn->second.c_str() : "?");
      const auto sl = slots.find(mp);
      if (sl != slots.end())
        for (const auto& [proto, slot] : sl->second)
          std::printf(" %s:%s", proto.c_str(), slot.c_str());
      std::printf("\n");
    }
  }
  // Lookup-miss identity — join the missed pens with each proto's
  // unresolved name slots / table size. A null slot is a name that
  // matched no bank record at resolve time; an OOB pen indexes past
  // the +0x18 name count entirely.
  if (!d.model.raster.matLookupMissPens.empty()) {
    for (const auto& [ps, n] : d.model.raster.matLookupMissPens)
      std::printf("    lookupMiss pen=%d tableSize=%d n=%d\n",
                  ps.first, ps.second, n);
    auto dumpSlots = [&](const char* proto,
                         const std::optional<mdk::RuntimeModel>& pm) {
      if (!pm) return;
      for (std::size_t i = 0; i != pm->names.size(); ++i) {
        const mdk::ArenaRenderMaterial* mp =
            i < pm->materials.size() ? pm->materials[i] : nullptr;
        const auto& nf = pm->names[i].name;
        const std::string slot(nf.data(),
                               strnlen(nf.data(), nf.size()));
        std::printf("    slot %s[%zu] name=%s -> %s\n", proto, i,
                    slot.c_str(),
                    mp == nullptr ? "UNRESOLVED"
                    : mp->isIndexRecord ? "indexRec" : "record");
      }
      std::printf("    slot %s table size=%zu names=%zu\n", proto,
                  pm->materials.size(), pm->names.size());
    };
    dumpSlots("KURT", bnd.protoKurt);
    dumpSlots("BONES", bnd.protoBones);
    dumpSlots("PROFSHIP", bnd.protoProf);
    dumpSlots(course >= 4 ? "GUNTA" : "SWH150", bnd.protoEsc);
  }

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
