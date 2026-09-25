// save_game.cpp — Phase 14B .SAV envelope + packet model.
// See save_game.h for the evidence summary; instruction addresses
// cited inline are BUILD_A (MDK95.EXE).

#include "core/save_game.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace mdk {

namespace {

// OBSERVED — the registry at 0x49b2ac, dumped from the binary image
// (16 {tag,size} records; a 17th slot is the list terminator).
constexpr SavePacketSpec kRegistry[] = {
    {saveTag('D', 'E', 'M', 'O'), 0},     // demo playback header
    {saveTag('D', 'E', 'N', 'D'), 0},     // demo stream end sentinel
    {saveTag('K', 'E', 'Y', 'S'), 20},    // demo input frame
    {saveTag('R', 'A', 'T', 'E'), 4},     // demo playback pacing
    {saveTag('S', 'A', 'V', 'E'), 2},     // save header + cipher seed
    {saveTag('S', 'E', 'N', 'D'), 0},     // save terminator
    {saveTag('G', 'A', 'M', 'E'), 24},    // mode/level/health/state
    {saveTag('T', 'H', 'M', 'B'), 3648},  // 768B palette + 64x45 image
    {saveTag('M', 'O', 'R', 'E'), 52},    // session/timer globals
    {saveTag('P', 'L', 'A', 'Y'), 239},   // 0x541554 player stats block
    {saveTag('D', 'A', 'M', 'P'), 724},   // 0x540bfc player motion block
    {saveTag('C', 'A', 'M', 'E'), 200},   // 0x540b28 camera block
    {saveTag('A', 'R', 'E', 'N'), 1126},  // 0x466 arena record
    {saveTag('A', 'L', 'I', 'E'), 814},   // 0x32e dynamic-object record
    {saveTag('F', 'A', 'N', 'D'), 72},    // 0x48 fan record
    {saveTag('B', 'U', 'L', 'L'), 252},   // 0x540ed4 shot-pool slot
};
constexpr int kRegistryCount = sizeof(kRegistry) / sizeof(kRegistry[0]);

constexpr std::uint32_t kTagSave = saveTag('S', 'A', 'V', 'E');
constexpr std::uint32_t kTagSend = saveTag('S', 'E', 'N', 'D');
constexpr std::uint32_t kTagGame = saveTag('G', 'A', 'M', 'E');
constexpr std::uint32_t kTagThmb = saveTag('T', 'H', 'M', 'B');

std::uint32_t rd32(const std::byte* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
         (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::int32_t rdi32(const std::byte* p) {
  return static_cast<std::int32_t>(rd32(p));
}
void wr32(std::byte* p, std::uint32_t v) {
  p[0] = std::byte(v);
  p[1] = std::byte(v >> 8);
  p[2] = std::byte(v >> 16);
  p[3] = std::byte(v >> 24);
}

// FUN_00426668 / FUN_00426380 — the rolling byte cipher. `key` starts
// at seed&0xff, `delta` at seed>>8 (state bytes 0x54be40/0x54be41).
void cipherRun(std::byte* p, std::size_t n, std::uint16_t seed) {
  std::uint8_t key = std::uint8_t(seed & 0xff);
  const std::uint8_t delta = std::uint8_t(seed >> 8);
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = std::byte(std::uint8_t(p[i]) ^ key);
    key = std::uint8_t(key + delta);
  }
}

} // namespace

const SavePacketSpec* savePacketTable(int& count) {
  count = kRegistryCount;
  return kRegistry;
}

const SavePacketSpec* savePacketSpec(std::uint32_t tag) {
  for (int i = 0; i < kRegistryCount; ++i)
    if (kRegistry[i].tag == tag) return &kRegistry[i];
  return nullptr;
}

const char* saveErrorName(SaveError e) {
  switch (e) {
    case SaveError::kOk: return "ok";
    case SaveError::kReadFail: return "read-fail";
    case SaveError::kBadSizeField: return "bad-size-field";
    case SaveError::kChecksumMismatch: return "checksum-mismatch";
    case SaveError::kMissingSaveTag: return "missing-save-tag";
    case SaveError::kTruncatedPacket: return "truncated-packet";
    case SaveError::kUnknownTag: return "unknown-tag";
    case SaveError::kPacketSize: return "packet-size-mismatch";
    case SaveError::kMissingSend: return "missing-send";
    case SaveError::kMissingGame: return "missing-game";
    case SaveError::kBadLevelId: return "bad-level-id";
    case SaveError::kBadHealth: return "bad-health";
  }
  return "?";
}

const SavePacketView* SaveGame::find(std::uint32_t tag) const {
  for (const auto& p : packets)
    if (p.tag == tag) return &p;
  return nullptr;
}

std::vector<SavePacketView> SaveGame::all(std::uint32_t tag) const {
  std::vector<SavePacketView> out;
  for (const auto& p : packets)
    if (p.tag == tag) out.push_back(p);
  return out;
}

bool SaveGame::arenaHead(const SavePacketView& pkt, SaveArenaHead& out) {
  if (pkt.payload.size() < 0x14) return false;
  const auto* d = pkt.payload.data();
  std::memcpy(out.name, d, 8);
  out.name[8] = 0;
  out.objectCount = rdi32(d + 0x0c);
  out.fanCount = rdi32(d + 0x10);
  return true;
}

SaveError saveGameParse(const std::byte* file, std::size_t len,
                        SaveGame& out, bool strictPackets) {
  out = SaveGame{};
  // FUN_004264f0 — header validation ladder.
  if (file == nullptr || len < 18) return SaveError::kReadFail;
  out.fileSize = rd32(file);
  if (out.fileSize != len) return SaveError::kBadSizeField;
  out.checksum = rd32(file + 4);
  if (saveEnvelopeChecksum({file, len}, 8) != out.checksum)
    return SaveError::kChecksumMismatch;

  // The stream starts at file offset 8. The leading SAVE packet is
  // stored clear; its 2-byte payload is the cipher seed.
  const std::size_t streamLen = len - 8;
  out.plain.assign(file + 8, file + 8 + streamLen);
  if (rd32(out.plain.data()) != kTagSave || rd32(out.plain.data() + 4) != 2)
    return SaveError::kMissingSaveTag;
  out.seed = std::uint16_t(std::uint8_t(out.plain[8]) |
                           (std::uint16_t(std::uint8_t(out.plain[9])) << 8));
  // Ciphered region: everything after the seed (stream offset 10).
  cipherRun(out.plain.data() + 10, streamLen - 10, out.seed);

  // Packet walk — FUN_00427ab4 semantics: 8-byte header, payload of
  // the recorded size; tag/size checked against the registry.
  bool sawSend = false;
  for (std::size_t off = 0; off + 8 <= streamLen;) {
    SavePacketView pv;
    pv.tag = rd32(out.plain.data() + off);
    const std::uint32_t size = rd32(out.plain.data() + off + 4);
    pv.streamOffset = std::uint32_t(off);
    if (off + 8 + size > streamLen) return SaveError::kTruncatedPacket;
    const SavePacketSpec* spec = savePacketSpec(pv.tag);
    if (spec == nullptr) return SaveError::kUnknownTag;
    if (strictPackets && size != spec->size) return SaveError::kPacketSize;
    pv.payload = {out.plain.data() + off + 8, size};
    out.packets.push_back(pv);
    off += 8 + size;
    if (pv.tag == kTagSend) {
      sawSend = true;
      if (off != streamLen) return SaveError::kTruncatedPacket;
      break;
    }
  }
  if (!sawSend) return SaveError::kMissingSend;

  // GAME — the loader's first authoritative packet (FUN_004278c0).
  const SavePacketView* g = out.find(kTagGame);
  if (g == nullptr) return SaveError::kMissingGame;
  if (g->payload.size() < 24) return SaveError::kPacketSize;
  const auto* d = g->payload.data();
  out.game.modeField = rdi32(d + 0x00);
  out.game.levelId = rdi32(d + 0x04);
  out.game.field8 = rdi32(d + 0x08);
  out.game.health = rdi32(d + 0x0c);
  out.game.deathCount = rdi32(d + 0x10);
  out.game.field54163b = rdi32(d + 0x14);
  out.headerOnly = !out.game.full();
  // FUN_004278c0's acceptance checks (CORROBORATED — real saves carry
  // health=150, so the bound is >=0x97 fails / 150 valid).
  if (out.game.levelId < 0 || out.game.levelId >= 6)
    return SaveError::kBadLevelId;
  if (out.game.health < 1 || out.game.health > 150)
    return SaveError::kBadHealth;
  return SaveError::kOk;
}

SaveError saveGameLoadFile(const std::filesystem::path& path,
                           SaveGame& out, bool strictPackets) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return SaveError::kReadFail;
  const auto sz = f.tellg();
  if (sz <= 0 || sz > (1 << 22)) return SaveError::kReadFail;
  f.seekg(0);
  std::vector<std::byte> buf;
  buf.resize(std::size_t(sz));
  if (!f.read(reinterpret_cast<char*>(buf.data()), sz))
    return SaveError::kReadFail;
  return saveGameParse(buf.data(), buf.size(), out, strictPackets);
}

std::uint32_t saveEnvelopeChecksum(std::span<const std::byte> file,
                                   std::size_t streamOffset) {
  std::uint32_t sum = 0;
  for (std::size_t i = streamOffset; i < file.size(); ++i)
    sum += std::uint8_t(file[i]);
  return sum;
}

std::vector<std::byte> saveGameWriteHeaderOnly(const SaveWriteInput& in) {
  // Packet payload assembly — SAVE (clear) + ciphered THMB/GAME/SEND,
  // then FUN_00426440's size/checksum patch.
  std::vector<std::byte> stream;
  auto put32 = [&](std::uint32_t v) {
    std::byte b[4];
    wr32(b, v);
    stream.insert(stream.end(), b, b + 4);
  };
  auto putPayload = [&](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::byte*>(p);
    stream.insert(stream.end(), b, b + n);
  };
  auto putPacket = [&](std::uint32_t tag, const void* p, std::size_t n) {
    put32(tag);
    put32(std::uint32_t(n));
    putPayload(p, n);
  };

  // SAVE — written before the cipher arms (FUN_00427ed4 order).
  const std::uint16_t seed = in.seed;
  putPacket(kTagSave, &seed, 2);

  // THMB — 3648 bytes; zero-filled when the caller has no capture.
  std::vector<std::byte> thmb(3648, std::byte(0));
  if (!in.thumbnail.empty())
    std::memcpy(thmb.data(), in.thumbnail.data(),
                in.thumbnail.size() < 3648 ? in.thumbnail.size() : 3648);
  putPacket(kTagThmb, thmb.data(), thmb.size());

  // GAME — FUN_00426e98's header-only form: mode field is the raw
  // 0x541492 value clamped to (mode==6)?6:3; health floors at 100
  // when the live value is < 0x65; +0x08 is a don't-care dword.
  std::int32_t game[6];
  game[0] = (in.modeField == 6) ? 6 : 3;
  game[1] = in.levelId;
  game[2] = 0;
  game[3] = (in.health < 0x65) ? 100 : in.health;
  game[4] = in.deathCount;
  game[5] = in.field54163b;
  putPacket(kTagGame, game, sizeof(game));

  putPacket(kTagSend, nullptr, 0);

  // Cipher arm + envelope patch.
  return saveGameEnvelope(std::move(stream), seed);
}

std::vector<std::byte> saveGameEnvelope(std::vector<std::byte> stream,
                                        std::uint16_t seed) {
  // The leading SAVE packet (8-byte header + 2-byte seed) stays clear;
  // everything after it runs through the rolling cipher.
  cipherRun(stream.data() + 10, stream.size() - 10, seed);
  std::vector<std::byte> file(8 + stream.size());
  wr32(file.data(), std::uint32_t(file.size()));
  std::memcpy(file.data() + 8, stream.data(), stream.size());
  wr32(file.data() + 4, saveEnvelopeChecksum(file, 8));
  return file;
}

// ---------------------------------------------------------------------------
// SaveStore
// ---------------------------------------------------------------------------

std::filesystem::path SaveStore::pathFor(const std::string& stem) const {
  return dir_ / (stem + ".SAV");
}

bool SaveStore::exists(const std::string& stem) const {
  std::error_code ec;
  return std::filesystem::is_regular_file(pathFor(stem), ec);
}

bool SaveStore::writeLastgame(const SaveWriteInput& in) const {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  const auto bytes = saveGameWriteHeaderOnly(in);
  if (bytes.empty()) return false;
  std::ofstream f(lastgamePath(), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()),
          std::streamsize(bytes.size()));
  return bool(f);
}

bool SaveStore::deleteLastgame() const {
  std::error_code ec;
  return std::filesystem::remove(lastgamePath(), ec);
}

SaveError SaveStore::loadLastgame(SaveGame& out) const {
  return saveGameLoadFile(lastgamePath(), out);
}

} // namespace mdk
