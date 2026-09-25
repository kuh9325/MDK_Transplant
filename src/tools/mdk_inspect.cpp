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
#include "core/frontend_machines.h"
#include "core/fti_directory.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/gameplay_input.h"
#include "core/mti_directory.h"
#include "core/mto_directory.h"
#include "core/player_motion.h"
#include "core/player_surface.h"
#include "core/player_vertical.h"
#include "core/progression_runtime.h"
#include "core/save_full_restore.h"
#include "core/save_game.h"
#include "core/sni_directory.h"
#include "core/stream_context.h"
#include "core/traversal_runtime.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
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
               "                            --yaw DEG --frames N)\n"
               "       mdk-inspect --data-path DIR --freefall-runtime "
               "<relative-path>\n"
               "                            (a FALL3D.BNI path; reads the\n"
               "                            FALLPU_<course+1> pickup list and\n"
               "                            steps the Phase 13A freefall core.\n"
               "                            Options: --course 0..4 --skill 0..2\n"
               "                            --seed N --frames N)\n"
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
               "                            restored arena N)\n"
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

} // namespace

int main(int argc, char** argv) {
  std::optional<std::string> dataPath;
  std::optional<std::string> target;
  std::optional<std::string> visualInfoName;
  std::optional<std::string> fontInfoName;
  std::optional<std::string> spriteInfoName;
  std::optional<unsigned> fontInfoCode;
  bool entriesMode = false;
  bool collisionProbe = false;
  bool arenaObjects = false;
  bool surfaceCensus = false;
  bool arenaRender = false;
  bool traversalRuntime = false;
  bool freefallRuntime = false;
  bool campaignHandoff = false;
  bool campaignSequence = false;
  std::optional<std::string> saveInfoPath;
  std::optional<std::string> saveRoundtripPath;
  std::optional<std::string> saveRestorePath;
  int saveActivateIdx = -1;
  int ffCourse = 0;
  int ffSkill = 0;
  unsigned ffSeed = 0xC0FFEE;
  bool scriptDisasm = false;
  bool objScriptDisasm = false;
  std::string scriptDisasmName;
  std::optional<std::string> travArena;
  float travStart[3] = {0.0f, 0.0f, 0.0f};
  bool travStartGiven = false;
  float travYaw = 0.0f;
  bool travYawGiven = false;
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
    } else if (!std::strcmp(a, "--freefall-runtime")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // FALL3D.BNI path
      freefallRuntime = true;
    } else if (!std::strcmp(a, "--campaign-handoff")) {
      const char* v = value(a);
      if (!v) return usage();
      target = v;                 // FALL3D.BNI path
      campaignHandoff = true;
    } else if (!std::strcmp(a, "--campaign-sequence")) {
      campaignSequence = true;
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
    for (int f = 0; f < travFrames; ++f) {
      const int phase = f < 10 ? 0 : f < 30 ? 1 : f < 35 ? 0 : f < 50 ? 3 : 0;
      const auto out =
          mdk::stepTraversalRuntime(rt, rawFor(phase), bindings, timing);
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
    return 0;
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
      !spriteInfoName && !collisionProbe && !arenaObjects &&
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
    for (int f = 0; f < travFrames; ++f) {
      const int phase = f < 10 ? 0 : f < 30 ? 1 : f < 35 ? 0 : f < 50 ? 3 : 0;
      const auto out =
          mdk::stepTraversalRuntime(rt, rawFor(phase), bindings, timing);
      ++framesRun;
      floorObjSeen |= (rt.cs.contactFlags & 2) != 0;
      groundedSeen |= out.grounded;
      airborneSeen |= !out.grounded;
      contactSeen |= out.contactObj != 0;
      partnerSeen |= out.partnerArenaIndex >= 0;
      std::printf(
          "f=%03d a=%d p=%d pos=(%8.2f,%8.2f,%8.2f) yaw=%6.1f "
          "mv=%5.2f sv=%5.2f vv=%6.2f gnd=%d ctc=%08x sld=%d ev=%d/%d "
          "cam=(%8.2f,%8.2f,%8.2f)%s%s\n",
          out.frame, out.curArenaIndex, out.partnerArenaIndex,
          (double)out.pos[0], (double)out.pos[1], (double)out.pos[2],
          (double)out.yawDeg, (double)out.moveVel,
          (double)out.strafeVel, (double)out.vertVel,
          out.grounded ? 1 : 0, out.contactObj, out.slideChannel,
          out.eventType, out.eventMag,
          (double)out.camera.pos[0], (double)out.camera.pos[1],
          (double)out.camera.pos[2],
          out.overheadViewActive ? " OVH" : "",
          out.viewOnPartner ? " VP" : "");
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
    const std::uint32_t code =
        mdk::cmiScriptCodeOffset(cmi, img, scriptDisasmName);
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
    std::printf("script: %s  codeOff=0x%x\n", scriptDisasmName.c_str(),
                code);
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
