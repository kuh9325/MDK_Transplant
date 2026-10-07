// mdk-inspect — developer-facing read-only inspector for original MDK
// data files. Prints safe header/container/directory metadata only:
// never dumps payloads, never writes into the data root.
//
// Usage:
//   mdk-inspect --data-path DIR <relative-path>
//   mdk-inspect --data-path DIR --container <relative-path>
//   mdk-inspect --data-path DIR --entries <relative-path>
//   mdk-inspect --data-path DIR --visual-info <relative-path> <record>
//   mdk-inspect --data-path DIR --font-info <relative-path> <record>
//               [<code>]
//   mdk-inspect --data-path DIR --collision-probe <relative-path>
//               [<x> <y> <z>]   (Phase 5D: locate the level-stream
//               collision blob, run one swept query + floor probe)
//   mdk-inspect --selftest        (synthetic in-memory checks)

#include "core/arena_render.h"
#include "core/binary_reader.h"
#include "core/bni_directory.h"
#include "core/bni_image.h"
#include "core/cmi_directory.h"
#include "core/collision_query.h"
#include "core/container.h"
#include "core/data_root.h"
#include "core/dti_structure.h"
#include "core/dynamic_objects.h"
#include "core/enemy_runtime.h"
#include "core/file_family.h"
#include "core/freefall_runtime.h"
#include "core/frontend_host.h"
#include "core/frontend_machines.h"
#include "core/frontend_shell.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/gameplay_input.h"
#include "core/indexed_image.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/player_motion.h"
#include "core/player_projectiles.h"
#include "core/player_surface.h"
#include "core/player_vertical.h"
#include "core/progression_runtime.h"
#include "core/save_full_restore.h"
#include "core/save_full_write.h"
#include "core/save_game.h"
#include "core/sni_directory.h"
#include "core/stream_context.h"
#include "core/stream_scene.h"
#include "core/traversal_runtime.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::size_t kInspectHeadBytes = 64;
// Whole-file read cap for --entries: far above every observed file
// (largest in BUILD_A ≈ 8 MB) while staying a sane bound.
constexpr std::size_t kEntriesMaxBytes = 512ull * 1024 * 1024;

int usage() {
  std::fprintf(stderr,
               "usage: mdk-inspect --data-path DIR [--container | "
               "--entries] <relative-path>\n"
               "       mdk-inspect --data-path DIR --visual-info "
               "<relative-path> <record>\n"
               "       mdk-inspect --data-path DIR --font-info "
               "<relative-path> <record> [<code>]\n"
               "       mdk-inspect --data-path DIR --sprite-info "
               "<relative-path> <record>\n"
               "       mdk-inspect --data-path DIR --collision-probe "
               "<relative-path> [<x> <y> <z>]\n"
               "       mdk-inspect --data-path DIR --collision-census "
               "<relative-path>   (a .DTI path; per-arena flag census +\n"
               "                            spawn-pose support grid — QA\n"
               "                            floor-support audit)\n"
               "       mdk-inspect --data-path DIR --arena-objects "
               "<relative-path>   (a .DTI path; the sibling .CMI and\n"
               "                            <stem>O.MTO are loaded too)\n"
               "       mdk-inspect --data-path DIR --surface-census "
               "<relative-path>   (a .DTI path; the sibling <stem>O.MTO\n"
               "                            is scanned for surface polys)\n"
               "       mdk-inspect --data-path DIR --arena-render "
               "<relative-path>   (a .DTI path; the sibling <stem>O.MTO\n"
               "                            and <stem>S.MTI are decoded into\n"
               "                            the Phase 6A render-data view.\n"
               "                            Options: --arena NAME --start X Y Z)\n"
               "       mdk-inspect --data-path DIR --script-disasm "
               "<cmi-path> <name|*>\n"
               "       mdk-inspect --data-path DIR --obj-script-disasm "
               "<cmi-path>   (every table-0 object script)\n"
               "       mdk-inspect --data-path DIR --traversal-runtime "
               "<relative-path>\n"
               "                            (a .DTI path; loads the sibling\n"
               "                            .CMI + <stem>O.MTO, assembles the\n"
               "                            Phase 5G runtime and steps frames.\n"
               "                            Options: --arena NAME --start X Y Z\n"
               "                            --yaw DEG --frames N --pdamage A@F\n"
               "                            --script-pc HEX — QA script entry\n"
               "       mdk-inspect --data-path DIR --freefall-runtime "
               "<relative-path>\n"
               "                            (a FALL3D.BNI path; reads the\n"
               "                            FALLPU_<course+1> pickup list and\n"
               "                            steps the Phase 13A freefall core.\n"
               "                            Options: --course 0..4 --skill 0..2\n"
               "                            --seed N --frames N)\n"
               "       mdk-inspect --data-path DIR --stream-init\n"
               "                            (Phase 19A mode-5 init diagnostic on\n"
               "                            STREAM/STREAM.{BNI,MTI}; bound actors,\n"
               "                            window, counters, seams. Options:\n"
               "                            --course 0..4 --skill 0..2 --seed N\n"
               "                            --stream-frames N — bounded updater-\n"
               "                            reach + animator trace: per frame,\n"
               "                            updater families dispatched, winLo/\n"
               "                            winHi, tunnel feed, FUN_004555bc body\n"
               "                            per object, state digest; no draw/\n"
               "                            TELETYPE bodies)\n"
               "       mdk-inspect --data-path DIR --campaign-handoff "
               "<relative-path>\n"
               "                            (a FALL3D.BNI path; fast-forwards\n"
               "                            the freefall course to completion,\n"
               "                            runs the Phase 13B handoff and prints\n"
               "                            the transition record + one traversal\n"
               "                            frame on the success route.\n"
               "                            Options: --course 0..4 --skill 0..2\n"
               "                            --seed N --frames N)\n"
               "       mdk-inspect --data-path DIR --campaign-sequence\n"
               "                            (Phase 14A: prints the 0x4999e8 level\n"
               "                            table with per-dir data presence, then\n"
               "                            drives the ProgressionSession through\n"
               "                            new-game -> terminal with real-data\n"
               "                            traversal loads where present)\n"
               "       mdk-inspect --save-info <file.SAV>\n"
               "                            (Phase 14B: envelope + packet table +\n"
               "                            proven GAME/PLAY/DAMP/CAME/AREN fields)\n"
               "       mdk-inspect --save-roundtrip <file.SAV>\n"
               "                            (parse -> re-emit header-only ->\n"
               "                            re-parse -> compare)\n"
               "       mdk-inspect --data-path DIR --save-restore <file.SAV>\n"
               "                            (Phase 14C: full-save restore —\n"
               "                            MORE/PLAY/DAMP/CAME/AREN/ALIE/FAND/BULL\n"
               "                            into a live TraversalRuntime, then\n"
               "                            steps --frames frames;\n"
               "                            --save-activate N also runs the\n"
               "                            dormant-arena activation route on\n"
               "                            restored arena N;\n"
               "                            --save-write-full <out.SAV> emits the\n"
               "                            Phase 14D full-save stream after the\n"
               "                            frame steps (deterministic under\n"
               "                            --seed), re-parses and re-restores it,\n"
               "                            and reports equivalence)\n"
               "       mdk-inspect [--data-path DIR] --frontend-script\n"
               "                            (Phase 18A: deterministic\n"
               "                            scripted-input run over the\n"
               "                            frontend shell — mode/sub-mode,\n"
               "                            overlay arms, save slots, requests.\n"
               "                            With --data-path the SAVES dir is\n"
               "                            enumerated for real; writes are\n"
               "                            reported but never performed)\n"
               "       mdk-inspect --selftest\n"
               "       mdk-inspect --selftest-player-surface\n"
               "       mdk-inspect --selftest-camera-pose\n"
               "       mdk-inspect --selftest-camera-obstruction\n");
  return 2;
}

std::string stemOf(const std::string& relPath) {
  const auto slash = relPath.find_last_of("/\\");
  const auto dot = relPath.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return {};
  }
  const auto begin = slash == std::string::npos ? 0 : slash + 1;
  return relPath.substr(begin, dot - begin);
}

// Shared FALLPU_<course+1> pickup-table read for --freefall-runtime
// and --campaign-handoff. FALLPU entries are 12-byte {name[8], u32}
// records terminated by a NUL first name byte (OBSERVED: FUN_0040ef28
// count loop + the BNI census in docs/reverse-engineering/
// EXECUTABLE_MAP.md). Appends to `data.pickups`; returns false when
// the BNI or the record cannot be read/parsed (detail says which).
bool inspectReadFallpu(const mdk::DataRoot& root,
                       const std::string& bniPath,
                       mdk::FreefallCourseData& data,
                       std::string* detail) {
  const auto bniFile = root.readFile(bniPath, kEntriesMaxBytes, detail);
  if (!bniFile) return false;
  const auto dir = mdk::inspectBniDirectory(
      std::span<const std::byte>(bniFile->data(), bniFile->size()));
  if (dir.status != mdk::BniDirectoryStatus::kOk) {
    if (detail)
      *detail = "bni parse: " +
                std::string(mdk::bniDirectoryStatusName(dir.status));
    return false;
  }
  char recName[16];
  std::snprintf(recName, sizeof recName, "FALLPU_%d", data.course + 1);
  const mdk::BniRecord* rec = mdk::findBniRecord(dir, recName);
  if (!rec) {
    if (detail) *detail = std::string(recName) + " not found";
    return false;
  }
  const std::byte* p = bniFile->data() + rec->payloadFileOffset;
  const std::byte* end = bniFile->data() + rec->payloadEnd;
  for (; p + 12 <= end; p += 12) {
    if (p[0] == std::byte{0}) break;  // terminator entry
    mdk::FreefallPickupRec r{};
    for (int k = 0; k < 8; ++k)
      r.name[k] = static_cast<char>(p[k]);
    r.name[8] = '\0';
    data.pickups.push_back(r);
  }
  return true;
}

int selftest() {
  // Synthetic 28-byte tag-envelope fixture (not original data):
  //   u32 len = 24 (= size-4), name "TEST.MAT" + NUL pad.
  const std::byte raw[] = {
      std::byte{0x18}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{'T'},  std::byte{'E'},  std::byte{'S'},  std::byte{'T'},
      std::byte{'.'},  std::byte{'M'},  std::byte{'A'},  std::byte{'T'},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x0c}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };
  const auto info = mdk::inspectContainer(raw, sizeof(raw));
  bool ok = info.hasDeclaredLength && info.lengthValid &&
            info.declaredLength == 24 &&
            info.shape == mdk::ContainerShape::kTaggedName &&
            info.logicalName == "TEST.MAT" &&
            mdk::nameStemMatches(info, "test");
  std::fprintf(stderr, "selftest envelope: %s\n", ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic SNI-like fixture: envelope + count=1 + one 24-byte
  // record {name[12], u32, blobOff=0x18, size=4} + 4 payload bytes +
  // name trailer. Total 0x18+0x18+4+12 = 0x48 = 72 bytes.
  std::byte sni[72] = {};
  const auto put32 = [&](std::size_t off, std::uint32_t v) {
    sni[off + 0] = static_cast<std::byte>(v & 0xff);
    sni[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    sni[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    sni[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto putName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(sni); ++i) {
      sni[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  put32(0x00, sizeof(sni) - 4);
  putName(0x04, "TEST.SND");
  put32(0x10, sizeof(sni) - 12);
  put32(0x14, 1);                  // count
  putName(0x18, "ENTRY1");
  put32(0x18 + 0x0c, 3);           // unknown field
  put32(0x18 + 0x10, 0x30 - 4);    // blobOffset → file 0x30
  put32(0x18 + 0x14, 4);           // payloadSize
  putName(sizeof(sni) - 12, "TEST.SND");

  const auto dir = mdk::inspectSniDirectory(
      std::span<const std::byte>(sni, sizeof(sni)));
  ok = dir.status == mdk::SniDirectoryStatus::kOk &&
       dir.count == 1 && dir.entries.size() == 1 &&
       dir.entries[0].name() == "ENTRY1" &&
       dir.entries[0].payloadFileOffset() == 0x30 &&
       dir.entries[0].payloadSize == 4 && dir.trailerPresent &&
       dir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest sni-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic MTI-like fixture: envelope + count=2 + two 24-byte
  // records: one extended-header payload record and one index record.
  // Layout: 0x18 + 2*24 = 0x48 dir end; payload at 0x48 (8-byte ext
  // header: n=2,a=64,b=32 + 4 data bytes) then name trailer.
  // Total = 0x48 + 12 + 12 = 0x60 = 96 bytes.
  std::byte mti[96] = {};
  const auto mput32 = [&](std::size_t off, std::uint32_t v) {
    mti[off + 0] = static_cast<std::byte>(v & 0xff);
    mti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    mti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    mti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto mput16 = [&](std::size_t off, std::uint16_t v) {
    mti[off + 0] = static_cast<std::byte>(v & 0xff);
    mti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto mputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(mti); ++i) {
      mti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  mput32(0x00, sizeof(mti) - 4);
  mputName(0x04, "TEST.MTI");
  mput32(0x10, sizeof(mti) - 12);
  mput32(0x14, 2);                    // count
  mputName(0x18, "MAT0");             // record 0: payload (ext header)
  mput32(0x18 + 0x08, 0x00010001);    // flags: extended header
  mput32(0x18 + 0x0c, 0);             // raw param
  mput32(0x18 + 0x10, 0x40600000);    // raw param (float-looking)
  mput32(0x18 + 0x14, 0x48 - 4);      // blobOffset -> file 0x48
  mputName(0x30, "IDX0");             // record 1: index record
  mput32(0x30 + 0x08, 0xffffffff);    // index discriminator
  mput32(0x30 + 0x0c, 7);             // index value
  mput32(0x30 + 0x10, 0);
  mput32(0x30 + 0x14, 0);
  mput16(0x48, 2);                    // payload header: u16 @+0 (n)
  mput16(0x4c, 64);                   // u16 @+4 (fieldA)
  mput16(0x4e, 32);                   // u16 @+6 (fieldB)
  mputName(sizeof(mti) - 12, "TEST.MTI");

  const auto mdir = mdk::inspectMtiDirectory(
      std::span<const std::byte>(mti, sizeof(mti)));
  ok = mdir.status == mdk::MtiDirectoryStatus::kOk &&
       mdir.count == 2 && mdir.entries.size() == 2 &&
       mdir.entries[0].name() == "MAT0" &&
       !mdir.entries[0].isIndexRecord() &&
       mdir.entries[0].hasExtendedHeader() &&
       mdir.entries[0].payloadFileOffset() == 0x48 &&
       mdir.entries[0].headerCount == 2 &&
       mdir.entries[0].headerFieldA == 64 &&
       mdir.entries[0].headerFieldB == 32 &&
       mdir.entries[0].payloadDataFileOffset == 0x50 &&
       mdir.entries[1].isIndexRecord() &&
       mdir.entries[1].fieldAt0x0C == 7 &&
       mdir.trailerPresent && mdir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest mti-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic MTO-like fixture (no original data): tagged envelope +
  // count=1 + one 12-byte record {name[8]="OV1", blockOff=0x24} + one
  // overlay block + name trailer. File size 0xb8.
  //
  // Block @0x24, len=0x88 → [0x24, 0xac):
  //   +0x00 len=0x88, +0x04 ofsA=0x4c, +0x08 ofsB=0x5c, +0x0c ofsC=0x70
  //   embedded file @0x34 (innerSize=0x40 → innerEnd=0x74):
  //     innerLen=0x3c, name "OV1.MAT"@0x38, sec=0x34@0x44, count=1@0x48,
  //     rec @0x4c {"TEX", 0,0,0, imgOff=0x2c}, payload @0x64 {64,32},
  //     trailer "OV1.MAT"@0x68
  //   regionA: size=0x0c @0x74, struct {0,0,0} @0x78 → end 0x84
  //   regionB [0x84, 0x98) — off+4+0x5c = 0x84
  //   regionC @0x98 (off+4+0x70): c1..c4 = 0, u32, extra → 0xac = bend
  std::byte mto[0xb8] = {};
  const auto oput32 = [&](std::size_t off, std::uint32_t v) {
    mto[off + 0] = static_cast<std::byte>(v & 0xff);
    mto[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    mto[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    mto[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto oput16 = [&](std::size_t off, std::uint16_t v) {
    mto[off + 0] = static_cast<std::byte>(v & 0xff);
    mto[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto oputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(mto); ++i) {
      mto[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  oput32(0x00, sizeof(mto) - 4);
  oputName(0x04, "TEST.MAT");
  oput32(0x10, sizeof(mto) - 12);
  oput32(0x14, 1);                // overlay count
  oputName(0x18, "OV1");          // table-1 record
  oput32(0x20, 0x24);             // block file offset
  oput32(0x24, 0x88);             // blockLength → bend 0xac
  oput32(0x28, 0x4c);             // ofsA → 0x24+8+0x4c = 0x78
  oput32(0x2c, 0x5c);             // ofsB → 0x24+4+0x5c = 0x84
  oput32(0x30, 0x70);             // ofsC → 0x24+4+0x70 = 0x98
  oput32(0x34, 0x3c);             // innerLen → innerSize 0x40
  oputName(0x38, "OV1.MAT");
  oput32(0x44, 0x34);             // inner secondary = innerSize-12
  oput32(0x48, 1);                // inner count
  oputName(0x4c, "TEX");          // inner record name[8]
  oput32(0x60, 0x2c);             // rec+0x14: img(0x38)+0x2c = 0x64
  oput16(0x64, 64);               // payload header {64, 32}
  oput16(0x66, 32);
  oputName(0x68, "OV1.MAT");      // inner trailer → innerEnd 0x74
  oput32(0x74, 0x0c);             // regionA size (self-exclusive)
  // regionA struct @0x78: ca=cb=cc=0 (zeros)
  // regionB [0x84,0x98) zeros; regionC @0x98: c1..c4=0 + u32 (zeros)
  oputName(sizeof(mto) - 12, "TEST.MAT");

  const auto odir = mdk::inspectMtoDirectory(
      std::span<const std::byte>(mto, sizeof(mto)));
  ok = odir.status == mdk::MtoDirectoryStatus::kOk &&
       odir.count == 1 && odir.entries.size() == 1 &&
       odir.entries[0].name() == "OV1" &&
       odir.entries[0].blockFileOffset == 0x24 &&
       odir.blocks.size() == 1 &&
       odir.blocks[0].blockLength == 0x88 &&
       odir.blocks[0].innerName() == "OV1.MAT" &&
       odir.blocks[0].innerCount == 1 &&
       odir.blocks[0].innerRecords.size() == 1 &&
       odir.blocks[0].innerRecords[0].name() == "TEX" &&
       odir.blocks[0].innerRecords[0].headerFieldA == 64 &&
       odir.blocks[0].innerRecords[0].headerFieldB == 32 &&
       odir.blocks[0].innerRecords[0].payloadDataFileOffset == 0x68 &&
       odir.blocks[0].innerTrailerPresent &&
       odir.blocks[0].innerSecondaryEqualsTrailerOffset &&
       odir.blocks[0].regionASize == 0x0c &&
       odir.blocks[0].regionACountA == 0 &&
       odir.blocks[0].regionBOffset == 0x84 &&
       odir.blocks[0].regionBSize == 0x14 &&
       odir.blocks[0].regionCOffset == 0x98 &&
       odir.blocks[0].regionCCount1 == 0 &&
       odir.blocks[0].regionCExtraOffset == 0xac &&
       odir.trailerPresent && odir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest mto-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic CMI-like fixture (no original data): tagged envelope +
  // the four counted variable-length tables + a small data region +
  // name trailer. Table 0 uses the zero-count variant (OBSERVED in
  // LEVEL4/5/7); records are {u8 len, name[len] incl NUL, u32 value}
  // with values that are image-relative offsets (image = file+4).
  //
  //   0x14 T0 count=0 → T1 count @0x18
  //   T1: {3,"E1\0",v} T2: {6,"FIRST\0",v}{7,"SECOND\0",0}
  //   T3: {2,"L\0",v} → data region [0x4a, 0x74)
  // Built byte-exact below; every nonzero value points into the data
  // region [dataStart, size-12).
  std::byte cmi[0x80] = {};
  const auto cput32 = [&](std::size_t off, std::uint32_t v) {
    cmi[off + 0] = static_cast<std::byte>(v & 0xff);
    cmi[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    cmi[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    cmi[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto cputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(cmi); ++i) {
      cmi[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  // Record writer: {u8 len, bytes, u32 value}; len includes the NUL.
  const auto cputRec = [&](std::size_t off, const char* n,
                           std::uint32_t v) {
    const std::size_t len = std::strlen(n) + 1;  // counted incl NUL
    cmi[off] = static_cast<std::byte>(len);
    for (std::size_t i = 0; i < len; ++i) {
      cmi[off + 1 + i] = static_cast<std::byte>(n[i]);  // copies NUL
    }
    cput32(off + 1 + len, v);
    return off + 1 + len + 4;
  };
  cput32(0x00, sizeof(cmi) - 4);
  cputName(0x04, "TEST.CMD");
  cput32(0x10, sizeof(cmi) - 12);
  cput32(0x14, 0);                          // T0: zero count
  std::size_t q = 0x18;
  cput32(q, 1); q += 4;                     // T1: one record
  q = cputRec(q, "E1", 0x50 - 4);           // value -> file 0x50
  cput32(q, 2); q += 4;                     // T2: two records
  q = cputRec(q, "FIRST", 0x50 - 4);
  q = cputRec(q, "SECOND", 0);              // null value (T1 form)
  cput32(q, 1); q += 4;                     // T3: one record
  q = cputRec(q, "L", 0x60 - 4);            // value -> file 0x60
  // q == 0x4a: data region [0x4a, 0x74). Two length-prefixed strings
  // then a u32 (the CODE-CORROBORATED T3-target head shape) at 0x60.
  cmi[0x60] = std::byte{4}; cputName(0x61, "AB"); // {len4 "AB\0?"}
  cmi[0x65] = std::byte{2}; cmi[0x66] = std::byte{'Z'};
  cmi[0x67] = std::byte{0};
  cput32(0x68, 0x6c - 4);                   // second-level offset
  cputName(sizeof(cmi) - 12, "TEST.CMD");   // trailer at 0x74

  const auto cdir = mdk::inspectCmiDirectory(
      std::span<const std::byte>(cmi, sizeof(cmi)));
  ok = cdir.status == mdk::CmiDirectoryStatus::kOk &&
       cdir.tables.size() == 4 &&
       cdir.tables[0].count == 0 && cdir.tables[0].records.empty() &&
       cdir.tables[0].endFileOffset == 0x18 &&
       cdir.tables[1].count == 1 &&
       cdir.tables[1].records[0].name() == "E1" &&
       cdir.tables[1].records[0].nameLength == 3 &&
       cdir.tables[1].records[0].nameEndsWithTerminator &&
       cdir.tables[1].records[0].value == 0x4c &&
       cdir.tables[1].records[0].valueFileOffset() ==
           std::optional<std::uint64_t>(0x50) &&
       cdir.tables[2].count == 2 &&
       cdir.tables[2].records[1].value == 0 &&
       !cdir.tables[2].records[1].valueFileOffset().has_value() &&
       cdir.tables[3].records[0].valueFileOffset() ==
           std::optional<std::uint64_t>(0x60) &&
       cdir.dataRegionOffset == q &&
       cdir.dataRegionEnd == sizeof(cmi) - 12 &&
       cdir.trailerPresent && cdir.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest cmi-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic DTI-like fixture (no original data): tagged envelope +
  // the five-entry image-relative TOC + one of each section:
  //   s0  29 u32s (grid 8x4 -> plane (8+4)*4 = 48 bytes; altFillA=-1
  //       -> single plane)
  //   s1  count=1 {word0=1,key=0,f=(1.5,2.5,3.5,4.5)}
  //   s2  count=1 {"TST_1\0\0\0", imgOff->payload, f32} payload:
  //       count=2 -> type6 "connect" + type2 "HotGen" with name@0x18
  //   s3  count=0x10 + 768 palette bytes
  //   s4  48 grid bytes, then name trailer
  std::byte dti[0x460] = {};
  const auto dput32 = [&](std::size_t off, std::uint32_t v) {
    dti[off + 0] = static_cast<std::byte>(v & 0xff);
    dti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    dti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    dti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto dputf = [&](std::size_t off, float f) {
    dput32(off, std::bit_cast<std::uint32_t>(f));
  };
  const auto dputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(dti); ++i) {
      dti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  // Layout (file offsets; TOC stores image offsets = file - 4):
  //   s0 0x28..0x9c  s1 0x9c..0xb8  s2 0xb8..0x118  s3 0x118..0x41c
  //   s4 0x41c..0x44c  trailer 0x44c..0x458
  const std::uint64_t dSize = 0x458;
  dput32(0x00, static_cast<std::uint32_t>(dSize - 4));
  dputName(0x04, "TEST.DAT");
  dput32(0x10, static_cast<std::uint32_t>(dSize - 12));
  dput32(0x14, 0x24);    // s0 @img 0x24 -> file 0x28
  dput32(0x18, 0x98);    // s1 -> file 0x9c
  dput32(0x1c, 0xb4);    // s2 -> file 0xb8
  dput32(0x20, 0x114);   // s3 -> file 0x118
  dput32(0x24, 0x418);   // s4 -> file 0x41c
  // s0 words: only the proven-consumed fields need sane values
  dputf(0x2c, -4.0f); dputf(0x30, 0.0f); dputf(0x34, 190.0f);
  dputf(0x38, 96.0f);
  dput32(0x3c, 0xf0); dput32(0x40, 0xe7);            // fill bytes
  dput32(0x44, 0xf0); dput32(0x48, 0x12c);           // scroll bases
  dput32(0x4c, 8); dput32(0x50, 4);                  // grid 8x4
  dput32(0x54, 0xffffffff); dput32(0x58, 0xffffffff);// altFillA/B
  // s1: count=1 {word0=1, key=0, f32 x4}
  dput32(0x9c, 1);
  dput32(0xa0, 1); dput32(0xa4, 0);
  dputf(0xa8, 1.5f); dputf(0xac, 2.5f);
  dputf(0xb0, 3.5f); dputf(0xb4, 4.5f);
  // s2: count=1, record {"TST_1\0\0\0", imgOff=0xc8->file 0xcc, 4.0f}
  dput32(0xb8, 1);
  dputName(0xbc, "TST_1");
  dput32(0xc4, 0xc8);          // image offset -> file 0xcc
  dputf(0xc8, 4.0f);
  // payload @0xcc: count=2; sub[0] type6 connect; sub[1] type2 HotGen
  dput32(0xcc, 2);
  dput32(0xd0, 6); dput32(0xd4, 1000); dput32(0xd8, 1);
  dputf(0xdc, 1.0f); dputf(0xe0, 2.0f); dputf(0xe4, 3.0f);
  dputf(0xe8, 4.0f); dputf(0xec, 5.0f); dputf(0xf0, 6.0f);
  dput32(0xf4, 2); dput32(0xf8, 9); dput32(0xfc, 0);
  dputf(0x100, -1.0f); dputf(0x104, -2.0f); dputf(0x108, -3.0f);
  dputName(0x10c, "XGS");      // name @+0x18 of sub[1]
  // s3 @0x118: count + 768-byte palette
  dput32(0x118, 0x10);
  // s4 @0x41c: 48 bytes (grid (8+4)*4)
  dputName(dSize - 12, "TEST.DAT");

  const auto dst = mdk::inspectDtiStructure(
      std::span<const std::byte>(dti, dSize));
  ok = dst.status == mdk::DtiStructureStatus::kOk &&
       dst.tocImageOffsets[0] == 0x24 &&
       dst.tocImageOffsets[4] == 0x418 &&
       dst.sections[0].fileStart == 0x28 &&
       dst.sections[0].fileEnd == 0x9c &&
       dst.sections[2].fileStart == 0xb8 &&
       dst.sections[2].fileEnd == 0x118 &&
       dst.params[9] == 8 && dst.params[10] == 4 &&
       dst.keyedRecords.size() == 1 &&
       dst.keyedRecords[0].key == 0 &&
       dst.keyedRecords[0].floatAt(0) == 1.5f &&
       dst.arenas.size() == 1 &&
       dst.arenas[0].name() == "TST_1" &&
       dst.arenas[0].nameEndsWithTerminator &&
       dst.arenas[0].payloadFileOffset == 0xcc &&
       dst.arenas[0].scalar() == 4.0f &&
       dst.arenas[0].subRecords.size() == 2 &&
       dst.arenas[0].subRecords[0].type == 6 &&
       dst.arenas[0].subRecords[0].fields[0] == 1000 &&
       dst.arenas[0].subRecords[0].fieldAsFloat(2) == 1.0f &&
       dst.arenas[0].subRecords[1].type == 2 &&
       dst.arenas[0].subRecords[1].name18() == "XGS" &&
       dst.s2PayloadRegionStart == 0xcc &&
       dst.paletteCount == 0x10 &&
       dst.paletteBytes.size() == 768 &&
       dst.gridPlaneSize == 48 && dst.gridPlaneCount == 1 &&
       dst.sections[4].fileEnd == dSize - 12 &&
       dst.trailerPresent && dst.secondaryEqualsTrailerOffset;
  std::fprintf(stderr, "selftest dti-structure: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic FTI-like fixture (no original data): length-only
  // envelope + count=2 + two 12-byte records {name[8], imgOff}. One
  // name fills all 8 bytes (no NUL — OBSERVED legal form).
  //   0x00 len=size-4, 0x04 count=2, 0x08..0x20 records,
  //   payloads 0x20..0x38.
  std::byte fti[0x38] = {};
  const auto fput32 = [&](std::size_t off, std::uint32_t v) {
    fti[off + 0] = static_cast<std::byte>(v & 0xff);
    fti[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    fti[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    fti[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto fputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(fti); ++i) {
      fti[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  fput32(0x00, sizeof(fti) - 4);
  fput32(0x04, 2);
  fputName(0x08, "EIGHTCHR");        // fills name[8] — no NUL
  fput32(0x10, 0x20 - 4);            // imgOff -> file 0x20
  fputName(0x14, "TWO");
  fput32(0x1c, 0x30 - 4);            // imgOff -> file 0x30

  const auto fdir = mdk::inspectFtiDirectory(
      std::span<const std::byte>(fti, sizeof(fti)));
  ok = fdir.status == mdk::FtiDirectoryStatus::kOk &&
       fdir.count == 2 && fdir.records.size() == 2 &&
       fdir.directoryEnd == 0x20 &&
       fdir.records[0].name() == "EIGHTCHR" &&
       !fdir.records[0].nameHasTerminator &&
       fdir.records[0].payloadFileOffset == 0x20 &&
       fdir.records[0].payloadEnd == 0x30 &&
       fdir.records[1].name() == "TWO" &&
       fdir.records[1].nameHasTerminator &&
       fdir.records[1].payloadFileOffset == 0x30 &&
       fdir.records[1].payloadEnd == sizeof(fti) &&
       fdir.offsetsSortedAscending && fdir.offsetsUnique &&
       fdir.firstPayloadAtDirectoryEnd;
  std::fprintf(stderr, "selftest fti-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic BNI-like fixture (no original data): length-only
  // envelope + count=2 + two 16-byte records {name[12], imgOff};
  // one 9-char name (OBSERVED legal form: "BONESANIM").
  //   0x00 len, 0x04 count=2, 0x08..0x28 records, payloads 0x28..0x40.
  std::byte bni[0x40] = {};
  const auto bput32 = [&](std::size_t off, std::uint32_t v) {
    bni[off + 0] = static_cast<std::byte>(v & 0xff);
    bni[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
    bni[off + 2] = static_cast<std::byte>((v >> 16) & 0xff);
    bni[off + 3] = static_cast<std::byte>((v >> 24) & 0xff);
  };
  const auto bputName = [&](std::size_t off, const char* s) {
    for (std::size_t i = 0; s[i] && off + i < sizeof(bni); ++i) {
      bni[off + i] = static_cast<std::byte>(s[i]);
    }
  };
  bput32(0x00, sizeof(bni) - 4);
  bput32(0x04, 2);
  bputName(0x08, "BONESANIM");       // 9 chars in name[12]
  bput32(0x08 + 0x0c, 0x28 - 4);     // imgOff -> file 0x28
  bputName(0x18, "RES2");
  bput32(0x18 + 0x0c, 0x38 - 4);     // imgOff -> file 0x38

  const auto bdir = mdk::inspectBniDirectory(
      std::span<const std::byte>(bni, sizeof(bni)));
  ok = bdir.status == mdk::BniDirectoryStatus::kOk &&
       bdir.count == 2 && bdir.records.size() == 2 &&
       bdir.directoryEnd == 0x28 &&
       bdir.records[0].name() == "BONESANIM" &&
       bdir.records[0].nameHasTerminator &&
       bdir.records[0].payloadFileOffset == 0x28 &&
       bdir.records[0].payloadEnd == 0x38 &&
       bdir.records[1].name() == "RES2" &&
       bdir.records[1].payloadFileOffset == 0x38 &&
       bdir.records[1].payloadEnd == sizeof(bni) &&
       bdir.offsetsSortedAscending && bdir.offsetsUnique &&
       bdir.firstPayloadAtDirectoryEnd;
  std::fprintf(stderr, "selftest bni-directory: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic BNI image checks (no original data): record lookup by
  // name plus the Phase 4A paletted-bitmap decode
  // {rgb[768], u16 w, u16 h, px[w*h]}.
  ok = mdk::findBniRecord(bdir, "bonesanim") == &bdir.records[0] &&
       mdk::findBniRecord(bdir, "RES2") == &bdir.records[1] &&
       mdk::findBniRecord(bdir, "MISSING") == nullptr;
  std::fprintf(stderr, "selftest bni-record-lookup: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  std::vector<std::byte> pal(772 + 6);  // 2x3 paletted image
  for (int i = 0; i < 256; ++i) {
    pal[i * 3 + 0] = static_cast<std::byte>(i);
    pal[i * 3 + 1] = static_cast<std::byte>(255 - i);
    pal[i * 3 + 2] = static_cast<std::byte>(i);
  }
  pal[768] = std::byte{2};
  pal[770] = std::byte{3};
  for (int i = 0; i < 6; ++i) {
    pal[772 + i] = static_cast<std::byte>(i + 40);
  }
  const auto probe = mdk::probeBniImage(pal);
  std::string derr;
  const auto img = mdk::decodeBniPalettedImage(pal, &derr);
  ok = probe.shape == mdk::BniImageShape::kPaletted &&
       img && img->width == 2 && img->height == 3 &&
       img->stride == 2 && img->pixels.size() == 6 &&
       img->pixels[5] == 45 && img->hasPalette &&
       img->palette[7].r == 7 && img->palette[7].g == 248;
  std::fprintf(stderr, "selftest bni-paletted-image: %s\n",
               ok ? "PASS" : "FAIL");
  if (!ok) {
    return 1;
  }

  // Synthetic indexed-only + composed-palette check (Phase 4B; no
  // original data): {u16 w, u16 h, px} decoded against a 768-byte
  // palette composed SYS_PAL-head + PAL-tail style.
  {
    std::vector<std::byte> idx(4 + 6);  // 3x2 indexed-only image
    idx[0] = std::byte{3};
    idx[2] = std::byte{2};
    for (int i = 0; i < 6; ++i) {
      idx[4 + i] = static_cast<std::byte>(i + 9);
    }
    std::vector<std::byte> sysPal(192), palRec(768);
    for (int i = 0; i < 192; ++i) {
      sysPal[i] = static_cast<std::byte>(i + 1);
    }
    for (int i = 0; i < 768; ++i) {
      palRec[i] = static_cast<std::byte>((i * 3) & 0xff);
    }
    const auto composed =
        mdk::composeStreamPalette(sysPal, palRec, &derr);
    const auto img2 = composed
        ? mdk::decodeBniIndexedImage(idx, *composed, &derr)
        : std::nullopt;
    ok = composed && img2 && img2->width == 3 && img2->height == 2 &&
         img2->pixels.size() == 6 && img2->pixels[5] == 14 &&
         img2->hasPalette &&
         img2->palette[0].r == 1 &&          // SYS_PAL head
         img2->palette[64].r ==              // PAL tail @+0xc0
             static_cast<std::uint8_t>((0xc0 * 3) & 0xff);
    std::fprintf(stderr, "selftest bni-indexed-image: %s\n",
                 ok ? "PASS" : "FAIL");
  }
  if (!ok) {
    return 1;
  }

  // Synthetic FONTSML-layout font check (Phase 4C; no original
  // data): u32 offsetTable[256] then {s8 top, s8 bottom, u8 width,
  // u8 px[w*(top+bottom+1)]} glyph records.
  {
    std::vector<std::byte> fontBuf(0x400, std::byte{0});
    // 'A' (0x41) -> 2x2 glyph at 0x400; '!' (0x21) unmapped.
    fontBuf[0x41 * 4] = std::byte{0x00};
    fontBuf[0x41 * 4 + 1] = std::byte{0x04};  // offset 0x400
    fontBuf.push_back(std::byte{1});          // top = 1
    fontBuf.push_back(std::byte{0});          // bottom = 0
    fontBuf.push_back(std::byte{2});          // width = 2
    fontBuf.push_back(std::byte{9});
    fontBuf.push_back(std::byte{0});
    fontBuf.push_back(std::byte{0});
    fontBuf.push_back(std::byte{10});
    const auto font = mdk::decodeFtiFont(fontBuf, &derr);
    const auto* g = font ? font->glyphFor(0x41) : nullptr;
    ok = font && font->mappedCount == 1 && g && g->rows() == 2 &&
         g->pixels.size() == 4 && g->pixels[0] == 9 &&
         g->pixels[3] == 10 && !font->glyphFor(0x21) &&
         mdk::ftiFontDigest(*font) != 0 &&
         !mdk::decodeFtiFont(
              std::span<const std::byte>(fontBuf.data(), 0x100), &derr)
              .has_value();  // truncated table rejected
    std::fprintf(stderr, "selftest fti-font: %s\n",
                 ok ? "PASS" : "FAIL");
  }
  return ok ? 0 : 1;
}

// Phase 5F — synthetic end-to-end surface-contact selftest. Drives the
// real seams (not the original VM): collisionApply -> surfaceContactHook
// -> surfaceDispatch (FUN_0040b5d0), surfaceConveyorDelta (FUN_00412ef0)
// -> integratePlayerMotion, slideZoneTrigger (opcode 0xe0) ->
// applyPlayerVerticalCollision, and surfaceApplyPending (FUN_0040b4dc).
// The route is contact ordinary floor -> no effect; contact a conveyor
// surface -> 5B displacement; contact a slide-zone -> 5C bounce flag;
// then the pending-flag re-arm. No --data-path required.
int selftestPlayerSurface() {
  bool ok = true;
  auto check = [&](bool c, const char* what) {
    if (!c) ok = false;
    std::fprintf(stderr, "  %-56s %s\n", what, c ? "ok" : "FAIL");
  };
  const float dt = 1.0f / 30.0f;

  // Flat floor at z=10 carrying surface byte 2 (the armed bit set).
  float verts[9] = {-50, -50, 10, 50, -50, 10, 50, 50, 10};
  mdk::CollisionPoly poly = {};
  poly.v[0] = 0;
  poly.v[1] = 1;
  poly.v[2] = 2;
  poly.surface = 2;   // surfId 2 -> dispatch slot index 1
  poly.flags = 0x10;  // the contact/re-arm bit, armed
  mdk::CollisionNode node = {};
  node.nz = 1.0f;
  node.d = -10.0f;
  node.childNear = -1;
  node.childFar = -1;
  node.polysPos = 1;  // count=1, firstIdx=0
  mdk::CollisionArena arena = {};
  arena.verts = verts;
  arena.polys = &poly;
  arena.nodes = &node;
  arena.deepFloorZ = -1000.0f;

  mdk::SurfaceObjectState ctx = {};
  ctx.polys = &poly;
  ctx.polyCount = 1;
  ctx.config[1] = 0x8 | 0x80;  // surfId 2: channel-8 + the 0x80 mark fx
  const float dir[3] = {1.0f, 0.0f, 0.0f};
  mdk::surfaceRecordCreate(ctx, 2, dir, 6.0f);   // conveyor +X, rate 6

  mdk::CollisionState cs;
  cs.arena = &arena;
  cs.arenaValid = 1;
  cs.queryEnabled = 1;
  cs.objectDataLoaded = 1;
  cs.surface = &ctx;
  cs.surfaceContextMask = 0x8;
  cs.contactHook = &mdk::surfaceContactHook;
  cs.pos[0] = 0.0f;
  cs.pos[1] = 0.0f;
  cs.pos[2] = 20.0f;

  // 1. Contact on the surface poly runs the dispatch: the 0x80 effect
  //    marks surfId 2 and clears the armed flag.
  const mdk::CollisionPoly* hit = mdk::collisionApply(
      cs, 0.0f, 0.0f, -15.0f, 0.5f, nullptr, nullptr);
  check(hit == &poly, "sweep contacted the surface floor");
  check((ctx.marks & (1u << 2)) != 0, "0x80 effect set the surf-2 mark");
  check((poly.flags & 0x10) == 0, "0x80 effect cleared the armed flag");

  // 2. Conveyor: standing on the surface poly yields dir*rate*dt, which
  //    the 5B integrator carries into the displacement while grounded.
  float conv[3] = {0, 0, 0};
  mdk::surfaceConveyorDelta(ctx, hit, dt, conv);
  check(std::fabs(conv[0] - 6.0f * dt) < 1e-5f && conv[1] == 0.0f &&
            conv[2] == 0.0f,
        "conveyor delta = dir*rate*dt");
  mdk::PlayerMotionEnvironment me = {};
  me.groundContact = true;
  me.conveyorX = conv[0];
  me.conveyorY = conv[1];
  me.conveyorZ = conv[2];
  mdk::PlayerMotionState ms = {};
  const mdk::PlayerMotionOutput mo =
      mdk::integratePlayerMotion(mdk::GameplayInputFrame{}, me, ms);
  check(std::fabs(mo.dispX - conv[0]) < 1e-5f,
        "conveyor reached the 5B displacement");

  // 3. An ordinary (surface=0) poly produces no dispatch result.
  mdk::CollisionPoly plain = {};
  plain.v[0] = 0;
  plain.v[1] = 1;
  plain.v[2] = 2;
  plain.surface = 0;
  plain.flags = 0x10;
  mdk::SurfaceFxState fx;
  const std::uint8_t r0 = mdk::surfaceDispatch(
      ctx, 0, 0x8, &plain, -0xb, cs.pos, cs.pos, cs.entryPos, fx,
      nullptr, nullptr);
  check(r0 == 0, "surface=0 poly -> no dispatch effect");

  // 4. A type-9 slide-zone over the floor sets the bounce flag on a
  //    grounded contact; fed into the 5C seam it suppresses the
  //    hard-landing event and clears on the post-step.
  mdk::DtiSubRecord z9 = {};
  z9.type = 9;
  auto putf = [&](int i, float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    z9.fields[i] = u;
  };
  putf(3, -50);
  putf(4, -50);
  putf(5, 0);
  putf(6, 50);
  putf(7, 50);
  putf(8, 15);
  const float pos[3] = {0, 0, 10};
  mdk::SlideZoneResult zr = mdk::slideZoneTrigger(
      &z9, 1, 1, pos, false, true, 0.0f, 0.0f, dt);
  check(zr.inside && zr.setBounceFlag && zr.slideRedirect,
        "slide-zone inside + grounded -> bounce flag + redirect");
  mdk::VerticalCollisionResult res = {};
  res.bounce = zr.setBounceFlag;
  res.contactObj = 1;
  res.hasFloor = true;
  res.floorZ = 10.0f;
  res.posZ = 10.0f;
  mdk::PlayerVerticalState vs = {};
  mdk::PlayerMotionState vms = {};
  mdk::PlayerVerticalEnvironment ve = {};
  ve.frameStep = 1;
  ve.deltaSeconds = dt;
  ve.deepFloorZ = -1000.0f;
  vs.vertVel = -60.0f;  // a hard impact
  mdk::PlayerVerticalFrame vf;
  mdk::applyPlayerVerticalCollision(ve, vms, vs, res, vf);
  check(vs.bounceFlag == 1 && !vf.hardLanding,
        "bounce flag suppressed the hard-landing event");
  mdk::playerVerticalPostStep(ve, vs);
  check(vs.bounceFlag == 0, "post-step cleared the bounce flag");

  // 5. The pending pass re-arms the flag and clears the mark.
  mdk::surfaceApplyPending(ctx, 0);
  check((poly.flags & 0x10) != 0 && ctx.marks == 0,
        "mode-0 re-armed the flag and cleared the mark");

  mdk::surfaceRecordsDestroy(ctx);
  std::fprintf(stderr, "selftest player-surface: %s\n",
               ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

// Phase 5K — camera-pose selftest. Verifies the reconstructed
// FUN_004301e0 tail + FUN_00431100 against hand-computed values from
// the OBSERVED formulas (position branches, basis, the M1/M2 matrix
// pair, projection scalars, view config, the overhead block). No
// --data-path required.
int selftestCameraPose() {
  bool ok = true;
  auto check = [&](bool c, const char* what) {
    if (!c) ok = false;
    std::fprintf(stderr, "  %-56s %s\n", what, c ? "ok" : "FAIL");
  };
  auto near = [](float a, double e, double eps = 1e-4) {
    return std::fabs((double)a - e) < eps;
  };

  // Neutral pose: player at origin, viewYaw 90 (yaw 0), pitch 0.
  {
    mdk::PlayerCameraState st;
    mdk::PlayerCameraEnvironment e = {};
    e.viewYawDeg = 90.0f;
    mdk::updatePlayerCamera(e, st);
    check(near(st.pose.pos[0], -8.0) && near(st.pose.pos[1], 0.0) &&
              near(st.pose.pos[2], 4.5),
          "pitch=0 pullback pose (-8, 0, +4.5)");
    check(near(st.pose.back[0], -1.0) && near(st.pose.back[2], 0.0),
          "back row = -forward (yaw 0 -> +X)");
    check(near(st.pose.up[2], 1.0), "bank 0 -> up = +Z");
    check(near(st.pose.scaleX, 0.8333333) &&
              near(st.pose.scaleY, 1.3888889) && st.pose.scaleZ == -1.0f,
          "projection scalars 0.8333/1.3889/-1");
    check(near(st.pose.view[0][1], -0.8333333) &&
              near(st.pose.view[1][2], -1.3888889) &&
              near(st.pose.view[2][0], 1.0),
          "M1 rows fold scaleX/scaleY/scaleZ");
    check(near(st.pose.view[2][3], 8.0) &&
              near(st.pose.basis[2][3], -8.0),
          "M1/M2 translations (scaleZ sign split)");
    check(st.pose.modeZoom == 2.4f && st.pose.viewW == 600 &&
              st.pose.viewH == 360 && st.pose.viewCX == 300 &&
              st.pose.viewCY == 180 && st.pose.viewOX == 0 &&
              st.pose.viewOY == 0,
          "normal view config 2.4 / 600x360 @ (300,180)");
  }

  // pitch > 0 branch: T = (1-cosP)*5 rise term + pullback*cosP.
  {
    mdk::PlayerCameraState st;
    mdk::PlayerCameraEnvironment e = {};
    e.playerPos[0] = 10.0f;
    e.playerPos[1] = 20.0f;
    e.playerPos[2] = 5.0f;
    e.viewYawDeg = 90.0f;
    e.effPitchDeg = 30.0f;
    mdk::updatePlayerCamera(e, st);
    check(near(st.pose.pos[0], 3.7417) && near(st.pose.pos[2], 13.5),
          "pitch +30 -> rise + inward pullback");
    check(st.pose.pos[2] > 5.0f + 4.5f,
          "positive pitch lifts the camera above the anchor");
    // pitch < -20: D = (pitch+100)*pullback*0.0125 shrinks the arm.
    mdk::PlayerCameraState st2;
    e.effPitchDeg = -50.0f;
    mdk::updatePlayerCamera(e, st2);
    check(near(st2.pose.pos[0], 6.7861) && near(st2.pose.pos[2], 5.6698),
          "pitch -50 -> D = 5.0 shrunk pullback");
  }

  // Overhead block: FUN_00431100 — raw yaw, +1 scaleZ, no tail.
  {
    mdk::PlayerCameraState st;
    st.overheadHeight = 50.0f;
    st.pose.back[0] = 7.0f;   // stale sentinel — must survive
    mdk::PlayerCameraEnvironment e = {};
    e.playerPos[0] = 1.0f;
    e.playerPos[1] = 2.0f;
    e.playerPos[2] = 3.0f;
    e.yawDeg = 0.0f;          // raw 0x540c2c (not 90-yaw)
    mdk::updatePlayerCameraOverhead(e, st);
    check(near(st.pose.pos[2], 53.0) && near(st.pose.pos[0], 1.0),
          "overhead camPos = player + (0,0,h)");
    check(st.pose.scaleZ == 1.0f, "overhead scaleZ = +1");
    check(st.pose.back[0] == 7.0f, "overhead leaves basis rows stale");
    check(near(st.pose.basis[2][2], -1.0) &&
              near(st.pose.basis[0][1], -1.0),
          "overhead M2 rows (down-look basis)");
  }

  std::fprintf(stderr, "selftest camera-pose: %s\n",
               ok ? "PASS" : "FAIL");
  return ok ? 0 : 3;
}

// Phase 5M — camera-obstruction + nudge selftest. Exercises
// FUN_00430bf8 (synthetic wall/floor/object fixtures) and
// FUN_0042b0c0 against hand-computed values from the OBSERVED
// disassembly. No --data-path required.
int selftestCameraObstruction() {
  bool ok = true;
  auto check = [&](bool c, const char* what) {
    if (!c) ok = false;
    std::fprintf(stderr, "  %-56s %s\n", what, c ? "ok" : "FAIL");
  };
  auto near = [](float a, double e, double eps = 1e-4) {
    return std::fabs((double)a - e) < eps;
  };
  auto mkPoly = [](std::uint16_t a, std::uint16_t b, std::uint16_t c) {
    mdk::CollisionPoly p = {};
    p.v[0] = a;
    p.v[1] = b;
    p.v[2] = c;
    return p;
  };
  auto mkNode = [](float nx, float ny, float nz, float d,
                   std::uint32_t pos, std::uint32_t neg,
                   std::int16_t cn, std::int16_t cf) {
    mdk::CollisionNode n = {};
    n.nx = nx;
    n.ny = ny;
    n.nz = nz;
    n.d = d;
    n.polysPos = pos;
    n.polysNeg = neg;
    n.childNear = cn;
    n.childFar = cf;
    return n;
  };
  auto pset = [](std::uint32_t count, std::uint32_t first) {
    return (first << 16) | count;
  };

  // Wall at x=-4 facing +x; a player at the origin facing +X puts
  // the eye->camera segment through it (crossing ~(-3.9, 0, 10)).
  float wallVerts[9] = {-4, -10, 0, -4, 10, 0, -4, 0, 20};
  mdk::CollisionPoly wallPoly[1] = {mkPoly(0, 1, 2)};
  mdk::CollisionNode wallNode[1] = {
      mkNode(1, 0, 0, 4, pset(1, 0), 0, -1, -1)};
  mdk::CollisionArena wallArena = {};
  wallArena.verts = wallVerts;
  wallArena.polys = wallPoly;
  wallArena.nodes = wallNode;

  // Empty arena (node, no polys) — the sweep misses.
  mdk::CollisionNode emptyNode[1] = {
      mkNode(0, 0, 1, -10, 0, 0, -1, -1)};
  mdk::CollisionArena emptyArena = {};
  emptyArena.nodes = emptyNode;

  auto mkState = [](const mdk::CollisionArena* a) {
    mdk::CollisionState cs;
    cs.arena = a;
    cs.queryEnabled = 1;
    cs.arenaValid = 1;
    cs.objectDataLoaded = 1;
    cs.pos[0] = 0.0f;
    cs.pos[1] = 0.0f;
    cs.pos[2] = 5.0f;
    return cs;
  };
  auto mkEnv = [](mdk::CollisionState& cs) {
    mdk::PlayerCameraEnvironment e = {};
    e.playerPos[2] = 5.0f;
    e.viewYawDeg = 90.0f;
    e.yawDeg = 0.0f;
    e.collision = &cs;
    return e;
  };

  // 1. Miss: empty arena -> nothing moves.
  {
    mdk::CollisionState cs = mkState(&emptyArena);
    mdk::PlayerCameraState st;
    auto e = mkEnv(cs);
    const mdk::PlayerCameraFrame fr = mdk::updatePlayerCamera(e, st);
    check(fr.obstructionSeam, "gate fires the obstruction seam");
    check(near(cs.pos[0], 0.0) && near(st.pose.pos[0], -8.0),
          "miss -> player/camera unchanged");
  }

  // 2. Static hit: box face stops 0.1 short of the wall ->
  //    dist=4.1; the PLAYER is pushed and the camera follows.
  {
    mdk::CollisionState cs = mkState(&wallArena);
    mdk::PlayerCameraState st;
    auto e = mkEnv(cs);
    mdk::updatePlayerCamera(e, st);
    check(near(cs.pos[0], 4.1), "hit -> player pushed +4.1");
    check(near(st.pose.pos[0], -3.9) && near(st.pose.pos[2], 9.5),
          "camera follows to the box margin");
    check(near(st.pose.view[2][3], 3.9),
          "M1 commit folds the displaced camPos");
  }

  // 3. Gates: b710 == 0 and |d58| != 0 suppress the pass.
  {
    mdk::CollisionState cs = mkState(&wallArena);
    mdk::PlayerCameraState st;
    st.obstructionEnabled = false;
    auto e = mkEnv(cs);
    mdk::updatePlayerCamera(e, st);
    check(near(cs.pos[0], 0.0) && near(st.pose.pos[0], -8.0),
          "b710 == 0 -> no obstruction");
    mdk::CollisionState cs2 = mkState(&wallArena);
    mdk::PlayerCameraState st2;
    auto e2 = mkEnv(cs2);
    e2.lookActive = true;
    mdk::updatePlayerCamera(e2, st2);
    check(near(cs2.pos[0], 0.0) && near(st2.pose.pos[0], -8.0),
          "look-active -> no obstruction");
  }

  // 4. Grounding probe (0x540e4c != 0): candidate (4.1,0) and
  //    retry1 (4.1,-2.05) miss the partial floor; retry2
  //    (4.1,+2.05) lands -> the mirrored displacement applies.
  {
    float verts[18] = {-4, -10, 0, -4, 10, 0, -4, 0, 20,
                       3, 1.5f, 5, 6, 1.5f, 5, 4, 3, 5};
    mdk::CollisionPoly polys[2] = {mkPoly(0, 1, 2), mkPoly(3, 4, 5)};
    mdk::CollisionNode nodes[2] = {
        mkNode(1, 0, 0, 4, pset(1, 0), 0, 1, -1),
        mkNode(0, 0, 1, -5, pset(1, 1), 0, -1, -1)};
    mdk::CollisionArena a = {};
    a.verts = verts;
    a.polys = polys;
    a.nodes = nodes;
    mdk::CollisionState cs = mkState(&a);
    mdk::PlayerCameraState st;
    auto e = mkEnv(cs);
    e.contactToken = &polys[0];
    mdk::updatePlayerCamera(e, st);
    check(near(cs.pos[0], 4.1) && near(cs.pos[1], 2.05),
          "probe retry2 -> player (4.1, +2.05)");
    check(near(st.pose.pos[0], -3.9) && near(st.pose.pos[1], 2.05),
          "camera follows the retried delta");
  }

  // 5. All probes miss -> early return skips BOTH the apply and
  //    the object pass (an overlapping object stays inert).
  {
    float verts[18] = {-4, -10, 0, -4, 10, 0, -4, 0, 20,
                       8, 8, 5, 10, 8, 5, 9, 10, 5};
    mdk::CollisionPoly polys[2] = {mkPoly(0, 1, 2), mkPoly(3, 4, 5)};
    mdk::CollisionNode nodes[2] = {
        mkNode(1, 0, 0, 4, pset(1, 0), 0, 1, -1),
        mkNode(0, 0, 1, -5, pset(1, 1), 0, -1, -1)};
    mdk::CollisionArena a = {};
    a.verts = verts;
    a.polys = polys;
    a.nodes = nodes;
    int model = 0;
    mdk::CollisionElement elem = {};
    float eb[6] = {-6, -1, 8, -5, 1, 11};
    std::memcpy(elem.aabb, eb, sizeof(eb));
    mdk::CollisionElementSet set = {};
    set.count = 1;
    set.elems = &elem;
    mdk::CollisionObject obj = {};
    obj.named = true;
    obj.model = &model;
    obj.elements = &set;
    obj.flags14b = 1;
    std::memcpy(obj.aabb, eb, sizeof(eb));
    a.objects = &obj;
    mdk::CollisionState cs = mkState(&a);
    mdk::PlayerCameraState st;
    auto e = mkEnv(cs);
    e.contactToken = &polys[0];
    mdk::updatePlayerCamera(e, st);
    check(near(cs.pos[0], 0.0) && near(st.pose.pos[0], -8.0),
          "probes exhausted -> early return, no object pass");
  }

  // 6. Object pass: the AABB clamps the segment at x=-5; the
  //    player is pushed by (clamp - camPos).xy = (+3, 0).
  {
    int model = 0;
    mdk::CollisionElement elem = {};
    float eb[6] = {-6, -1, 8, -5, 1, 11};
    std::memcpy(elem.aabb, eb, sizeof(eb));
    mdk::CollisionElementSet set = {};
    set.count = 1;
    set.elems = &elem;
    mdk::CollisionObject obj = {};
    obj.named = true;
    obj.model = &model;
    obj.elements = &set;
    obj.flags14b = 1;
    std::memcpy(obj.aabb, eb, sizeof(eb));
    mdk::CollisionArena a = emptyArena;
    a.objects = &obj;
    mdk::CollisionState cs = mkState(&a);
    mdk::PlayerCameraState st;
    auto e = mkEnv(cs);
    mdk::updatePlayerCamera(e, st);
    check(near(cs.pos[0], 3.0) && near(st.pose.pos[0], -5.0),
          "object clamp -> player +3, camera to x=-5");
    obj.flags14b = 0;
    mdk::CollisionState cs2 = mkState(&a);
    mdk::PlayerCameraState st2;
    auto e2 = mkEnv(cs2);
    mdk::updatePlayerCamera(e2, st2);
    check(near(cs2.pos[0], 0.0) && near(st2.pose.pos[0], -8.0),
          "flags14b bit0 clear -> object skipped");
  }

  // 7. Nudge: pos += M2row0 * (arg*0.25); tick = trunc(arg*scale);
  //    M1 row0 += row2 * (tick/600); all three M1 t's refold.
  {
    mdk::PlayerCameraState st;
    st.obstructionEnabled = false;
    mdk::PlayerCameraEnvironment e = {};
    e.viewYawDeg = 90.0f;
    e.yawDeg = 0.0f;
    mdk::updatePlayerCamera(e, st);
    mdk::cameraNudge(1, st);
    check(near(st.pose.pos[1], -0.25) && near(st.pose.pos[0], -8.0),
          "nudge(+1) shifts camPos -0.25 along M2row0");
    check(st.nudgeTick == 20, "tick = trunc(1 * 20.0) = 20");
    const double r = 20.0 / 600.0;
    check(near(st.pose.view[0][0], r, 1e-6) &&
              near(st.pose.view[0][1], -0.8333333, 1e-6),
          "M1 row0 += row2 * (tick/600)");
    check(near(st.pose.view[0][3], -(-8.0 * r + 5.0 / 24.0), 1e-5) &&
              near(st.pose.view[2][3], 8.0, 1e-4),
          "all three M1 translations refolded");
    check(near(st.pose.basis[2][3], -8.0), "M2 untouched");
    st.nudgeScale = 12.7f;
    mdk::cameraNudge(-1, st);
    check(st.nudgeTick == -12, "tick truncates toward zero");
    mdk::cameraNudgeApplyMode(st, 0);
    check(st.nudgeScale == 12.0f, "mode 0 -> scale 12");
    mdk::cameraNudgeApplyMode(st, 2);
    check(st.nudgeScale == 15.0f, "mode 2 -> scale 15");
    mdk::cameraNudgeApplyMode(st, 7);
    check(st.nudgeScale == 20.0f, "default mode -> scale 20");
  }

  std::fprintf(stderr, "selftest camera-obstruction: %s\n",
               ok ? "PASS" : "FAIL");
  return ok ? 0 : 3;
}

// Phase 19A.2E — completion-writer reason labels (diagnostic-only;
// maps the port-side StreamCompletion enum, not a native field).
const char* streamCompletionName(mdk::StreamCompletion c) {
  switch (c) {
    case mdk::StreamCompletion::kHero:   return "hero";
    case mdk::StreamCompletion::kWindow: return "window";
    case mdk::StreamCompletion::kDeath:  return "death";
    default:                             return "none";
  }
}

} // namespace

// Phase 15A — combat-harness shared helpers (used by
// --traversal-runtime and --save-restore): `--hit` spec parsing +
// the per-frame shot injection. The pool's own collision + damage
// tail resolves every shot — no health/phase fields are written by
// the harness.
namespace {

struct HitSpec {
  std::string objName, elemName;
  int type = 0;
  int count = 1;
  int frame = 0;                // QA: earliest frame the shot may fire
};

// Phase 16B.1 — `--pdamage AMT@FRM`: applies the authentic producer
// (playerDamageApply = FUN_0046771c) at the END of frame FRM — the
// same producer the enemy/projectile/splash passes call — so the
// dispatch tail consumes the accumulator at frame FRM+1 exactly the
// way an in-level hit does. Nothing here writes loco/anim state.
struct PDamageSpec {
  int amount = 0;
  int frame = -1;
};

bool combatBossMatches(const mdk::DynamicObject& o, const char* nm) {
  if (nm[0] == '*') return true;
  // Live-only: the shot scan skips +0x08<=0 objects, and dead
  // objects linger in the collision list until teardown/reap —
  // without the filter a killed match keeps soaking up shots.
  return o.health > 0 && o.col.named != 0 &&
         (o.scriptClass == nm || o.model.modelName() == nm);
}

void fireCombatHits(mdk::TraversalRuntime& rt,
                    std::vector<HitSpec>& hitSpecs,
                    bool& elemShotFired, int frameNow) {
  for (auto& h : hitSpecs) {
    if (h.count <= 0 || frameNow < h.frame) continue;
    if (!h.elemName.empty() && elemShotFired) continue;
    const mdk::CollisionArena* ar =
        (rt.cs.arena != nullptr)
            ? rt.cs.arena
            : (rt.cs.carrierBusy == 0 ? rt.cs.carrier : nullptr);
    if (ar == nullptr) { h.count = 0; continue; }
    mdk::DynamicObject* boss = nullptr;
    for (const mdk::CollisionObject* o = ar->objects; o;
         o = o->next) {
      auto* cand = reinterpret_cast<mdk::DynamicObject*>(
          const_cast<mdk::CollisionObject*>(o));
      if (combatBossMatches(*cand, h.objName.c_str())) {
        boss = cand;
        break;
      }
    }
    if (boss == nullptr) continue;   // spawn may land later —
                                     // retry next frame
    // Aim through the object position — NOT the +0x198 AABB
    // centre: movers' boxes stay frozen at the pose where
    // +0x14a&0x20 latched, so the box can sit far from the live
    // model. pad covers the element cluster around pos; speed
    // crosses it fully in one tick.
    float tp[3] = {boss->pos[0], boss->pos[1], boss->pos[2]};
    float pad = 24.0f;
    float elemHalf[3] = {0.0f, 0.0f, 0.0f};
    int elemIdx = -1;
    const mdk::CollisionElementSet* es = boss->col.elements;
    if (es != nullptr) {
      float reach = 0.0f;
      // Union of the live elements — pos can be a detached
      // waypoint/anchor for driven objects (the XG walker's
      // body elements sit far from +0x10); zero-volume elems
      // union the world origin in and explode the box.
      float umin[3] = {0, 0, 0}, umax[3] = {0, 0, 0};
      int liveElems = 0;
      for (int e = 0; e < es->count; ++e) {
        const float* bb = es->elems[e].aabb;
        if (bb[0] == bb[3] && bb[1] == bb[4] && bb[2] == bb[5])
          continue;   // zero-volume elem (folded/unposed wing)
        for (int k = 0; k < 3; ++k) {
          if (liveElems == 0 || bb[k] < umin[k]) umin[k] = bb[k];
          if (liveElems == 0 || bb[3 + k] > umax[k])
            umax[k] = bb[3 + k];
        }
        ++liveElems;
      }
      if (liveElems != 0) {
        float ctr[3] = {(umin[0] + umax[0]) * 0.5f,
                        (umin[1] + umax[1]) * 0.5f,
                        (umin[2] + umax[2]) * 0.5f};
        for (int k = 0; k < 3; ++k) tp[k] = ctr[k];
      }
      for (int e = 0; e < es->count; ++e) {
        const float* bb = es->elems[e].aabb;
        if (bb[0] == bb[3] && bb[1] == bb[4] && bb[2] == bb[5])
          continue;
        const float cx = (bb[0] + bb[3]) * 0.5f - tp[0];
        const float cy = (bb[1] + bb[4]) * 0.5f - tp[1];
        const float cz = (bb[2] + bb[5]) * 0.5f - tp[2];
        const float rx = (bb[3] - bb[0]) * 0.5f;
        const float ry = (bb[4] - bb[1]) * 0.5f;
        const float rz = (bb[5] - bb[2]) * 0.5f;
        const float r = std::sqrt(cx * cx + cy * cy + cz * cz) +
                        std::sqrt(rx * rx + ry * ry + rz * rz);
        if (r > reach) reach = r;
      }
      if (reach > 0.0f) pad = reach + 6.0f;
      if (!h.elemName.empty()) {
        for (int e = 0; e < es->count; ++e) {
          if (boss->model.elemName(static_cast<std::size_t>(e)) !=
              h.elemName)
            continue;
          const float* bb = es->elems[e].aabb;
          tp[0] = (bb[0] + bb[3]) * 0.5f;
          tp[1] = (bb[1] + bb[4]) * 0.5f;
          tp[2] = (bb[2] + bb[5]) * 0.5f;
          const float rx = (bb[3] - bb[0]) * 0.5f;
          const float ry = (bb[4] - bb[1]) * 0.5f;
          const float rz = (bb[5] - bb[2]) * 0.5f;
          pad = std::sqrt(rx * rx + ry * ry + rz * rz) + 6.0f;
          elemHalf[0] = rx; elemHalf[1] = ry; elemHalf[2] = rz;
          elemIdx = e;
          break;
        }
      }
    }
    int slot = -1;
    for (int i = 0; i < 3; ++i)
      if (rt.shots[i].state == 0) { slot = i; break; }
    if (slot < 0) continue;    // pool busy — retry next frame
    float dir[3] = {tp[0] - rt.cs.pos[0], tp[1] - rt.cs.pos[1],
                    tp[2] - rt.cs.pos[2]};
    const float dl =
        std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dl > 1e-6f) { dir[0] /= dl; dir[1] /= dl; dir[2] /= dl; }
    mdk::PlayerShot& s = rt.shots[slot];
    s = mdk::PlayerShot{};
    s.state = 1;
    s.classIdx = -1;
    if (elemIdx >= 0) {
      // Element shots probe the six axis approaches off the
      // element's own AABB through the real collisionObjectProbe
      // and keep the first whose segment resolves the target
      // element — modelling the player standing where the
      // turret's face is actually exposed. A blind camera/radial
      // aim sends the segment through hull elements whose tris
      // occlude embedded turret boxes (XBS_BOT/XBS_RIGH vs
      // T1..T7), resolving a hull hit instead. The shot itself
      // still flies through the pool and resolves identically —
      // probing picks the direction, the engine does the rest.
      const float diag = std::sqrt(elemHalf[0] * elemHalf[0] +
                                   elemHalf[1] * elemHalf[1] +
                                   elemHalf[2] * elemHalf[2]);
      bool aimed = false;
      for (int ax = 0; ax < 3 && !aimed; ++ax) {
        for (int sgn = -1; sgn <= 1 && !aimed; sgn += 2) {
          float d[3] = {0.0f, 0.0f, 0.0f};
          d[ax] = static_cast<float>(sgn);
          float start[3] = {tp[0] + d[0] * (elemHalf[0] + 2.0f),
                            tp[1] + d[1] * (elemHalf[1] + 2.0f),
                            tp[2] + d[2] * (elemHalf[2] + 2.0f)};
          float end[3] = {tp[0] - d[0] * (diag + 16.0f),
                          tp[1] - d[1] * (diag + 16.0f),
                          tp[2] - d[2] * (diag + 16.0f)};
          int pe = -1, pt = -1;
          mdk::collisionObjectProbe(&boss->col, start, end,
                                    &pe, &pt);
          static const bool traceAim =
              std::getenv("MDK_TRACE_AIM") != nullptr;
          if (traceAim)
            std::fprintf(stderr,
                "    [aim] %s ax=%d sgn=%d start=(%.1f,%.1f,%.1f) "
                "end=(%.1f,%.1f,%.1f) pe=%d (want %d)\n",
                h.elemName.c_str(), ax, sgn,
                start[0], start[1], start[2],
                end[0], end[1], end[2], pe, elemIdx);
          if (pe != elemIdx) continue;
          // World-occlusion gate: the approach must also be clear in
          // the arena BSP — probing is object-local, but the real
          // shot dies on any wall between spawn and the element face
          // (e.g. XH1_KEY* sit inside the door recess; a lateral
          // approach resolves locally yet spawns inside the housing
          // wall). Require a clean stab to just past the target.
          float ocEnd[3] = {tp[0] - d[0] * (elemHalf[ax] + 4.0f),
                            tp[1] - d[1] * (elemHalf[ax] + 4.0f),
                            tp[2] - d[2] * (elemHalf[ax] + 4.0f)};
          float ocPt[3] = {0, 0, 0};
          bool occluded = false;
          if (rt.cs.arena != nullptr &&
              mdk::collisionStabFull(*rt.cs.arena, start, ocEnd, ocPt,
                                     nullptr) != nullptr)
            occluded = true;
          if (!occluded && rt.cs.carrier != nullptr &&
              rt.cs.carrierBusy == 0 &&
              mdk::collisionStabFull(*rt.cs.carrier, start, ocEnd, ocPt,
                                     nullptr) != nullptr)
            occluded = true;
          if (traceAim && occluded)
            std::fprintf(stderr, "      (occluded by BSP)\n");
          if (occluded) continue;
          dir[0] = -d[0]; dir[1] = -d[1]; dir[2] = -d[2];
          s.pos[0] = start[0]; s.pos[1] = start[1];
          s.pos[2] = start[2];
          aimed = true;
          elemShotFired = true;
        }
      }
      if (!aimed) {
        s.pos[0] = tp[0] - dir[0] * pad;
        s.pos[1] = tp[1] - dir[1] * pad;
        s.pos[2] = tp[2] - dir[2] * pad;
        elemShotFired = true;
      }
    } else {
      s.pos[0] = tp[0] - dir[0] * pad;
      s.pos[1] = tp[1] - dir[1] * pad;
      s.pos[2] = tp[2] - dir[2] * pad;
    }
    // flyTracer convention: pos += speedH*dt*(cosY*cosP,
    // sinY*cosP, -sinP) — yaw = atan2(dy,dx); positive pitch
    // travels -z.
    s.yawDeg = std::atan2(dir[1], dir[0]) * (180.0f / 3.14159265f);
    s.pitchDeg = -std::asin(dir[2]) * (180.0f / 3.14159265f);
    s.arena = rt.cur;
    s.fieldCc = 2.0f;
    s.type = static_cast<std::int16_t>(h.type);
    s.flyKind = (h.type == 0 || h.type == 2)
                    ? mdk::kShotFlyTracer
                    : mdk::kShotFlyGrenade;
    s.lifetime = 0x4b;
    // Cover the full cluster in one tick: spawn is tp-pad, so
    // 2*pad+16 reaches past the far element faces.
    s.speedH = (2.0f * pad + 16.0f) * 30.0f;
    ++rt.shotSerial;
    --h.count;
    static const bool traceHit =
        std::getenv("MDK_TRACE_HIT") != nullptr;
    if (traceHit)
      std::fprintf(stderr,
        "  [hit] %s tp=(%.1f,%.1f,%.1f) spawn=(%.1f,%.1f,%.1f) "
        "aabb=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f) pad=%.1f "
        "es=%p n=%d tris0=%d\n",
        h.objName.c_str(), tp[0], tp[1], tp[2],
        s.pos[0], s.pos[1], s.pos[2],
        boss->col.aabb[0], boss->col.aabb[3],
        boss->col.aabb[1], boss->col.aabb[4],
        boss->col.aabb[2], boss->col.aabb[5], pad,
        (const void*)es,
        es ? es->count : -1,
        (es && es->count > 0) ? es->elems[0].triCount : -1);
    if (traceHit && es != nullptr) {
      std::fprintf(stderr,
          "    pos=(%.1f,%.1f,%.1f) org=(%.1f,%.1f,%.1f) xf0=%.2f "
          "maskB=%08x\n",
          boss->pos[0], boss->pos[1], boss->pos[2],
          boss->col.origin[0], boss->col.origin[1],
          boss->col.origin[2], boss->col.xform[0],
          boss->col.elemMaskB);
      for (int e = 0; e < es->count; ++e) {
        const float* bb = es->elems[e].aabb;
        const float* lb = es->elems[e].localAabb;
        std::fprintf(stderr,
            "    elem %s aabb=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f) "
            "lbb=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f) tris=%d "
            "v=%d t=%d\n",
            boss->model.elemName(static_cast<std::size_t>(e)).c_str(),
            bb[0], bb[3], bb[1], bb[4], bb[2], bb[5],
            lb[0], lb[3], lb[1], lb[4], lb[2], lb[5],
            es->elems[e].triCount,
            es->elems[e].verts != nullptr,
            es->elems[e].tris != nullptr);
        if (es->elems[e].verts != nullptr) {
          float vmn[3] = {1e30f, 1e30f, 1e30f},
                vmx[3] = {-1e30f, -1e30f, -1e30f};
          const std::size_t nv =
              boss->model.elemVerts[e].size() / 3;
          for (std::size_t vi = 0; vi < nv; ++vi) {
            for (int k = 0; k < 3; ++k) {
              const float c = es->elems[e].verts[vi * 3 + k];
              if (c < vmn[k]) vmn[k] = c;
              if (c > vmx[k]) vmx[k] = c;
            }
          }
          std::fprintf(stderr,
              "      verts(%zu)=(%.1f..%.1f, %.1f..%.1f, "
              "%.1f..%.1f) v0=(%.1f,%.1f,%.1f)\n",
              nv, vmn[0], vmx[0], vmn[1], vmx[1],
              vmn[2], vmx[2],
              es->elems[e].verts[0], es->elems[e].verts[1],
              es->elems[e].verts[2]);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Phase 18A/18B.1 — deterministic frontend scripted-input
// diagnostic. The shell now runs on the real FrontendHostServices
// seams (Phase 18B.1): Phase A performs read-only probes against a
// --data-path corpus (SAVES enumeration/inspect, LASTGAME, MDKS_*
// slides) when one is supplied; Phase B drives the scripted-input
// shell against a TEMP save root where writes are real. The tool
// never writes into the supplied data path.

const char* frontendRequestName(mdk::FrontendRequest r) {
  switch (r) {
  case mdk::FrontendRequest::None: return "none";
  case mdk::FrontendRequest::Quit: return "Quit";
  case mdk::FrontendRequest::StartNewGame: return "StartNewGame";
  case mdk::FrontendRequest::ContinueLastGame: return "ContinueLastGame";
  case mdk::FrontendRequest::LoadSave: return "LoadSave";
  case mdk::FrontendRequest::WriteSaveDone: return "WriteSaveDone";
  case mdk::FrontendRequest::AbortToFrontend: return "AbortToFrontend";
  case mdk::FrontendRequest::ResumeTraversal: return "ResumeTraversal";
  case mdk::FrontendRequest::CycleBrightness: return "CycleBrightness";
  case mdk::FrontendRequest::CaptureUtility: return "CaptureUtility";
  case mdk::FrontendRequest::OpenLegacyScreen: return "OpenLegacyScreen";
  }
  return "?";
}

const char* frontendFxName(mdk::FrontendFx f) {
  switch (f) {
  case mdk::FrontendFx::PauseSounds: return "PauseSounds";
  case mdk::FrontendFx::ResumeSounds: return "ResumeSounds";
  case mdk::FrontendFx::ResumeSoundsAlt: return "ResumeSoundsAlt";
  case mdk::FrontendFx::MenuSongStart: return "MenuSongStart";
  case mdk::FrontendFx::LoadFrontendResources:
    return "LoadFrontendResources";
  case mdk::FrontendFx::TransitionArmed: return "TransitionArmed";
  case mdk::FrontendFx::AbortDialogResources:
    return "AbortDialogResources";
  case mdk::FrontendFx::SaveNameThumbnailGrab:
    return "SaveNameThumbnailGrab";
  }
  return "?";
}

const char* frontendScreenName(mdk::FrontendScreen s) {
  switch (s) {
  case mdk::FrontendScreen::Root: return "root";
  case mdk::FrontendScreen::Options: return "options";
  case mdk::FrontendScreen::Display: return "display";
  case mdk::FrontendScreen::Sound: return "sound";
  case mdk::FrontendScreen::Mouse: return "mouse";
  case mdk::FrontendScreen::Keyboard: return "keyboard";
  }
  return "?";
}

void printFrontendState(const char* tag, mdk::FrontendShell& sh) {
  // Drain the per-frame outputs for printing.
  std::string reqs;
  while (sh.pendingRequest() != mdk::FrontendRequest::None) {
    if (!reqs.empty()) reqs += ',';
    reqs += frontendRequestName(sh.pendingRequest());
    if (!sh.requestName().empty()) {
      reqs += '(';
      reqs += sh.requestName();
      reqs += ')';
    }
    sh.consumeRequest();
  }
  std::string fx;
  for (const auto f : sh.drainFx()) {
    if (!fx.empty()) fx += ',';
    fx += frontendFxName(f);
  }
  const int sel = sh.flow().screen() == mdk::FrontendScreen::Root
                      ? sh.flow().root().selection()
                      : -1;
  std::printf("  %-26s mode=%d sub=%2d screen=%-8s sel=%2d "
              "saves=%d paused=%d idle=%d quit=%d reqs=[%s] fx=[%s]\n",
              tag, sh.primaryMode(), sh.subMode(),
              frontendScreenName(sh.flow().screen()), sel,
              sh.flow().root().savesExist() ? 1 : 0,
              sh.paused() ? 1 : 0, sh.idleTicks(),
              sh.quitRequested() ? 1 : 0,
              reqs.empty() ? "-" : reqs.c_str(),
              fx.empty() ? "-" : fx.c_str());
}

void printSlotDetail(const mdk::FrontendHostServices& host,
                     std::string_view stem) {
  const auto d = host.inspectSlotDetail(stem);
  if (!d) {
    std::printf("        slot %-8s: unopenable (invalid entry)\n",
                std::string(stem).c_str());
    return;
  }
  std::printf("        slot %-8s: valid=%d full=%d modeField=%d "
              "level=%d health=%d deaths=%d thmb=%zuB err=%s\n",
              std::string(stem).c_str(), d->summary.valid ? 1 : 0,
              d->summary.fullSave ? 1 : 0, d->summary.modeField,
              d->summary.levelId, d->summary.health,
              d->summary.deathCount, d->thumbnail.size(),
              mdk::saveErrorName(d->error));
}

void printSlideProbe(const mdk::FrontendHostServices& host, int n) {
  const auto info = host.slideInfo(n);
  if (!info) {
    std::printf("        slide %d: no data root\n", n);
    return;
  }
  const auto data = host.slideData(n);
  std::printf("        slide %-3d %-16s exists=%d %dx%d %lluB "
              "data=%zuB gate600x360=%d\n",
              n, info->relPath.c_str(), info->exists ? 1 : 0,
              info->width, info->height,
              (unsigned long long)info->bytes,
              data ? data->size() : 0,
              host.slideExists(n) ? 1 : 0);
}

int frontendScript(const std::optional<std::string>& dataPath) {
  namespace fs = std::filesystem;
  std::string err;

  // ---- Phase A: read-only probes against the real corpus ------
  std::optional<mdk::DataRoot> dataRoot;
  std::optional<mdk::FrontendHostServices> realHost;
  if (dataPath) {
    dataRoot = mdk::DataRoot::open(*dataPath, &err);
    if (dataRoot &&
        fs::is_directory(fs::path(*dataPath) / "SAVES")) {
      realHost.emplace(fs::path(*dataPath) / "SAVES");
      realHost->setDataRoot(&*dataRoot);
    }
  }
  if (realHost) {
    std::printf("== host probes (read-only, %s/SAVES) ==\n",
                dataPath->c_str());
    std::printf("  LASTGAME.SAV exists+envelope-valid: %d\n",
                realHost->lastGameExists() ? 1 : 0);
    const auto names = realHost->enumerateSaves();
    std::printf("  enumeration (%zu):", names.size());
    for (const auto& n : names) std::printf(" %s", n.c_str());
    std::printf("\n");
    for (const auto& n : names) {
      // The list controller sees saveListStem-truncated names; the
      // host probes by stem.
      printSlotDetail(*realHost, mdk::saveListStem(n));
    }
    for (int i = 0; i <= 5; ++i) printSlideProbe(*realHost, i);
  } else {
    std::printf("== host probes skipped (no --data-path) ==\n");
  }

  // ---- Phase B: temp writable save root, real host seams ------
  const fs::path tmp = fs::temp_directory_path() / "mdk_inspect_fehost";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "SAVES");
  fs::create_directories(tmp / "MISC");
  mdk::FrontendHostServices host(tmp / "SAVES");
  std::optional<mdk::DataRoot> tmpRoot =
      mdk::DataRoot::open(tmp, &err);
  if (tmpRoot) host.setDataRoot(&*tmpRoot);
  std::printf("== host probes (temp root %s) ==\n",
              host.saveRoot().string().c_str());
  std::printf("  LASTGAME.SAV exists (empty root): %d\n",
              host.lastGameExists() ? 1 : 0);

  // Synthetic MDKS_* GIFs: the probe reads the GIF logical-screen
  // header only (no decode — the 600x360 gate is the OBSERVED check).
  {
    auto putGif = [&](int n, unsigned w, unsigned h) {
      const std::byte g[10] = {
          std::byte('G'), std::byte('I'), std::byte('F'),
          std::byte('8'), std::byte('9'), std::byte('a'),
          std::byte(w & 0xff), std::byte(w >> 8),
          std::byte(h & 0xff), std::byte(h >> 8)};
      std::ofstream f(tmp / "MISC" /
                          (std::string("MDKS_") +
                           (n < 10 ? "00" : "0") +
                           std::to_string(n) + ".GIF"),
                      std::ios::binary);
      f.write(reinterpret_cast<const char*>(g), sizeof(g));
    };
    putGif(0, 600, 360);
    putGif(1, 320, 200);
    std::ofstream(tmp / "MISC" / "MDKS_002.GIF") << "notagif!!";
  }
  for (int i = 0; i <= 3; ++i) printSlideProbe(host, i);

  // Seed saves through the writer: two header-only + LASTGAME.
  mdk::SaveWriteInput wi;
  wi.modeField = 3; wi.levelId = 2; wi.health = 100;
  host.writeSaveFile("SLOT01", mdk::saveGameWriteHeaderOnly(wi));
  wi.levelId = 4; wi.deathCount = 1;
  host.writeSaveFile("BOSS2", mdk::saveGameWriteHeaderOnly(wi));
  wi.levelId = 1; wi.deathCount = 0;
  std::printf("  writeLastgame: %d\n",
              host.saves().writeLastgame(wi) ? 1 : 0);
  std::printf("  LASTGAME.SAV exists (seeded): %d\n",
              host.lastGameExists() ? 1 : 0);
  {
    const auto names = host.enumerateSaves();
    std::printf("  enumeration (%zu):", names.size());
    for (const auto& n : names) std::printf(" %s", n.c_str());
    std::printf("\n");
    for (const auto& n : names)
      printSlotDetail(host, mdk::saveListStem(n));
  }

  // The write seam's content source: the embedder snapshots the live
  // session into FrontendSaveData. The diagnostic has no live
  // traversal runtime, so `full` stays empty — a full (F2) write then
  // fails as the OBSERVED FUN_00422dec path does (dialog stays open).
  mdk::FrontendSaveData saveData;
  saveData.headerOnly.modeField = 3;
  saveData.headerOnly.levelId = 1;
  saveData.headerOnly.health = 80;
  mdk::FrontendShell sh(host.makeSeams(
      [&saveData]() -> std::optional<mdk::FrontendSaveData> {
        return saveData;
      }));

  mdk::FrontendMenuInput in;
  auto step = [&](const char* tag) {
    sh.update(in);
    sh.endFrame(33.3);
    printFrontendState(tag, sh);
    in = {};
  };
  auto press = [&](const char* tag, bool mdk::FrontendMenuInput::*f) {
    in.*f = true;
    step(tag);
    step("  (release)");
  };

  std::printf("== frontend script (temp root, real seams) ==\n");
  std::printf("[boot] fresh entry (FUN_0041d85c arg=0)\n");
  step("boot");

  std::printf("[nav] selection moves down through the 5 items\n");
  press("down -> sel+1", &mdk::FrontendMenuInput::nextHeld);
  press("down -> sel+1", &mdk::FrontendMenuInput::nextHeld);

  std::printf("[saves] sel2 Saved Game -> sub 1 list\n");
  in.confirmEdge = true;
  step("confirm -> arm list");
  {
    const auto* l = sh.saveList();
    if (l) {
      std::printf("      slots (%d):", l->count());
      for (const auto& s : l->stems()) std::printf(" %s", s.c_str());
      std::printf("\n");
      for (int i = 0; i < l->count(); ++i) {
        if (i > 0) {
          press("  list down", &mdk::FrontendMenuInput::nextHeld);
        }
        if (const auto* s = sh.saveList()->selected()) {
          std::printf("        slot %d: name=%-8s valid=%d full=%d "
                      "modeField=%d level=%d health=%d deaths=%d\n",
                      i, s->name.c_str(), s->valid ? 1 : 0,
                      s->fullSave ? 1 : 0, s->modeField, s->levelId,
                      s->health, s->deathCount);
        }
      }
    }
  }
  in.cancelEdge = true;
  step("Esc -> list exit");

  std::printf("[continue] sel0 Continue -> ContinueLastGame\n");
  in.confirmEdge = true;
  step("confirm -> continue");
  std::printf("      (host reports load failure -> fresh re-entry)\n");
  sh.notifyLoadResult(false);
  printFrontendState("load-failed", sh);

  std::printf("[new game] sel1 New Game -> StartNewGame\n");
  press("down", &mdk::FrontendMenuInput::nextHeld);
  in.confirmEdge = true;
  step("confirm -> new game");

  std::printf("[options] sel3 Options -> sub 11, Esc backs out\n");
  press("down", &mdk::FrontendMenuInput::nextHeld);
  press("down", &mdk::FrontendMenuInput::nextHeld);
  in.confirmEdge = true;
  step("confirm -> options");
  in.cancelEdge = true;
  step("Esc -> back");

  std::printf("[ingame] host sets mode=3 running -> F2 save gate\n");
  sh.setPrimaryMode(mdk::mode::observed::traversal);
  sh.setRunning(true);
  sh.setSaveBlockFlags(false, false, /*xStrike=*/true);
  in.f2Edge = true;
  step("F2 (X_STRIKE live)");
  sh.setSaveBlockFlags(false, false, false);
  in.f2Edge = true;
  step("F2 (gate open)");
  for (const char c : std::string_view("MDK")) {
    in.typedChar = c;
    step("  type");
  }
  in.confirmEdge = true;
  step("Enter -> commit");
  std::printf("      full bytes not producible without a live session\n");
  std::printf("      -> write fails, dialog stays open (OBSERVED)\n");
  in.cancelEdge = true;
  step("Esc -> cancel entry");

  std::printf("[autosave] mode=5 armAutosave bypasses the manual\n");
  std::printf("         gate; confirm -> writes a real header-only\n");
  sh.setPrimaryMode(mdk::mode::observed::stats);
  sh.setSaveBlockFlags(true, true, /*xStrike=*/true);
  sh.setLevelIndex(2);
  sh.armAutosave();
  printFrontendState("autosave armed", sh);
  in.confirmEdge = true;
  step("confirm YES");
  in.confirmEdge = true;
  step("confirm name");
  std::printf("      wrote %s: %d\n",
              (tmp / "SAVES" / "3.SAV").string().c_str(),
              int(fs::exists(tmp / "SAVES" / "3.SAV")));
  printSlotDetail(host, "3");
  {
    const auto names = host.enumerateSaves();
    std::printf("      re-enumeration (%zu):", names.size());
    for (const auto& n : names) std::printf(" %s", n.c_str());
    std::printf("\n");
  }

  std::printf("[ingame] Esc arms the abort console and the same-frame\n");
  std::printf("         dispatch self-cancels it (OBSERVED quirk)\n");
  in.cancelEdge = true;
  step("Esc");

  std::printf("[ingame] F10 abort -> Y -> AbortToFrontend\n");
  in.f10Edge = true;
  step("F10 -> abort arm");
  in.keyYEdge = true;
  step("Y -> yes");

  std::printf("[returning] enterFrontend(arg!=0): transition armed,\n");
  std::printf("            Esc arm suppressed; host ack releases\n");
  sh.enterFrontend(true);
  mdk::frontendHostTransitionArmed(sh);
  printFrontendState("enterFrontend(1)", sh);
  in.cancelEdge = true;
  step("Esc (suppressed)");
  mdk::frontendHostTransitionComplete(sh);
  std::printf("      transition complete -> suppressEscAbort=%d\n",
              sh.suppressEscAbort() ? 1 : 0);
  in.cancelEdge = true;
  step("Esc (released)");

  fs::remove_all(tmp);
  std::printf("[done] temp root removed\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Collision-census descent trace — mirrors bspSweep's visit/crossing
// structure (FUN_00408260) for a fixed segment, recording every node
// whose poly set the real sweep would polyScan. QA only: classifies a
// support-grid miss as "poly never testable via BSP descent" (data-side
// quirk the original shares) vs "poly in a tested set but no contact"
// (port-side suspect).
// ---------------------------------------------------------------------------
// QA mirror of FUN_004089c0 (boxTri) for census forensics — identical
// projected box-vs-triangle logic; reports which axis separated.
int censusBoxTri(const float* contact, const float* ext,
                 const float* v0, const float* v1, const float* v2,
                 int* failAxis) {
  static const int kAxis[3][2] = {{1, 2}, {0, 2}, {0, 1}};
  const float* verts[3] = {v0, v1, v2};
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(ext[a]) < 0.1f) continue;
    const int a0 = kAxis[a][0], a1 = kAxis[a][1];
    float rel[3][2];
    std::uint32_t codes[3] = {0, 0, 0};
    int outside = 0;
    for (int i = 0; i < 3; ++i) {
      rel[i][0] = verts[i][a0] - contact[a0];
      rel[i][1] = verts[i][a1] - contact[a1];
      std::uint32_t c = 0;
      if (rel[i][0] >= -ext[a0]) {
        if (rel[i][0] > ext[a0]) c = 2;
      } else {
        c = 1;
      }
      if (rel[i][1] >= -ext[a1]) {
        if (rel[i][1] > ext[a1]) c |= 8;
      } else {
        c |= 4;
      }
      if (c == 0) break;
      codes[i] = c;
      ++outside;
    }
    if (outside != 3) continue;
    if ((codes[0] & codes[1] & codes[2]) != 0) {
      if (failAxis) *failAxis = a;
      return 0;
    }
    if (((codes[0] | codes[1] | codes[2]) & 3) == 0) {
      std::uint32_t mask = 3;
      for (int i = 0; i < 3 && mask != 0; ++i) {
        const int j = (i + 1) % 3;
        const std::uint32_t diff = codes[i] ^ codes[j];
        if (diff & 4) {
          const float t =
              ((rel[j][0] - rel[i][0]) * (-ext[a1] - rel[i][1])) /
                  (rel[j][1] - rel[i][1]) +
              rel[i][0];
          if (-ext[a0] <= t && t <= ext[a0]) mask = 0;
        }
        if (diff & 8) {
          const float t =
              ((rel[j][0] - rel[i][0]) * (ext[a1] - rel[i][1])) /
                  (rel[j][1] - rel[i][1]) +
              rel[i][0];
          if (-ext[a0] <= t && t <= ext[a0]) mask = 0;
        }
      }
      if (mask != 0) {
        if (failAxis) *failAxis = a;
        return 0;
      }
    } else {
      std::uint32_t mask = 0xc;
      for (int i = 0; i < 3 && mask != 0; ++i) {
        const int j = (i + 1) % 3;
        if ((codes[i] & 3) == 0) mask &= codes[i];
        const std::uint32_t diff = codes[i] ^ codes[j];
        if (diff & 1) {
          const float t =
              ((rel[j][1] - rel[i][1]) * (-ext[a0] - rel[i][0])) /
                  (rel[j][0] - rel[i][0]) +
              rel[i][1];
          if (-ext[a1] <= t) {
            if (t <= ext[a1]) {
              mask = 0;
            } else {
              mask &= 8;
            }
          } else {
            mask &= 4;
          }
        }
        if (diff & 2) {
          const float t =
              ((rel[j][1] - rel[i][1]) * (ext[a0] - rel[i][0])) /
                  (rel[j][0] - rel[i][0]) +
              rel[i][1];
          if (-ext[a1] <= t) {
            if (t <= ext[a1]) {
              mask = 0;   // OBSERVED: breaks edge scan, axis passes
              break;
            }
            mask &= 8;
          } else {
            mask &= 4;
          }
        }
      }
      if (mask != 0) {
        if (failAxis) *failAxis = a;
        return 0;
      }
    }
  }
  return 1;
}

struct CensusSweepTrace {
  struct TestedSet {
    int node;
    int neg;                 // 1 = polysNeg tested, 0 = polysPos
    std::uint32_t first;
    std::uint32_t count;
    float cx, cy, cz;        // contact point on the node plane
    float fcx, fcy, fcz;     // first-attempt contact (dFace-adjusted)
  };
  const mdk::CollisionNode* nodes;
  float pos[3];
  float target[3];
  float delta[3];
  float ext[3];
  float fatMargin;
  std::vector<TestedSet> tested;
  std::vector<int> visited;

  void descend(const mdk::CollisionNode* node) {
    for (;;) {
      const int nodeIdx = static_cast<int>(node - nodes);
      visited.push_back(nodeIdx);
      const float margin = std::fabs(ext[2] * node->nz) +
                           std::fabs(ext[0] * node->nx) +
                           std::fabs(ext[1] * node->ny);
      const float dStart = pos[1] * node->ny + node->d +
                           pos[2] * node->nz + pos[0] * node->nx;
      std::uint8_t sideStart = 0;
      if (-fatMargin <= dStart) {
        sideStart = 1;
        if (node->childNear >= 0) descend(nodes + node->childNear);
      }
      if (dStart <= fatMargin) {
        sideStart |= 2;
        if (node->childFar >= 0) descend(nodes + node->childFar);
      }
      const float dTarget = target[1] * node->ny + node->d +
                            target[2] * node->nz + target[0] * node->nx;
      std::uint8_t sideTarget = 0;
      if (-fatMargin <= dTarget) sideTarget = 1;
      if (dTarget <= fatMargin) sideTarget |= 2;
      if ((sideStart | sideTarget) == 3 &&
          ((dStart < 0.0f && dStart <= dTarget) ||
           (0.0f <= dStart && dTarget <= dStart))) {
        float dFace;
        if (0.0f <= dStart) {
          dFace = margin;
          if (dStart < margin) dFace = dStart;
        } else {
          dFace = -margin;
          if (-margin < dStart) dFace = dStart;
        }
        const float dDelta = delta[2] * node->nz + delta[0] * node->nx +
                             delta[1] * node->ny;
        if (dDelta != 0.0f) {
          const float t = -dStart / dDelta;
          const float tf = -(dStart - dFace) / dDelta;
          // real gate: t1 <= bestT(5000 on a miss column) && t1 <= 1
          if (tf > 1.0f) goto descend;
          const std::uint32_t set =
              (dStart < 0.0f) ? node->polysNeg : node->polysPos;
          tested.push_back(
              {nodeIdx, dStart < 0.0f ? 1 : 0, set >> 16,
               set & 0xffffu, t * delta[0] + pos[0],
               t * delta[1] + pos[1], t * delta[2] + pos[2],
               tf * delta[0] + pos[0], tf * delta[1] + pos[1],
               tf * delta[2] + pos[2]});
        }
      }
    descend:
      const std::uint8_t next =
          static_cast<std::uint8_t>(sideTarget & (sideStart ^ sideTarget));
      if (next & 1) {
        if (node->childNear < 0) return;
        node = nodes + node->childNear;
      } else if (next & 2) {
        if (node->childFar < 0) return;
        node = nodes + node->childFar;
      } else {
        return;
      }
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::string> dataPath;
  std::optional<std::string> target;
  std::optional<std::string> visualInfoName;
  std::optional<std::string> fontInfoName;
  std::optional<std::string> spriteInfoName;
  std::optional<unsigned> fontInfoCode;
  bool entriesMode = false;
  bool collisionProbe = false;
  bool collisionCensus = false;
  bool arenaObjects = false;
  bool surfaceCensus = false;
  bool arenaRender = false;
  bool traversalRuntime = false;
  bool freefallRuntime = false;
  bool streamInit = false;
  int streamFrames = 0;
  bool campaignHandoff = false;
  bool campaignSequence = false;
  bool frontendScriptMode = false;
  std::optional<std::string> saveInfoPath;
  std::optional<std::string> saveRoundtripPath;
  std::optional<std::string> saveRestorePath;
  std::optional<std::string> saveWriteFullPath;
  int saveActivateIdx = -1;
  int ffCourse = 0;
  int ffSkill = 0;
  unsigned ffSeed = 0xC0FFEE;
  bool scriptDisasm = false;
  bool objScriptDisasm = false;
  std::string scriptDisasmName;
  std::optional<std::uint32_t> scriptDisasmOff;
  std::vector<HitSpec> hitSpecs;
  std::vector<PDamageSpec> pdmgSpecs;
  std::vector<int> itemUseFrames;
  struct HoldSpec { int key; int first; int last; };
  std::vector<HoldSpec> holdSpecs;
  std::vector<HoldSpec> pressSpecs;
  // QA route driver — see the --route parse branch for semantics.
  struct RouteWaypoint { float x, y, z, r; bool jump; };
  std::vector<RouteWaypoint> routeWps;
  std::vector<std::pair<int, int>> ammoSpecs;
  std::vector<std::string> bossNames;
  std::optional<std::string> travArena;
  float travStart[3] = {0.0f, 0.0f, 0.0f};
  bool travStartGiven = false;
  float travYaw = 0.0f;
  bool travYawGiven = false;
  std::uint32_t travScriptPc = 0;
  int travFrames = 90;
  float probePos[3] = {0.0f, 0.0f, 0.0f};
  int probePosGiven = 0;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "--data-path")) {
      const char* v = value(a);
      if (!v) return usage();
      dataPath = v;
    } else if (!std::strcmp(a, "--container")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
    } else if (!std::strcmp(a, "--entries")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      entriesMode = true;
    } else if (!std::strcmp(a, "--visual-info")) {
      const char* v = value(a);
      if (!v) return usage();
      const char* n = value(a);
      if (!n) return usage();
      target = v;
      visualInfoName = n;
    } else if (!std::strcmp(a, "--font-info")) {
      const char* v = value(a);
      if (!v) return usage();
      const char* n = value(a);
      if (!n) return usage();
      target = v;
      fontInfoName = n;
      // Optional glyph code: decimal or 0xNN.
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        const char* c = argv[++i];
        char* endp = nullptr;
        const unsigned long cv = std::strtoul(c, &endp, 0);
        if (!endp || *endp != '\0' || cv > 0xff) {
          std::fprintf(stderr, "invalid glyph code: %s (want 0-255)\n",
                       c);
          return usage();
        }
        fontInfoCode = static_cast<unsigned>(cv);
      }
    } else if (!std::strcmp(a, "--sprite-info")) {
      const char* v = value(a);
      if (!v) return usage();
      const char* n = value(a);
      if (!n) return usage();
      target = v;
      spriteInfoName = n;
    } else if (!std::strcmp(a, "--collision-probe")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      collisionProbe = true;
      // Optional probe position (defaults to the vert-bounds centre).
      // A leading '-' followed by a digit is a negative coordinate,
      // not a flag.
      for (int k = 0; k < 3; ++k) {
        if (i + 1 < argc &&
            (argv[i + 1][0] != '-' ||
             (argv[i + 1][1] >= '0' && argv[i + 1][1] <= '9'))) {
          const char* c = argv[++i];
          char* endp = nullptr;
          const double dv = std::strtod(c, &endp);
          if (!endp || *endp != '\0') {
            std::fprintf(stderr, "invalid probe coordinate: %s\n", c);
            return usage();
          }
          probePos[k] = static_cast<float>(dv);
          probePosGiven |= 1 << k;
        }
      }
      if (probePosGiven != 0 && probePosGiven != 7) {
        std::fprintf(stderr, "--collision-probe needs either no "
                             "position or all of <x> <y> <z>\n");
        return usage();
      }
    } else if (!std::strcmp(a, "--collision-census")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      collisionCensus = true;
    } else if (!std::strcmp(a, "--arena-objects")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      arenaObjects = true;
    } else if (!std::strcmp(a, "--surface-census")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      surfaceCensus = true;
    } else if (!std::strcmp(a, "--arena-render")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      arenaRender = true;
    } else if (!std::strcmp(a, "--traversal-runtime")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;
      traversalRuntime = true;
    } else if (!std::strcmp(a, "--hit")) {
      // Phase 15A combat harness: "NAME[@ELEM][:TYPE][xN]". Each
      // remaining count injects ONE shot per frame into a free pool
      // slot, aimed through the named object/element — the pool's
      // own collision + damage tail resolves it (FUN_00460d44 /
      // the w0 hit path). TYPE: weapon 0..4 (default 0); N default 1.
      const char* v = value(a);
      if (!v) return usage();
      HitSpec h;
      std::string spec = v;
      const auto cm = spec.rfind(',');
      if (cm != std::string::npos && cm + 2 < spec.size() &&
          spec[cm + 1] == 'f' &&
          std::isdigit(static_cast<unsigned char>(spec[cm + 2]))) {
        h.frame = std::atoi(spec.c_str() + cm + 2);
        spec.erase(cm);
      }
      const auto x = spec.rfind('x');
      if (x != std::string::npos && x + 1 < spec.size() &&
          std::isdigit(static_cast<unsigned char>(spec[x + 1]))) {
        h.count = std::atoi(spec.c_str() + x + 1);
        spec.erase(x);
      }
      const auto colon = spec.rfind(':');
      if (colon != std::string::npos &&
          colon + 1 < spec.size() &&
          std::isdigit(static_cast<unsigned char>(spec[colon + 1]))) {
        h.type = std::atoi(spec.c_str() + colon + 1);
        spec.erase(colon);
      }
      const auto at = spec.find('@');
      if (at != std::string::npos) {
        h.objName = spec.substr(0, at);
        h.elemName = spec.substr(at + 1);
      } else {
        h.objName = spec;
      }
      hitSpecs.push_back(h);
    } else if (!std::strcmp(a, "--pdamage")) {
      // Phase 16B.1: "AMT@FRM" — playerDamageApply(AMT) at the end of
      // frame FRM (post-dispatch, matching the in-level producers).
      const char* v = value(a);
      if (!v) return usage();
      PDamageSpec d;
      const std::string spec = v;
      const auto at = spec.find('@');
      if (at == std::string::npos) {
        std::fprintf(stderr, "--pdamage wants AMT@FRM\n");
        return usage();
      }
      d.amount = std::atoi(spec.substr(0, at).c_str());
      d.frame = std::atoi(spec.c_str() + at + 1);
      pdmgSpecs.push_back(d);
    } else if (!std::strcmp(a, "--itemuse")) {
      // P0-C: inject the itemUse key edge (binding slot 26 = code 28)
      // on the given frame — the real 0x4ce774 edge path.
      const char* v = value(a);
      if (!v) return usage();
      itemUseFrames.push_back(std::atoi(v));
    } else if (!std::strcmp(a, "--hold")) {
      // QA enabler: hold raw key CODE level-high for frames A..B.
      const char* v = value(a);
      if (!v) return usage();
      const std::string spec(v);
      const size_t at = spec.find('@'), dash = spec.find('-', at + 1);
      if (at == std::string::npos || dash == std::string::npos) {
        std::fprintf(stderr, "--hold wants KEY@A-B\n");
        return usage();
      }
      HoldSpec h;
      h.key = std::atoi(spec.substr(0, at).c_str());
      h.first = std::atoi(spec.substr(at + 1, dash - at - 1).c_str());
      h.last = std::atoi(spec.c_str() + dash + 1);
      holdSpecs.push_back(h);
    } else if (!std::strcmp(a, "--press")) {
      // QA enabler: inject the raw key CODE edge on a single frame —
      // the 0x4ce7xx edge path (weapon hotkeys, item use, sniper
      // toggle are edge-read, not level-read).
      const char* v = value(a);
      if (!v) return usage();
      const std::string spec(v);
      const size_t at = spec.find('@');
      if (at == std::string::npos) {
        std::fprintf(stderr, "--press wants KEY@FRAME\n");
        return usage();
      }
      HoldSpec h;
      h.key = std::atoi(spec.substr(0, at).c_str());
      h.first = h.last = std::atoi(spec.c_str() + at + 1);
      pressSpecs.push_back(h);
    } else if (!std::strcmp(a, "--route")) {
      // QA route driver: one waypoint per --route "x,y,z[,r[,j]]".
      // Steers with REAL key levels (105/106 turn, 103 forward, 56
      // jump-hold on flag 'j'); a waypoint counts reached only while
      // grounded, so scripted vehicle legs can't consume waypoints.
      // r = arrive radius (default 4). No runtime fields are written.
      const char* v = value(a);
      if (!v) return usage();
      RouteWaypoint w;
      std::string spec(v);
      std::vector<float> vals;
      size_t p = 0;
      bool jumpFlag = false;
      while (p <= spec.size()) {
        const size_t c = spec.find(',', p);
        const std::string tok =
            spec.substr(p, c == std::string::npos ? spec.size() - p
                                                  : c - p);
        if (tok == "j") jumpFlag = true;
        else vals.push_back(std::strtof(tok.c_str(), nullptr));
        if (c == std::string::npos) break;
        p = c + 1;
      }
      if (vals.size() < 3) {
        std::fprintf(stderr, "--route wants x,y,z[,r[,j]]\n");
        return usage();
      }
      w.x = vals[0]; w.y = vals[1]; w.z = vals[2];
      w.r = vals.size() > 3 ? vals[3] : 4.f;
      w.jump = jumpFlag;
      routeWps.push_back(w);
    } else if (!std::strcmp(a, "--ammo")) {
      // QA enabler: seed ammo block entry I to N at runtime init
      // (a level-start save carries session ammo; a fresh QA start
      // has none — the fire-chain proof needs live 0x541633).
      const char* v = value(a);
      if (!v) return usage();
      const std::string spec(v);
      const size_t at = spec.find('@');
      if (at == std::string::npos) {
        std::fprintf(stderr, "--ammo wants I@N\n");
        return usage();
      }
      ammoSpecs.emplace_back(std::atoi(spec.substr(0, at).c_str()),
                             std::atoi(spec.c_str() + at + 1));
    } else if (!std::strcmp(a, "--boss")) {
      const char* v = value(a);
      if (!v) return usage();
      bossNames.push_back(v);
    } else if (!std::strcmp(a, "--freefall-runtime")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // FALL3D.BNI path
      freefallRuntime = true;
    } else if (!std::strcmp(a, "--stream-init")) {
      streamInit = true;          // fixed paths — STREAM/STREAM.{BNI,MTI}
    } else if (!std::strcmp(a, "--stream-frames")) {
      const char* v = value(a);
      if (!v) return usage();
      streamFrames = std::atoi(v); // frames to step after stream-init
    } else if (!std::strcmp(a, "--campaign-handoff")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // FALL3D.BNI path
      campaignHandoff = true;
    } else if (!std::strcmp(a, "--campaign-sequence")) {
      campaignSequence = true;
    } else if (!std::strcmp(a, "--frontend-script")) {
      frontendScriptMode = true;
    } else if (!std::strcmp(a, "--save-info")) {
      const char* v = value(a);
      if (!v) return usage();
      saveInfoPath = v;
    } else if (!std::strcmp(a, "--save-roundtrip")) {
      const char* v = value(a);
      if (!v) return usage();
      saveRoundtripPath = v;
    } else if (!std::strcmp(a, "--save-restore")) {
      const char* v = value(a);
      if (!v) return usage();
      saveRestorePath = v;
    } else if (!std::strcmp(a, "--save-activate")) {
      const char* v = value(a);
      if (!v) return usage();
      char* endp = nullptr;
      const long n = std::strtol(v, &endp, 10);
      if (!endp || *endp != '\0' || n < 0) {
        std::fprintf(stderr, "invalid --save-activate: %s\n", v);
        return usage();
      }
      saveActivateIdx = static_cast<int>(n);
    } else if (!std::strcmp(a, "--save-write-full")) {
      const char* v = value(a);
      if (!v) return usage();
      saveWriteFullPath = v;
    } else if (!std::strcmp(a, "--course")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      const long v = std::strtol(c, &endp, 10);
      if (!endp || *endp != '\0' || v < 0 || v > 4) {
        std::fprintf(stderr, "invalid --course (0..4): %s\n", c);
        return usage();
      }
      ffCourse = static_cast<int>(v);
    } else if (!std::strcmp(a, "--skill")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      const long v = std::strtol(c, &endp, 10);
      if (!endp || *endp != '\0' || v < 0 || v > 2) {
        std::fprintf(stderr, "invalid --skill (0..2): %s\n", c);
        return usage();
      }
      ffSkill = static_cast<int>(v);
    } else if (!std::strcmp(a, "--seed")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      const unsigned long v = std::strtoul(c, &endp, 0);
      if (!endp || *endp != '\0' || v > 0xfffffffful) {
        std::fprintf(stderr, "invalid --seed: %s\n", c);
        return usage();
      }
      ffSeed = static_cast<unsigned>(v);
    } else if (!std::strcmp(a, "--script-disasm")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // .CMI path
      scriptDisasm = true;
      const char* n = value(a);   // arena/script record name
      if (!n) return usage();
      scriptDisasmName = n;
      if (i + 1 < argc && argv[i + 1][0] != '-') {  // optional raw off
        scriptDisasmOff = static_cast<std::uint32_t>(
            std::strtoul(argv[++i], nullptr, 0));
      }
    } else if (!std::strcmp(a, "--obj-script-disasm")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // .CMI path — dumps every table-0
      objScriptDisasm = true;     // object script (Phase 12A census)
    } else if (!std::strcmp(a, "--arena")) {
      const char* v = value(a);
      if (!v) return usage();
      travArena = v;
    } else if (!std::strcmp(a, "--start")) {
      for (int k = 0; k < 3; ++k) {
        const char* c = value(a);
        if (!c) return usage();
        char* endp = nullptr;
        travStart[k] = static_cast<float>(std::strtod(c, &endp));
        if (!endp || *endp != '\0') {
          std::fprintf(stderr, "invalid --start coordinate: %s\n", c);
          return usage();
        }
      }
      travStartGiven = true;
    } else if (!std::strcmp(a, "--yaw")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      travYaw = static_cast<float>(std::strtod(c, &endp));
      if (!endp || *endp != '\0') {
        std::fprintf(stderr, "invalid --yaw: %s\n", c);
        return usage();
      }
      travYawGiven = true;
    } else if (!std::strcmp(a, "--script-pc")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      const long pv = std::strtol(c, &endp, 16);
      if (!endp || *endp != '\0' || pv < 1) {
        std::fprintf(stderr, "invalid --script-pc: %s\n", c);
        return usage();
      }
      travScriptPc = static_cast<std::uint32_t>(pv);
    } else if (!std::strcmp(a, "--frames")) {
      const char* c = value(a);
      if (!c) return usage();
      char* endp = nullptr;
      const long fv = std::strtol(c, &endp, 10);
      if (!endp || *endp != '\0' || fv < 1 || fv > 100000) {
        std::fprintf(stderr, "invalid --frames: %s\n", c);
        return usage();
      }
      travFrames = static_cast<int>(fv);
    } else if (!std::strcmp(a, "--selftest")) {
      return selftest();
    } else if (!std::strcmp(a, "--selftest-player-surface")) {
      return selftestPlayerSurface();
    } else if (!std::strcmp(a, "--selftest-camera-pose")) {
      return selftestCameraPose();
    } else if (!std::strcmp(a, "--selftest-camera-obstruction")) {
      return selftestCameraObstruction();
    } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
      return usage();
    } else if (a[0] == '-') {
      std::fprintf(stderr, "unknown argument: %s\n", a);
      return usage();
    } else {
      target = a;
    }
  }

  // --frontend-script: Phase 18A deterministic scripted-input
  // diagnostic over the frontend shell. --data-path is optional:
  // without it the SAVES fixtures are synthetic.
  if (frontendScriptMode) {
    return frontendScript(dataPath);
  }

  // --campaign-sequence: Phase 14A diagnostic. No <relative-path>
  // target — it iterates the whole 0x4999e8 table. Prints the static
  // campaign table (id → dir, freefall/mode-7/terminal flags, data
  // presence) then drives one ProgressionSession deterministically
  // through new-game → freefall legs → traversal completions → mode 5
  // → mode 6 advances → the id-4→5 literal store → mode 7 → mode 8
  // terminal → frontend, loading real traversal data where present.
  if (campaignSequence) {
    if (!dataPath) return usage();
    std::string err;
    const auto root = mdk::DataRoot::open(*dataPath, &err);
    if (!root) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 2;
    }

    int tableCount = 0;
    const auto* table = mdk::progressionCampaignTable(tableCount);
    std::printf("campaign: 0x4999e8 id->dir table, %d entries\n",
                tableCount);
    std::printf("%3s %3s  %-30s %-8s %-5s %-8s %s\n", "id", "dir",
                "traversal-path", "freefall", "mode7", "terminal",
                "data");

    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        digest ^= (v >> (i * 8)) & 0xff;
        digest *= 1099511628211ull;
      }
    };

    bool present[8] = {};
    for (int i = 0; i < tableCount; ++i) {
      const auto& e = table[i];
      char base[64];
      std::snprintf(base, sizeof base, "TRAVERSE/LEVEL%d/LEVEL%d",
                    e.dir, e.dir);
      const std::string dtiPath = std::string(base) + ".DTI";
      // Presence probe: the DTI must resolve AND parse-load cleanly
      // to count as usable data.
      const auto sz = root->fileSize(dtiPath, &err);
      present[i] = sz.has_value();
      std::printf("%3d %3d  %-30s %-8s %-5s %-8s %s\n", e.levelId,
                  e.dir, dtiPath.c_str(), e.freefallFirst ? "yes" : "no",
                  e.viaMode7 ? "yes" : "no", e.terminal ? "yes" : "no",
                  present[i] ? "present" : "ABSENT");
      mix(static_cast<std::uint64_t>(e.dir));
      mix(present[i] ? 1 : 0);
    }

    // Deterministic transition simulation. Freefall legs use a
    // completed-course runtime (the proven 13B boundary) so the real
    // traversal load runs per level where data exists.
    std::printf("\nsequence:\n");
    mdk::ProgressionSession sess;
    mdk::progressionStartCampaign(sess, /*skill=*/1);
    std::printf("  new-game: mode=%d sub=%d id=%d health=%d\n",
                sess.mode, sess.loaderSub, sess.levelId, sess.health);
    mix(static_cast<std::uint64_t>(sess.mode));

    auto reportLoad = [&]() {
      mdk::TraversalRuntime trav;
      const auto le = mdk::progressionLoadTraversalForCurrentLevel(
          *root, sess, trav, &err);
      std::printf("    load: err=%s", mdk::progressionErrorName(le));
      if (le == mdk::ProgressionError::kOk)
        std::printf(" arenas=%zu spawn=%d pos=(%.1f,%.1f,%.1f) "
                    "yaw=%.1f\n",
                    trav.arenas.size(),
                    trav.cur ? trav.cur->index : -1,
                    (double)trav.cs.pos[0], (double)trav.cs.pos[1],
                    (double)trav.cs.pos[2], (double)trav.motion.yawDeg);
      else
        std::printf(" (%s)\n", err.c_str());
      mix(static_cast<std::uint64_t>(le));
    };

    int guard = 64;
    int rc = 0;
    while (guard-- > 0) {
      if (sess.terminalDone) break;
      switch (sess.mode) {
        case 6: {
          const auto e = mdk::progressionStepLoader(sess, true);
          std::printf("  mode6:  sub-done -> mode=%d id=%d sub=%d "
                      "health=%d (%s)\n",
                      sess.mode, sess.levelId, sess.loaderSub,
                      sess.health, mdk::progressionErrorName(e));
          mix(static_cast<std::uint64_t>(sess.levelId));
          if (e == mdk::ProgressionError::kBadMode) rc = 1;
          break;
        }
        case 2: {
          mdk::FreefallRuntime ff;
          mdk::FreefallCourseData fc;
          fc.course = sess.levelId;
          fc.skill = sess.skill;
          mdk::freefallInit(ff, fc, ffSeed);
          ff.finished = true;
          ff.phase = mdk::FreefallRuntime::Phase::kDone;
          ff.health = 80;
          mdk::TraversalRuntime trav;
          mdk::ProgressionHandoff ho;
          const auto e = mdk::progressionFreefallHandoff(
              *root, sess, ff, &trav, ho, &err);
          std::printf("  fall%d:  -> mode=%d route=%d dir=%d "
                      "load=%s arenas=%zu\n",
                      sess.levelId, sess.mode,
                      static_cast<int>(ho.route), ho.traversalDir,
                      mdk::traversalLoadErrorName(ho.loadError),
                      trav.arenas.size());
          mix(static_cast<std::uint64_t>(ho.traversalDir < 0
                                             ? 0xffff
                                             : ho.traversalDir));
          if (e != mdk::ProgressionError::kOk ||
              ho.route != mdk::ProgressionRoute::kTraversal)
            rc = 1;
          break;
        }
        case 3: {
          // Traversal leg: if this is the terminal id the script's
          // 0x51 arm routes to mode 8; otherwise the victory edge.
          if (sess.levelId == 5) {
            const auto e = mdk::progressionEnterCinematic(sess);
            std::printf("  trav5:  opcode 0x51 -> mode=%d (%s)\n",
                        sess.mode, mdk::progressionErrorName(e));
          } else {
            const auto e = mdk::progressionTraversalComplete(sess);
            std::printf("  trav%d:  victory -> mode=%d (%s)\n",
                        sess.levelId, sess.mode,
                        mdk::progressionErrorName(e));
          }
          mix(static_cast<std::uint64_t>(sess.mode));
          break;
        }
        case 5: {
          const auto e = mdk::progressionStepIntermission(sess, true);
          std::printf("  mode5:  tally-done -> mode=%d id=%d (%s)\n",
                      sess.mode, sess.levelId,
                      mdk::progressionErrorName(e));
          mix(static_cast<std::uint64_t>(sess.levelId));
          break;
        }
        case 7: {
          const auto e = mdk::progressionStepMode7(sess);
          std::printf("  mode7:  -> mode=%d id=%d (%s)\n", sess.mode,
                      sess.levelId, mdk::progressionErrorName(e));
          reportLoad();
          break;
        }
        case 8: {
          const auto e = mdk::progressionStepCinematic(sess, true);
          std::printf("  mode8:  cinematic-done -> mode=%d "
                      "terminal=%d (%s)\n",
                      sess.mode, sess.terminalDone ? 1 : 0,
                      mdk::progressionErrorName(e));
          break;
        }
        default:
          std::printf("  stop:   unexpected mode=%d\n", sess.mode);
          rc = 1;
          guard = 0;
          break;
      }
    }
    std::printf("final:   mode=%d id=%d transitions=%d terminal=%d\n",
                sess.mode, sess.levelId, sess.transitionCount,
                sess.terminalDone ? 1 : 0);
    std::printf("digest:  %016llx\n", (unsigned long long)digest);
    return rc;
  }

  // --save-info: Phase 14B — parse a .SAV through the
  // original-compatible reader and print the envelope, packet table,
  // and every gameplay-authoritative field with PROVEN semantics.
  // Works on a direct file path (saves are runtime files — never
  // resolved through the read-only data root).
  if (saveInfoPath) {
    mdk::SaveGame sg;
    const auto e = mdk::saveGameLoadFile(*saveInfoPath, sg);
    std::printf("file:      %s\n", saveInfoPath->c_str());
    std::printf("parse:     %s\n", mdk::saveErrorName(e));
    if (e != mdk::SaveError::kOk) return 1;
    std::printf("envelope:  size=%u checksum=0x%06x seed=0x%04x\n",
                sg.fileSize, sg.checksum, sg.seed);
    std::printf("shape:     %s\n",
                sg.headerOnly ? "header-only (mode field < 1000)"
                              : "full (mode field >= 1000)");
    std::printf("packets:   %zu\n", sg.packets.size());
    for (const auto& p : sg.packets) {
      const auto* spec = mdk::savePacketSpec(p.tag);
      char tag[5] = {char(p.tag), char(p.tag >> 8), char(p.tag >> 16),
                     char(p.tag >> 24), 0};
      std::printf("  @%-6u %-4s size=%-5u registry=%u%s\n",
                  p.streamOffset, tag, (unsigned)p.payload.size(),
                  spec ? spec->size : 0,
                  spec && spec->size != p.payload.size() ? " !SIZE" : "");
    }
    const auto& g = sg.game;
    std::printf("game:      modeField=%d mode=%d levelId=%d health=%d "
                "deathCount=%d field54163b=%d field8=0x%x\n",
                g.modeField, g.mode(), g.levelId, g.health,
                g.deathCount, g.field54163b, (unsigned)g.field8);
    // Proven PLAY fields — health at +0, ammo block 0x54161f..33 at
    // +0xcb, weapon indicators 0x541618..1b at +0xc4, 0x54163b +0xe7.
    if (const auto* play = sg.find(mdk::saveTag('P', 'L', 'A', 'Y'))) {
      const auto* d = play->payload.data();
      auto r = [&](std::size_t o) {
        return int(std::uint32_t(std::uint8_t(d[o])) |
                   (std::uint32_t(std::uint8_t(d[o + 1])) << 8) |
                   (std::uint32_t(std::uint8_t(d[o + 2])) << 16) |
                   (std::uint32_t(std::uint8_t(d[o + 3])) << 24));
      };
      std::printf("play:      health=%d ammo=[%d %d %d %d %d %d] "
                  "wpn-ind=[%d %d %d %d] field63b=%d\n",
                  r(0), r(0xcb), r(0xcf), r(0xd3), r(0xd7), r(0xdb),
                  r(0xdf), int(std::uint8_t(d[0xc4])),
                  int(std::uint8_t(d[0xc5])), int(std::uint8_t(d[0xc6])),
                  int(std::uint8_t(d[0xc7])), r(0xe7));
    }
    // DAMP +0x00 = the 0x540bfc player position; CAME +0x00 = camera.
    auto f32 = [](const std::byte* p) {
      std::uint32_t u = std::uint32_t(std::uint8_t(p[0])) |
                        (std::uint32_t(std::uint8_t(p[1])) << 8) |
                        (std::uint32_t(std::uint8_t(p[2])) << 16) |
                        (std::uint32_t(std::uint8_t(p[3])) << 24);
      float f;
      std::memcpy(&f, &u, 4);
      return double(f);
    };
    if (const auto* damp = sg.find(mdk::saveTag('D', 'A', 'M', 'P')))
      std::printf("damp:      pos=(%.2f,%.2f,%.2f)\n",
                  f32(damp->payload.data()), f32(damp->payload.data() + 4),
                  f32(damp->payload.data() + 8));
    if (const auto* came = sg.find(mdk::saveTag('C', 'A', 'M', 'E')))
      std::printf("came:      pos=(%.2f,%.2f,%.2f)\n",
                  f32(came->payload.data()), f32(came->payload.data() + 4),
                  f32(came->payload.data() + 8));
    for (const auto& p : sg.all(mdk::saveTag('A', 'R', 'E', 'N'))) {
      mdk::SaveArenaHead h;
      if (mdk::SaveGame::arenaHead(p, h))
        std::printf("aren:      name=%-8s objects=%d fans=%d\n", h.name,
                    h.objectCount, h.fanCount);
    }
    std::printf("alie:      %zu  fand: %zu  bull: %zu\n",
                sg.all(mdk::saveTag('A', 'L', 'I', 'E')).size(),
                sg.all(mdk::saveTag('F', 'A', 'N', 'D')).size(),
                sg.all(mdk::saveTag('B', 'U', 'L', 'L')).size());
    return 0;
  }

  // --save-roundtrip: parse, re-emit the header-only GAME form, parse
  // again, and compare every authoritative field. Also verifies the
  // checksum over a re-patched copy of the input.
  if (saveRoundtripPath) {
    mdk::SaveGame sg;
    const auto e = mdk::saveGameLoadFile(*saveRoundtripPath, sg);
    if (e != mdk::SaveError::kOk) {
      std::fprintf(stderr, "parse: %s\n", mdk::saveErrorName(e));
      return 1;
    }
    mdk::SaveWriteInput in;
    in.modeField = sg.game.mode();
    in.levelId = sg.game.levelId;
    in.health = sg.game.health;
    in.deathCount = sg.game.deathCount;
    in.field54163b = sg.game.field54163b;
    in.seed = sg.seed;
    const auto bytes = mdk::saveGameWriteHeaderOnly(in);
    mdk::SaveGame rt;
    const auto e2 = mdk::saveGameParse(bytes.data(), bytes.size(), rt);
    if (e2 != mdk::SaveError::kOk) {
      std::fprintf(stderr, "re-parse: %s\n", mdk::saveErrorName(e2));
      return 1;
    }
    // The writer floors health < 0x65 to 100 — compare against the
    // stored (floored) expectation, not the raw input.
    const int expectHealth = sg.game.health < 101 ? 100 : sg.game.health;
    const int expectMode = (sg.game.mode() == 6) ? 6 : 3;
    const bool ok =
        rt.game.mode() == expectMode && rt.game.levelId == sg.game.levelId &&
        rt.game.health == expectHealth &&
        rt.game.deathCount == sg.game.deathCount &&
        rt.game.field54163b == sg.game.field54163b;
    std::printf("roundtrip: %s  mode=%d levelId=%d health=%d "
                "deaths=%d x63b=%d  (%zu -> %zu bytes, %zu -> %zu "
                "packets)\n",
                ok ? "OK" : "MISMATCH", rt.game.mode(), rt.game.levelId,
                rt.game.health, rt.game.deathCount, rt.game.field54163b,
                (std::size_t)sg.fileSize, bytes.size(),
                sg.packets.size(),
                rt.packets.size());
    return ok ? 0 : 1;
  }

  // --save-restore: the Phase 14C full-save path (FUN_00427218) —
  // parses, rebuilds the level through the suppressed-spawn load,
  // applies every packet family, resolves the reference tables, and
  // steps the restored runtime for --frames frames.
  if (saveRestorePath) {
    mdk::SaveGame sg;
    const auto e = mdk::saveGameLoadFile(*saveRestorePath, sg);
    if (e != mdk::SaveError::kOk) {
      std::fprintf(stderr, "parse: %s\n", mdk::saveErrorName(e));
      return 1;
    }
    if (!dataPath) {
      std::fprintf(stderr, "--save-restore needs --data-path\n");
      return usage();
    }
    std::string err;
    const auto root = mdk::DataRoot::open(*dataPath, &err);
    if (!root) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 2;
    }
    mdk::ProgressionSession sess;
    mdk::TraversalRuntime rt;
    mdk::FullRestoreReport rep;
    const auto re = mdk::applyFullSaveToTraversal(sg, *root, sess, rt,
                                                  &rep, &err);
    std::printf("file:      %s\n", saveRestorePath->c_str());
    std::printf("restore:   %s\n", mdk::saveErrorName(re));
    if (re != mdk::SaveError::kOk) {
      std::fprintf(stderr, "detail:  %s\n", err.c_str());
      return 1;
    }
    std::printf("game:      modeField=%d mode=%d levelId=%d identity=%s\n",
                rep.modeField, rep.mode, rep.levelId,
                rep.identityOk ? "ok" : "MISMATCH");
    std::printf("packets:   aren=%d/%d applied, alie=%d (%d scripted), "
                "fand=%d, bull=%d (%d active)\n",
                rep.arenPackets, rep.arenApplied, rep.objectsAllocated,
                rep.scriptedObjects, rep.fansAllocated, rep.shotSlots,
                rep.shotsActive);
    std::printf("refs:      arena=%d/%d obj=%d/%d cmi=%d/%d "
                "(resolved/failed)\n",
                rep.arenaRefsResolved, rep.arenaRefsFailed,
                rep.objectRefsResolved, rep.objectRefsFailed,
                rep.cmiRefsResolved, rep.cmiRefsFailed);
    std::printf("arenas:    cur=%d partner=%d load=%d\n",
                rep.curArenaIndex, rep.partnerArenaIndex,
                rep.loadArenaIndex);
    std::printf("player:    pos=(%.2f,%.2f,%.2f) yaw=%.1f health=%d\n",
                (double)rep.playerPos[0], (double)rep.playerPos[1],
                (double)rep.playerPos[2], (double)rep.playerYawDeg,
                rep.health);
    for (const auto& u : rep.unmapped)
      std::printf("unmapped:  %s +0x%02x..+0x%02x (%uB)\n", u.packet,
                  u.offset, u.offset + u.size, u.size);
    for (const auto& w : rep.warnings)
      std::printf("warning:   %s\n", w.c_str());
    for (const auto& a : rt.arenas) {
      std::size_t bound = 0;
      for (const auto& o : a->dyn.storage)
        if (o->col.elements != nullptr) ++bound;
      std::printf("  arena[%2d] %-9s objs=%2zu elems=%2zu flags44=%02x "
                  "spawned=%d latch=%d\n",
                  a->index, a->name.c_str(), a->dyn.storage.size(), bound,
                  a->flags44, a->objectsSpawned ? 1 : 0,
                  a->eventLatch.col.elements != nullptr ? 1 : 0);
    }

    // --save-activate N: run the dormant-arena activation path on
    // restored arena N — the FUN_00432c34 route the loader's attach
    // tail takes for a stream-loaded arena (geometry ensure + the
    // FUN_004321dc/FUN_0045a3b0 element-set rebind on named objects),
    // then FUN_00432d9c partner attach so the frame pass iterates its
    // objects (script/path/anim ticks). Exercises restored arenas the
    // save never had active (e.g. the six +0x114 objects' arenas).
    if (saveActivateIdx >= 0) {
      if (static_cast<std::size_t>(saveActivateIdx) >=
          rt.arenas.size()) {
        std::fprintf(stderr, "--save-activate %d: only %zu arenas\n",
                     saveActivateIdx, rt.arenas.size());
        return 1;
      }
      mdk::TraversalArena& a = *rt.arenas[saveActivateIdx];
      mdk::traversalEnsureLoaded(rt, a);
      for (auto& o : a.dyn.storage)
        if (o->col.named) mdk::objectArenaActivate(rt, *o);
      mdk::traversalAttachPartner(rt, a);
      std::printf("activate:  arena[%d] %-9s partner=%d\n", a.index,
                  a.name.c_str(),
                  rt.partner == &a ? 1 : 0);
      for (auto& o : a.dyn.storage) {
        std::printf("  obj hp=%d named=%d elem=%d anim=%d pc=%d "
                    "wait=%.3f pos=(%.1f,%.1f,%.1f)\n",
                    o->health, o->col.named ? 1 : 0,
                    o->col.elements != nullptr ? 1 : 0,
                    o->animRec != nullptr ? 1 : 0,
                    o->field108 != nullptr ? 1 : 0,
                    (double)o->field22c, (double)o->pos[0],
                    (double)o->pos[1], (double)o->pos[2]);
      }
    }

    // Step the restored world — the same scripted input as
    // --traversal-runtime (idle -> KeyUp -> idle -> KeyJump).
    const mdk::GameplayInputBindings bindings;
    auto rawFor = [](int phase) {
      mdk::RawGameplayInput r;
      if (phase == 1) r.keyLevel[103 >> 5] |= 1u << (103 & 31);
      if (phase == 3) r.keyLevel[56 >> 5] |= 1u << (56 & 31);
      return r;
    };
    mdk::FrontendTimingState timing;
    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        digest ^= (v >> (i * 8)) & 0xff;
        digest *= 1099511628211ull;
      }
    };
    // Post-restore state digest (before frame 0) — kept on a separate
    // accumulator so the frame digest's golden values are unchanged.
    {
      std::uint64_t sd = 1469598103934665603ull;
      auto mixS = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
          sd ^= (v >> (i * 8)) & 0xff;
          sd *= 1099511628211ull;
        }
      };
      for (int i = 0; i < 3; ++i) {
        std::uint32_t u;
        std::memcpy(&u, &rt.cs.pos[i], 4);
        mixS(u);
      }
      mixS(static_cast<std::uint32_t>(rep.curArenaIndex));
      mixS(static_cast<std::uint32_t>(rep.partnerArenaIndex));
      mixS(static_cast<std::uint32_t>(rt.fieldHealth));
      mixS(static_cast<std::uint32_t>(rt.scriptGFlags));
      mixS(static_cast<std::uint32_t>(rt.inventoryCount));
      std::printf("digest0:   %016llx  (post-restore)\n",
                  (unsigned long long)sd);
    }
    // Phase 15A — restored-fight continuation: `--hit` specs fire
    // through the same per-frame injection the --traversal-runtime
    // path uses, so a mid-fight snapshot can be driven to its
    // completion latch after the restore.
    bool sawEndLevel = false;
    bool sawEnding = false;
    // MDK_IDLE_INPUT=1 — QA-only opt-out of the scripted phase
    // schedule (genuinely idle frames; needed to test spawn-adjacent
    // scripted triggers the default schedule walks the player off).
    const bool idleInput = std::getenv("MDK_IDLE_INPUT") != nullptr;
    for (int f = 0; f < travFrames; ++f) {
      const int phase =
          idleInput ? 0
                    : f < 10 ? 0 : f < 30 ? 1 : f < 35 ? 0 : f < 50 ? 3 : 0;
      bool elemShotFired = false;
      fireCombatHits(rt, hitSpecs, elemShotFired, f);
      auto raw = rawFor(phase);
      for (const auto& h : holdSpecs)
        if (f >= h.first && f <= h.last)
          raw.keyLevel[h.key >> 5] |= 1u << (h.key & 31);
      const auto out =
          mdk::stepTraversalRuntime(rt, raw, bindings, timing);
      sawEndLevel = sawEndLevel || out.endLevelRequested;
      sawEnding = sawEnding || out.endingRequested;
      std::printf("f=%03d a=%d p=%d pos=(%8.2f,%8.2f,%8.2f) yaw=%6.1f "
                  "vv=%6.2f gnd=%d ctc=%08x\n",
                  out.frame, out.curArenaIndex, out.partnerArenaIndex,
                  (double)out.pos[0], (double)out.pos[1],
                  (double)out.pos[2], (double)out.yawDeg,
                  (double)out.vertVel, out.grounded ? 1 : 0,
                  out.contactObj);
      for (int i = 0; i < 3; ++i) {
        std::uint32_t u;
        std::memcpy(&u, &out.pos[i], 4);
        mix(u);
      }
      mix(static_cast<std::uint32_t>(out.curArenaIndex));
      mix(static_cast<std::uint32_t>(out.grounded ? 1 : 0));
      mix(static_cast<std::uint32_t>(rt.scriptInsnTotal));
      if (f == 0)
        std::printf("digest1:   %016llx  (1 frame)\n",
                    (unsigned long long)digest);
    }
    std::printf("digest:    %016llx  (%d frames)  insn=%d gfl=%08x\n",
                (unsigned long long)digest, travFrames,
                rt.scriptInsnTotal, rt.scriptGFlags);
    // Phase 15A — the restored arena's object state + completion
    // latches, printed whenever the fight was exercised (same fields
    // as the --traversal-runtime boss summary).
    if (!hitSpecs.empty() || sawEndLevel || sawEnding) {
      std::printf("boss-runtime:\n");
      for (const auto& ap : rt.arenas) {
        if (!ap->dyn.storage.empty())
          std::printf("  [arena %s storage=%zu vars48=(%.2f,%.2f,%.2f,"
                      "%.2f) gVars=(%.2f,%.2f,%.2f,%.2f) flags58=%08x]\n",
                      ap->name.c_str(), ap->dyn.storage.size(),
                      ap->objVars48[0], ap->objVars48[1],
                      ap->objVars48[2], ap->objVars48[3],
                      rt.scriptGVars[0], rt.scriptGVars[1],
                      rt.scriptGVars[2], rt.scriptGVars[3],
                      ap->flags58);
        for (const auto& up : ap->dyn.storage) {
          const mdk::DynamicObject& o = *up;
          if ((o.col.flags148 & 0x20) != 0) continue;
          std::printf("  %s model=%s arena=%s hp=%d sub=%02x "
                      "pc=%p f230=%p f148=%04x dead=%d L=[%s]\n",
                      o.scriptClass.c_str(), o.model.modelName().c_str(),
                      ap->name.c_str(), o.health, (unsigned)o.field11e,
                      o.field108, o.field230,
                      (unsigned)o.col.flags148,
                      (o.col.flags148 & 0x20) != 0 ? 1 : 0,
                      [&] {
                        std::string s;
                        for (int k = 0; k < 4; ++k) {
                          char b[24];
                          std::snprintf(b, sizeof b, "%s%.3g",
                                        k ? "," : "",
                                        (double)o.scriptLocals[k]);
                          s += b;
                        }
                        return s;
                      }().c_str());
        }
      }
      for (const auto& r : mdk::traversalSpawnLog())
        std::printf("  spawn %s name=%s arena=%s v%d pc=0x%x by=%s\n",
                    r.cls.c_str(), r.name.c_str(), r.arena.c_str(),
                    r.variant, r.spawnPc, r.spawner.c_str());
      std::printf(
          "  completion: endLevel=%d ending=%d vsnapPending=%d "
          "vsnaps=%d objDeathCalls=%d shotHits=%d\n",
          sawEndLevel ? 1 : 0, sawEnding ? 1 : 0,
          rt.pendingViewSnap == -1 ? 1 : 0,
          rt.seams.pendingViewSnaps, rt.seams.objectDeathCalls,
          rt.shotHitCount);
    }

    // --save-write-full: Phase 14D — re-emit the (possibly stepped)
    // runtime as a full-save stream, then verify it end-to-end
    // through the native parser + full-restore path. Never writes
    // over the input file.
    if (saveWriteFullPath) {
      if (*saveWriteFullPath == *saveRestorePath) {
        std::fprintf(stderr, "--save-write-full refuses to overwrite "
                             "the input save\n");
        return 1;
      }
      mdk::SaveWriteFullInput win;
      win.seed = static_cast<std::uint16_t>(ffSeed & 0xffff);
      mdk::FullWriteReport wrep;
      std::string werr;
      const auto bytes =
          mdk::saveGameWriteFull(rt, sess, win, &wrep, &werr);
      if (bytes.empty()) {
        std::fprintf(stderr, "write-full: %s\n", werr.c_str());
        return 1;
      }
      {
        FILE* f = std::fopen(saveWriteFullPath->c_str(), "wb");
        if (!f) {
          std::fprintf(stderr, "write-full: cannot open %s\n",
                       saveWriteFullPath->c_str());
          return 1;
        }
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
      }
      std::printf("write-full: %zuB -> %s (aren=%d obj=%d fand=%d "
                  "ids=%d seed=%04x)\n", bytes.size(),
                  saveWriteFullPath->c_str(), wrep.arenasWritten,
                  wrep.objectsWritten, wrep.fansWritten, wrep.saveIds,
                  (unsigned)win.seed);
      for (const auto& w : wrep.warnings)
        std::printf("write-warn: %s\n", w.c_str());
      // Verify: parse -> restore -> key-state equivalence.
      mdk::SaveGame sg2;
      const auto pe = mdk::saveGameParse(bytes.data(), bytes.size(),
                                         sg2);
      std::printf("reparse:   %s\n", mdk::saveErrorName(pe));
      if (pe != mdk::SaveError::kOk) return 1;
      mdk::ProgressionSession sess2;
      mdk::TraversalRuntime rt2;
      mdk::FullRestoreReport rep2;
      const auto re2 = mdk::applyFullSaveToTraversal(sg2, *root, sess2,
                                                     rt2, &rep2, &err);
      std::printf("rerestore: %s  (arena-refs %d/%d obj %d/%d "
                  "cmi %d/%d)\n", mdk::saveErrorName(re2),
                  rep2.arenaRefsResolved, rep2.arenaRefsFailed,
                  rep2.objectRefsResolved, rep2.objectRefsFailed,
                  rep2.cmiRefsResolved, rep2.cmiRefsFailed);
      for (const auto& w : rep2.warnings)
        std::printf("warn2:     %s\n", w.c_str());
      if (re2 != mdk::SaveError::kOk) return 1;
      const bool eq =
          rt2.cs.pos[0] == rt.cs.pos[0] &&
          rt2.cs.pos[1] == rt.cs.pos[1] &&
          rt2.cs.pos[2] == rt.cs.pos[2] &&
          rt2.motion.yawDeg == rt.motion.yawDeg &&
          rt2.fieldHealth == rt.fieldHealth &&
          rt2.scriptGFlags == rt.scriptGFlags &&
          rt2.cur == rt2.arenas[rep.curArenaIndex].get() &&
          rt2.shots[0].state == rt.shots[0].state &&
          sess2.deathCount == sess.deathCount;
      std::printf("equiv:     %s (pos/yaw/health/gflags/arena/shots/"
                  "deaths)\n", eq ? "OK" : "MISMATCH");
      return eq ? 0 : 1;
    }
    return 0;
  }

  // --stream-init: Phase 19A.2A mode-5 init diagnostic. Loads the
  // proven STREAM/STREAM.{BNI,MTI} + MISC/MDKFONT.FTI record set, binds
  // StreamAssets by the OBSERVED record names (FUN_0042b270), runs
  // StreamScene::init, and prints the post-init state: resource-bind
  // status, pool/bucket occupancy, window bounds, actor identities,
  // counter seeds, camera state, fade/completion flags and the seam
  // counters. No rendering, no script execution.
  if (streamInit) {
    if (!dataPath) return usage();
    std::string err;
    const auto root = mdk::DataRoot::open(*dataPath, &err);
    if (!root) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 2;
    }
    const auto bni = root->readFile("STREAM/STREAM.BNI",
                                    kEntriesMaxBytes, &err);
    const auto mti = root->readFile("STREAM/STREAM.MTI",
                                    kEntriesMaxBytes, &err);
    const auto fti = root->readFile("MISC/MDKFONT.FTI",
                                    kEntriesMaxBytes, &err);
    std::printf("stream-init: course=%d skill=%d seed=%08x health=100\n",
                ffCourse, ffSkill, ffSeed);
    std::printf("files:       STREAM.BNI=%s  STREAM.MTI=%s  "
                "MDKFONT.FTI=%s\n",
                bni ? "ok" : "MISSING", mti ? "ok" : "MISSING",
                fti ? "ok" : "MISSING");
    if (!bni) {
      std::fprintf(stderr, "read: %s\n", err.c_str());
      return 1;
    }
    const auto dir = mdk::inspectBniDirectory(
        std::span<const std::byte>(bni->data(), bni->size()));
    if (dir.status != mdk::BniDirectoryStatus::kOk) {
      std::fprintf(stderr, "bni: %s — %s\n",
                   std::string(mdk::bniDirectoryStatusName(dir.status))
                       .c_str(),
                   dir.detail.c_str());
      return 1;
    }
    auto tagOf = [&](const char* name) -> int {
      const mdk::BniRecord* r = mdk::findBniRecord(dir, name);
      return r ? static_cast<int>(r - dir.records.data()) : -1;
    };
    auto payloadOf = [&](const char* name) -> const std::byte* {
      const mdk::BniRecord* r = mdk::findBniRecord(dir, name);
      return r ? bni->data() + r->payloadFileOffset : nullptr;
    };
    auto payloadEnd = [&](const char* name) -> const std::byte* {
      const mdk::BniRecord* r = mdk::findBniRecord(dir, name);
      return r ? bni->data() + r->payloadEnd : nullptr;
    };
    mdk::StreamAssets a{};
    const std::byte* pal = payloadOf("PAL");
    a.palettePal = pal ? reinterpret_cast<const std::uint8_t*>(pal) +
                           mdk::kStreamPaletteTailOffset
                       : nullptr;
    if (fti) {
      const auto fdir = mdk::inspectFtiDirectory(
          std::span<const std::byte>(fti->data(), fti->size()));
      if (fdir.status == mdk::FtiDirectoryStatus::kOk) {
        if (const mdk::FtiRecord* sp =
                mdk::findFtiRecord(fdir, mdk::kStreamSystemRecord))
          a.paletteGlobal = reinterpret_cast<const std::uint8_t*>(
              fti->data() + sp->payloadFileOffset);
      }
    }
    a.bgTag = tagOf("BG");
    const int planet = tagOf("PLANET");
    for (int i = 0; i != 4; ++i) a.planetTag[i] = planet;  // sub-images
                                                         // not decomposed
    a.lightTag = tagOf("LIGHT");
    a.sndWind = tagOf("WIND");
    a.sndHitside = tagOf("HITSIDE");
    a.sndRescue = tagOf("RESCUE");
    a.sndApple = tagOf("APPLE");
    for (int i = 0; i != 7; ++i) {
      char nm[8];
      std::snprintf(nm, sizeof nm, "HURT%d", i + 1);
      a.sndHurt[i] = tagOf(nm);
    }
    std::optional<mdk::RuntimeModel> pKurt, pBones, pProf, pEsc;
    int protoAbsent = 0, protoParseFail = 0;
    auto bindProto = [&](const char* name,
                         std::optional<mdk::RuntimeModel>& out) {
      const std::byte* p = payloadOf(name);
      if (!p) {
        ++protoAbsent;
        return;
      }
      // BNI payloads omit the record's leading flag word — the
      // original passes it as FUN_00428400's EDX arg (flag=1 for the
      // named-element stream records; OBSERVED at the FUN_0042b270
      // call sites). Re-head for the shared parser, same convention
      // as traversalShotModel.
      const std::byte* pe = payloadEnd(name);
      std::vector<std::uint8_t> headed(
          4 + static_cast<std::size_t>(pe - p));
      const std::uint8_t fl[4] = {1, 0, 0, 0};
      std::memcpy(headed.data(), fl, 4);
      std::memcpy(headed.data() + 4, p,
                  static_cast<std::size_t>(pe - p));
      out = mdk::parseGeometryRecord(headed.data(),
                                     headed.data() + headed.size());
      if (!out) ++protoParseFail;
    };
    bindProto("KURT", pKurt);
    bindProto("BONES", pBones);
    bindProto("PROFSHIP", pProf);
    const bool isFinal = ffCourse >= 4;
    bindProto(isFinal ? "GUNTA" : "SWH150", pEsc);
    a.protoKurt = pKurt ? &*pKurt : nullptr;
    a.protoBones = pBones ? &*pBones : nullptr;
    a.protoProfship = pProf ? &*pProf : nullptr;
    a.protoEscort = pEsc ? &*pEsc : nullptr;
    a.animEscort = reinterpret_cast<const std::uint8_t*>(
        payloadOf(isFinal ? "GUNTANIM" : "SWHANM"));
    a.animBones = reinterpret_cast<const std::uint8_t*>(
        payloadOf("BONESANIM"));
    a.animKurt = reinterpret_cast<const std::uint8_t*>(
        payloadOf("KURTANIM"));
    a.animHvr = reinterpret_cast<const std::uint8_t*>(
        payloadOf("FL_HVR"));
    a.animWave = reinterpret_cast<const std::uint8_t*>(
        payloadOf("FL_WAVE"));
    // The bound the ObjectAnimView walk is checked against — all the
    // animation records live in the BNI image, so the file end is the
    // faithful upper limit (matches the native's unrestricted in-image
    // reach while keeping a malformed record inside the file).
    a.animLimit = reinterpret_cast<const std::uint8_t*>(
        bni->data() + bni->size());
    auto yesno = [](const void* p) { return p ? "ok" : "MISSING"; };
    std::printf("binds:       PAL=%s SYS_PAL=%s BG=%s PLANET=%s "
                "LIGHT=%s\n",
                yesno(a.palettePal), yesno(a.paletteGlobal),
                a.bgTag >= 0 ? "ok" : "MISSING",
                planet >= 0 ? "ok" : "MISSING",
                a.lightTag >= 0 ? "ok" : "MISSING");
    int hurtBound = 0;
    for (int i = 0; i != 7; ++i) hurtBound += a.sndHurt[i] >= 0;
    std::printf("             WIND=%s HITSIDE=%s RESCUE=%s APPLE=%s "
                "HURT=%d/7\n",
                a.sndWind >= 0 ? "ok" : "MISSING",
                a.sndHitside >= 0 ? "ok" : "MISSING",
                a.sndRescue >= 0 ? "ok" : "MISSING",
                a.sndApple >= 0 ? "ok" : "MISSING", hurtBound);
    std::printf("             KURT=%s BONES=%s PROFSHIP=%s %s=%s "
                "(absent=%d parseFail=%d)\n",
                yesno(a.protoKurt), yesno(a.protoBones),
                yesno(a.protoProfship), isFinal ? "GUNTA" : "SWH150",
                yesno(a.protoEscort), protoAbsent, protoParseFail);
    std::printf("             anims %s=%s BONESANIM=%s KURTANIM=%s "
                "FL_HVR=%s FL_WAVE=%s\n",
                isFinal ? "GUNTANIM" : "SWHANM", yesno(a.animEscort),
                yesno(a.animBones), yesno(a.animKurt), yesno(a.animHvr),
                yesno(a.animWave));

    mdk::StreamScene sc;
    const bool ok = sc.init(a, ffCourse, ffSkill, ffSeed, 100);
    const mdk::StreamSnapshot s = sc.snapshot();
    std::printf("init:        %s  window=[%d,%d) radius=%.4f penT=%.3f\n",
                ok ? "ok" : "FAILED", s.winLo, s.winHi,
                (double)s.radius, (double)s.penT);
    std::printf("seeds:       driftMax=%.2f radiusMin=%.2f "
                "radiusMax=%.2f penBase=%d penTarget=%d\n",
                (double)s.driftMax, (double)s.radiusMin,
                (double)s.radiusMax, s.penBase, s.penTarget);
    std::printf("actors:      hero=%d escort=%d pickup=%d marker=%d "
                "twin=%d strayIdx=-1\n",
                s.heroIdx, s.escortIdx, s.pickupIdx, s.markerIdx,
                s.twinIdx);
    std::printf("pool:        live=%d free=%d  buckets[0]=%d [5]=%d "
                "[16]=%d errors=0\n",
                s.liveObjects, s.freeObjects, sc.poolBucketCount(0),
                sc.poolBucketCount(5), sc.poolBucketCount(16));
    std::printf("flags:       fade=%.3f complete=%d health=%d\n",
                (double)s.fade, s.complete, s.health);
    // Phase 19A.2E — counter/drain/completion surface (all diagnostic;
    // reason is the port-side first-writer tag, NOT a native field):
    //   health  = 0x541554 displayed bonus pool (drained per wall beat)
    //   fade    = 0x4eda9c frame drain accumulator
    //   complete= 0x4ed748 latch  reason = first of the 3 write sites
    //   exit    = frame fn returned 1 → dispatcher teardown
    std::printf("counter:     health=%d win=[%d,%d) fade=%.3f "
                "complete=%d reason=%s exit=%d\n",
                s.health, s.winLo, s.winHi, (double)s.fade, s.complete,
                streamCompletionName(s.completionSrc), sc.finished());
    std::printf("camera:      eye=(%.2f,%.2f,%.2f) lookT=%.2f "
                "view.diag=(%.2f,%.2f,%.2f)\n",
                (double)s.eye[0], (double)s.eye[1], (double)s.eye[2],
                (double)s.lookT, (double)s.camView[0],
                (double)s.camView[5], (double)s.camView[10]);
    const mdk::StreamSeams& sm = sc.seams();
    std::printf("seams:       bind=%d free=%d teletype=%d palRamp=%d "
                "fillSel=%d limiter=%d\n",
                sm.resourceBind, sm.resourceFree, sm.teletype,
                sm.paletteRamp, sm.fillSelect, sm.limiter);
    std::printf("events:      %zu  hash=%016llx\n",
                sc.events().size(), (unsigned long long)s.stateHash);

    // --stream-frames N: Phase 19A.2B/2C/2D bounded actor+animator+
    // teletype diagnostic. Steps the real-data scene (neutral input,
    // 1/30s) and reports, per frame, which updater families the
    // dispatch walk reached (seam deltas), the window bounds, the
    // tunnelExtend feed (fillSelect — every non-terminal extend ends
    // at ae60), the FUN_004555bc dispatch count, the per-object
    // animator trace (slot:body[rec]f/acc/latch — C classless, F fuse,
    // T fuse teardown, H hold, N null, A advance, S advance+sound),
    // the TELETYPE service trace (phase I/C/S/N/P, ring indices,
    // char/hold timers, line count, entry flags, str cursor, per-call
    // draws) and a folded state digest. Bounded: no draw emission —
    // updater + animator + queue-service reach only. The real mode-5
    // stream posts no TELETYPE text (OBSERVED), so the service runs
    // the idle arm each frame.
    if (streamFrames > 0 && ok) {
      std::printf("frames:      frame updaters winLo winHi ext anim "
                  "trace(slot:body[rec]f/acc/latch) "
                  "hero(slot,pathT,spd) tt hash "
                  "ctr(hp,fade,c:reason,exit)\n");
      mdk::StreamInput in{};
      // Synthetic 30Hz clock for the 2fb68 limiter (46c650 ms source)
      // — deterministic across runs; starts past the base==0 arm.
      in.nowMs = 1000;
      mdk::StreamSeams prev = sc.seams();
      // Phase 19A.2E — deterministic semantic digest over the
      // counter/completion state only (health, fade raw bits,
      // latch+reason, exit, window bounds). Deliberately independent
      // of stateHash so the diagnostic surface is diffable on its own.
      std::uint64_t ctrDigest = 1469598103934665603ull;
      auto mix = [&ctrDigest](std::uint64_t v) {
        for (int i = 0; i != 8; ++i) {
          ctrDigest ^= (v >> (i * 8)) & 0xff;
          ctrDigest *= 1099511628211ull;
        }
      };
      // Phase 19A.2F — run totals + the draw semantic digest (FNV-1a
      // over the per-frame event census and the post-frame draw
      // globals; deterministic across runs, diagnostic only).
      std::uint64_t drawDigest = 1469598103934665603ull;
      auto dmix = [&drawDigest](std::uint64_t v) {
        for (int i = 0; i != 8; ++i) {
          drawDigest ^= (v >> (i * 8)) & 0xff;
          drawDigest *= 1099511628211ull;
        }
      };
      // Phase 19A.2G — ribbon/trail digests: penDigest folds the
      // emitted pen scalar + packed clip flags per kRibbonTri (the
      // pen-pair stream); ribDigest folds the full pre-clip geometry
      // (9 view-space floats + pen + flags) in emit order.
      std::uint64_t penDigest = 1469598103934665603ull;
      std::uint64_t ribDigest = 1469598103934665603ull;
      auto pmix = [&penDigest](std::uint64_t v) {
        for (int i = 0; i != 8; ++i) {
          penDigest ^= (v >> (i * 8)) & 0xff;
          penDigest *= 1099511628211ull;
        }
      };
      auto rmix = [&ribDigest](std::uint64_t v) {
        for (int i = 0; i != 8; ++i) {
          ribDigest ^= (v >> (i * 8)) & 0xff;
          ribDigest *= 1099511628211ull;
        }
      };
      long evPalT = 0, evBgT = 0, evMdlT = 0, evSprT = 0, evHudT = 0,
           evPresT = 0, evTtT = 0, evExitT = 0, evRibT = 0;
      int framesRun = 0, exitFrame = -1;
      for (int f = 0; f != streamFrames; ++f) {
        const std::size_t evBase = sc.events().size();
        const bool keepGoing = sc.step(in, 1.0f / 30.0f);
        in.nowMs += 33;                             // ~30.3fps wall ms
        ++framesRun;
        const mdk::StreamSeams cur = sc.seams();
        // Phase 19A.2F — per-frame draw-event census over the new
        // events only (the queue holds the whole run).
        int evPal = 0, evBg = 0, evMdl = 0, evSpr = 0, evHud = 0,
            evPres = 0, evTt = 0, evRib = 0;
        for (std::size_t ei = evBase; ei != sc.events().size(); ++ei) {
          const mdk::StreamEvent& ev = sc.events()[ei];
          switch (ev.kind) {
            case mdk::StreamEvent::kPaletteSet:   ++evPal;  break;
            case mdk::StreamEvent::kBackdropBlit: ++evBg;   break;
            case mdk::StreamEvent::kModelDraw:    ++evMdl;  break;
            case mdk::StreamEvent::kSpriteDraw:   ++evSpr;  break;
            case mdk::StreamEvent::kHudBlit:      ++evHud;  break;
            case mdk::StreamEvent::kTeletypeDraw: ++evTt;   break;
            case mdk::StreamEvent::kPresent:      ++evPres; break;
            case mdk::StreamEvent::kExitMode:     ++evExitT; break;
            case mdk::StreamEvent::kRibbonTri:
              ++evRib;
              pmix((std::uint32_t)ev.tag |
                   ((std::uint64_t)(std::uint32_t)ev.aux << 32));
              for (int k = 0; k != 9; ++k)
                rmix(std::bit_cast<std::uint32_t>(ev.f[k]));
              rmix((std::uint32_t)ev.tag |
                   ((std::uint64_t)(std::uint32_t)ev.aux << 32));
              break;
            default: break;
          }
        }
        evPalT += evPal; evBgT += evBg; evMdlT += evMdl;
        evSprT += evSpr; evHudT += evHud; evPresT += evPres;
        evTtT += evTt; evRibT += evRib;
        dmix((std::uint32_t)evPal | ((std::uint64_t)evBg << 8) |
             ((std::uint64_t)evMdl << 16) | ((std::uint64_t)evSpr << 24) |
             ((std::uint64_t)evHud << 32) | ((std::uint64_t)evPres << 40) |
             ((std::uint64_t)evTt << 48));
        dmix((std::uint32_t)evRib);
        const mdk::StreamSnapshot dfs = sc.snapshot();
        dmix(dfs.paletteDacHash);
        dmix(((std::uint64_t)dfs.limiter[1]) |
             ((std::uint64_t)dfs.limiterBase << 32));
        std::string fam;
        if (cur.heroUpdate > prev.heroUpdate) fam += "hero ";
        if (cur.strayUpdate > prev.strayUpdate) fam += "stray ";
        if (cur.escortUpdate > prev.escortUpdate) fam += "escort ";
        if (cur.pickupUpdate > prev.pickupUpdate) fam += "pickup ";
        if (cur.genericUpdate > prev.genericUpdate) fam += "generic ";
        if (cur.twinSync > prev.twinSync) fam += "twinSync";
        // Per-object animator trace — one entry per 555bc dispatch.
        std::string tr;
        for (const mdk::StreamAnimTick& t : sc.animLog()) {
          char buf[64];
          std::snprintf(buf, sizeof buf, " %d:%c[%d]f%d/%.2f/%d",
                        t.slot, t.body, t.rec, (int)t.frame,
                        (double)t.acc, (int)t.latch);
          tr += buf;
        }
        // Phase 19A.2D — TELETYPE service trace (one record per
        // FUN_0041cb44 call; the real stream never posts).
        const mdk::StreamTtTick* tt =
            sc.ttLog().empty() ? nullptr : &sc.ttLog().back();
        char ttb[112];
        if (tt) {
          std::snprintf(
              ttb, sizeof ttb,
              " tt%c q%u/%u t4=%.3f t8=%.3f ln=%u fl=%x cur=%08x "
              "ch=%d dr=%d ov=%d",
              tt->phase, tt->qRead, tt->qWrite, (double)tt->charTimer,
              (double)tt->holdTimer, tt->curLine, tt->flags, tt->cursor,
              tt->chars, tt->draws, tt->overflow);
        } else {
          std::snprintf(ttb, sizeof ttb, " tt-");
        }
        const mdk::StreamSnapshot& fs = dfs;
        std::printf("             %5d  %-40s [%d,%d) %3d %4d%s "
                    "h(%d,%.2f,%.2f)%s %016llx "
                    "hp=%d f=%.3f c=%d:%s x=%d\n",
                    f, fam.c_str(), fs.winLo, fs.winHi,
                    cur.fillSelect - prev.fillSelect,
                    cur.animCalls - prev.animCalls, tr.c_str(),
                    fs.heroIdx,
                    (double)fs.heroPathT, (double)fs.heroSpeed, ttb,
                    (unsigned long long)fs.stateHash,
                    fs.health, (double)fs.fade, fs.complete,
                    streamCompletionName(fs.completionSrc),
                    sc.finished());
        mix((std::uint32_t)fs.health);
        std::uint32_t fadeBits;
        std::memcpy(&fadeBits, &fs.fade, 4);   // raw f32 — no rounding
        mix(fadeBits);
        mix((std::uint32_t)fs.complete |
            ((std::uint64_t)fs.completionSrc << 32) |
            ((std::uint64_t)sc.finished() << 40));
        mix(((std::uint64_t)(std::uint32_t)fs.winLo) |
            ((std::uint64_t)(std::uint32_t)fs.winHi << 32));
        // Draw-stage line (19A.2F/2G): event census, limiter record,
        // backdrop accumulators, DAC hash, gates, ribbon trims.
        // Diagnostic only — folded into nothing; stateHash stays
        // canonical.
        std::printf("                 draw: pal=%d bg=%d mdl=%d spr=%d "
                    "hud=%d pres=%d rib=%d | lim t1=%u t2=%u t3=%.4f "
                    "base=%u tgt=%u | uv=(%d,%d) dac=%016llx due=%d "
                    "blink=%d ovf=%d | rc=%d cull=%d rej=%d clip=%d\n",
                    evPal, evBg, evMdl, evSpr, evHud, evPres, evRib,
                    (unsigned)fs.limiter[1], (unsigned)fs.limiter[2],
                    (double)std::bit_cast<float>(fs.limiter[3]),
                    (unsigned)fs.limiterBase, (unsigned)fs.limiterTarget,
                    (int)fs.bgScroll[0], (int)fs.bgScroll[1],
                    (unsigned long long)fs.paletteDacHash, fs.drawDue,
                    fs.hudBlink,
                    cur.drawListOverflow - prev.drawListOverflow,
                    cur.ribbonDraw - prev.ribbonDraw,
                    cur.ribbonPlaneCull - prev.ribbonPlaneCull,
                    cur.ribbonReject - prev.ribbonReject,
                    cur.ribbonClip - prev.ribbonClip);
        if (!keepGoing && exitFrame < 0) exitFrame = f;
        if (!keepGoing) break;              // natural exit frame
        prev = cur;
      }
      const mdk::StreamSnapshot ts = sc.snapshot();
      const mdk::StreamSeams sm = sc.seams();
      std::printf("tt-digest:   hash=%016llx posts=%d svc=%d draws=%d "
                  "ovf=%d (arena FNV-1a over 0x54b7a4..0x54b834)\n",
                  (unsigned long long)ts.ttHash, sm.teletypePost,
                  sm.teletypeService, sm.teletypeDraw,
                  sm.teletypeOverflow);
      // Phase 19A.2F draw-stage run summary — the closure-gate report.
      int exitFill = -1;
      for (const auto& e : sc.events())
        if (e.kind == mdk::StreamEvent::kExitMode) exitFill = e.aux;
      std::printf("draw-summary: frames=%d exitFrame=%d | pal=%ld "
                  "bg=%ld mdl=%ld spr=%ld hud=%ld tt=%ld pres=%ld "
                  "exit=%ld fill=%d\n",
                  framesRun, exitFrame, evPalT, evBgT, evMdlT, evSprT,
                  evHudT, evTtT, evPresT, evExitT, exitFill);
      // Phase 19A.2G — trail/ribbon diagnostic: e620 call census by
      // outcome, emitted-tri total, path-history updates and window
      // high water, the arena high water, and the two ribbon digests
      // (pen-pair stream / full pre-clip geometry in emit order).
      std::printf("ribbon:        calls=%d tris=%ld emit=%d cull=%d "
                  "gate=%d reject=%d clip=%d | trail upd=%d max=%d "
                  "arena=%d ovf=%d | penDg=%016llx ribDg=%016llx\n",
                  sm.ribbonDraw, evRibT, sm.ribbonEmit,
                  sm.ribbonPlaneCull, sm.ribbonGate, sm.ribbonReject,
                  sm.ribbonClip, sm.trailUpdate, sm.trailMax,
                  sm.drawListMax, sm.drawListOverflow,
                  (unsigned long long)penDigest,
                  (unsigned long long)ribDigest);
      std::printf("              limiter=%d rec{t1=%u,t2=%u,t3=%.4f,"
                  "t4=%.6f,t5=%u} base=%u tgt=%u | ribbon=%d ovf=%d | "
                  "dac=%016llx drawDigest=%016llx | reason=%s "
                  "finished=%d\n",
                  sm.limiter, (unsigned)ts.limiter[1],
                  (unsigned)ts.limiter[2],
                  (double)std::bit_cast<float>(ts.limiter[3]),
                  (double)std::bit_cast<float>(ts.limiter[4]),
                  (unsigned)ts.limiter[5], (unsigned)ts.limiterBase,
                  (unsigned)ts.limiterTarget, sm.ribbonDraw,
                  sm.drawListOverflow,
                  (unsigned long long)ts.paletteDacHash,
                  (unsigned long long)drawDigest,
                  streamCompletionName(ts.completionSrc),
                  sc.finished());
      // Phase 19A.3 — full seam/error census for the golden audit:
      // every dispatch/lifecycle counter plus the FUN_00408eb0 pool-
      // error seam and the DAT_0054148e quit latch. On a clean
      // real-data run poolErr/quit/stay 0 and the host-boundary
      // counters carry only their documented call counts.
      std::printf("seam-census: hero=%d stray=%d escort=%d pickup=%d "
                  "generic=%d twin=%d | anim=%d C=%d F=%d T=%d H=%d "
                  "N=%d A=%d S=%d | backdrop=%d drawList=%d flush=%d "
                  "hudBlit=%d listener=%d palRamp=%d fillSel=%d "
                  "limiter=%d | ttClear=%d bind=%d free=%d | "
                  "poolErr=%d quit=%d\n",
                  sm.heroUpdate, sm.strayUpdate, sm.escortUpdate,
                  sm.pickupUpdate, sm.genericUpdate, sm.twinSync,
                  sm.animCalls, sm.animClassless, sm.animFuse,
                  sm.animFuseEnd, sm.animHold, sm.animNull,
                  sm.animAdvance, sm.animSound, sm.backdrop,
                  sm.drawList, sm.drawFlush, sm.hudBlit, sm.listener,
                  sm.paletteRamp, sm.fillSelect, sm.limiter,
                  sm.teletype, sm.resourceBind, sm.resourceFree,
                  sc.poolErrorCalls(), sc.quitSignaled() ? 1 : 0);
      std::printf("ctr-digest:  %016llx  (%d frames — health, fade "
                  "bits, latch+reason, exit, win)\n",
                  (unsigned long long)ctrDigest, streamFrames);
    }
    // Mode-8 boundary check (bounded): FINISH.BNI existence is the
    // only probe — mode 8 is FUN_0047b038's FLIC/MVE pipeline, a
    // distinct init path that this substrate does NOT host.
    const auto fin = root->resolve("MISC/FINISH.BNI", nullptr);
    std::printf("mode8:       FINISH.BNI=%s — distinct FLIC/MVE "
                "pipeline (FUN_0047b038); not hosted by StreamScene\n",
                fin ? "present" : "absent");
    return ok ? 0 : 1;
  }


  if (!dataPath || !target) {
    return usage();
  }

  std::string err;
  const auto root = mdk::DataRoot::open(*dataPath, &err);
  if (!root) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }

  std::printf("request:   %s\n", target->c_str());
  const auto resolved = root->resolve(*target, &err);
  if (!resolved) {
    std::fprintf(stderr, "resolve:   FAILED (%s)\n", err.c_str());
    return 1;
  }
  std::printf("resolved:  %s\n", resolved->string().c_str());

  const auto size = root->fileSize(*target, &err);
  if (!size) {
    std::fprintf(stderr, "stat:      FAILED (%s)\n", err.c_str());
    return 1;
  }
  std::printf("size:      %llu bytes\n",
              static_cast<unsigned long long>(*size));

  const auto head = root->readPrefix(*target, kInspectHeadBytes, &err);
  if (!head) {
    std::fprintf(stderr, "read:      FAILED (%s)\n", err.c_str());
    return 1;
  }

  const auto family = mdk::fileFamilyForPath(*target);
  const auto support = mdk::fileFamilySupport(family);
  std::printf("family:    %s\n", std::string(mdk::fileFamilyName(family)).c_str());
  std::printf("envelope:  %s (by extension)\n",
              std::string(mdk::parserFamilyName(
                            mdk::parserFamilyForPath(*target))
                          )
                  .c_str());
  std::printf("support:   %s\n",
              std::string(mdk::familySupportName(support)).c_str());

  const auto info = mdk::inspectContainer(
      std::span<const std::byte>(head->data(), head->size()), *size);
  if (info.hasDeclaredLength) {
    std::printf("u32@0:     %u (0x%08x) — %s size-4\n", info.declaredLength,
                info.declaredLength,
                info.lengthValid ? "equals" : "DOES NOT equal");
  } else {
    std::printf("u32@0:     (file too small)\n");
  }

  switch (info.shape) {
    case mdk::ContainerShape::kNone:
      std::printf("envelope:  none (not this container format)\n");
      break;
    case mdk::ContainerShape::kLengthEnvelope:
      std::printf("envelope:  length-only (u32 valid; no tag/name "
                  "field)\n");
      break;
    case mdk::ContainerShape::kTaggedName:
      std::printf("envelope:  tagged-name\n");
      break;
  }

  if (info.hasTag) {
    std::printf("tag@4:     %s\n", mdk::tagToString(info.tag).c_str());
  }
  if (info.shape == mdk::ContainerShape::kTaggedName) {
    std::printf("name:      %s\n", info.logicalName.c_str());
    const std::string stem = stemOf(*target);
    if (!stem.empty()) {
      std::printf("stem:      %s — %s\n", stem.c_str(),
                  mdk::nameStemMatches(info, stem)
                      ? "match (case-insensitive)"
                      : "MISMATCH");
    }
    // Second observed u32 (offset 16): reported raw, not interpreted.
    mdk::BinaryReader r2(
        std::span<const std::byte>(head->data(), head->size()));
    if (r2.seek(16)) {
      if (const auto d2 = r2.u32le()) {
        std::printf("u32@16:    %u (0x%08x) — %s size-12 "
                    "(interior field; semantics not decoded)\n",
                    *d2, *d2,
                    *size >= 12 &&
                            static_cast<std::uint64_t>(*d2) == *size - 12
                        ? "equals"
                        : "does not equal");
      }
    }
  }

  if (!entriesMode && !visualInfoName && !fontInfoName &&
      !spriteInfoName && !collisionProbe && !collisionCensus &&
      !arenaObjects &&
      !surfaceCensus && !arenaRender && !traversalRuntime &&
      !freefallRuntime && !campaignHandoff && !scriptDisasm &&
      !objScriptDisasm) {
    return 0;
  }

  // --arena-objects: Phase 5E BUILD_A smoke. Loads the .DTI target
  // plus the sibling .CMI and <stem>O.MTO, builds the CMI enemy
  // table, resolves each arena's HotGen/HotPick records, spawns the
  // runtime objects (FUN_00456808 semantics), and reports the +0x68
  // list per arena — geometry parse, deep copy, transform rebuild and
  // list attach all exercised on original data.
  if (arenaObjects) {
    // Derive sibling paths: <dir>/<stem>.CMI and <dir>/<stem>O.MTO.
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--arena-objects wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string cmiPath = dir + stem + ".CMI";
    const std::string mtoPath = dir + stem + "O.MTO";

    const auto dtiFile = root->readFile(dtiPath, kEntriesMaxBytes, &err);
    const auto cmiFile = root->readFile(cmiPath, kEntriesMaxBytes, &err);
    const auto mtoFile = root->readFile(mtoPath, kEntriesMaxBytes, &err);
    if (!dtiFile || !cmiFile || !mtoFile) {
      std::fprintf(stderr, "read-file: FAILED (%s) — need .DTI + "
                           "sibling .CMI + <stem>O.MTO\n",
                   err.c_str());
      return 1;
    }
    std::printf("siblings:  %s + %s\n", cmiPath.c_str(), mtoPath.c_str());

    const auto dti = mdk::inspectDtiStructure(
        std::span<const std::byte>(dtiFile->data(), dtiFile->size()));
    const auto cmi = mdk::inspectCmiDirectory(
        std::span<const std::byte>(cmiFile->data(), cmiFile->size()));
    const auto mto = mdk::inspectMtoDirectory(
        std::span<const std::byte>(mtoFile->data(), mtoFile->size()));
    if (dti.status != mdk::DtiStructureStatus::kOk ||
        cmi.status != mdk::CmiDirectoryStatus::kOk ||
        mto.status != mdk::MtoDirectoryStatus::kOk) {
      std::fprintf(stderr, "parse: FAILED (dti=%s cmi=%s mto=%s)\n",
                   std::string(mdk::dtiStructureStatusName(dti.status))
                       .c_str(),
                   std::string(mdk::cmiDirectoryStatusName(cmi.status))
                       .c_str(),
                   std::string(mdk::mtoDirectoryStatusName(mto.status))
                       .c_str());
      return 1;
    }

    const mdk::EnemyTable enemies = mdk::buildEnemyTable(cmi);
    std::printf("enemy-tbl: %zu entries (%zu unresolved -> MTO)\n",
                enemies.entries.size(),
                [&] {
                  std::size_t n = 0;
                  for (const auto& e : enemies.entries)
                    n += e.unresolved ? 1 : 0;
                  return n;
                }());

    // Lazily resolved model cache — mirrors FUN_004286c8's
    // deferred-geometry table.
    struct Cache {
      std::vector<std::optional<mdk::RuntimeModel>> models;
      std::vector<bool> tried;
      const mdk::EnemyTable* enemies;
      std::span<const std::byte> cmiFile;
      const mdk::CmiDirectory* cmiDir;
      const mdk::MtoDirectory* mtoDir;
      std::span<const std::byte> mtoFile;
      int resolved = 0;
      int failed = 0;
    } cache;
    cache.models.resize(enemies.entries.size());
    cache.tried.resize(enemies.entries.size(), false);
    cache.enemies = &enemies;
    cache.cmiFile = std::span<const std::byte>(cmiFile->data(),
                                             cmiFile->size());
    cache.cmiDir = &cmi;
    cache.mtoDir = &mto;
    cache.mtoFile = std::span<const std::byte>(mtoFile->data(),
                                              mtoFile->size());
    auto modelFor = [](int idx, void* ctx) -> const mdk::RuntimeModel* {
      auto* c = static_cast<Cache*>(ctx);
      if (idx < 0 || static_cast<std::size_t>(idx) >= c->models.size())
        return nullptr;
      const std::size_t i = static_cast<std::size_t>(idx);
      if (!c->tried[i]) {
        c->tried[i] = true;
        const auto span = mdk::enemyModelData(
            *c->enemies, idx, c->cmiFile, *c->cmiDir, c->mtoDir,
            c->mtoFile);
        if (span) {
          c->models[i] = mdk::parseGeometryRecord(
              span->data(), span->data() + span->size());
        }
        if (c->models[i]) {
          ++c->resolved;
        } else {
          ++c->failed;
        }
      }
      return c->models[i] ? &*c->models[i] : nullptr;
    };

    // Per arena: copy the record (the original rewrites fields in
    // place at load), resolve names, spawn, report.
    int totalSpawn = 0;
    for (const auto& arec : dti.arenas) {
      mdk::DtiArenaRecord work = arec;   // mutable copy for the rewrite
      const auto failed =
          mdk::resolveArenaRecordNames(work, enemies);
      int types[10] = {};
      for (const auto& sr : work.subRecords)
        if (sr.type < 10) ++types[sr.type];
      mdk::DynamicArena arena;
      arena.name = arec.name();
      std::vector<mdk::DynamicObject*> spawned;
      const int n = mdk::spawnArenaObjects(arena, work, modelFor, &cache,
                                          &spawned);
      totalSpawn += n;
      std::printf("arena %-8.8s  subs=%u t2=%d t4=%d t6=%d  spawn=%d",
                  arec.name().c_str(), arec.subRecordCount, types[2],
                  types[4], types[6], n);
      if (!failed.empty()) {
        std::printf("  unresolved-names=%zu", failed.size());
      }
      std::printf("\n");
      // QA route planning: dump every sub-record type with its
      // fields (type-6 portals get the side/box decode).
      for (const auto& r : work.subRecords) {
        std::printf("  sub t=%-2u f0=%u f1=%u box=(%.0f,%.0f,%.0f)-"
                    "(%.0f,%.0f,%.0f)\n",
                    r.type, r.fields[0], r.fields[1],
                    (double)r.fieldAsFloat(2),
                    (double)r.fieldAsFloat(3),
                    (double)r.fieldAsFloat(4),
                    (double)r.fieldAsFloat(5),
                    (double)r.fieldAsFloat(6),
                    (double)r.fieldAsFloat(7));
        if (r.type != 6) continue;
        std::printf("  portal side=%u ->%u box=(%.0f,%.0f,%.0f)-"
                    "(%.0f,%.0f,%.0f)\n",
                    r.fields[1], r.fields[0],
                    (double)r.fieldAsFloat(2),
                    (double)r.fieldAsFloat(3),
                    (double)r.fieldAsFloat(4),
                    (double)r.fieldAsFloat(5),
                    (double)r.fieldAsFloat(6),
                    (double)r.fieldAsFloat(7));
      }
      for (const auto* o : spawned) {
        std::printf("  obj idx=%-3u spawn=%-5u pos=(%g, %g, %g) "
                    "elems=%zd tris=%d aabb=(%g..%g, %g..%g, %g..%g)\n",
                    o->enemyIndex, o->spawnId, (double)o->pos[0],
                    (double)o->pos[1], (double)o->pos[2],
                    o->model.elems.size(),
                    o->model.elems.empty() ? 0 : o->model.elems[0].triCount,
                    (double)o->col.aabb[0], (double)o->col.aabb[3],
                    (double)o->col.aabb[1], (double)o->col.aabb[4],
                    (double)o->col.aabb[2], (double)o->col.aabb[5]);
        // FUN_004585c4 census — every spawned mover target.
        if (o->col.flags14a & 0x20)
          std::printf("    mvr %s model=%s f148=%06x f14a=%02x\n",
                      o->scriptClass.c_str(), o->model.modelName().c_str(),
                      (unsigned)o->col.flags148,
                      (unsigned)o->col.flags14a);
      }
    }
    std::printf("total:     spawned=%d  models resolved=%d failed=%d\n",
                totalSpawn, cache.resolved, cache.failed);

    // Floor-probe smoke on the first spawned object: force the
    // standable bit (script-assigned in the original — opcode 0x29)
    // and probe straight down through the object's position.
    for (const auto& arec : dti.arenas) {
      mdk::DtiArenaRecord work = arec;
      mdk::resolveArenaRecordNames(work, enemies);
      mdk::DynamicArena arena;
      arena.name = arec.name();
      if (mdk::spawnArenaObjects(arena, work, modelFor, &cache,
                                 nullptr) == 0) {
        continue;
      }
      auto& o = *arena.storage.front();
      o.col.flags149 |= 1;
      mdk::CollisionArena ca = {};
      ca.objects = arena.col.objects;
      ca.deepFloorZ = -1000.0f;
      mdk::CollisionState cs;
      cs.arena = &ca;
      cs.queryEnabled = 1;
      cs.arenaValid = 1;
      cs.objectDataLoaded = 1;
      cs.pos[0] = o.pos[0];
      cs.pos[1] = o.pos[1];
      cs.pos[2] = o.pos[2] + 2.0f;
      mdk::collisionFloorProbe(cs);
      std::printf("floorprobe arena=%s obj@(%g,%g,%g): flags=0x%02x "
                  "floorZ=%g hit=%s\n",
                  arena.name.c_str(), (double)o.pos[0],
                  (double)o.pos[1], (double)o.pos[2], cs.contactFlags,
                  (double)cs.floorZ,
                  cs.floorObj == &o.col ? "spawned-object" : "none");
      break;
    }
    return 0;
  }

  // --traversal-runtime: Phase 5G BUILD_A smoke. Assembles the native
  // traversal runtime over the .DTI target's sibling triple (.DTI +
  // .CMI + <stem>O.MTO), resolves the s0 spawn, attaches the initial
  // arena (MTO region-C collision + spawn-once objects), and steps
  // the frame driver in the original FUN_00436100 order. Headless —
  // script-object calls, the scripted-move gate, the arena event
  // list, the world tick and camera/teleport blocks are counted as
  // seams, never emulated. --arena/--start/--yaw are NATIVE
  // DIAGNOSTIC OVERRIDES, not original spawn behavior.
  if (traversalRuntime) {
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--traversal-runtime wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string cmiPath = dir + stem + ".CMI";
    const std::string mtoPath = dir + stem + "O.MTO";

    mdk::TraversalRuntime rt;
    const auto le = mdk::traversalRuntimeLoad(
        *root, dtiPath, cmiPath, mtoPath, rt, &err);
    if (le != mdk::TraversalLoadError::kOk) {
      std::fprintf(stderr, "traversal-load: FAILED (%s: %s)\n",
                   mdk::traversalLoadErrorName(le), err.c_str());
      return 1;
    }
    std::printf("level:     %s + %s + %s\n", dtiPath.c_str(),
                cmiPath.c_str(), mtoPath.c_str());
    // NATIVE DIAGNOSTIC DEFAULT — a standalone arena run has no
    // campaign carry-in, so the health field starts at 0; seed the
    // observed live-player value (150, per real saves) so the
    // `0xf1` hp gates behave and --save-write-full emits a
    // GAME.health inside the parser's (0,150] bound.
    if (rt.fieldHealth <= 0) rt.fieldHealth = 150;
    std::printf("arenas:    %zu  enemy-tbl=%zu  models=%d ok/%d fail\n",
                rt.arenas.size(), rt.level.enemies.entries.size(),
                rt.level.modelsResolved, rt.level.modelsFailed);
    if (!rt.level.unresolvedNames.empty())
      std::printf("unresolved spawn names: %zu\n",
                  rt.level.unresolvedNames.size());
    std::printf("spawn(s0): arena=%d (%s) pos=(%g, %g, %g) yaw=%g\n",
                rt.cur->index, rt.cur->name.c_str(),
                (double)rt.cs.pos[0], (double)rt.cs.pos[1],
                (double)rt.cs.pos[2], (double)rt.motion.yawDeg);
    for (const auto& a : rt.arenas)
      std::printf(
          "  arena[%2d] %-9s geom=%d objs=%2zu subs=%u scal=%g %s\n",
          a->index, a->name.c_str(), a->geometryLoaded ? 1 : 0,
          a->dyn.storage.size(), a->rec->subRecordCount,
          (double)a->scalar,
          a->hasScriptObject ? "script-obj" : "");

    // NATIVE DIAGNOSTIC OVERRIDE — not original spawn behavior.
    if (travArena || travStartGiven || travYawGiven) {
      int arenaIdx = rt.cur->index;
      if (travArena) {
        arenaIdx = -1;
        for (const auto& a : rt.arenas)
          if (a->name == *travArena) arenaIdx = a->index;
        if (arenaIdx < 0) {
          char* endp = nullptr;
          const long v = std::strtol(travArena->c_str(), &endp, 10);
          if (endp && *endp == '\0') arenaIdx = static_cast<int>(v);
        }
      }
      float pos[3];
      for (int i = 0; i < 3; ++i)
        pos[i] = travStartGiven ? travStart[i] : rt.cs.pos[i];
      const float yaw = travYawGiven ? travYaw : rt.motion.yawDeg;
      const auto oe = mdk::traversalRuntimeDiagnosticStart(
          rt, arenaIdx, pos, yaw, &err);
      if (oe != mdk::TraversalLoadError::kOk) {
        std::fprintf(stderr, "diagnostic-start: FAILED (%s: %s)\n",
                     mdk::traversalLoadErrorName(oe), err.c_str());
        return 1;
      }
      std::printf("override:  arena=%d (%s) pos=(%g, %g, %g) yaw=%g "
                  "— NATIVE DIAGNOSTIC OVERRIDE\n",
                  rt.cur->index, rt.cur->name.c_str(), (double)pos[0],
                  (double)pos[1], (double)pos[2], (double)yaw);
    }
    // QA-only script entry: seed the current arena's persisted script
    // PC (+0x220, the same field a script goto/link writes) at a raw
    // image offset. Lets the diagnostic reach link-gated regions
    // without routing through gameplay triggers.
    if (travScriptPc) {
      rt.cur->script.pcImageOff = travScriptPc;
      rt.cur->script.active = true;
      rt.cur->script.waitSeconds = 0.0f;
      std::printf("script-pc: %s pc=+%x — QA SCRIPT ENTRY\n",
                  rt.cur->name.c_str(), travScriptPc);
    }

    // QA ammo seeds — 0x54161f..0x541633 block entries (a real
    // session carries these across the level transition; a fresh
    // inspect start has none).
    for (const auto& am : ammoSpecs) {
      if (am.first >= 0 && am.first < 6) {
        rt.ammo[am.first] = am.second;
        std::printf("ammo-seed: ammo[%d]=%d — QA ONLY\n", am.first,
                    am.second);
      }
    }

    // Scripted input: idle -> KeyUp (forward) -> idle -> KeyJump.
    // Key bindings are the factory defaults (KeyUp=103, KeyJump=56).
    const mdk::GameplayInputBindings bindings;
    auto rawFor = [](int phase) {
      mdk::RawGameplayInput r;
      if (phase == 1) r.keyLevel[103 >> 5] |= 1u << (103 & 31);
      if (phase == 3) r.keyLevel[56 >> 5] |= 1u << (56 & 31);
      return r;
    };
    mdk::FrontendTimingState timing; // f0=1, dt=1/30, step=1
    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        digest ^= (v >> (i * 8)) & 0xff;
        digest *= 1099511628211ull;
      }
    };
    const mdk::TraversalArena* startArena = rt.cur;
    int framesRun = 0;
    bool floorObjSeen = false;
    bool groundedSeen = false;
    bool airborneSeen = false;
    bool contactSeen = false;
    bool partnerSeen = false;
    // Phase 15A — boss/combat harness state. `--hit` shots are
    // injected below; `--boss` objects are change-detected each
    // frame; completion latches accumulate.
    bool sawEndLevel = false;
    bool sawEnding = false;
    struct BossSnap {
      std::int32_t health = -1;
      std::uint8_t mark = 0, sub = 0;
      const void* pc = nullptr;
      const void* f230 = nullptr;
      std::uint32_t f148 = 0;
      bool dead = false;
      const void* animRec = nullptr;
      std::int16_t animFrame = 0, animLatch = 0;
      std::array<std::int16_t, 12> eh{};
      bool operator==(const BossSnap&) const = default;
    };
    std::unordered_map<const mdk::DynamicObject*, BossSnap> lastSnap;
    const auto& bossMatches = combatBossMatches;
    // Phase 17C.1 — audio event diagnostic. The runtime's drain-once
    // audioFx queue is consumed each frame; events fold into a
    // deterministic digest + op census, with the first few kept as a
    // bounded sample log. No waveform data is touched.
    std::uint64_t audioDg = 1469598103934665603ull;
    std::size_t audioOps[9] = {};
    std::size_t audioTotal = 0;
    std::vector<mdk::TraversalAudioEvent> audioLog;
    // MDK_IDLE_INPUT=1 — QA-only opt-out of the scripted phase
    // schedule (genuinely idle frames; needed to test spawn-adjacent
    // scripted triggers the default schedule walks the player off).
    const bool idleInput = std::getenv("MDK_IDLE_INPUT") != nullptr;
    // QA route driver state: wpIdx advances only on grounded contact
    // (scripted vehicle legs move the player without consuming
    // waypoints); routeStall counts frames since forward progress.
    std::size_t wpIdx = 0;
    bool prevGrounded = true;
    int routeStall = 0;
    int routeArena = -2;
    float routeBestDist = 1e30f;
    bool routeStallReported = false;
    for (int f = 0; f < travFrames; ++f) {
      const int phase =
          (idleInput || !routeWps.empty())
              ? 0
              : f < 10 ? 0 : f < 30 ? 1 : f < 35 ? 0 : f < 50 ? 3 : 0;
      // Phase 15A — combat harness: each pending --hit injects ONE
      // shot into a free pool slot, aimed through the named
      // object/element. The pool's own collision + damage tail
      // (w0/w1 hit path, FUN_00460d44 for 2/3/4) resolves it — no
      // health/phase fields are written by the harness.
      // Element shots are serialized one per frame: a shot's hit
      // writes the element mark into +0x21e/+0x21c, which the next
      // frame's script pass consumes. Two element hits landing in
      // the same pool tick overwrite the mark before consumption —
      // the same reason the original can't register simultaneous
      // element kills — so at most one @ELEM spec fires per frame.
      bool elemShotFired = false;
      fireCombatHits(rt, hitSpecs, elemShotFired, f);
      const bool colProfFrame =
          std::getenv("MDK_COL_PROFILE_FRAME") != nullptr;
      const mdk::CollisionProfile colPrev =
          colProfFrame ? mdk::collisionProfile() : mdk::CollisionProfile{};
      auto raw = rawFor(phase);
      for (const auto& h : holdSpecs)
        if (f >= h.first && f <= h.last)
          raw.keyLevel[h.key >> 5] |= 1u << (h.key & 31);
      for (const auto& p : pressSpecs)
        if (f == p.first)
          raw.keyEdge[p.key >> 5] |= 1u << (p.key & 31);
      for (const int iu : itemUseFrames)
        if (iu == f) raw.keyEdge[0] |= (1u << 28);   // kSlotItemUse
      if (std::getenv("MDK_BT_TRACE"))
        std::fprintf(stderr, "BTFRAME %d\n", f);
      // QA route driver: steer toward routeWps[wpIdx] with real key
      // levels. Waypoints only advance while grounded — scripted
      // vehicle legs move the player without consuming the route.
      if (!routeWps.empty()) {
        const int arenaNow = rt.cur ? rt.cur->index : -1;
        if (arenaNow != routeArena) {
          std::printf("route: arena %d->%d (%s) f=%d pos=(%.1f,%.1f,"
                      "%.1f)\n",
                      routeArena, arenaNow,
                      rt.cur ? rt.cur->name.c_str() : "?", f,
                      (double)rt.cs.pos[0], (double)rt.cs.pos[1],
                      (double)rt.cs.pos[2]);
          routeArena = arenaNow;
        }
        if (wpIdx < routeWps.size() && (prevGrounded || f == 0)) {
          const RouteWaypoint& w = routeWps[wpIdx];
          const float dx = w.x - rt.cs.pos[0];
          const float dy = w.y - rt.cs.pos[1];
          const float dist = std::sqrt(dx * dx + dy * dy);
          float targ = std::atan2(dy, dx) * (180.f / 3.14159265f);
          float err = targ - rt.motion.yawDeg;
          while (err > 180.f) err -= 360.f;
          while (err < -180.f) err += 360.f;
          if (err > 2.f)
            raw.keyLevel[105 >> 5] |= 1u << (105 & 31);  // turn left
          else if (err < -2.f)
            raw.keyLevel[106 >> 5] |= 1u << (106 & 31);  // turn right
          if (std::fabs(err) < 25.f || w.jump)
            raw.keyLevel[103 >> 5] |= 1u << (103 & 31);  // forward
          if (w.jump)
            raw.keyLevel[56 >> 5] |= 1u << (56 & 31);    // jump-hold
          if (dist < routeBestDist) {
            routeBestDist = dist;
            routeStall = 0;
            routeStallReported = false;
          } else if (++routeStall == 300 && !routeStallReported) {
            std::printf("route: STALL wp%zu f=%d pos=(%.1f,%.1f,%.1f) "
                        "arena=%d dist=%.1f\n",
                        wpIdx, f, (double)rt.cs.pos[0],
                        (double)rt.cs.pos[1], (double)rt.cs.pos[2],
                        arenaNow, (double)dist);
            routeStallReported = true;
          }
          if (dist < w.r) {
            std::printf("route: wp%02d reached f=%d pos=(%.1f,%.1f,"
                        "%.1f) arena=%d\n",
                        (int)wpIdx, f, (double)rt.cs.pos[0],
                        (double)rt.cs.pos[1], (double)rt.cs.pos[2],
                        arenaNow);
            ++wpIdx;
            routeStall = 0;
            routeBestDist = 1e30f;
            routeStallReported = false;
          }
        }
      }
      const auto out =
          mdk::stepTraversalRuntime(rt, raw, bindings, timing);
      prevGrounded = out.grounded;
      ++framesRun;
      for (const int iu : itemUseFrames)
        if (iu == f || iu + 1 == f || iu + 10 == f) {
          const char* itemName = "-";
          std::string census;
          for (const auto& ap : rt.arenas)
            for (const auto& up : ap->dyn.storage) {
            const mdk::DynamicObject& fo = *up;
            if (fo.enemyIndex >= rt.level.enemies.entries.size())
              continue;
            const char* nm =
                rt.level.enemies.entries[fo.enemyIndex].name.c_str();
            if (!census.empty()) census += ',';
            census += nm;
            census += '/';
            census += std::to_string(fo.field30a);
            if (nm[0] == 'S' && nm[1] == 'W' && fo.field30a >= 1 &&
                fo.field30a <= 9) {
              itemName = nm;
            }
            }
          const int chg = (rt.inventorySel >= 0 && rt.inventorySel < 5)
                              ? rt.inventory[rt.inventorySel].charges
                              : -1;
          std::printf(
              "      iu   f=%03d inv=%d sel=%d chg=%d loco=%03x ev=%d/%d "
              "use=%d spawn=%d objs=%zu item=%s td=%d die=%d\n",
              f, rt.inventoryCount, rt.inventorySel, chg,
              (unsigned)rt.locoState, (int)rt.eventType,
              (int)rt.eventMag, rt.seams.itemUseCalls,
              rt.seams.itemSpawnCalls, rt.cur->dyn.storage.size(),
              itemName, rt.seams.objectTeardownCalls,
              rt.seams.objectDeathCalls);
          if (!census.empty() && census.size() < 400)
            std::printf("      iuc  [%s]\n", census.c_str());
        }
      // Drain the frame's audio events (drain-once semantics) into the
      // diagnostic census before any consumer could.
      if (!rt.audioFx.empty()) {
        for (const mdk::TraversalAudioEvent& ev : rt.audioFx) {
          ++audioTotal;
          const int oi = static_cast<int>(ev.op);
          if (oi >= 0 && oi < 9) ++audioOps[oi];
          for (int i = 0; i < 8; ++i) {
            audioDg ^= (std::uint64_t(ev.seq) >> (i * 8)) & 0xff;
            audioDg *= 1099511628211ull;
          }
          audioDg ^= std::uint64_t(ev.op) & 0xff;
          audioDg *= 1099511628211ull;
          for (const char c : ev.name) {
            audioDg ^= (unsigned char)c;
            audioDg *= 1099511628211ull;
          }
          for (int i = 0; i < 3; ++i) {
            const std::uint32_t b =
                static_cast<std::uint32_t>(ev.pos[i] * 256.0f);
            for (int k = 0; k < 4; ++k) {
              audioDg ^= (b >> (k * 8)) & 0xff;
              audioDg *= 1099511628211ull;
            }
          }
          audioDg ^= std::uint64_t(ev.hasPos) & 0xff;
          audioDg *= 1099511628211ull;
          for (int i = 0; i < 4; ++i) {
            audioDg ^= (ev.mode >> (i * 8)) & 0xff;
            audioDg *= 1099511628211ull;
          }
          if (audioLog.size() < 16) audioLog.push_back(ev);
        }
        rt.audioFx.clear();
      }
      sawEndLevel = sawEndLevel || out.endLevelRequested;
      sawEnding = sawEnding || out.endingRequested;
      if (colProfFrame) {
        const mdk::CollisionProfile& cp = mdk::collisionProfile();
        std::printf(
            "      col f=%03d stab=+%llu/%llun m1=+%llu/%llun "
            "sweep=+%llu/%llun/%llup probe=+%llu/%llut "
            "splash=+%llu falloff=+%llu\n",
            f,
            (unsigned long long)(cp.stabCalls - colPrev.stabCalls),
            (unsigned long long)(cp.stabNodes - colPrev.stabNodes),
            (unsigned long long)(cp.stabM1Calls - colPrev.stabM1Calls),
            (unsigned long long)(cp.stabM1Nodes - colPrev.stabM1Nodes),
            (unsigned long long)(cp.sweepCalls - colPrev.sweepCalls),
            (unsigned long long)(cp.sweepNodes - colPrev.sweepNodes),
            (unsigned long long)(cp.sweepPolyTests -
                                 colPrev.sweepPolyTests),
            (unsigned long long)(cp.probeCalls - colPrev.probeCalls),
            (unsigned long long)(cp.probeTris - colPrev.probeTris),
            (unsigned long long)(cp.splashCalls - colPrev.splashCalls),
            (unsigned long long)(cp.falloffCalls -
                                 colPrev.falloffCalls));
      }
      // Phase 15A — storage census per frame (object-lifetime debug).
      if (!bossNames.empty() && bossNames[0][0] == '*') {
        std::size_t tot = 0;
        std::string det;
        for (const auto& ap : rt.arenas) {
          tot += ap->dyn.storage.size();
          for (const auto& up : ap->dyn.storage)
            det += " " + up->scriptClass;
        }
        std::printf("      [f%03d objs=%zu:%s ]\n", f, tot,
                    det.c_str());
      }
      // Phase 15A — shared-var change detection: the +0x48 per-arena
      // f32 vars (operand mode 1) drive phase gates like MEAT_10's
      // XCBOSS wake, so log every edge.
      static float lastVars[4] = {-1, -1, -1, -1};
      static std::uint32_t lastF58 = ~0u;
      if (!bossNames.empty()) {
        for (const auto& ap : rt.arenas) {
          if (travArena && ap->name != *travArena) continue;
          if (std::memcmp(ap->objVars48, lastVars, sizeof lastVars) != 0 ||
              ap->flags58 != lastF58) {
            std::printf("      [f%03d vars48=(%.3g,%.3g,%.3g,%.3g) "
                        "f58=%08x]\n", f,
                        ap->objVars48[0], ap->objVars48[1],
                        ap->objVars48[2], ap->objVars48[3],
                        ap->flags58);
            std::memcpy(lastVars, ap->objVars48, sizeof lastVars);
            lastF58 = ap->flags58;
          }
        }
      }
      // Phase 15A — boss change-detection: print when a watched
      // object's health / mark / subtype / pc / flags / elemHp or
      // dead state changes.
      if (!bossNames.empty()) {
        for (const auto& ap : rt.arenas) {
          for (const auto& up : ap->dyn.storage) {
            const mdk::DynamicObject& o = *up;
            bool want = false;
            for (const auto& nm : bossNames)
              want = want || bossMatches(o, nm.c_str());
            if (!want) continue;
            BossSnap sn;
            sn.health = o.health;
            sn.mark = o.field21e;
            sn.sub = o.field11e;
            sn.pc = o.field108;
            sn.f230 = o.field230;
            sn.f148 = o.col.flags148;
            sn.dead = (o.col.flags148 & 0x20) != 0;
            sn.animRec = o.animRec;
            sn.animFrame = o.animFrame;
            sn.animLatch = o.animLatch;
            const std::size_t neh = o.elemHp.size() < 12
                                        ? o.elemHp.size() : 12;
            for (std::size_t i = 0; i < neh; ++i) sn.eh[i] = o.elemHp[i];
            auto it = lastSnap.find(&o);
            if (it == lastSnap.end()) {
              lastSnap[&o] = sn;
              continue;
            }
            if (!(it->second == sn)) {
              const auto* pcb =
                  static_cast<const std::byte*>(o.field108);
              const auto* img = rt.level.cmiBytes.data();
              const std::ptrdiff_t pco =
                  (pcb >= img && pcb < img + rt.level.cmiBytes.size())
                      ? pcb - img
                      : -1;
              std::printf(
                  "      boss %s hp=%d mark=%02x sub=%02x "
                  "pc=+%lx f230=%p f148=%04x f149=%02x dead=%d "
                  "f0=%.1f e8=%.2f e6=%d ec=%p "
                  "anim=%p f=%d l=%04x acc=%.2f "
                  "f11a=%d f11b=%d f138=%s eh=[%d,%d,%d,%d] "
                  "L=[%.3g,%.3g,%.3g,%.3g] "
                  "aabb=(%.3g..%.3g,%.3g..%.3g,%.3g..%.3g) el=%zd\n",
                  o.scriptClass.c_str(), (int)o.health,
                  (unsigned)o.field21e, (unsigned)o.field11e,
                  static_cast<unsigned long>(pco), o.field230,
                  (unsigned)o.col.flags148, (unsigned)o.col.flags149,
                  sn.dead ? 1 : 0,
                  (double)o.fieldF0, (double)o.fieldE8,
                  (int)o.fieldE6, o.fieldEC,
                  o.animRec, (int)o.animFrame,
                  (unsigned)(std::uint16_t)o.animLatch,
                  (double)o.animAcc,
                  (int)o.field11a, (int)o.field11b,
                  o.field138 ? o.field138->scriptClass.c_str() : "-",
                  o.elemHp.size() > 0 ? (int)o.elemHp[0] : -1,
                  o.elemHp.size() > 1 ? (int)o.elemHp[1] : -1,
                  o.elemHp.size() > 2 ? (int)o.elemHp[2] : -1,
                  o.elemHp.size() > 3 ? (int)o.elemHp[3] : -1,
                  (double)o.scriptLocals[0], (double)o.scriptLocals[1],
                  (double)o.scriptLocals[2], (double)o.scriptLocals[3],
                  (double)o.col.aabb[0], (double)o.col.aabb[3],
                  (double)o.col.aabb[1], (double)o.col.aabb[4],
                  (double)o.col.aabb[2], (double)o.col.aabb[5],
                  o.model.elems.size());
              it->second = sn;
            }
          }
        }
      }
      floorObjSeen |= (rt.cs.contactFlags & 2) != 0;
      groundedSeen |= out.grounded;
      airborneSeen |= !out.grounded;
      contactSeen |= out.contactObj != 0;
      partnerSeen |= out.partnerArenaIndex >= 0;
      // Phase 16A — the player-animation identity fields:
      // st=locoState (hex) af=animFrame at=K_table[frameIdx]
      // (FUN_00461954's selected sprite) dr=draw gate.
      // Sniper-scope diagnostics — scope latch (0x540c9c), scope
      // phase (0x540ca0), FOV zoom (0x540b58), weapon-5 ammo
      // (0x541633), live X_STRIKE bolts (FUN_0045a4dc's spawn) and
      // the weapon5Spawn seam — the fire->spawn->world chain stays
      // visible past unscope while a bolt is still live.
      int strikeCount = 0;
      int kamikazeCount = 0;   // field30a==5 — cmd-0x80's dropped
                             // X_TOOTH bomblets (the live world-side
                             // fire consequence on the path end).
      if (rt.cur != nullptr)
        for (const auto& up : rt.cur->dyn.storage) {
          if (up->scriptClass == "X_STRIKE") ++strikeCount;
          if (up->field30a == 5u) ++kamikazeCount;
        }
      char sphBuf[160] = "";
      if (rt.flagC9c != 0 || rt.transitionPhase != 0 ||
          strikeCount != 0 || kamikazeCount != 0) {
        std::snprintf(sphBuf, sizeof sphBuf,
                      " c9c=%d ca0=%d zoom=%.3f wpn=%d am=%d"
                      " e14=%d d0c=%d fcd=%.2f w5s=%d deny=%d xst=%d"
                      " kmk=%d",
                      rt.flagC9c, rt.transitionPhase,
                      (double)rt.camera.zoom, rt.wpnSel0, rt.ammo[5],
                      rt.fieldE14,
                      rt.fieldD0c, (double)rt.fireCadence,
                      (int)rt.seams.weapon5SpawnCalls,
                      (int)rt.seams.fireDenyCalls, strikeCount,
                      kamikazeCount);
      }
      std::printf(
          "f=%03d a=%d p=%d pos=(%8.2f,%8.2f,%8.2f) yaw=%6.1f "
          "mv=%5.2f sv=%5.2f vv=%6.2f gnd=%d ctc=%08x sld=%d ev=%d/%d "
          "st=%03x af=%d at=%s[%d]%s "
          "cam=(%8.2f,%8.2f,%8.2f)%s%s%s\n",
          out.frame, out.curArenaIndex, out.partnerArenaIndex,
          (double)out.pos[0], (double)out.pos[1], (double)out.pos[2],
          (double)out.yawDeg, (double)out.moveVel,
          (double)out.strafeVel, (double)out.vertVel,
          out.grounded ? 1 : 0, out.contactObj, out.slideChannel,
          out.eventType, out.eventMag,
          out.locoState, out.animFrame,
          out.animTableIdx >= 0
              ? mdk::playerAnimTableName(out.animTableIdx) : "-",
          out.animFrameIdx, out.animDrawn ? " dr" : "",
          (double)out.camera.pos[0], (double)out.camera.pos[1],
          (double)out.camera.pos[2],
          out.overheadViewActive ? " OVH" : "",
          out.viewOnPartner ? " VP" : "", sphBuf);
      // Phase 16B.1 — authentic damage probes: the producer lands at
      // end-of-frame (post-dispatch), exactly where the in-level
      // enemy/projectile passes call it; the next frame's dispatch
      // tail consumes the accumulator.
      if (!pdmgSpecs.empty()) {
        std::printf(
            "      dmg hp=%d accum=%.3f e10=%.3f eb8=%d dac=%d "
            "st=%03x ev=%d/%d\n",
            (int)rt.fieldHealth, (double)rt.vert.landingAccum,
            (double)rt.fieldE10, (int)rt.fieldEb8, (int)rt.fieldDac,
            (unsigned)rt.locoState, (int)rt.eventType,
            (int)rt.eventMag);
        for (const auto& d : pdmgSpecs) {
          if (d.frame != f) continue;
          const int hpBefore = rt.fieldHealth;
          mdk::playerDamageApply(rt, d.amount, rt.cs.pos);
          std::printf(
              "      pdmg f=%03d amt=%d hp=%d->%d accum=%.3f "
              "(authentic FUN_0046771c producer)\n",
              f, d.amount, hpBefore, (int)rt.fieldHealth,
              (double)rt.vert.landingAccum);
        }
      }
      for (const auto& ap : rt.arenas) {
        static std::uint32_t lastF58[64] = {};
        static std::uint32_t lastPc[64] = {};
        static std::uint32_t lastWait[64] = {};
        if (ap->index < 64 &&
            (ap->flags58 != lastF58[ap->index] ||
             ap->script.pcImageOff != lastPc[ap->index] ||
             (ap->script.waitSeconds > 0.0f) != (lastWait[ap->index] != 0))) {
          std::printf("      f58[%s] %08x -> %08x pc=+%x wait=%.2f depth=%d\n",
                      ap->name.c_str(), lastF58[ap->index],
                      (unsigned)ap->flags58,
                      (unsigned)ap->script.pcImageOff,
                      (double)ap->script.waitSeconds,
                      (int)ap->script.callDepth);
          lastF58[ap->index] = ap->flags58;
          lastPc[ap->index] = ap->script.pcImageOff;
          lastWait[ap->index] = (ap->script.waitSeconds > 0.0f) ? 1u : 0u;
        }
      }
      // Connector (door) state dump — connState/anim/flags per frame.
      for (const auto& ap : rt.arenas)
        for (const auto& up : ap->dyn.storage) {
          const mdk::DynamicObject& o = *up;
          if (!(o.col.flags14a & 0x10)) continue;
          std::printf(
              "      door %s home=%s st=%02x anim=%c fr=%.2f cur=%d "
              "lat=%04x f148=%04x f14a=%02x r=%.1f done=%d\n",
              o.scriptClass.c_str(),
              o.arena && o.arena->owner ? o.arena->owner->name.c_str()
                                        : "?",
              o.connState, o.animRec ? 'Y' : 'n',
              (double)o.animAcc, (int)o.animFrame,
              (unsigned)(std::uint16_t)o.animLatch,
              (unsigned)o.col.flags148, (unsigned)o.col.flags14a,
              (double)o.connRadius, o.animDone() ? 1 : 0);
        }
      // Mover census — FUN_004585c4 targets (+0x14a&0x20): model,
      // child pointer, anim/impulse state per frame.
      for (const auto& ap : rt.arenas)
        for (const auto& up : ap->dyn.storage) {
          const mdk::DynamicObject& o = *up;
          if (!(o.col.flags14a & 0x20)) continue;
          std::printf(
              "      mvr %s model=%s home=%s child=%s anim=%c "
              "fr=%.2f cur=%d lat=%04x f148=%04x f14a=%02x f14c=%02x "
              "vel.z=%.2f yaw=%.2f scl=%.2f done=%d\n",
              o.scriptClass.c_str(), o.model.modelName().c_str(),
              o.arena && o.arena->owner ? o.arena->owner->name.c_str()
                                        : "?",
              o.moverChild ? o.moverChild->model.modelName().c_str()
                           : "-",
              o.animRec ? 'Y' : 'n',
              (double)o.animAcc, (int)o.animFrame,
              (unsigned)(std::uint16_t)o.animLatch,
              (unsigned)o.col.flags148, (unsigned)o.col.flags14a,
              (unsigned)o.col.flags14c,
              (double)o.field30, (double)o.yawDeg,
              (double)o.col.scale, o.animDone() ? 1 : 0);
        }
      // QA-only (MDK_TRACE_DOOR=substr): per-frame trace of every live
      // object whose model/class contains the substring — pos, world
      // AABB, anim cursor — for watching scripted movers like
      // LEVEL3's XH1_DOOR land.
      if (const char* dw = std::getenv("MDK_TRACE_DOOR")) {
        const std::string want = dw;
        for (const auto& ap : rt.arenas)
          for (const auto& up : ap->dyn.storage) {
            const mdk::DynamicObject& o = *up;
            const std::string tok = o.model.modelName();
            if (tok.find(want) == std::string::npos &&
                o.scriptClass.find(want) == std::string::npos)
              continue;
            std::printf(
                "      TDOOR f=%d %s home=%s pos=(%.1f,%.1f,%.1f) "
                "aabb=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) anim=%c cur=%d "
                "lat=%04x f148=%04x f14a=%02x hp=%d v21e=%02x\n",
                f, tok.c_str(),
                o.arena && o.arena->owner ? o.arena->owner->name.c_str()
                                          : "?",
                (double)o.pos[0], (double)o.pos[1], (double)o.pos[2],
                (double)o.col.aabb[0], (double)o.col.aabb[1],
                (double)o.col.aabb[2], (double)o.col.aabb[3],
                (double)o.col.aabb[4], (double)o.col.aabb[5],
                o.animRec ? 'Y' : 'n', (int)o.animFrame,
                (unsigned)(std::uint16_t)o.animLatch,
                (unsigned)o.col.flags148, (unsigned)o.col.flags14a,
                o.health, (unsigned)o.field21e);
          }
      }
      // The digest mixes only deterministic state — raw contact
      // tokens are process addresses and are mixed as booleans.
      mix(static_cast<std::uint64_t>(out.frame));
      mix(static_cast<std::uint64_t>(out.curArenaIndex));
      mix(static_cast<std::uint64_t>(
          out.partnerArenaIndex < 0 ? 0xffff : out.partnerArenaIndex));
      for (int i = 0; i < 3; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &out.pos[i], 4);
        mix(bits);
      }
      std::uint32_t bits;
      std::memcpy(&bits, &out.yawDeg, 4);
      mix(bits);
      std::memcpy(&bits, &out.vertVel, 4);
      mix(bits);
      mix(out.contactObj != 0 ? 1 : 0);
      mix(out.grounded ? 1 : 0);
      mix(static_cast<std::uint64_t>(out.locoState));
      mix(static_cast<std::uint64_t>(out.slideChannel));
      // Phase 16A — the player-anim identity: frame counter, resolved
      // (table, index) identity of the selected sprite record, the
      // movement-anim phase (bit pattern) and the draw gate. All
      // deterministic under the injected input stream.
      mix(static_cast<std::uint64_t>(out.animFrame));
      mix(static_cast<std::uint64_t>(out.animTableIdx));
      mix(static_cast<std::uint64_t>(out.animFrameIdx));
      {
        std::uint32_t ph;
        std::memcpy(&ph, &out.animPhase, 4);
        mix(ph);
      }
      // The draw-gate bit (out.animDrawn) is presentation output, not
      // gameplay state. The six canonical digests were blessed while
      // the 0x5414d4 frame latch was unmodeled (gate always 0); the
      // latch is now set per FUN_0042fb68, so folding the live bit
      // would shift every digest without a gameplay delta — verified
      // by pinning this slot and reproducing the canonical values.
      mix(0);
      mix(out.currentArenaSwapped ? 1 : 0);
      // Phase 5J — fold the deterministic look/view derived state
      // into the digest (offset, view yaw, effective pitch, the
      // z-delta follower and the pitch lift). No original bytes.
      for (float v : {out.lookOffsetDeg, out.viewYawDeg,
                      out.viewPitchDeg, out.viewZDelta,
                      out.viewPitchLift, out.viewScalar}) {
        std::uint32_t vbits;
        std::memcpy(&vbits, &v, 4);
        mix(vbits);
      }
      // Phase 5K — fold the derived camera pose into the digest:
      // position, basis rows, both 3x4 matrices, projection scalars
      // and the overhead/view-on-partner selects. Floats are mixed
      // as bit patterns — deterministic, no original bytes.
      {
        const mdk::PlayerCameraPose& c = out.camera;
        auto mixf = [&](float v) {
          std::uint32_t vbits;
          std::memcpy(&vbits, &v, 4);
          mix(vbits);
        };
        for (float v : c.pos) mixf(v);
        for (float v : c.back) mixf(v);
        for (float v : c.up) mixf(v);
        mixf(c.sinPitch);
        mixf(c.cosPitch);
        for (const auto& r : c.view)
          for (float v : r) mixf(v);
        for (const auto& r : c.basis)
          for (float v : r) mixf(v);
        for (float v : {c.scaleX, c.scaleY, c.scaleZ, c.modeZoom})
          mixf(v);
        mix(static_cast<std::uint64_t>(c.viewCX));
        mix(static_cast<std::uint64_t>(c.viewCY));
        mix(static_cast<std::uint64_t>(c.viewW));
        mix(static_cast<std::uint64_t>(c.viewH));
        mix(out.overheadViewActive ? 1 : 0);
        mix(out.viewOnPartner ? 1 : 0);
      }
      // Phase 5H — fold tr_alcmd VM derived state into the digest:
      // current-arena persisted PC/wait, cumulative instruction +
      // spawn counters, and a surface-state summary. No script bytes.
      if (rt.cur) {
        mix(static_cast<std::uint64_t>(rt.cur->script.pcImageOff));
        std::uint32_t wbits;
        std::memcpy(&wbits, &rt.cur->script.waitSeconds, 4);
        mix(wbits);
        mix(static_cast<std::uint64_t>(rt.cur->script.callDepth));
        mix(static_cast<std::uint64_t>(rt.cur->surface.opMaskA));
        mix(static_cast<std::uint64_t>(rt.cur->surface.opMaskB));
        for (int i = 0; i < mdk::kSurfaceSlots; ++i)
          mix(static_cast<std::uint64_t>(rt.cur->surface.handlerOff[i]));
      }
      mix(static_cast<std::uint64_t>(rt.scriptInsnTotal));
      mix(static_cast<std::uint64_t>(rt.scriptSpawned));
      mix(static_cast<std::uint64_t>(rt.arenas.empty()
                                        ? 0 : rt.arenas.size()));
    }
    const auto& s = rt.seams;
    std::printf("digest:    %016llx  (%d frames)\n",
                (unsigned long long)digest, framesRun);
    std::printf("seams:     stream=%d prepass=%d scriptObj=%d "
                "move=%d evlist=%d slide=%d mantle=%d timers=%d "
                "world=%d xworld=%d prof=%d tail=%d teleport=%d "
                "vsnap=%d migrations=%d t1=%d t3=%d portals=%d "
                "deep=%d overhead=%d camcol=%d\n",
                s.streamStageCalls, s.objectPrepass,
                s.scriptObjectCalls, s.scriptedMoveCalls,
                s.arenaEventListCalls, s.slideHelperCalls,
                s.mantleCalls, s.timersCalls, s.worldTickCalls,
                s.extraWorldTickCalls, s.profilerHooks,
                s.postTailCalls, s.teleportCalls,
                s.pendingViewSnaps, s.objectMigrations,
                s.type1Triggers, s.type3Prefetches,
                s.portalsCrossed, s.deepFloorFallbacks,
                s.overheadViewCalls, s.cameraObstructionCalls);
    std::printf("script:    runs=%d insn=%d spawned=%d diag=%d\n",
                rt.scriptRuns, rt.scriptInsnTotal, rt.scriptSpawned,
                static_cast<int>(rt.scriptDiag.size()));
    for (const std::string& m : rt.scriptDiag)
      std::printf("           ! %s\n", m.c_str());
    // QA-only (MDK_OBJDUMP=1): per-arena census of every live dynamic
    // object — class/name token, pos, connector state, collision
    // presence — for route-planning and spawn evidence.
    if (std::getenv("MDK_OBJDUMP") != nullptr) {
      for (const auto& tap : rt.arenas) {
        const mdk::TraversalArena& ta = *tap;
        int n = 0;
        for (const auto& up : ta.dyn.storage) {
          const mdk::DynamicObject& o = *up;
          const std::string tok = o.model.modelName();
          std::printf("OBJ %2d %-8s %-2d %-12s cls=%-10s pos=(%8.2f,%8.2f,"
                      "%8.2f) hp=%d f14a=%05x conn=%d cs=%02x"
                      " aabb=(%8.2f,%8.2f,%8.2f)-(%8.2f,%8.2f,%8.2f)"
                      " el=%d\n",
                      ta.index, ta.name.c_str(), n,
                      tok.empty() ? "-" : tok.c_str(),
                      o.scriptClass.empty() ? "-" : o.scriptClass.c_str(),
                      (double)o.pos[0], (double)o.pos[1],
                      (double)o.pos[2], o.health, o.col.flags14a,
                      o.connDest ? o.connDest->index : -1, o.connState,
                      (double)o.col.aabb[0], (double)o.col.aabb[1],
                      (double)o.col.aabb[2], (double)o.col.aabb[3],
                      (double)o.col.aabb[4], (double)o.col.aabb[5],
                      o.elemSet.count);
          ++n;
        }
      }
    }
    // Phase 17B.1 — traversal HUD core diagnostic: the bound-record
    // census (25 expected slots: 18 images, 4 sprite tables, SNIPERS2
    // stream layer, SNIPERS1 bezel, FONTBIG), the latches/gates the
    // compositor reads, and a deterministic FNV-1a64 digest over the
    // composed 600x360 overlay's pen indices (reconstructed output —
    // no proprietary bytes). Pixels only ever carry index values; the
    // digest is a content checksum, not a dump.
    {
      const mdk::TraversalHudState& h = rt.hud;
      int hudBound = 0, hudMiss = 0;
      const auto imgSlot = [&](const mdk::TraversalHudImage& im) {
        (im.px != nullptr && im.w > 0 && im.h > 0) ? ++hudBound
                                                 : ++hudMiss;
      };
      imgSlot(h.scStat); imgSlot(h.scBstat);
      imgSlot(h.snipRng); imgSlot(h.snipWep); imgSlot(h.snipTxt);
      imgSlot(h.skull);
      for (const auto& im : h.snipL) imgSlot(im);
      for (const auto& im : h.snipW) imgSlot(im);
      const auto sprSlot = [&](const mdk::FtiSprite& sp) {
        sp.frames.empty() ? ++hudMiss : ++hudBound;
      };
      sprSlot(h.cross); sprSlot(h.bombtarg);
      sprSlot(h.sniperga); sprSlot(h.pickups);
      h.overlayPx.empty() ? ++hudMiss : ++hudBound;   // SNIPERS2
      h.bezelPx.empty() ? ++hudMiss : ++hudBound;     // SNIPERS1
      h.fontBigOk ? ++hudBound : ++hudMiss;           // FONTBIG
      const std::uint64_t hudDg = mdk::fnv1a64(
          std::span<const std::byte>(
              reinterpret_cast<const std::byte*>(h.fb.pixels()),
              h.fb.pixelCount()));
      int hudNz = 0;
      for (std::size_t i = 0; i < h.fb.pixelCount(); ++i)
        hudNz += h.fb.pixels()[i] != 0 ? 1 : 0;
      std::printf(
          "hud:       lvl=%d bound=%d miss=%d vpm=%d scope=%d/%d "
          "hp=%d blink=%d wpn=%d/%d ammo=[%d,%d,%d,%d,%d,%d] "
          "inv=%d/%d/%d timer=%.0f/%.0f latch=%.0f ev=%.2f/%d "
          "skull=%d/%d/%03x seams=msg%d/flush%d/lut%d "
          "fb=%dx%d nz=%d dg=%016llx%s\n",
          rt.field541498, hudBound, hudMiss, s.hudViewportModes,
          rt.flagC9c, rt.transitionPhase, rt.fieldHealth,
          h.blinkPhase, rt.wpnSel0, rt.wpnSel1,
          rt.ammo[0], rt.ammo[1], rt.ammo[2], rt.ammo[3],
          rt.ammo[4], rt.ammo[5],
          rt.inventoryCount, rt.inventorySel, rt.invHudTimer,
          (double)rt.fadeTimer5414a0, (double)rt.fadeTimer5414a4,
          (double)rt.fadeTimer5414a8,
          (double)rt.eventTimer, rt.eventTimerObj ? 1 : 0,
          rt.fieldHealth == 0 ? 1 : 0, rt.fieldDac,
          (unsigned)rt.locoState,
          s.hudMsgPosts, s.hudMsgFlush, s.hudLutRemaps,
          h.fb.width(), h.fb.height(), hudNz,
          (unsigned long long)hudDg, h.bound ? "" : " (UNBOUND)");
    }
    // Phase 17C.1 — audio contract diagnostic: drained-event census +
    // a digest over (seq, op, name, pos). The digest pins event ORDER
    // + identity + position; ownerKey pointers are intentionally
    // excluded (address-dependent — nondeterministic across runs).
    std::printf(
        "audio:     ev=%zu play=%zu ensure=%zu restart=%zu stop=%zu "
        "pos=%zu repos=%zu release=%zu vol=%zu rate=%zu dg=%016llx\n",
        audioTotal, audioOps[0], audioOps[1], audioOps[2], audioOps[3],
        audioOps[4], audioOps[5], audioOps[6], audioOps[7], audioOps[8],
        (unsigned long long)audioDg);
    for (const mdk::TraversalAudioEvent& ev : audioLog) {
      std::printf("           af=%03d seq=%u %s %s%s%s",
                  ev.frame, ev.seq, mdk::traversalAudioOpName(ev.op),
                  ev.name.c_str(),
                  ev.owner == mdk::TraversalAudioOwner::kObject
                      ? " owner=obj"
                      : ev.owner == mdk::TraversalAudioOwner::kPlayer
                            ? " owner=player"
                            : ev.owner == mdk::TraversalAudioOwner::kZone
                                  ? " owner=zone"
                                  : "",
                  ev.hasPos ? " pos=" : "");
      if (ev.mode != 0) std::printf(" mode=%x", ev.mode);
      if (ev.hasPos)
        std::printf("(%.1f,%.1f,%.1f)", (double)ev.pos[0],
                    (double)ev.pos[1], (double)ev.pos[2]);
      std::printf("\n");
    }
    // Phase 15A — boss-runtime summary: end-state of each watched
    // object plus the completion-latch edges observed this run.
    if (!bossNames.empty() || !hitSpecs.empty() || sawEndLevel ||
        sawEnding) {
      std::printf("boss-runtime:\n");
      for (const auto& ap : rt.arenas) {
        if (!ap->dyn.storage.empty())
          std::printf("  [arena %s storage=%zu vars48=(%.2f,%.2f,%.2f,"
                      "%.2f) gVars=(%.2f,%.2f,%.2f,%.2f) flags58=%08x]\n",
                      ap->name.c_str(), ap->dyn.storage.size(),
                      ap->objVars48[0], ap->objVars48[1],
                      ap->objVars48[2], ap->objVars48[3],
                      rt.scriptGVars[0], rt.scriptGVars[1],
                      rt.scriptGVars[2], rt.scriptGVars[3],
                      ap->flags58);
        for (const auto& up : ap->dyn.storage) {
          const mdk::DynamicObject& o = *up;
          bool want = bossNames.empty() && !hitSpecs.empty()
                          ? (o.col.named != 0)
                          : false;
          for (const auto& nm : bossNames)
            want = want || bossMatches(o, nm.c_str());
          for (const auto& h : hitSpecs)
            want = want || bossMatches(o, h.objName.c_str());
          if (!want) continue;
          std::printf(
              "  %s model=%s arena=%s hp=%d sub=%02x mark=%02x "
              "pos=(%.1f,%.1f,%.1f) elemHp=[", o.scriptClass.c_str(),
              o.model.modelName().c_str(),
              o.arena && o.arena->owner ? o.arena->owner->name.c_str()
                                        : "?",
              (int)o.health, (unsigned)o.field11e,
              (unsigned)o.field21e,
              o.pos[0], o.pos[1], o.pos[2]);
          for (std::size_t i = 0; i < o.elemHp.size(); ++i)
            std::printf("%s%d", i ? "," : "", (int)o.elemHp[i]);
          auto pcOff = [&](const void* p) -> std::ptrdiff_t {
            const auto* b =
                static_cast<const std::byte*>(p);
            const auto* base = rt.level.cmiBytes.data();
            const auto* end = base + rt.level.cmiBytes.size();
            return (b >= base && b < end) ? b - base : -1;
          };
          std::printf("] pc=%p(+%lx) f230=%p f110=%p f148=%04x dead=%d "
                      "child=%s f14a=%02x f14b=%02x f14c=%02x "
                      "f149=%02x vel=(%.2f,%.2f,%.2f) f34=%.2f f38=%.2f "
                      "t12c=(%.1f,%.1f,%.1f) t120=(%.1f,%.1f,%.1f) "
                      "fEC=%p f2a0=%d f11a=%02x f138=%s\n",
                      o.field108,
                      static_cast<unsigned long>(
                          pcOff(o.field108)),
                      o.field230, o.field110,
                      (unsigned)o.col.flags148,
                      (o.col.flags148 & 0x20) ? 1 : 0,
                      o.field158 ? o.field158->scriptClass.c_str()
                                 : "-",
                      (unsigned)o.col.flags14a,
                      (unsigned)o.col.flags14b,
                      (unsigned)o.col.flags14c,
                      (unsigned)o.col.flags149,
                      (double)o.field28, (double)o.field2c,
                      (double)o.field30,
                      (double)o.field34, (double)o.field38,
                      (double)o.field12c[0], (double)o.field12c[1],
                      (double)o.field12c[2],
                      (double)o.field120[0], (double)o.field120[1],
                      (double)o.field120[2],
                      o.fieldEC, (int)o.field2a0,
                      (unsigned)o.field11a,
                      o.field138 ? o.field138->scriptClass.c_str()
                                 : "-");
        }
      }
      for (const auto& r : mdk::traversalSpawnLog())
        std::printf("  spawn %s name=%s arena=%s v%d pc=0x%x by=%s\n",
                    r.cls.c_str(), r.name.c_str(), r.arena.c_str(),
                    r.variant, r.spawnPc, r.spawner.c_str());
      std::printf(
          "  completion: endLevel=%d ending=%d vsnapPending=%d "
          "vsnaps=%d objDeathCalls=%d shotHits=%d\n",
          sawEndLevel ? 1 : 0, sawEnding ? 1 : 0,
          rt.pendingViewSnap == -1 ? 1 : 0,
          rt.seams.pendingViewSnaps, rt.seams.objectDeathCalls,
          rt.shotHitCount);
      if (std::getenv("MDK_COL_PROFILE") != nullptr) {
        const mdk::CollisionProfile& cp = mdk::collisionProfile();
        std::printf(
            "  colProfile: stab=%llu/%llun/%llup m1=%llu/%llun "
            "sweep=%llu/%lluit/%llun/%llup probe=%llu/%llut "
            "splash=%llu falloff=%llu\n",
            (unsigned long long)cp.stabCalls,
            (unsigned long long)cp.stabNodes,
            (unsigned long long)cp.stabPolyTests,
            (unsigned long long)cp.stabM1Calls,
            (unsigned long long)cp.stabM1Nodes,
            (unsigned long long)cp.sweepCalls,
            (unsigned long long)cp.sweepIters,
            (unsigned long long)cp.sweepNodes,
            (unsigned long long)cp.sweepPolyTests,
            (unsigned long long)cp.probeCalls,
            (unsigned long long)cp.probeTris,
            (unsigned long long)cp.splashCalls,
            (unsigned long long)cp.falloffCalls);
      }
    }
    // Bounded checks: gates + at least one grounded frame. Arenas
    // with their own MTO collision blob must also show a collision
    // contact. 'C*' corridor arenas carry no blob (OBSERVED: the MTO
    // has exactly the 10 HMO_* blocks) — their traversal is driven by
    // the per-arena CMI script VM (+0x220 -> FUN_004388d8, exposed as
    // the scriptObj seam) and the type-1/type-3/portal seams, so the
    // honest corridor check is that a partner attach fired; the
    // observed +0x44e failsafe catch (posZ <= -50) is reported but
    // not asserted — reaching it depends on start height / frames.
    // A crossed portal reclassifies the run: the start arena's
    // requirements are replaced by gates + post-swap partner state.
    // contactFlags bit1 is the OBSERVED *object*-floor flag
    // (collisionFloorProbe walks cs.arena->objects only) — reported
    // but not asserted: arenas without rideable objects never set it.
    const bool carrierGeom =
        rt.partner && rt.partner->geometryLoaded;
    bool ok;
    if (s.portalsCrossed > 0) {
      // A portal swap is itself the demonstrated traversal: the run
      // left the start arena through the proven type-6 path, so the
      // start arena's contact requirement no longer applies — the
      // honest assertions are valid gates plus the observed
      // post-swap ca4=dest partner slot.
      ok = rt.cs.arenaValid && partnerSeen;
    } else if (startArena->geometryLoaded) {
      ok = rt.cs.arenaValid && contactSeen && groundedSeen;
    } else {
      ok = rt.cs.arenaValid && partnerSeen &&
           (s.type1Triggers > 0 || s.type3Prefetches > 0);
    }
    std::printf("checks:    geom=%d gates=%d contact=%d grounded=%d "
                "airborne=%d floor-obj=%d partner=%d car-vld=%d "
                "car-bsy=%d car-geom=%d — %s\n",
                startArena->geometryLoaded ? 1 : 0, rt.cs.arenaValid,
                contactSeen ? 1 : 0, groundedSeen ? 1 : 0,
                airborneSeen ? 1 : 0, floorObjSeen ? 1 : 0,
                partnerSeen ? 1 : 0, rt.cs.carrierValid ? 1 : 0,
                rt.cs.carrierBusy ? 1 : 0, carrierGeom ? 1 : 0,
                ok ? "PASS" : "FAIL");
    // --save-write-full: snapshot the live runtime as a full-save
    // stream (mid-fight goldens — a restore + continue run proves
    // the counter/bit/pc state carries). The GAME packet needs a
    // session row: level id comes from the dir's LEVEL<n> name via
    // the documented 0x4999e8 inverse, deathCount stays 0, and the
    // charge latch carries verbatim from the runtime.
    if (saveWriteFullPath) {
      int levelDir = -1;
      if (const char* p = std::strstr(dtiPath.c_str(), "LEVEL"))
        levelDir = static_cast<int>(std::strtol(p + 5, nullptr, 10));
      int levelId = -1;
      for (int i = 0; i < 8; ++i)
        if (mdk::progressionLevelDir(i) == levelDir) levelId = i;
      if (levelId < 0) {
        std::fprintf(stderr,
                     "write-full: no campaign id for LEVEL%d\n",
                     levelDir);
        return 1;
      }
      mdk::ProgressionSession sess;
      sess.mode = 3;
      sess.levelId = levelId;
      sess.field54163b = rt.field54163b;
      mdk::SaveWriteFullInput win;
      win.seed = static_cast<std::uint16_t>(ffSeed & 0xffff);
      mdk::FullWriteReport wrep;
      std::string werr;
      const auto bytes =
          mdk::saveGameWriteFull(rt, sess, win, &wrep, &werr);
      if (bytes.empty()) {
        std::fprintf(stderr, "write-full: %s\n", werr.c_str());
        return 1;
      }
      FILE* fp = std::fopen(saveWriteFullPath->c_str(), "wb");
      if (fp == nullptr) {
        std::fprintf(stderr, "write-full: cannot open %s\n",
                     saveWriteFullPath->c_str());
        return 1;
      }
      std::fwrite(bytes.data(), 1, bytes.size(), fp);
      std::fclose(fp);
      std::printf("write-full: %s (%zu bytes, arenas=%d objs=%d "
                  "fans=%d ids=%d levelId=%d)\n",
                  saveWriteFullPath->c_str(), bytes.size(),
                  wrep.arenasWritten, wrep.objectsWritten,
                  wrep.fansWritten, wrep.saveIds, levelId);
      for (const std::string& w : wrep.warnings)
        std::printf("             ! %s\n", w.c_str());
    }
    return ok ? 0 : 1;
  }

  // --freefall-runtime: Phase 13A FALL3D diagnostic. Reads the
  // FALLPU_<course+1> pickup record from the target FALL3D.BNI,
  // initializes the freefall core with --course/--skill/--seed and
  // steps --frames frames at the standard 30 fps frame model,
  // printing authoritative state and a determinism digest. FALLPU
  // entries are 12-byte {name[8], u32} records terminated by a NUL
  // first name byte (OBSERVED: FUN_0040ef28 count loop + the BNI
  // census in docs/reverse-engineering/EXECUTABLE_MAP.md).
  if (freefallRuntime) {
    const auto bniFile = root->readFile(*target, kEntriesMaxBytes, &err);
    if (!bniFile) {
      std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
      return 1;
    }
    mdk::FreefallCourseData course;
    course.course = ffCourse;
    course.skill = ffSkill;
    const auto dir = mdk::inspectBniDirectory(
        std::span<const std::byte>(bniFile->data(), bniFile->size()));
    if (dir.status == mdk::BniDirectoryStatus::kOk) {
      char recName[16];
      std::snprintf(recName, sizeof recName, "FALLPU_%d", ffCourse + 1);
      if (const mdk::BniRecord* rec = mdk::findBniRecord(dir, recName)) {
        const std::byte* p = bniFile->data() + rec->payloadFileOffset;
        const std::byte* end = bniFile->data() + rec->payloadEnd;
        for (; p + 12 <= end; p += 12) {
          if (p[0] == std::byte{0}) break;  // terminator entry
          mdk::FreefallPickupRec r{};
          for (int k = 0; k < 8; ++k)
            r.name[k] = static_cast<char>(p[k]);
          r.name[8] = '\0';
          course.pickups.push_back(r);
        }
      } else {
        std::printf("fallpu:    %s — NOT FOUND (no pickups)\n",
                    recName);
      }
    } else {
      std::printf("bni:       %s — %s (no pickups)\n",
                  std::string(mdk::bniDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
    }

    mdk::FreefallRuntime rt;
    mdk::freefallInit(rt, course, ffSeed);
    std::printf("freefall:  course=%d skill=%d seed=%08x pickups=%zu "
                "bones=%d\n",
                ffCourse, ffSkill, ffSeed, course.pickups.size(),
                rt.bonesCourse ? 1 : 0);
    std::printf("difficulty: wave=%d speed=%.4f wander=%.4f delay=%d "
                "radar-delay=%d\n",
                rt.waveSize, (double)rt.radarSpeed, (double)rt.wanderScale,
                rt.missileDelay, rt.radarDelay);

    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        digest ^= (v >> (i * 8)) & 0xff;
        digest *= 1099511628211ull;
      }
    };
    int framesRun = 0;
    bool done = false;
    for (int f = 0; f < travFrames && !done; ++f) {
      mdk::FreefallInput in{};  // idle — scripted-input seam TODO
      done = mdk::freefallStep(rt, in, 1, 1.0f, 1.0f / 30.0f);
      ++framesRun;
      const mdk::FreefallObject* pl =
          rt.listHead >= 0 ? &rt.pool[rt.listHead] : nullptr;
      int counts[6] = {};
      for (int i = rt.listHead; i >= 0; i = rt.pool[i].next)
        if (rt.pool[i].type >= 0 && rt.pool[i].type < 6)
          ++counts[rt.pool[i].type];
      std::printf(
          "f=%03d ph=%d t=%6.2f hp=%3d fade=%5.2f pl=(%7.2f,%7.2f,%8.2f) "
          "cam=(%7.2f,%7.2f,%8.2f) obj 0-5=%d/%d/%d/%d/%d/%d ev=%zu\n",
          f, static_cast<int>(rt.phase), (double)rt.timeline, rt.health,
          (double)rt.fade,
          pl ? (double)pl->px : 0.0, pl ? (double)pl->py : 0.0,
          pl ? (double)pl->pz : 0.0,
          (double)rt.cameraPos[0], (double)rt.cameraPos[1],
          (double)rt.cameraPos[2],
          counts[0], counts[1], counts[2], counts[3], counts[4],
          counts[5], rt.events.size());
      for (const auto& e : rt.events)
        std::printf("      ev kind=%d a=%d b=%d\n", e.kind, e.a, e.b);
      mix(static_cast<std::uint64_t>(rt.rng));
      std::uint32_t tb;
      std::memcpy(&tb, &rt.timeline, 4);
      mix(tb);
      mix(static_cast<std::uint64_t>(rt.health));
      for (int i = rt.listHead; i >= 0; i = rt.pool[i].next) {
        const mdk::FreefallObject& o = rt.pool[i];
        mix(static_cast<std::uint64_t>(o.type) << 16 |
            static_cast<std::uint64_t>(static_cast<std::uint16_t>(o.timer)));
        std::uint32_t bits;
        std::memcpy(&bits, &o.px, 4);
        mix(bits);
        std::memcpy(&bits, &o.py, 4);
        mix(bits);
        std::memcpy(&bits, &o.pz, 4);
        mix(bits);
      }
    }
    std::printf("frames:    %d  finished=%d died=%d digest=%016llx\n",
                framesRun, rt.finished ? 1 : 0, rt.died ? 1 : 0,
                (unsigned long long)digest);
    return 0;
  }

  // --campaign-handoff: Phase 13B diagnostic. Fast-forwards the
  // freefall course to completion (same 30 fps step model as
  // --freefall-runtime, silent), then drives the native
  // freefall→traversal handoff: FUN_0040fa68 teardown, the health
  // branch, and FUN_004346e8/FUN_00433d40 for the mapped level —
  // or the frontend route when health <= 0. On the success route
  // one traversal frame runs so its deterministic state joins the
  // digest.
  if (campaignHandoff) {
    mdk::FreefallCourseData course;
    course.course = ffCourse;
    course.skill = ffSkill;
    const bool fallpu =
        inspectReadFallpu(*root, *target, course, &err);
    mdk::FreefallRuntime ff;
    mdk::freefallInit(ff, course, ffSeed);
    std::printf("freefall:  course=%d skill=%d seed=%08x "
                "pickups=%zu fallpu=%s\n",
                ffCourse, ffSkill, ffSeed, course.pickups.size(),
                fallpu ? "ok" : err.c_str());
    int framesRun = 0;
    bool done = false;
    for (int f = 0; f < travFrames && !done; ++f) {
      done = mdk::freefallStep(ff, mdk::FreefallInput{}, 1, 1.0f,
                               1.0f / 30.0f);
      ++framesRun;
    }
    std::printf("           frames=%d finished=%d died=%d health=%d "
                "events=%zu\n",
                framesRun, ff.finished ? 1 : 0, ff.died ? 1 : 0,
                ff.health, ff.events.size());

    mdk::ProgressionSession sess;
    mdk::progressionNewGame(sess, ffSkill);
    mdk::progressionEnterFreefall(sess);
    // The orchestrator owns 541498 (new game = 0); the diag selects
    // the course directly for mid-campaign starts.
    sess.levelId = ffCourse;
    mdk::TraversalRuntime trav;
    mdk::ProgressionHandoff ho;
    const auto pe = mdk::progressionFreefallHandoff(
        *root, sess, ff, &trav, ho, &err);
    std::printf("handoff:   err=%s route=%s mode=%d level=%d dir=%d\n",
                mdk::progressionErrorName(pe),
                ho.route == mdk::ProgressionRoute::kTraversal
                    ? "traversal" : "frontend",
                sess.mode, ho.levelId, ho.traversalDir);
    std::printf("           health=%d->%d skill=%d rng=%08x "
                "ammo-grants=%d teardown-list=%d bones=%d\n",
                ho.healthBefore, ho.healthAfter, ho.skill, ho.rng,
                ho.ammoGranted, ff.listHead, ff.bonesIdx);

    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        digest ^= (v >> (i * 8)) & 0xff;
        digest *= 1099511628211ull;
      }
    };
    mix(static_cast<std::uint64_t>(sess.mode));
    mix(static_cast<std::uint64_t>(ho.levelId));
    mix(static_cast<std::uint64_t>(ho.traversalDir < 0
                                       ? 0xffff : ho.traversalDir));
    mix(static_cast<std::uint64_t>(ho.healthBefore));
    mix(static_cast<std::uint64_t>(ho.healthAfter));
    mix(static_cast<std::uint64_t>(ho.rng));

    if (pe == mdk::ProgressionError::kOk &&
        ho.route == mdk::ProgressionRoute::kTraversal) {
      std::printf("traversal: dti=%s load=%s\n", ho.dtiPath.c_str(),
                  mdk::traversalLoadErrorName(ho.loadError));
      if (ho.loadError == mdk::TraversalLoadError::kOk) {
        std::printf(
            "           arenas=%zu spawn=%d cur=%s "
            "pos=(%.2f,%.2f,%.2f) yaw=%.2f\n",
            trav.arenas.size(), ho.spawnArena,
            trav.cur ? trav.cur->name.c_str() : "?",
            (double)ho.spawnPos[0], (double)ho.spawnPos[1],
            (double)ho.spawnPos[2], (double)ho.spawnYawDeg);
        std::printf(
            "           carried health=%d ammo=[%d %d %d %d %d %d] "
            "wpn=(%d %d %d %.1f)\n",
            trav.fieldHealth, trav.ammo[0], trav.ammo[1],
            trav.ammo[2], trav.ammo[3], trav.ammo[4], trav.ammo[5],
            trav.wpnSel0, trav.wpnSel1, trav.burstIndex,
            (double)trav.fireCadence);
        const mdk::GameplayInputBindings bindings;
        const mdk::FrontendTimingState timing;
        const auto fr = mdk::stepTraversalRuntime(
            trav, mdk::RawGameplayInput{}, bindings, timing);
        std::printf(
            "trav-f0:   arena=%d pos=(%.2f,%.2f,%.2f) yaw=%.2f "
            "grounded=%d loco=%02x\n",
            fr.curArenaIndex, (double)fr.pos[0], (double)fr.pos[1],
            (double)fr.pos[2], (double)fr.yawDeg,
            fr.grounded ? 1 : 0, fr.locoState);
        mix(static_cast<std::uint64_t>(fr.curArenaIndex));
        for (int i = 0; i < 3; ++i) {
          std::uint32_t bits;
          std::memcpy(&bits, &fr.pos[i], 4);
          mix(bits);
        }
        std::uint32_t bits;
        std::memcpy(&bits, &fr.yawDeg, 4);
        mix(bits);
        mix(fr.grounded ? 1 : 0);
        mix(static_cast<std::uint64_t>(fr.locoState));
      }
    }
    std::printf("digest:    %016llx\n", (unsigned long long)digest);
    return pe == mdk::ProgressionError::kOk ? 0 : 1;
  }

  // --surface-census: Phase 5F BUILD_A smoke. Reads the .DTI target
  // plus the sibling <stem>O.MTO and censuses the surface-contact data
  // the dispatcher (FUN_0040b5d0) and the volume/slide-zone records
  // consume: the poly surface byte (+0x23) and flag bits (+0x20) across
  // each arena's region-C collision blob, and the type-7 (fan/volume) /
  // type-9 (slide-zone) DTI sub-records. Metadata only — no payloads.
  if (scriptDisasm) {
    // Phase 5H — decode one CMI table-3 arena script record
    // (FUN_00458550 lookup) and print its tr_alcmd instructions.
    const auto cmiFile = root->readFile(*target, kEntriesMaxBytes, &err);
    if (!cmiFile) {
      std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
      return 1;
    }
    std::span<const std::byte> img(
        reinterpret_cast<const std::byte*>(cmiFile->data()),
        cmiFile->size());
    const auto cmi = mdk::inspectCmiDirectory(img);
    if (cmi.status != mdk::CmiDirectoryStatus::kOk) {
      std::fprintf(stderr, "cmi: parse FAILED (%s)\n",
                   std::string(mdk::cmiDirectoryStatusName(cmi.status))
                       .c_str());
      return 1;
    }
    std::uint32_t code =
        mdk::cmiScriptCodeOffset(cmi, img, scriptDisasmName);
    const char* codeKind = "t3";
    if (scriptDisasmOff) {
      code = *scriptDisasmOff;
      codeKind = "raw";
    }
    std::printf("cmi:   %s — %zu tables, %zu t3 records\n",
                target->c_str(), cmi.tables.size(),
                cmi.tables.size() > 3 ? cmi.tables[3].records.size()
                                      : 0);
    // "*" lists every table-3 record's resolved script offset.
    if (scriptDisasmName == "*") {
      if (cmi.tables.size() > 3)
        for (const auto& r : cmi.tables[3].records)
          std::printf("  t3 %-12s codeOff=0x%x\n", r.name().c_str(),
                      mdk::cmiScriptCodeOffset(cmi, img, r.name()));
      return 0;
    }
    if (code == 0 && scriptDisasmName.find('$') != std::string::npos) {
      // "ARENA$OBJ" — table-2 object-init record (value is the code
      // offset directly; OBSERVED FUN_004566f0) then table-0.
      code = mdk::cmiObjectScriptOffset(cmi, scriptDisasmName);
      codeKind = "t2";
      if (code == 0 && !cmi.tables.empty()) {
        for (const auto& r : cmi.tables[0].records) {
          if (r.name() == scriptDisasmName) {
            code = r.value;
            codeKind = "t0";
            break;
          }
        }
      }
    }
    std::printf("script: %s  codeOff=0x%x  (%s)\n",
                scriptDisasmName.c_str(), code, codeKind);
    if (code == 0) {
      std::printf("  (no script record / zero code offset — the "
                  "+0x220 gate stays cleared)\n");
      return 0;
    }
    const auto insns =
        mdk::traversalScriptDisasm(img, 4, code, 256);
    for (const auto& in : insns) {
      std::printf("  +%04x  %02x  %s\n",
                  in.off - code, in.opcode, in.text.c_str());
    }
    return 0;
  }

  if (objScriptDisasm) {
    // Phase 12A — disassemble every CMI table-0 object script
    // ("%s$%s_%u" records; code offset = value + 4, OBSERVED
    // FUN_00456808) for the projectile-family census.
    const auto cmiFile = root->readFile(*target, kEntriesMaxBytes, &err);
    if (!cmiFile) {
      std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
      return 1;
    }
    std::span<const std::byte> img(
        reinterpret_cast<const std::byte*>(cmiFile->data()),
        cmiFile->size());
    const auto cmi = mdk::inspectCmiDirectory(img);
    if (cmi.status != mdk::CmiDirectoryStatus::kOk) {
      std::fprintf(stderr, "cmi: parse FAILED (%s)\n",
                   std::string(mdk::cmiDirectoryStatusName(cmi.status))
                       .c_str());
      return 1;
    }
    std::printf("cmi:   %s — %zu t0 records\n",
                target->c_str(),
                cmi.tables.empty() ? 0 : cmi.tables[0].records.size());
    if (cmi.tables.empty()) return 0;
    for (const auto& rec : cmi.tables[0].records) {
      const std::uint64_t fo = 4 + static_cast<std::uint64_t>(rec.value);
      if (fo >= img.size()) {
        std::printf("=== %s  codeOff=0x%x  OUT OF BOUNDS\n",
                    rec.name().c_str(), rec.value);
        continue;
      }
      std::printf("=== %s  codeOff=0x%x\n", rec.name().c_str(),
                  rec.value);
      const auto insns =
          mdk::traversalScriptDisasm(img, 4, rec.value, 256);
      for (const auto& in : insns) {
        std::printf("  +%04x  %02x  %s\n",
                    in.off - rec.value, in.opcode, in.text.c_str());
      }
    }
    return 0;
  }

  if (surfaceCensus) {
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--surface-census wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string mtoPath = dir + stem + "O.MTO";

    const auto dtiFile = root->readFile(dtiPath, kEntriesMaxBytes, &err);
    const auto mtoFile = root->readFile(mtoPath, kEntriesMaxBytes, &err);
    if (!dtiFile || !mtoFile) {
      std::fprintf(stderr, "read-file: FAILED (%s) — need .DTI + "
                           "sibling <stem>O.MTO\n",
                   err.c_str());
      return 1;
    }
    const auto dti = mdk::inspectDtiStructure(
        std::span<const std::byte>(dtiFile->data(), dtiFile->size()));
    const auto mto = mdk::inspectMtoDirectory(
        std::span<const std::byte>(mtoFile->data(), mtoFile->size()));
    if (dti.status != mdk::DtiStructureStatus::kOk ||
        mto.status != mdk::MtoDirectoryStatus::kOk) {
      std::fprintf(stderr, "parse: FAILED (dti=%s mto=%s)\n",
                   std::string(mdk::dtiStructureStatusName(dti.status))
                       .c_str(),
                   std::string(mdk::mtoDirectoryStatusName(mto.status))
                       .c_str());
      return 1;
    }
    std::printf("dti:   %s — %zu arenas\n", dtiPath.c_str(),
                dti.arenas.size());
    std::printf("mto:   %s — %u blocks\n", mtoPath.c_str(), mto.count);

    // DTI: the type-7 (fan/volume) + type-9 (slide-zone) sub-records.
    int tot7 = 0, tot9 = 0;
    for (const auto& arec : dti.arenas) {
      int t7 = 0, t9 = 0;
      for (const auto& sr : arec.subRecords) {
        if (sr.type == 7) {
          ++t7;
        } else if (sr.type == 9) {
          ++t9;
        }
      }
      tot7 += t7;
      tot9 += t9;
      if (t7 || t9) {
        std::printf("  arena %-8.8s  type7=%d type9=%d subs=%u\n",
                    arec.name().c_str(), t7, t9, arec.subRecordCount);
      }
    }
    std::printf("dti-total:  type7=%d type9=%d\n", tot7, tot9);

    // MTO: per-block region-C collision blob -> poly surface census.
    const std::uint8_t* mb =
        reinterpret_cast<const std::uint8_t*>(mtoFile->data());
    const std::size_t mn = mtoFile->size();
    std::uint32_t surfHist[17] = {};  // [0]=none, [1..16]=surface id
    std::uint32_t over16 = 0;         // surface byte beyond the 16 slots
    std::uint32_t f04 = 0, f10 = 0, f20 = 0, f30 = 0;
    std::size_t totPolys = 0, surfPolys = 0, blobs = 0;
    for (std::size_t bi = 0; bi < mto.blocks.size(); ++bi) {
      const auto& b = mto.blocks[bi];
      mdk::CollisionArena arena;
      std::uint32_t counts[4] = {};
      if (b.regionCOffset >= mn) {
        continue;
      }
      if (!mdk::collisionBlobParse(mb + b.regionCOffset,
                                   mn - b.regionCOffset, &arena,
                                   counts)) {
        continue;
      }
      ++blobs;
      std::size_t blkSurf = 0;
      for (std::uint32_t p = 0; p < counts[2]; ++p) {
        const mdk::CollisionPoly& poly = arena.polys[p];
        const std::uint8_t s = poly.surface;
        const std::uint16_t f = poly.flags;
        if (s <= 16) {
          ++surfHist[s];
        } else {
          ++over16;
        }
        if (s != 0) {
          ++blkSurf;
        }
        if (f & 0x04) ++f04;
        if (f & 0x10) ++f10;
        if (f & 0x20) ++f20;
        if (f & 0x30) ++f30;
        ++totPolys;
      }
      surfPolys += blkSurf;
      if (blkSurf) {
        const std::string nm =
            bi < mto.entries.size() ? mto.entries[bi].name() : "?";
        std::printf("  block %-8.8s  polys=%u surface=%zu\n", nm.c_str(),
                    counts[2], blkSurf);
      }
    }
    std::printf("mto-total:  blobs=%zu polys=%zu surface-polys=%zu\n",
                blobs, totPolys, surfPolys);
    std::printf("surface-id histogram (byte +0x23 = id, 0 = none):\n");
    for (int i = 1; i <= 16; ++i) {
      if (surfHist[i]) {
        std::printf("  id %2d: %u polys\n", i, surfHist[i]);
      }
    }
    if (over16) {
      std::printf("  out-of-domain (>16): %u polys\n", over16);
    }
    std::printf("poly flag bits (+0x20, all %zu polys): "
                "0x04=%u 0x10=%u 0x20=%u 0x30=%u\n",
                totPolys, f04, f10, f20, f30);
    return 0;
  }

  // --arena-render: Phase 6A (G1-RE) smoke. Decodes each MTO block's
  // region-C geometry into the platform-neutral render-data view:
  // shared collision tables, per-vertex UVs, the material name table,
  // the embedded .MAT bank resolved against the level's shared MTI
  // bank (bank A first — the original's matlkup order), the palette
  // triplets, and the FUN_00409a6c BSP submission order.
  if (arenaRender) {
    const std::string& dtiPath = *target;
    const auto dot = dtiPath.find_last_of('.');
    const auto slash = dtiPath.find_last_of("/\\");
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash)) {
      std::fprintf(stderr, "--arena-render wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    const std::string mtoPath = dir + stem + "O.MTO";
    const std::string mtiPath = dir + stem + "S.MTI";

    const auto dtiFile = root->readFile(dtiPath, kEntriesMaxBytes, &err);
    const auto mtoFile = root->readFile(mtoPath, kEntriesMaxBytes, &err);
    if (!dtiFile || !mtoFile) {
      std::fprintf(stderr, "read-file: FAILED (%s) — need .DTI + "
                           "sibling <stem>O.MTO\n",
                   err.c_str());
      return 1;
    }
    // The shared bank is optional in the file set (all BUILD_A levels
    // have one); a missing/unreadable file decodes as an empty bank.
    std::vector<std::byte> mtiStorage;
    std::span<const std::byte> mtiSpan;
    if (auto mtiFile = root->readFile(mtiPath, kEntriesMaxBytes, &err)) {
      mtiStorage = std::move(*mtiFile);
      mtiSpan = std::span<const std::byte>(mtiStorage.data(),
                                           mtiStorage.size());
    }

    const auto mto = mdk::inspectMtoDirectory(
        std::span<const std::byte>(mtoFile->data(), mtoFile->size()));
    if (mto.status != mdk::MtoDirectoryStatus::kOk) {
      std::fprintf(stderr, "parse: FAILED (mto=%s)\n",
                   std::string(mdk::mtoDirectoryStatusName(mto.status))
                       .c_str());
      return 1;
    }
    std::printf("mto: %s — %u blocks; shared bank %s (%zu bytes)\n",
                mtoPath.c_str(), mto.count, mtiPath.c_str(),
                mtiSpan.size());

    const std::uint8_t* mb =
        reinterpret_cast<const std::uint8_t*>(mtoFile->data());
    const std::size_t mn = mtoFile->size();
    const auto fnv1a = [](std::uint64_t h, const void* p,
                          std::size_t n) {
      const auto* b = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ b[i]) * 0x100000001b3ull;
      }
      return h;
    };

    std::size_t blocksOk = 0;
    for (std::size_t bi = 0; bi < mto.blocks.size(); ++bi) {
      const auto& b = mto.blocks[bi];
      const std::string blkName =
          bi < mto.entries.size() ? mto.entries[bi].name() : "?";
      if (travArena && blkName != *travArena) continue;
      mdk::CollisionArena arena;
      std::uint32_t counts[4] = {};
      if (b.regionCOffset >= mn ||
          !mdk::collisionBlobParse(mb + b.regionCOffset,
                                   mn - b.regionCOffset, &arena,
                                   counts)) {
        std::printf("  block %-8.8s  collision blob: PARSE FAILED\n",
                    blkName.c_str());
        continue;
      }
      mdk::ArenaRenderData rd;
      if (!mdk::arenaRenderDataBuild(
              std::span<const std::byte>(mtoFile->data(), mn), b, arena,
              counts[1], counts[2], counts[3], mtiSpan, &rd)) {
        std::printf("  block %-8.8s  render data: BUILD FAILED\n",
                    blkName.c_str());
        continue;
      }
      ++blocksOk;

      // Vertex bounds + digest over the decoded view.
      float bb[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
      std::uint64_t h = 0xcbf29ce484222325ull;
      h = fnv1a(h, rd.verts, std::size_t(rd.vertCount) * 12);
      h = fnv1a(h, rd.polys.data(),
                rd.polys.size() * sizeof(mdk::ArenaRenderPoly));
      for (std::size_t p = 0; p < rd.vertCount; ++p) {
        for (int k = 0; k < 3; ++k) {
          const float v = rd.verts[p * 3 + k];
          if (v < bb[k]) bb[k] = v;
          if (v > bb[k + 3]) bb[k + 3] = v;
        }
      }

      // Material resolution census.
      std::uint32_t resolved = 0, missing = 0;
      std::string missingList;
      for (std::size_t i = 0; i < rd.materialOfName.size(); ++i) {
        if (rd.materialOfName[i] >= 0) {
          ++resolved;
        } else {
          ++missing;
          if (missingList.size() < 200) {
            if (!missingList.empty()) missingList += ',';
            missingList += rd.materialNames[i];
          }
        }
      }
      std::uint32_t cls[6] = {};
      std::uint32_t skipped = 0, altSpan = 0, edgeOverlay = 0,
                    edgeMasked = 0;
      for (std::size_t p = 0; p < rd.polys.size(); ++p) {
        if (rd.polys[p].flags & mdk::kArenaPolySkip) ++skipped;
        if (rd.polys[p].flags & mdk::kArenaPolyAltSpan) ++altSpan;
        if (rd.polys[p].aux22 & mdk::kArenaEdgeOverlay) ++edgeOverlay;
        if (rd.polys[p].aux22 & 0x70) ++edgeMasked;
        using mdk::ArenaMatClass;
        switch (rd.polyMaterialClass(p)) {
        case ArenaMatClass::kTextured: ++cls[0]; break;
        case ArenaMatClass::kUnresolved: ++cls[1]; break;
        case ArenaMatClass::kPen: ++cls[2]; break;
        case ArenaMatClass::kEffect770: ++cls[3]; break;
        case ArenaMatClass::kEffectE94: ++cls[4]; break;
        case ArenaMatClass::kEffect12970: ++cls[5]; break;
        }
      }

      // Render-order digest at the probe camera — live order is
      // painter's back-to-front (DAT_00499f8c = 1, never written).
      float cam[3];
      if (travStartGiven) {
        cam[0] = travStart[0];
        cam[1] = travStart[1];
        cam[2] = travStart[2];
      } else {
        for (int k = 0; k < 3; ++k) cam[k] = (bb[k] + bb[k + 3]) * 0.5f;
      }
      std::vector<std::uint32_t> order;
      mdk::arenaRenderOrder(arena, cam, false, &order);
      std::uint64_t oh = 0xcbf29ce484222325ull;
      oh = fnv1a(oh, order.data(), order.size() * sizeof(std::uint32_t));

      std::printf(
          "  block %-8.8s  verts=%u nodes=%u polys=%zu names=%zu\n"
          "    aabb=[%.1f %.1f %.1f .. %.1f %.1f %.1f]\n"
          "    bankA=%zu bankB=%zu names resolved=%u missing=%u\n"
          "    polys textured=%u unresolved=%u pen=%u fx770=%u "
          "fxe94=%u fx12970=%u skip-flag=%u\n"
          "    flags alt-span(bit0)=%u edge-overlay(b7)=%u "
          "edge-mask(0x70)=%u\n"
          "    palette=%zuB digest=%016llx order n=%zu digest=%016llx\n",
          blkName.c_str(), rd.vertCount, rd.nodeCount, rd.polys.size(),
          rd.materialNames.size(), bb[0], bb[1], bb[2], bb[3], bb[4],
          bb[5], rd.bankA.size(), rd.bankB.size(), resolved, missing,
          cls[0], cls[1], cls[2], cls[3], cls[4], cls[5], skipped,
          altSpan, edgeOverlay, edgeMasked,
          rd.paletteRgb.size(), (unsigned long long)h, order.size(),
          (unsigned long long)oh);
      if (missing) {
        std::printf("    missing names: %s%s\n", missingList.c_str(),
                    missingList.size() >= 200 ? "..." : "");
      }
    }
    std::printf("arena-render: %zu/%zu blocks decoded\n", blocksOk,
                mto.blocks.size());
    return 0;
  }

  // --collision-census: QA floor-support audit over every arena of a
  // .DTI level (sibling .CMI + <stem>O.MTO loaded too). For each arena:
  //   * flag census — poly +0x20 byte: bit 0x10 = render skip,
  //     bit 0x20 = collision skip. A floor-ish poly that renders but
  //     doesn't collide is the "visible floor / no support" escape
  //     mechanism (and the converse is an invisible wall).
  //   * object census — named/modelled objects, standable flags, and
  //     elemMaskB-masked element counts.
  //   * support grid — XY cells over the collision-vert bbox; a
  //     standing downward collisionApply + collisionFloorProbe at each
  //     cell, classified BSP / object / none. Interior no-support
  //     cells ringed by supported cells are escape holes.
  //   * per no-support cell, whether a rendered floor-ish poly or a
  //     standable object element covers the cell (expected-vs-actual
  //     owner disagreement).
  // Each arena is attached via the diagnostic entry and settled 45
  // frames so runtime-spawned and mover objects populate +0x68; the
  // census then measures the resulting collision state.
  if (collisionCensus) {
    const std::string dtiPath = *target;
    const auto slash = dtiPath.find_last_of("/\\");
    const auto dot = dtiPath.find_last_of('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--collision-census wants a .DTI path\n");
      return 1;
    }
    const std::string dir =
        slash == std::string::npos ? "" : dtiPath.substr(0, slash + 1);
    const std::string stem = dtiPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        dot - (slash == std::string::npos ? 0 : slash + 1));
    mdk::TraversalRuntime rt;
    const auto le = mdk::traversalRuntimeLoad(
        *root, dtiPath, dir + stem + ".CMI", dir + stem + "O.MTO", rt,
        &err);
    if (le != mdk::TraversalLoadError::kOk) {
      std::fprintf(stderr, "traversal-load: FAILED (%s: %s)\n",
                   mdk::traversalLoadErrorName(le), err.c_str());
      return 1;
    }
    if (rt.fieldHealth <= 0) rt.fieldHealth = 150;

    std::printf("collision-census: %s — %zu arenas\n",
                dtiPath.c_str(), rt.arenas.size());
    const mdk::GameplayInputBindings cBinds;
    mdk::FrontendTimingState cTiming;
    for (const auto& a : rt.arenas) {
      // Attach + settle so spawned/mover objects populate +0x68.
      // Idle input — the census measures the collision state, not
      // the player path.
      {
        float cp[3] = {0.f, 0.f, 0.f};
        mdk::traversalRuntimeDiagnosticStart(rt, a->index, cp, 0.f,
                                             nullptr);
        for (int f = 0; f < 45; ++f) {
          mdk::RawGameplayInput ri{};
          mdk::stepTraversalRuntime(rt, ri, cBinds, cTiming);
          if (rt.cur != a.get()) break;   // script pulled us elsewhere
        }
      }
      const mdk::CollisionArena& col = a->dyn.col;
      const int npoly = static_cast<int>(a->surface.polyCount);
      if (col.verts == nullptr || npoly == 0) {
        std::printf("  arena[%2d] %-9s no collision geometry\n",
                    a->index, a->name.c_str());
        continue;
      }
      // -- flag census ------------------------------------------------
      int colSkip = 0, rendSkip = 0, floorish = 0, escape = 0,
          invisWall = 0;
      float bmin[3] = {1e30f, 1e30f, 1e30f};
      float bmax[3] = {-1e30f, -1e30f, -1e30f};
      struct FloorHole { int poly; float cx, cy, cz; };
      std::vector<FloorHole> holes;
      for (int p = 0; p < npoly; ++p) {
        const mdk::CollisionPoly& pl = col.polys[p];
        const float* v0 = col.verts + pl.v[0] * 3;
        const float* v1 = col.verts + pl.v[1] * 3;
        const float* v2 = col.verts + pl.v[2] * 3;
        const float ux = v1[0] - v0[0], uy = v1[1] - v0[1],
                    uz = v1[2] - v0[2];
        const float wx = v2[0] - v0[0], wy = v2[1] - v0[1],
                    wz = v2[2] - v0[2];
        // face normal (winding-independent magnitude)
        const float nx = uy * wz - uz * wy;
        const float ny = uz * wx - ux * wz;
        const float nz = ux * wy - uy * wx;
        const float len =
            std::sqrt(nx * nx + ny * ny + nz * nz);
        const bool floorP =
            len > 0.f && std::fabs(nz) / len > 0.6f;  // walkable slope
        const bool rs = (pl.flags & 0x10) != 0;       // render skip
        const bool cs2 = (pl.flags & 0x20) != 0;      // collision skip
        colSkip += cs2;
        rendSkip += rs;
        floorish += floorP;
        if (floorP && !rs && cs2) {
          ++escape;
          if (holes.size() < 24)
            holes.push_back(
                {p, (v0[0] + v1[0] + v2[0]) / 3.f,
                 (v0[1] + v1[1] + v2[1]) / 3.f,
                 (v0[2] + v1[2] + v2[2]) / 3.f});
        }
        if (floorP && rs && !cs2) ++invisWall;
        for (int k = 0; k < 3; ++k) {
          for (int c = 0; c < 3; ++c) {
            const float vv = col.verts[pl.v[k] * 3 + c];
            if (vv < bmin[c]) bmin[c] = vv;
            if (vv > bmax[c]) bmax[c] = vv;
          }
        }
      }
      // -- object census ----------------------------------------------
      int objs = 0, standable = 0, maskedElems = 0, totalElems = 0;
      int standableHit = 0, standableMiss = 0;
      for (const mdk::CollisionObject* o = col.objects; o;
           o = o->next) {
        if (!o->named || o->model == nullptr) continue;
        ++objs;
        if (o->flags149 & 1) ++standable;
        if (o->elements != nullptr) {
          totalElems += o->elements->count;
          for (int e = 0; e < o->elements->count && e < 32; ++e)
            if (o->elemMaskB & (1u << e)) ++maskedElems;
        }
        // Per-element probe over each unmasked element AABB center —
        // does the floor probe see standable object geometry?
        // (+0x148 bit4 skips the object in the floor probe entirely)
        if ((o->flags149 & 1) && (o->flags148 & 0x10) == 0 &&
            o->elements != nullptr) {
          for (int e = 0; e < o->elements->count && e < 32; ++e) {
            if (o->elemMaskB & (1u << e)) continue;
            const float* ea = o->elements->elems[e].aabb;
            mdk::CollisionState cs;
            cs.arena = &col;
            cs.queryEnabled = 1;
            cs.arenaValid = 1;
            cs.objectDataLoaded = 1;
            cs.pos[0] = (ea[0] + ea[3]) * 0.5f;
            cs.pos[1] = (ea[1] + ea[4]) * 0.5f;
            cs.pos[2] = ea[5] + 1.0f;
            mdk::collisionFloorProbe(cs);
            if (cs.floorObj == o) ++standableHit; else ++standableMiss;
          }
        }
      }
      if (standableHit + standableMiss > 0)
        std::printf("           standable-probe: hit=%d miss=%d\n",
                    standableHit, standableMiss);
      std::printf("  arena[%2d] %-9s polys=%d colSkip=%d rendSkip=%d "
                  "floorish=%d | renderNoCollide=%d invisWall=%d | "
                  "objs=%d standable=%d maskedElems=%d/%d | "
                  "bounds=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f)\n",
                  a->index, a->name.c_str(), npoly, colSkip, rendSkip,
                  floorish, escape, invisWall, objs, standable,
                  maskedElems, totalElems,
                  (double)bmin[0], (double)bmax[0],
                  (double)bmin[1], (double)bmax[1],
                  (double)bmin[2], (double)bmax[2]);
      // -- support grid ------------------------------------------------
      // 2-unit cells over the XY bbox; from vert-top sweep down past
      // the bottom. Owner: BSP poly (returned hit), object element
      // (floorObj set after probe), or nothing.
      {
        const float span = bmax[2] - bmin[2] + 10.f;
        std::vector<std::uint8_t> sup;   // 0 none 1 bsp 2 obj
        const int nx =
            std::max(1, static_cast<int>((bmax[0] - bmin[0]) / 2.f));
        const int ny =
            std::max(1, static_cast<int>((bmax[1] - bmin[1]) / 2.f));
        sup.assign(static_cast<std::size_t>(nx) * ny, 0);
        // QA-only (MDK_FLOORMAP=<name-substr>): capture the landed z
        // per supported cell and print a coarse ASCII heightmap —
        // route planning aid, not part of the census accounting.
        const char* fmWant = std::getenv("MDK_FLOORMAP");
        const bool fmOn =
            fmWant != nullptr && a->name.find(fmWant) !=
            std::string::npos;
        std::vector<float> fmZ;
        if (fmOn) fmZ.assign(static_cast<std::size_t>(nx) * ny,
                            -1e30f);
        int nBsp = 0, nObj = 0;
        for (int gy = 0; gy < ny; ++gy) {
          for (int gx = 0; gx < nx; ++gx) {
            mdk::CollisionState cs;
            cs.arena = &col;
            cs.queryEnabled = 1;
            cs.arenaValid = 1;
            cs.objectDataLoaded = 1;
            const float x = bmin[0] + (gx + 0.5f) * 2.f;
            const float y = bmin[1] + (gy + 0.5f) * 2.f;
            cs.pos[0] = x;
            cs.pos[1] = y;
            cs.pos[2] = bmax[2] + 4.f;
            cs.playerBox[0] = x - 1.25f;
            cs.playerBox[1] = y - 1.25f;
            cs.playerBox[2] = cs.pos[2];
            cs.playerBox[3] = x + 1.25f;
            cs.playerBox[4] = y + 1.25f;
            cs.playerBox[5] = cs.pos[2] + 4.25f;
            const mdk::CollisionPoly* hit =
                mdk::collisionApply(cs, 0.f, 0.f, -span, 0.5f,
                                    nullptr, nullptr);
            mdk::collisionFloorProbe(cs);
            std::uint8_t s = 0;
            if (hit != nullptr) {
              s = 1;
              ++nBsp;
            } else if (cs.floorObj != nullptr ||
                       (cs.contactFlags & 2) != 0) {
              s = 2;
              ++nObj;
            }
            sup[static_cast<std::size_t>(gy) * nx + gx] = s;
            if (fmOn && s != 0)
              fmZ[static_cast<std::size_t>(gy) * nx + gx] =
                  cs.pos[2];
          }
        }
        // QA-only (MDK_STRIPMAP="x0,y0,x1,y1[,zTop]"): for each cell
        // in the rect, peel the column — repeatedly sweep down,
        // restart just under each contact — and report the LOWEST
        // reachable surface. Reveals walkable floors hidden under
        // overhangs that the top-surface floormap cannot see.
        if (const char* sm = std::getenv("MDK_STRIPMAP")) {
          float x0 = 0, y0 = 0, x1 = 0, y1 = 0, zTop = bmax[2] + 4.f;
          const int got = std::sscanf(sm, "%f,%f,%f,%f,%f",
                                      &x0, &y0, &x1, &y1, &zTop);
          if (got >= 4 && zTop > 1e29f) zTop = bmax[2] + 4.f;
          if (got >= 4) {
            // Cells of 2u; char = bucket of the lowest surface z in
            // 10u steps above zLo2; ' ' = cell outside arena bounds,
            // '.' = column peeled to void.
            std::printf("           stripmap (%g,%g)-(%g,%g) "
                        "lowest-surface map (2u/cell, top=+y):\n",
                        (double)x0, (double)y0, (double)x1,
                        (double)y1);
            for (float y = y1; y >= y0; y -= 4.0f) {
              std::string row;
              for (float x = x0; x <= x1; x += 2.0f) {
                float zc = zTop;
                int hits = 0;
                float lastZ = -1e30f;
                for (int peel = 0; peel < 8; ++peel) {
                  mdk::CollisionState cs2;
                  cs2.arena = &col;
                  cs2.queryEnabled = 1;
                  cs2.arenaValid = 1;
                  cs2.objectDataLoaded = 1;
                  cs2.pos[0] = x; cs2.pos[1] = y; cs2.pos[2] = zc;
                  cs2.playerBox[0] = x - 1.25f;
                  cs2.playerBox[1] = y - 1.25f;
                  cs2.playerBox[2] = zc;
                  cs2.playerBox[3] = x + 1.25f;
                  cs2.playerBox[4] = y + 1.25f;
                  cs2.playerBox[5] = zc + 4.25f;
                  const mdk::CollisionPoly* h = mdk::collisionApply(
                      cs2, 0.f, 0.f, -(zc - bmin[2] + 20.f), 0.5f,
                      nullptr, nullptr);
                  if (h == nullptr) break;
                  ++hits;
                  lastZ = cs2.pos[2];
                  zc = cs2.pos[2] - 0.75f;
                }
                if (!hits) { row += '.'; continue; }
                const int b2 = static_cast<int>((lastZ - 0.f) / 10.f);
                row += (b2 < 0) ? '~'
                     : b2 < 10 ? static_cast<char>('0' + b2)
                     : b2 < 36 ? static_cast<char>('A' + b2 - 10)
                     : '#';
              }
              std::printf("           y=%6.1f %s\n", (double)y,
                          row.c_str());
            }
          }
        }
        // interior holes: unsupported cell with >=3 supported
        // 4-neighbors (excludes the rim/void fringe around geometry).
        // EVERY hole is classified below — verbose forensics print
        // for the first 12 per arena and for every cell classified
        // as a true unsupported visible floor.
        int holeN = 0;
        std::vector<std::pair<float, float>> holePos;
        for (int gy = 0; gy < ny; ++gy)
          for (int gx = 0; gx < nx; ++gx) {
            if (sup[static_cast<std::size_t>(gy) * nx + gx] != 0)
              continue;
            int nb = 0;
            if (gx > 0 && sup[static_cast<std::size_t>(gy) * nx + gx - 1]) ++nb;
            if (gx + 1 < nx &&
                sup[static_cast<std::size_t>(gy) * nx + gx + 1]) ++nb;
            if (gy > 0 &&
                sup[static_cast<std::size_t>(gy - 1) * nx + gx]) ++nb;
            if (gy + 1 < ny &&
                sup[static_cast<std::size_t>(gy + 1) * nx + gx]) ++nb;
            if (nb >= 3) {
              ++holeN;
              holePos.emplace_back(bmin[0] + (gx + 0.5f) * 2.f,
                                   bmin[1] + (gy + 0.5f) * 2.f);
            }
          }
        const int cells = nx * ny;
        std::printf("           grid %dx%d cells=%d supported=%d "
                    "(bsp=%d obj=%d) interiorHoles=%d\n",
                    nx, ny, cells, nBsp + nObj, nBsp, nObj, holeN);
        if (fmOn) {
          // ASCII heightmap: downsampled so output stays ~100x50.
          // char = floor-z bucket (10u steps, '0'..'9'+'A'..), '.'=void
          const int sx = std::max(1, nx / 100 + 1);
          const int sy = std::max(1, ny / 50 + 1);
          float zLo = 1e30f, zHi = -1e30f;
          for (const float z : fmZ)
            if (z > -1e29f) {
              if (z < zLo) zLo = z;
              if (z > zHi) zHi = z;
            }
          std::printf("           floor-map z=%.0f..%.0f "
                      "(%d u/cell, top=+y):\n",
                      (double)zLo, (double)zHi, sx * 2);
          for (int ry = ny - 1; ry >= 0; ry -= sy) {
            std::string row;
            for (int rx = 0; rx < nx; rx += sx) {
              float acc = 0.f;
              int cnt = 0;
              for (int j = 0; j < sy && ry - j >= 0; ++j)
                for (int i = 0; i < sx && rx + i < nx; ++i) {
                  const float z =
                      fmZ[static_cast<std::size_t>(ry - j) * nx +
                          rx + i];
                  if (z > -1e29f) { acc += z; ++cnt; }
                }
              if (!cnt) {
                row += '.';
              } else {
                const int bucket = static_cast<int>(
                    (acc / cnt - zLo) / 10.f);
                row += bucket < 10
                    ? static_cast<char>('0' + bucket)
                    : bucket < 36
                        ? static_cast<char>('A' + bucket - 10)
                        : '#';
              }
            }
            std::printf("           %s\n", row.c_str());
          }
        }

        // ---- classify EVERY interior hole --------------------------
        // cls: 0 void (no covering geometry) | 1 object-elem |
        // 2 down-facing covering poly (ceiling/underside — correctly
        //   not support) | 3 stepped-hit (per-frame support exists;
        //   the column sweep slide-off is an artefact) |
        // 4 slide-off (column hit then slid, no stepped support) |
        // 5 UNSUPPORTED VISIBLE FLOOR (up-facing rendered poly, no
        //   support by any probe — needs investigation) | 6 unknown
        struct HoleInfo {
          float x, y, nz, z;
          std::uint16_t flags;
          int poly, expect, cls, iters, stepHit;
          float stepZ;
        };
        std::vector<HoleInfo> infos;
        infos.reserve(holePos.size());
        for (const auto& hp : holePos) {
          HoleInfo hi{hp.first, hp.second, 0.f, 0.f, 0, -1, 0, 6, 0,
                      -1, -1e30f};
          // expected owner: a rendered (not render-skip) floor-ish
          // poly whose XY projection covers the cell, or a standable
          // object's element AABB covering it.
          for (int p = 0; p < npoly && hi.expect != 1; ++p) {
            const mdk::CollisionPoly& pl = col.polys[p];
            if ((pl.flags & 0x10) != 0) continue;   // render-skipped
            const float* v0 = col.verts + pl.v[0] * 3;
            const float* v1 = col.verts + pl.v[1] * 3;
            const float* v2 = col.verts + pl.v[2] * 3;
            const float ux = v1[0] - v0[0], uy = v1[1] - v0[1],
                        uz = v1[2] - v0[2];
            const float wx = v2[0] - v0[0], wy = v2[1] - v0[1],
                        wz = v2[2] - v0[2];
            const float nz = ux * wy - uy * wx;
            const float nn =
                std::sqrt((uy * wz - uz * wy) * (uy * wz - uz * wy) +
                          (uz * wx - ux * wz) * (uz * wx - ux * wz) +
                          nz * nz);
            if (nn <= 0.f || std::fabs(nz) / nn <= 0.6f) continue;
            // point-in-triangle (XY)
            const float d1 = (hp.first - v1[0]) * (v0[1] - v1[1]) -
                             (v0[0] - v1[0]) * (hp.second - v1[1]);
            const float d2 = (hp.first - v2[0]) * (v1[1] - v2[1]) -
                             (v1[0] - v2[0]) * (hp.second - v2[1]);
            const float d3 = (hp.first - v0[0]) * (v2[1] - v0[1]) -
                             (v2[0] - v0[0]) * (hp.second - v0[1]);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
            const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos)) {
              hi.expect = 1;
              hi.nz = nz / nn;
              hi.z = (v0[2] + v1[2] + v2[2]) / 3.f;
              hi.flags = pl.flags;
              hi.poly = p;
            }
          }
          if (hi.expect != 1)
            for (const mdk::CollisionObject* o = col.objects; o;
                 o = o->next) {
              if (!o->named || o->model == nullptr ||
                  (o->flags148 & 0x10) != 0 || !(o->flags149 & 1) ||
                  o->elements == nullptr)
                continue;
              for (int e = 0; e < o->elements->count && e < 32; ++e) {
                if (o->elemMaskB & (1u << e)) continue;
                const float* ea = o->elements->elems[e].aabb;
                if (hp.first >= ea[0] && hp.first <= ea[3] &&
                    hp.second >= ea[1] && hp.second <= ea[4]) {
                  hi.expect = 2;
                  break;
                }
              }
              if (hi.expect == 2) break;
            }
          if (hi.expect == 1 && hi.nz >= 0.f) {
            // Real long-column replay (isolates iters — slide count)
            // + stepped re-probe (per-frame gameplay support).
            mdk::collisionProfileReset();
            {
              mdk::CollisionState vcs;
              vcs.arena = &col;
              vcs.queryEnabled = 1;
              vcs.arenaValid = 1;
              vcs.objectDataLoaded = 0;
              vcs.pos[0] = hp.first;
              vcs.pos[1] = hp.second;
              vcs.pos[2] = bmax[2] + 4.f;
              mdk::collisionApply(vcs, 0.f, 0.f, -span, 0.5f,
                                  nullptr, nullptr);
            }
            hi.iters = static_cast<int>(mdk::collisionProfile().sweepIters);
            {
              mdk::CollisionState scs;
              scs.arena = &col;
              scs.queryEnabled = 1;
              scs.arenaValid = 1;
              scs.objectDataLoaded = 1;
              scs.pos[0] = hp.first;
              scs.pos[1] = hp.second;
              scs.pos[2] = bmax[2] + 4.f;
              const mdk::CollisionPoly* sh = nullptr;
              while (!sh && scs.pos[2] > bmin[2] - 6.f) {
                sh = mdk::collisionApply(scs, 0.f, 0.f, -3.f, 0.5f,
                                         nullptr, nullptr);
              }
              if (sh) {
                hi.stepHit = static_cast<int>(sh - col.polys);
                hi.stepZ = scs.pos[2];
              }
            }
          }
          if (hi.expect == 0) hi.cls = 0;
          else if (hi.expect == 2) hi.cls = 1;
          else if (hi.nz < 0.f) hi.cls = 2;
          else if (hi.stepHit >= 0) hi.cls = 3;
          else if (hi.iters >= 2) hi.cls = 4;
          else hi.cls = 5;
          infos.push_back(hi);
        }
        {
          int cnt[7] = {};
          for (const auto& hi : infos) ++cnt[hi.cls];
          std::printf("           holes=%d classified: void=%d "
                      "object=%d down-facing=%d stepped-hit=%d "
                      "slide-off=%d UNSUPPORTED-FLOOR=%d unknown=%d\n",
                      (int)infos.size(), cnt[0], cnt[1], cnt[2],
                      cnt[3], cnt[4], cnt[5], cnt[6]);
        }
        // Verbose forensics: first 12 holes + every UNSUPPORTED-FLOOR
        // cell (cls 5 — must be investigated, never silently counted).
        int shown = 0;
        for (std::size_t k = 0; k < infos.size(); ++k) {
          const auto& hi = infos[k];
          const bool verbose = shown < 12 || hi.cls == 5;
          if (!verbose) continue;
          ++shown;
          const std::pair<float, float> hp{hi.x, hi.y};
          const int expect = hi.expect;
          const float expNz = hi.nz;
          const float expZ = hi.z;
          const std::uint16_t expFlags = hi.flags;
          const int expPoly = hi.poly;
          if (expect == 1) {
            // Descent trace for the column segment — same
            // pos/target/ext/fatMargin the real sweep used.
            CensusSweepTrace tr;
            tr.nodes = col.nodes;
            tr.pos[0] = hp.first; tr.pos[1] = hp.second;
            tr.pos[2] = bmax[2] + 4.f + 2.5f + 0.01f;
            tr.target[0] = hp.first; tr.target[1] = hp.second;
            tr.target[2] = tr.pos[2] - span;
            for (int k = 0; k < 3; ++k) {
              tr.delta[k] = tr.target[k] - tr.pos[k];
              tr.ext[k] = 0.f;
            }
            tr.ext[0] = tr.ext[1] = 0.4f; tr.ext[2] = 2.5f;
            tr.fatMargin = (0.4f + 0.4f + 2.5f) * 2.f;
            tr.descend(col.nodes);
            // Cross-check the replica against the REAL sweep: replay
            // the same column through collisionApply with the profile
            // counters reset. objectDataLoaded=0 isolates the pure
            // BSP sweep (no second sweep from the object pass).
            mdk::collisionProfileReset();
            {
              mdk::CollisionState vcs;
              vcs.arena = &col;
              vcs.queryEnabled = 1;
              vcs.arenaValid = 1;
              vcs.objectDataLoaded = 0;
              vcs.pos[0] = hp.first;
              vcs.pos[1] = hp.second;
              vcs.pos[2] = bmax[2] + 4.f;
              mdk::collisionApply(vcs, 0.f, 0.f, -span, 0.5f,
                                  nullptr, nullptr);
            }
            // value copy — the stepped probe below runs more sweeps.
            const mdk::CollisionProfile cp = mdk::collisionProfile();
            std::size_t trPolyTests = 0;
            for (const auto& ts : tr.tested)
              trPolyTests += 2u * ts.count;  // two attempts, full scan
            // Was the covering poly inside a tested node set?
            int inSet = -1;          // index into tr.tested
            for (std::size_t ti = 0; ti < tr.tested.size(); ++ti) {
              const auto& ts = tr.tested[ti];
              if (expPoly >= (int)ts.first &&
                  expPoly < (int)(ts.first + ts.count)) {
                inSet = static_cast<int>(ti);
                break;
              }
            }
            // Where IS the poly registered? Walk the whole tree's
            // sets and record owner nodes.
            std::vector<int> owners;
            {
              std::vector<int> stack;
              std::vector<char> seen(32768, 0);
              stack.push_back(0);
              while (!stack.empty()) {
                const int ni = stack.back();
                stack.pop_back();
                if (ni < 0 || ni >= 32768 || seen[ni]) continue;
                seen[ni] = 1;
                const mdk::CollisionNode& nd = col.nodes[ni];
                for (int side = 0; side < 2; ++side) {
                  const std::uint32_t set =
                      side ? nd.polysNeg : nd.polysPos;
                  const std::uint32_t f = set >> 16,
                                      c = set & 0xffffu;
                  if (expPoly >= (int)f && expPoly < (int)(f + c))
                    owners.push_back(ni);
                }
                if (nd.childNear >= 0) stack.push_back(nd.childNear);
                if (nd.childFar >= 0) stack.push_back(nd.childFar);
              }
            }
            int ownerVisited = 0;
            for (const int on : owners)
              if (std::find(tr.visited.begin(), tr.visited.end(), on) !=
                  tr.visited.end())
                ++ownerVisited;
            const int stepHit = hi.stepHit;   // stepped probe already
            const float stepZ = hi.stepZ;     // ran in classification
            const char* cls =
                inSet >= 0      ? "IN-TESTED-SET(port-suspect)"
                : ownerVisited  ? "owner-visited-not-crossed(data)"
                                : "owner-unreached(data)";
            std::printf("             hole@(%.1f, %.1f) expect=bsp-poly "
                        "#%d z=%.1f nz=%.2f flags=0x%02x %s",
                        (double)hp.first, (double)hp.second, expPoly,
                        (double)expZ, (double)expNz, expFlags, cls);
            if (inSet >= 0) {
              const auto& ts = tr.tested[inSet];
              const mdk::CollisionPoly& pl = col.polys[expPoly];
              const float* wv[3] = {col.verts + pl.v[0] * 3,
                                    col.verts + pl.v[1] * 3,
                                    col.verts + pl.v[2] * 3};
              const float c1[3] = {ts.fcx, ts.fcy, ts.fcz};
              const float c2[3] = {ts.cx, ts.cy, ts.cz};
              int fa1 = -1, fa2 = -1;
              const int b1 = censusBoxTri(c1, tr.ext, wv[0], wv[1],
                                          wv[2], &fa1);
              const int b2 = censusBoxTri(c2, tr.ext, wv[0], wv[1],
                                          wv[2], &fa2);
              std::printf(" verts=(%g,%g,%g)(%g,%g,%g)(%g,%g,%g)",
                          (double)wv[0][0], (double)wv[0][1],
                          (double)wv[0][2], (double)wv[1][0],
                          (double)wv[1][1], (double)wv[1][2],
                          (double)wv[2][0], (double)wv[2][1],
                          (double)wv[2][2]);
              std::printf(" [node#%d %s c1z=%.1f bt=%d(a%d) "
                          "c2z=%.1f bt=%d(a%d)]",
                          ts.node, ts.neg ? "neg" : "pos",
                          (double)ts.fcz, b1, fa1, (double)ts.cz, b2,
                          fa2);
            }
            std::printf(" owners=%d(%dvis) nodes=%zu/%llu "
                        "polyTests=%zu/%llu calls=%llu iters=%llu",
                        (int)owners.size(), ownerVisited,
                        tr.visited.size(),
                        (unsigned long long)cp.sweepNodes,
                        trPolyTests,
                        (unsigned long long)cp.sweepPolyTests,
                        (unsigned long long)cp.sweepCalls,
                        (unsigned long long)cp.sweepIters);
            if (stepHit >= 0)
              std::printf(" stepped-hit=#%d@z%.1f", stepHit,
                          (double)stepZ);
            std::printf("\n");
          }
          else
            std::printf("             hole@(%.1f, %.1f) expect=%s\n",
                        (double)hp.first, (double)hp.second,
                        expect == 2 ? "object-elem" : "none");
        }
      }
      for (const auto& h : holes)
        std::printf("           escape-poly #%d centroid(%.1f, %.1f, "
                    "%.1f) flags=0x%02x surface=%u\n", h.poly,
                    (double)h.cx, (double)h.cy, (double)h.cz,
                    col.polys[h.poly].flags,
                    col.polys[h.poly].surface);
    }
    return 0;
  }

  // --collision-probe: Phase 5D BUILD_A smoke. Scans the file for the
  // first self-consistent FUN_00419ee0 collision blob (the level
  // stream's {nodes,polys,verts} triple), then runs one real swept
  // FUN_004630d4 query + FUN_00435eec floor probe against it.
  if (collisionProbe) {
    const auto cfile = root->readFile(*target, kEntriesMaxBytes, &err);
    if (!cfile) {
      std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
      return 1;
    }
    const std::uint8_t* bytes =
        reinterpret_cast<const std::uint8_t*>(cfile->data());
    const std::size_t n = cfile->size();
    std::size_t blobOff = 0;
    mdk::CollisionArena arena;
    std::uint32_t counts[4] = {0, 0, 0, 0};
    bool found = false;
    for (std::size_t off = 0; off + 16 <= n; off += 4) {
      mdk::CollisionArena a;
      if (mdk::collisionBlobParse(bytes + off, n - off, &a, counts)) {
        blobOff = off;
        arena = a;
        found = true;
        break;
      }
    }
    if (!found) {
      std::printf("collision: no self-consistent FUN_00419ee0 blob "
                  "found\n");
      return 1;
    }
    std::printf("blob:      0x%08zx — countA=%u nodes=%u polys=%u "
                "verts=%u\n",
                blobOff, counts[0], counts[1], counts[2], counts[3]);
    // Vertex bounds for the default probe position.
    float bmin[3] = {1e30f, 1e30f, 1e30f};
    float bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (std::uint32_t i = 0; i < counts[3]; ++i) {
      for (int k = 0; k < 3; ++k) {
        const float v = arena.verts[i * 3 + k];
        if (v < bmin[k]) bmin[k] = v;
        if (v > bmax[k]) bmax[k] = v;
      }
    }
    std::printf("bounds:    min (%g, %g, %g)  max (%g, %g, %g)\n",
                (double)bmin[0], (double)bmin[1], (double)bmin[2],
                (double)bmax[0], (double)bmax[1], (double)bmax[2]);
    mdk::CollisionState cs;
    cs.arena = &arena;
    cs.queryEnabled = 1;
    cs.arenaValid = 1;
    cs.objectDataLoaded = 1;
    cs.pos[0] = probePosGiven ? probePos[0]
                            : (bmin[0] + bmax[0]) * 0.5f;
    cs.pos[1] = probePosGiven ? probePos[1]
                            : (bmin[1] + bmax[1]) * 0.5f;
    cs.pos[2] = probePosGiven ? probePos[2]
                            : (bmin[2] + bmax[2]) * 0.5f;
    std::printf("probe-pos: (%g, %g, %g)\n", (double)cs.pos[0],
                (double)cs.pos[1], (double)cs.pos[2]);
    // One vertical swept query spanning the whole vert range.
    const float dz = -(bmax[2] - bmin[2] + 20.0f);
    const mdk::CollisionNode* node = nullptr;
    const mdk::CollisionPoly* hit = mdk::collisionApply(
        cs, 0.0f, 0.0f, dz, 0.5f, nullptr, &node);
    std::printf("sweep:     dz=%g -> pos (%g, %g, %g)  contact=%s",
                (double)dz, (double)cs.pos[0], (double)cs.pos[1],
                (double)cs.pos[2], hit ? "yes" : "no");
    if (hit) {
      std::printf(" poly#%td", hit - arena.polys);
    }
    if (node) {
      std::printf(" node#%td n=(%g, %g, %g)", node - arena.nodes,
                  (double)node->nx, (double)node->ny,
                  (double)node->nz);
    }
    std::printf("\n");
    // FUN_00435eec at frame end: the blob carries no object list, so
    // bit1 clears — the original behaves the same on static floors.
    mdk::collisionFloorProbe(cs);
    std::printf("probe:     flags=0x%02x floorZ=%g (object list "
                "empty in blob — bit1 clears as on static floors)\n",
                cs.contactFlags, (double)cs.floorZ);
    return 0;
  }

  // --entries: enumerate interior directory metadata where a proven
  // parser exists (SNI Phase 3C; MTI Phase 3D; MTO Phase 3E; CMI
  // Phase 3F; DTI Phase 3G; FTI/BNI Phase 3H). Never prints payload
  // bytes. --visual-info: metadata-only report for one named record
  // against the proven BNI image layouts (Phase 4A) — no extraction.
  // --font-info: metadata-only report for one named FTI record against
  // the proven FONTSML/FONTBIG glyph layout (Phase 4C).
  // --sprite-info: metadata-only report for one named FTI record
  // against the proven ARROW sprite-table layout (Phase 4D).
  if (support != mdk::FamilySupport::kDirectoryMetadata) {
    const char* label = visualInfoName  ? "visual-info:"
                        : fontInfoName  ? "font-info:  "
                        : spriteInfoName ? "sprite-info:"
                                         : "entries:  ";
    std::printf("%s unsupported for family %s (support: %s) — "
                "no evidence-backed interior parser\n",
                label,
                std::string(mdk::fileFamilyName(family)).c_str(),
                std::string(mdk::familySupportName(support)).c_str());
    return 1;
  }

  const auto file = root->readFile(*target, kEntriesMaxBytes, &err);
  if (!file) {
    std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
    return 1;
  }

  if (spriteInfoName) {
    if (family != mdk::MdkFileFamily::kFti) {
      std::printf("sprite:    unsupported for family %s — Phase 4D "
                  "proves the FTI ARROW sprite-table layout only\n",
                  std::string(mdk::fileFamilyName(family)).c_str());
      return 1;
    }
    const auto dir = mdk::inspectFtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    if (dir.status != mdk::FtiDirectoryStatus::kOk) {
      std::printf("fti:       %s — %s\n",
                  std::string(mdk::ftiDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
      return 1;
    }
    const mdk::FtiRecord* rec = mdk::findFtiRecord(dir, *spriteInfoName);
    if (!rec) {
      std::printf("record:    %s — NOT FOUND\n",
                  spriteInfoName->c_str());
      return 1;
    }
    std::printf("record:    %s\n", rec->name().c_str());
    std::printf("span:      [0x%08llx, 0x%08llx) — %llu bytes\n",
                static_cast<unsigned long long>(rec->payloadFileOffset),
                static_cast<unsigned long long>(rec->payloadEnd),
                static_cast<unsigned long long>(rec->payloadSize()));
    const std::span<const std::byte> payload(
        file->data() + rec->payloadFileOffset, rec->payloadSize());
    std::string derr;
    const auto sprite = mdk::decodeFtiSprite(payload, &derr);
    if (!sprite) {
      std::printf("decode:    FAILED (%s)\n", derr.c_str());
      return 1;
    }
    std::printf("layout:    u32 blockBytes @+0; u32 frameCount @+4; "
                "u32 frameOffset[] @+8 (each +4-relative); frame "
                "{u16 w, u16 h, s16 hotX, s16 hotY, stream}\n");
    std::printf("header:    blockBytes=%u frames=%llu payload=%llu "
                "trailing=%llu\n",
                sprite->blockBytes,
                static_cast<unsigned long long>(sprite->frames.size()),
                static_cast<unsigned long long>(sprite->payloadBytes),
                static_cast<unsigned long long>(sprite->trailingBytes));
    std::printf("stream:    cmds <0x80 literal (n=cmd+1); 0x80-0xfd "
                "run (n=cmd-0x7c, value 0 = transparent); 0xfe row "
                "break; 0xff end\n");
    std::printf("colors:    final palette indices; byte 0 = "
                "transparent (skipped, not color-keyed)\n");
    for (std::size_t i = 0; i < sprite->frames.size(); ++i) {
      const auto& f = sprite->frames[i];
      std::printf("frame[%zu]:  @+0x%llx %ux%u hotspot (%d,%d) "
                  "stream %zu B | lit %u run %u (transp %u) rows %u | "
                  "adv %llu opaque %llu idx [%u..%u]\n",
                  i,
                  static_cast<unsigned long long>(f.frameFileOffset),
                  f.width, f.height, f.hotspotX, f.hotspotY,
                  f.stream.size(), f.literalPackets, f.runPackets,
                  f.transparentRuns, f.rowBreaks,
                  static_cast<unsigned long long>(f.pixelAdvances),
                  static_cast<unsigned long long>(f.opaqueWrites),
                  f.minPixelIndex, f.maxPixelIndex);
    }
    std::printf("digest:    %016llx\n",
                static_cast<unsigned long long>(
                    mdk::ftiSpriteDigest(*sprite)));
    return 0;
  }

  if (fontInfoName) {
    if (family != mdk::MdkFileFamily::kFti) {
      std::printf("font:      unsupported for family %s — Phase 4C "
                  "proves the FTI FONTSML/FONTBIG glyph layout only\n",
                  std::string(mdk::fileFamilyName(family)).c_str());
      return 1;
    }
    const auto dir = mdk::inspectFtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    if (dir.status != mdk::FtiDirectoryStatus::kOk) {
      std::printf("fti:       %s — %s\n",
                  std::string(mdk::ftiDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
      return 1;
    }
    const mdk::FtiRecord* rec = mdk::findFtiRecord(dir, *fontInfoName);
    if (!rec) {
      std::printf("record:    %s — NOT FOUND\n", fontInfoName->c_str());
      return 1;
    }
    std::printf("record:    %s\n", rec->name().c_str());
    std::printf("span:      [0x%08llx, 0x%08llx) — %llu bytes\n",
                static_cast<unsigned long long>(rec->payloadFileOffset),
                static_cast<unsigned long long>(rec->payloadEnd),
                static_cast<unsigned long long>(rec->payloadSize()));
    const std::span<const std::byte> payload(
        file->data() + rec->payloadFileOffset, rec->payloadSize());
    std::string derr;
    const auto font = mdk::decodeFtiFont(payload, &derr);
    if (!font) {
      std::printf("decode:    FAILED (%s)\n", derr.c_str());
      return 1;
    }
    std::printf("layout:    u32 offsetTable[256] @+0x000; glyph "
                "{s8 top, s8 bottom, u8 width, u8 px[w*(top+bottom+1)]}"
                " — row-major top-down indexed bytes\n");
    std::printf("mapping:   input byte -> table[byte] directly; "
                "0 = unmapped (no ASCII/CP437 transform)\n");
    std::printf("mapped:    %zu of 256 slots — first 0x%02x, last "
                "0x%02x\n", font->mappedCount, font->firstMapped,
                font->lastMapped);
    std::printf("glyphs@:   record+0x%llx; trailing slack %llu bytes\n",
                static_cast<unsigned long long>(font->glyphDataStart),
                static_cast<unsigned long long>(font->trailingBytes));
    std::printf("offsets:   %s, %s\n",
                font->offsetsSortedAscending ? "sorted ascending"
                                           : "NOT sorted",
                font->offsetsUnique ? "unique" : "NOT unique");
    std::printf("encoding:  verbatim 8-bit palette indices; byte 0 "
                "skips (transparent); max index used %u\n",
                font->maxPixelIndex);
    std::printf("missing:   table entry 0 -> advance-only; proven "
                "advance %d (FONTSML path) / %d (FONTBIG path)\n",
                mdk::kFtiFontSmlMissingAdvance,
                mdk::kFtiFontBigMissingAdvance);
    std::printf("digest:    %016llx\n",
                static_cast<unsigned long long>(
                    mdk::ftiFontDigest(*font)));
    if (fontInfoCode) {
      const auto code = static_cast<std::uint8_t>(*fontInfoCode);
      const mdk::FtiGlyph* g = font->glyphFor(code);
      std::printf("glyph[0x%02x]: ", code);
      if (!g) {
        std::printf("unmapped (advance-only)\n");
      } else {
        std::printf("@0x%08llx top=%d bottom=%d width=%u rows=%d "
                    "bitmap=%llu bytes consumed=%llu\n",
                    static_cast<unsigned long long>(g->offset),
                    int(g->top), int(g->bottom), unsigned(g->width),
                    g->rows(),
                    static_cast<unsigned long long>(g->pixels.size()),
                    static_cast<unsigned long long>(
                        g->consumedBytes()));
      }
    }
    return 0;
  }

  if (visualInfoName) {
    if (family != mdk::MdkFileFamily::kBni) {
      std::printf("visual:    unsupported for family %s — Phase 4A "
                  "proves BNI image payloads only\n",
                  std::string(mdk::fileFamilyName(family)).c_str());
      return 1;
    }
    const auto dir = mdk::inspectBniDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    if (dir.status != mdk::BniDirectoryStatus::kOk) {
      std::printf("bni:       %s — %s\n",
                  std::string(mdk::bniDirectoryStatusName(dir.status))
                      .c_str(),
                  dir.detail.c_str());
      return 1;
    }
    const mdk::BniRecord* rec = mdk::findBniRecord(dir, *visualInfoName);
    if (!rec) {
      std::printf("record:    %s — NOT FOUND\n", visualInfoName->c_str());
      return 1;
    }
    std::printf("record:    %s\n", rec->name().c_str());
    std::printf("span:      [0x%08llx, 0x%08llx) — %llu bytes\n",
                static_cast<unsigned long long>(rec->payloadFileOffset),
                static_cast<unsigned long long>(rec->payloadEnd),
                static_cast<unsigned long long>(rec->payloadSize()));
    const std::span<const std::byte> payload(
        file->data() + rec->payloadFileOffset, rec->payloadSize());
    const auto probe = mdk::probeBniImage(payload);
    std::printf("shape:     %s\n",
                std::string(mdk::bniImageShapeName(probe.shape)).c_str());
    if (probe.shape == mdk::BniImageShape::kOther) {
      std::printf("           payload does not match a proven image "
                  "layout\n");
      return 1;
    }
    std::printf("dims:      %dx%d (stride %d, row-major top-down)\n",
                probe.width, probe.height, probe.width);
    std::printf("pixels:    %llu indexed bytes @ payload+0x%zx\n",
                static_cast<unsigned long long>(probe.pixelBytes),
                probe.headerBytes);
    if (probe.shape == mdk::BniImageShape::kIndexedOnly) {
      // Indexed-only records carry no palette; the consumer binds one
      // from context. The only binding proven so far is the STREAM
      // backdrop (FUN_0042b270): SYS_PAL head + PAL record tail.
      if (!mdk::isStreamBackdropRequest(*target, *visualInfoName)) {
        std::printf("palette:   none embedded — no proven "
                    "external-palette binding for this context "
                    "(Phase 4B proves %s %s only)\n",
                    std::string(mdk::kStreamBniFile).c_str(),
                    std::string(mdk::kStreamImageRecord).c_str());
        return 0;
      }
      const auto fti = root->readFile(std::string(mdk::kStreamSystemFile),
                                      kEntriesMaxBytes, &err);
      if (!fti) {
        std::fprintf(stderr, "read-file: FAILED (%s)\n", err.c_str());
        return 1;
      }
      std::string derr;
      const auto img = mdk::decodeStreamBackdrop(
          std::span<const std::byte>(file->data(), file->size()),
          std::span<const std::byte>(fti->data(), fti->size()), &derr);
      if (!img) {
        std::printf("decode:    FAILED (%s)\n", derr.c_str());
        return 1;
      }
      std::printf("palette:   external — STREAM context "
                  "(FUN_0042b270): %s %s[0:64] + %s[64:256] "
                  "(record bytes [0xc0,0x300))\n",
                  std::string(mdk::kStreamSystemFile).c_str(),
                  std::string(mdk::kStreamSystemRecord).c_str(),
                  std::string(mdk::kStreamPaletteRecord).c_str());
      std::printf("decode:    ok — digest=%016llx\n",
                  static_cast<unsigned long long>(
                      mdk::imageDigest(*img)));
      return 0;
    }
    std::string derr;
    const auto img = mdk::decodeBniPalettedImage(payload, &derr);
    if (!img) {
      std::printf("decode:    FAILED (%s)\n", derr.c_str());
      return 1;
    }
    std::printf("palette:   256 embedded RGB entries (R,G,B order)\n");
    std::printf("decode:    ok — digest=%016llx\n",
                static_cast<unsigned long long>(mdk::imageDigest(*img)));
    return 0;
  }

  if (family == mdk::MdkFileFamily::kMti) {
    const auto dir = mdk::inspectMtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   MTI directory (count u32 @0x14, records "
                "24 bytes @0x18, name[8])\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::mtiDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::MtiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const auto& e = dir.entries[i];
      if (e.isIndexRecord()) {
        std::printf("  [%3zu] %-8s INDEX (field0x08=0xffffffff) "
                    "index=%u (0x%08x) field0x10=0x%08x "
                    "field0x14=0x%08x\n",
                    i, e.name().c_str(), e.fieldAt0x0C, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14);
      } else if (e.headerCount) {
        std::printf("  [%3zu] %-8s field0x08=0x%08x field0x0c=0x%08x "
                    "field0x10=0x%08x blobOff=0x%08x fileOff=0x%08llx "
                    "hdr{n=%u,a=%u,b=%u} dataOff=0x%08llx\n",
                    i, e.name().c_str(), e.fieldAt0x08, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14,
                    static_cast<unsigned long long>(e.payloadFileOffset()),
                    *e.headerCount, e.headerFieldA, e.headerFieldB,
                    static_cast<unsigned long long>(
                        e.payloadDataFileOffset));
      } else {
        std::printf("  [%3zu] %-8s field0x08=0x%08x field0x0c=0x%08x "
                    "field0x10=0x%08x blobOff=0x%08x fileOff=0x%08llx "
                    "hdr{a=%u,b=%u} dataOff=0x%08llx\n",
                    i, e.name().c_str(), e.fieldAt0x08, e.fieldAt0x0C,
                    e.fieldAt0x10, e.fieldAt0x14,
                    static_cast<unsigned long long>(e.payloadFileOffset()),
                    e.headerFieldA, e.headerFieldB,
                    static_cast<unsigned long long>(
                        e.payloadDataFileOffset));
      }
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kMto) {
    const auto dir = mdk::inspectMtoDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   MTO overlay directory (count u32 @0x14, "
                "records 12 bytes @0x18, name[8] + file offset)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::mtoDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::MtoDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t i = 0; i < dir.entries.size(); ++i) {
      const auto& e = dir.entries[i];
      const auto& b = dir.blocks[i];
      std::printf("  [%3zu] %-8s blockOff=0x%08llx blockLen=0x%x "
                  "inner=%-12s innerRecs=%u\n",
                  i, e.name().c_str(),
                  static_cast<unsigned long long>(e.blockFileOffset),
                  b.blockLength, b.innerName().c_str(), b.innerCount);
      std::printf("         fields{0x04=0x%x 0x08=0x%x 0x0c=0x%x} "
                  "innerTrailer=%s regionA{size=0x%x ca=%u cb=%u "
                  "cc=%u} regionB@0x%llx(0x%llx) regionC@0x%llx{c1=%u "
                  "c2=%u c3=%u c4=%u extra@0x%llx}\n",
                  b.fieldAt0x04, b.fieldAt0x08, b.fieldAt0x0C,
                  b.innerTrailerPresent ? "yes" : "no",
                  b.regionASize, b.regionACountA, b.regionACountB,
                  b.regionACountC,
                  static_cast<unsigned long long>(b.regionBOffset),
                  static_cast<unsigned long long>(b.regionBSize),
                  static_cast<unsigned long long>(b.regionCOffset),
                  b.regionCCount1, b.regionCCount2, b.regionCCount3,
                  b.regionCCount4,
                  static_cast<unsigned long long>(b.regionCExtraOffset));
      for (std::size_t j = 0; j < b.innerRecords.size(); ++j) {
        const auto& mr = b.innerRecords[j];
        if (mr.isIndexRecord()) {
          std::printf("           mat[%3zu] %-8s INDEX index=%u "
                      "field0x10=0x%08x field0x14=0x%08x\n",
                      j, mr.name().c_str(), mr.fieldAt0x0C,
                      mr.fieldAt0x10, mr.fieldAt0x14);
        } else if (mr.headerCount) {
          std::printf("           mat[%3zu] %-8s flags=0x%08x "
                      "f0x0c=0x%08x f0x10=0x%08x imgOff=0x%08x "
                      "fileOff=0x%08llx hdr{n=%u,a=%u,b=%u} "
                      "dataOff=0x%08llx\n",
                      j, mr.name().c_str(), mr.fieldAt0x08,
                      mr.fieldAt0x0C, mr.fieldAt0x10, mr.fieldAt0x14,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset -
                          mr.payloadHeaderBytes()),
                      *mr.headerCount, mr.headerFieldA, mr.headerFieldB,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset));
        } else {
          std::printf("           mat[%3zu] %-8s flags=0x%08x "
                      "f0x0c=0x%08x f0x10=0x%08x imgOff=0x%08x "
                      "fileOff=0x%08llx hdr{a=%u,b=%u} "
                      "dataOff=0x%08llx\n",
                      j, mr.name().c_str(), mr.fieldAt0x08,
                      mr.fieldAt0x0C, mr.fieldAt0x10, mr.fieldAt0x14,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset -
                          mr.payloadHeaderBytes()),
                      mr.headerFieldA, mr.headerFieldB,
                      static_cast<unsigned long long>(
                          mr.payloadDataFileOffset));
        }
      }
      for (std::size_t j = 0; j < b.regionAArrayA.size(); ++j) {
        const auto& nr = b.regionAArrayA[j];
        std::printf("           regA-A[%3zu] %-8s off=0x%08x\n",
                    j, nr.name().c_str(), nr.fieldAt0x08);
      }
      for (std::size_t j = 0; j < b.regionAArrayB.size(); ++j) {
        const auto& nr = b.regionAArrayB[j];
        std::printf("           regA-B[%3zu] %-8s off=0x%08x "
                    "(overlay-alien lookup target)\n",
                    j, nr.name().c_str(), nr.fieldAt0x08);
      }
      for (std::size_t j = 0; j < b.regionAArrayC.size(); ++j) {
        const auto& sr = b.regionAArrayC[j];
        std::printf("           regA-C[%3zu] {0x%08x 0x%08x 0x%08x "
                    "0x%04x 0x%04x off=0x%08x 0x%08x} "
                    "(overlay-sound record)\n",
                    j, sr.fieldAt0x00, sr.fieldAt0x04, sr.fieldAt0x08,
                    sr.fieldAt0x0C, sr.fieldAt0x0E, sr.fieldAt0x10,
                    sr.fieldAt0x14);
      }
      for (std::size_t j = 0; j < b.regionCNames.size(); ++j) {
        std::printf("           regC-name[%3zu] %s\n",
                    j, b.regionCNames[j].name().c_str());
      }
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kCmi) {
    const auto dir = mdk::inspectCmiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   CMI directory (four counted variable-"
                "length tables @0x14: u8 len + name[len] + u32 value; "
                "then data region)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::cmiDirectoryStatusName(dir.status)).c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::CmiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("trailer:   name[12] @ size-12 %s\n",
                dir.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                dir.secondaryLength,
                dir.secondaryEqualsTrailerOffset ? "equals"
                                                 : "does not equal");

    for (std::size_t t = 0; t < dir.tables.size(); ++t) {
      const auto& tab = dir.tables[t];
      std::printf("  table[%zu] count=%u count@0x%llx recs@0x%llx "
                  "end=0x%llx\n", t, tab.count,
                  static_cast<unsigned long long>(tab.countFileOffset),
                  static_cast<unsigned long long>(tab.recordsFileOffset),
                  static_cast<unsigned long long>(tab.endFileOffset));
      for (std::size_t i = 0; i < tab.records.size(); ++i) {
        const auto& e = tab.records[i];
        std::printf("    [%3zu] @0x%06llx len=%-3u %-18s value=0x%08x",
                    i, static_cast<unsigned long long>(e.fileOffset),
                    e.nameLength, e.name().c_str(), e.value);
        if (const auto tgt = e.valueFileOffset()) {
          std::printf(" ->file=0x%08llx%s",
                      static_cast<unsigned long long>(*tgt),
                      e.valueReachesDataRegion ? "" : " (outside data "
                                                     "region)");
        } else {
          std::printf(" (null)");
        }
        if (!e.nameEndsWithTerminator) {
          std::printf(" [no NUL in counted bytes]");
        }
        std::printf("\n");
      }
    }
    std::printf("  data region: [0x%llx, 0x%llx) — %llu bytes, "
                "structure not enumerated\n",
                static_cast<unsigned long long>(dir.dataRegionOffset),
                static_cast<unsigned long long>(dir.dataRegionEnd),
                static_cast<unsigned long long>(dir.dataRegionEnd -
                                                dir.dataRegionOffset));
    return 0;
  }

  if (family == mdk::MdkFileFamily::kDti) {
    const auto st = mdk::inspectDtiStructure(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   DTI structure (five image-relative TOC "
                "offsets @0x14: params / keyed records / arena table "
                "/ palette / grid)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::dtiStructureStatusName(st.status))
                    .c_str(),
                st.detail.empty() ? "" : " — ",
                st.detail.empty() ? "" : st.detail.c_str());
    if (st.status != mdk::DtiStructureStatus::kOk) {
      return 1;
    }
    std::printf("trailer:   name[12] @ size-12 %s\n",
                st.trailerPresent ? "present" : "ABSENT");
    std::printf("u32@0x10:  %u — %s trailer offset\n",
                st.secondaryLength,
                st.secondaryEqualsTrailerOffset ? "equals"
                                                : "does not equal");
    std::printf("toc:       [");
    for (std::size_t i = 0; i < mdk::kDtiTocEntries; ++i) {
      std::printf("%s0x%08x", i ? " " : "", st.tocImageOffsets[i]);
    }
    std::printf("] (image-relative; file = value + 4)\n");

    const auto& s0 = st.sections[mdk::kDtiSecParams];
    std::printf("  s0 params:   [0x%llx, 0x%llx) — %llu bytes; "
                "startArena=%u view=(%g %g %g %g) fill=(0x%x,0x%x) "
                "scroll=(0x%x,0x%x) grid=%ux%u altFill=(0x%x,0x%x)\n",
                static_cast<unsigned long long>(s0.fileStart),
                static_cast<unsigned long long>(s0.fileEnd),
                static_cast<unsigned long long>(s0.size()),
                st.params[0], std::bit_cast<float>(st.params[1]),
                std::bit_cast<float>(st.params[2]),
                std::bit_cast<float>(st.params[3]),
                std::bit_cast<float>(st.params[4]), st.params[5],
                st.params[6], st.params[7], st.params[8], st.params[9],
                st.params[10], st.params[0x0b], st.params[0x0c]);
    std::printf("             matrix[16] =");
    for (std::size_t i = 13; i < mdk::kDtiParamWords; ++i) {
      std::printf("%s0x%x", i == 13 ? " " : ",", st.params[i]);
    }
    std::printf("\n");

    const auto& s1 = st.sections[mdk::kDtiSecKeyed];
    std::printf("  s1 keyed:    [0x%llx, 0x%llx) count=%zu "
                "stride=24%s\n",
                static_cast<unsigned long long>(s1.fileStart),
                static_cast<unsigned long long>(s1.fileEnd),
                st.keyedRecords.size(),
                st.s1TrailingBytes ? " (trailing bytes)" : "");
    for (std::size_t i = 0; i < st.keyedRecords.size(); ++i) {
      const auto& e = st.keyedRecords[i];
      std::printf("    [%3zu] @0x%06llx word0=%u key=%u f=(%g %g %g "
                  "%g)\n", i,
                  static_cast<unsigned long long>(e.fileOffset),
                  e.word0, e.key, e.floatAt(0), e.floatAt(1),
                  e.floatAt(2), e.floatAt(3));
    }

    const auto& s2 = st.sections[mdk::kDtiSecArenas];
    std::printf("  s2 arenas:   [0x%llx, 0x%llx) count=%zu "
                "payloads@0x%llx\n",
                static_cast<unsigned long long>(s2.fileStart),
                static_cast<unsigned long long>(s2.fileEnd),
                st.arenas.size(),
                static_cast<unsigned long long>(
                    st.s2PayloadRegionStart));
    for (std::size_t i = 0; i < st.arenas.size(); ++i) {
      const auto& a = st.arenas[i];
      std::printf("    [%3zu] @0x%06llx %-9s imgOff=0x%08x "
                  "->file=0x%08llx scalar=%g subs=%u%s\n", i,
                  static_cast<unsigned long long>(a.fileOffset),
                  a.name().c_str(), a.payloadImageOffset,
                  static_cast<unsigned long long>(a.payloadFileOffset),
                  a.scalar(), a.subRecordCount,
                  a.nameEndsWithTerminator ? "" : " [no NUL]");
      for (std::size_t j = 0; j < a.subRecords.size(); ++j) {
        const auto& sub = a.subRecords[j];
        const char* tag = sub.type == 2   ? " HotGen"
                          : sub.type == 4 ? " HotPick"
                          : sub.type == 6 ? " connect"
                                          : "";
        std::printf("         sub[%3zu] @0x%06llx type=%u%s "
                    "f1=0x%08x f2=0x%08x tail=[%08x %08x %08x %08x "
                    "%08x %08x]%s\n",
                    j,
                    static_cast<unsigned long long>(sub.fileOffset),
                    sub.type, tag, sub.fields[0], sub.fields[1],
                    sub.fields[2], sub.fields[3], sub.fields[4],
                    sub.fields[5], sub.fields[6], sub.fields[7],
                    (sub.type == 2 || sub.type == 4)
                        ? (std::string(" name=\"") + sub.name18() +
                           "\"")
                              .c_str()
                        : "");
      }
    }

    const auto& s3 = st.sections[mdk::kDtiSecPalette];
    std::printf("  s3 palette:  [0x%llx, 0x%llx) count=%u "
                "rgb=[0x%llx, 0x%llx)%s\n",
                static_cast<unsigned long long>(s3.fileStart),
                static_cast<unsigned long long>(s3.fileEnd),
                st.paletteCount,
                static_cast<unsigned long long>(
                    st.paletteBytes.fileStart),
                static_cast<unsigned long long>(
                    st.paletteBytes.fileEnd),
                st.s3TrailingBytes ? " (trailing bytes)" : "");

    const auto& s4 = st.sections[mdk::kDtiSecGrid];
    std::printf("  s4 grid:     [0x%llx, 0x%llx) — %llu bytes = %u "
                "plane(s) x 0x%llx%s\n",
                static_cast<unsigned long long>(s4.fileStart),
                static_cast<unsigned long long>(s4.fileEnd),
                static_cast<unsigned long long>(s4.size()),
                st.gridPlaneCount,
                static_cast<unsigned long long>(st.gridPlaneSize),
                st.s4TrailingBytes ? " (trailing bytes)" : "");
    return 0;
  }

  if (family == mdk::MdkFileFamily::kFti) {
    const auto dir = mdk::inspectFtiDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   FTI directory (length envelope; count u32 "
                "@0x04, records 12 bytes @0x08: name[8] + imgOff)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::ftiDirectoryStatusName(dir.status))
                    .c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::FtiDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("offsets:   sorted=%s unique=%s first-at-dir-end=%s\n",
                dir.offsetsSortedAscending ? "yes" : "NO",
                dir.offsetsUnique ? "yes" : "NO",
                dir.firstPayloadAtDirectoryEnd ? "yes" : "NO");

    for (std::size_t i = 0; i < dir.records.size(); ++i) {
      const auto& e = dir.records[i];
      std::printf("  [%3zu] @0x%06llx %-8s imgOff=0x%08x "
                  "->file=0x%08llx span=[0x%08llx,0x%08llx) %lluB%s\n",
                  i,
                  static_cast<unsigned long long>(e.recordFileOffset),
                  e.name().c_str(), e.imageOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadEnd),
                  static_cast<unsigned long long>(e.payloadSize()),
                  e.nameHasTerminator ? "" : " [no NUL]");
    }
    return 0;
  }

  if (family == mdk::MdkFileFamily::kBni) {
    const auto dir = mdk::inspectBniDirectory(
        std::span<const std::byte>(file->data(), file->size()));
    std::printf("entries:   BNI directory (length envelope; count u32 "
                "@0x04, records 16 bytes @0x08: name[12] + imgOff)\n");
    std::printf("status:    %s%s%s\n",
                std::string(mdk::bniDirectoryStatusName(dir.status))
                    .c_str(),
                dir.detail.empty() ? "" : " — ",
                dir.detail.empty() ? "" : dir.detail.c_str());
    if (dir.status != mdk::BniDirectoryStatus::kOk) {
      return 1;
    }
    std::printf("count:     %u\n", dir.count);
    std::printf("dir-end:   0x%llx\n",
                static_cast<unsigned long long>(dir.directoryEnd));
    std::printf("offsets:   sorted=%s unique=%s first-at-dir-end=%s\n",
                dir.offsetsSortedAscending ? "yes" : "NO",
                dir.offsetsUnique ? "yes" : "NO",
                dir.firstPayloadAtDirectoryEnd ? "yes" : "NO");

    for (std::size_t i = 0; i < dir.records.size(); ++i) {
      const auto& e = dir.records[i];
      std::printf("  [%3zu] @0x%06llx %-12s imgOff=0x%08x "
                  "->file=0x%08llx span=[0x%08llx,0x%08llx) %lluB%s\n",
                  i,
                  static_cast<unsigned long long>(e.recordFileOffset),
                  e.name().c_str(), e.imageOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadFileOffset),
                  static_cast<unsigned long long>(e.payloadEnd),
                  static_cast<unsigned long long>(e.payloadSize()),
                  e.nameHasTerminator ? "" : " [no NUL]");
    }
    return 0;
  }

  const auto dir = mdk::inspectSniDirectory(
      std::span<const std::byte>(file->data(), file->size()));
  std::printf("entries:   SNI directory (count u32 @0x14, records "
              "24 bytes @0x18)\n");
  std::printf("status:    %s%s%s\n",
              std::string(mdk::sniDirectoryStatusName(dir.status)).c_str(),
              dir.detail.empty() ? "" : " — ",
              dir.detail.empty() ? "" : dir.detail.c_str());
  if (dir.status != mdk::SniDirectoryStatus::kOk) {
    return 1;
  }
  std::printf("count:     %u\n", dir.count);
  std::printf("dir-end:   0x%llx\n",
              static_cast<unsigned long long>(dir.directoryEnd));
  std::printf("trailer:   name[12] @ size-12 %s\n",
              dir.trailerPresent ? "present" : "ABSENT");
  std::printf("u32@0x10:  %u — %s trailer offset\n",
              dir.secondaryLength,
              dir.secondaryEqualsTrailerOffset ? "equals" : "does not equal");

  for (std::size_t i = 0; i < dir.entries.size(); ++i) {
    const auto& e = dir.entries[i];
    if (e.isSentinel()) {
      std::printf("  [%3zu] %-12s SENTINEL (field0x0c=size=0xffffffff) "
                  "marker fileOff=0x%08llx\n",
                  i, e.name().c_str(),
                  static_cast<unsigned long long>(e.payloadFileOffset()));
    } else {
      std::printf("  [%3zu] %-12s field0x0c=0x%08x blobOff=0x%08x "
                  "fileOff=0x%08llx size=%u\n",
                  i, e.name().c_str(), e.fieldAt0x0C, e.blobOffset,
                  static_cast<unsigned long long>(e.payloadFileOffset()),
                  e.payloadSize);
    }
  }
  return 0;
}
