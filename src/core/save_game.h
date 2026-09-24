#pragma once
// save_game.h — Phase 14B: the original .SAV envelope + packet model.
//
// EVIDENCE (instruction-level, BUILD_A — docs/reverse-engineering/
// SAVE_FORMAT.md): file writer FUN_00427ed4, packet writers
// FUN_00427970/FUN_004266b8-family, reader chain FUN_00427f94 →
// FUN_00426618/FUN_004264f0 → FUN_004278c0 (+FUN_00427218 for full
// saves). All five locally available real saves validate against this
// format (size + checksum + packet walk + SEND terminator).
//
//   Envelope (OBSERVED — FUN_004264f0 header check + FUN_00426440
//   writer close):
//     +0x00  u32  file size (== physical file length)
//     +0x04  u32  checksum = sum of the raw bytes [8, fileSize)
//     +0x08  packet stream
//
//   Packet (OBSERVED — FUN_00427970 writer / FUN_00427ab4 reader):
//     +0x00  u32  tag (fourcc, little-endian)
//     +0x04  u32  payload size
//     +0x08  payload[size]
//   Expected sizes come from the static registry at 0x49b2ac (16
//   entries — savePacketTable). A reader that expects a specific tag
//   compares both tag and size ("SAVE CORRUPT" diagnostics at
//   0x496a7d+); a DEND tag is the demo-stream end sentinel.
//
//   Stream obfuscation (OBSERVED — FUN_00426668 read byte /
//   FUN_00426380 write byte, state at 0x54be40/0x54be41):
//     the leading SAVE packet (tag, size, 2-byte payload) is stored
//     CLEAR; its payload is the u16 seed. Every byte AFTER the seed is
//     ciphered: plain = raw ^ key; key += delta; where key starts at
//     seed&0xff and delta = seed>>8. Byte-wise wraparound (u8).
//
//   Packet order in real saves (OBSERVED):
//     header-only:  SAVE THMB GAME SEND
//     full:         SAVE THMB GAME MORE PLAY DAMP CAME
//                   (AREN ALIE* FAND*)* BULL BULL BULL SEND
//
//   GAME payload (24 bytes, 6 u32 — OBSERVED writer FUN_00426e98,
//   reader FUN_004278c0):
//     +0x00  mode field: <1000 → header-only save, value = 0x541492
//           verbatim (3 traversal / 6 briefing); >=1000 → full save,
//           value = mode + 1000 (reader recovers the mode via
//           (field + 0x18) & 0xff).
//     +0x04  levelId → 0x541498, validated [0,6)
//     +0x08  uninitialized stack dword in the original writer —
//           present in real saves as code-address garbage; the reader
//           never touches it
//     +0x0c  health → 0x541554, validated (0, 0x97]
//     +0x10  → 0x541637 (death/restore counter)
//     +0x14  → 0x54163b
//
//   PLAY payload (239 bytes — a verbatim copy of the original
//   0x541554..0x541642 global block, OBSERVED via the writer memcpy
//   target): health at +0x00, the weapon-select/indicator bytes
//   0x541618..0x54161b at +0xc4, the six-dword ammo block
//   0x54161f..0x541633 at +0xcb, 0x54163b at +0xe7. Individual stat
//   fields inside the block are UNKNOWN beyond those offsets.
//
//   DAMP payload (724 bytes — the 0x540bfc player block): position
//   xyz at +0x00, previous position at +0x0c (0x540c08 commit vec).
//   CAME payload (200 bytes — the 0x540b28 camera block): camera
//   position at +0x00, basis/orientation tail UNKNOWN-level.
//   AREN payload (1126 bytes — the 0x466 arena record): arena name at
//   +0x00 (e.g. "DANT_1"), dynamic-object count at +0x0c (each object
//   is followed by an ALIE packet — count corroborated by the real
//   packet walk), fan count at +0x10 (each → FAND).
//   ALIE (814B = 0x32e object record), FAND (72B = 0x48 fan record),
//   BULL (252B ×3 = the 0x540ed4 three-slot shot pool).
//
// NATIVE PORT DECISIONS:
//   - The reader parses the whole stream and exposes typed views for
//     the proven packets only; unknown payloads stay raw byte views.
//   - The writer emits the header-only form (the LASTGAME/checkpoint
//     and briefing-save shape). Full-save writing is the documented
//     remaining seam — the AREN/ALIE/FAND records are raw original
//     memory images that this port deliberately does not reproduce
//     field-for-field.
//   - GAME+0x08 is written as 0 (the original stores uninit stack
//     garbage there; any value is load-neutral).
//   - No original bytes are embedded; all sizes/tags come from the
//     OBSERVED registry.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdk {

// OBSERVED — the expected-packet registry at 0x49b2ac (16 {tag,size}
// records; FUN_00427ab4 scans it by tag). Sizes are payload bytes.
struct SavePacketSpec {
  std::uint32_t tag;   // fourcc little-endian ("SAVE" = 0x45564153)
  std::uint32_t size;
};

const SavePacketSpec* savePacketTable(int& count);
const SavePacketSpec* savePacketSpec(std::uint32_t tag);  // nullptr = unregistered

// Fourcc helper — saveTag("SAVE") == 0x45564153.
constexpr std::uint32_t saveTag(char a, char b, char c, char d) {
  return std::uint32_t(std::uint8_t(a)) |
         (std::uint32_t(std::uint8_t(b)) << 8) |
         (std::uint32_t(std::uint8_t(c)) << 16) |
         (std::uint32_t(std::uint8_t(d)) << 24);
}

// Reader result — the original's failure ladder (see SAVE_FORMAT.md
// for the diagnostic each step maps to).
enum class SaveError : int {
  kOk = 0,
  kReadFail,          // open/read failure ("Cannot load game %s")
  kBadSizeField,      // u32@0 != file length
  kChecksumMismatch,  // Σ bytes[8..size) != u32@4
  kMissingSaveTag,    // first packet is not SAVE ("incorrect header")
  kTruncatedPacket,   // packet header/payload overruns the stream
  kUnknownTag,        // tag not in the 0x49b2ac registry
  kPacketSize,        // payload size != registry size
  kMissingSend,       // stream ended without the SEND terminator
  kMissingGame,       // no GAME packet (the loader's first real read)
  kBadLevelId,        // GAME.levelId outside [0,6)
  kBadHealth,         // GAME.health outside (0,0x97]
};
const char* saveErrorName(SaveError e);

struct SavePacketView {
  std::uint32_t tag = 0;
  std::span<const std::byte> payload;
  std::uint32_t streamOffset = 0;  // offset within the decoded stream
};

// GAME packet — all six dwords (see header for the field evidence).
struct SaveGamePacket {
  int modeField = 0;     // +0x00 (<1000 = header-only, mode verbatim)
  int levelId = 0;       // +0x04
  int field8 = 0;        // +0x08 — uninit dword in the original
  int health = 0;        // +0x0c
  int deathCount = 0;    // +0x10 → 0x541637
  int field54163b = 0;   // +0x14 → 0x54163b

  bool full() const { return modeField >= 1000; }
  // FUN_004278c0's decode: full saves store mode+1000 and recover via
  // (field + 0x18) & 0xff; header-only fields are the mode verbatim.
  int mode() const {
    return full() ? ((modeField + 0x18) & 0xff) : modeField;
  }
};

// AREN packet head — the proven prefix of the 1126-byte record.
struct SaveArenaHead {
  char name[9] = {};     // +0x00 — arena record name ("DANT_1" …)
  int objectCount = 0;   // +0x0c — ALIE packets that follow
  int fanCount = 0;      // +0x10 — FAND packets that follow
};

struct SaveGame {
  // Envelope (raw values as stored).
  std::uint32_t fileSize = 0;
  std::uint32_t checksum = 0;
  std::uint16_t seed = 0;        // SAVE packet payload (cipher seed)

  // Decoded stream + walked packets (payloads alias `plain`).
  std::vector<std::byte> plain;
  std::vector<SavePacketView> packets;

  bool headerOnly = false;       // parsed shape: no MORE packet
  SaveGamePacket game{};

  const SavePacketView* find(std::uint32_t tag) const;
  std::vector<SavePacketView> all(std::uint32_t tag) const;
  // AREN head parse — returns false when the payload is short.
  static bool arenaHead(const SavePacketView& pkt, SaveArenaHead& out);
};

// Full-file parse: envelope checks → deobfuscate → packet walk →
// GAME decode + the FUN_004278c0 validation (levelId [0,6),
// health (0,0x97]). `strictPackets` mirrors FUN_00427ab4's
// tag/size-versus-registry check for every packet; pass false for a
// lenient census walk (tags still must be registered to know sizes).
SaveError saveGameParse(const std::byte* file, std::size_t len,
                        SaveGame& out, bool strictPackets = true);
SaveError saveGameLoadFile(const std::filesystem::path& path,
                           SaveGame& out, bool strictPackets = true);

// ---------------------------------------------------------------------------
// Writer — header-only form (LASTGAME/checkpoint + briefing saves)
// ---------------------------------------------------------------------------

// What the writer needs — one GAME packet plus the thumbnail bytes.
// `seed` selects the obfuscation keystream (the original uses
// rand()×2; callers may pin it for determinism). `thumbnail` is the
// raw THMB payload (3648 bytes); an empty span emits a zeroed thumb —
// the load path only previews it.
struct SaveWriteInput {
  int modeField = 3;               // 3 traversal / 6 briefing (header-only
                                   // semantics — never the +1000 form)
  int levelId = 0;
  int health = 100;
  int deathCount = 0;
  int field54163b = 0;
  std::uint16_t seed = 0;          // 0 → caller wants a fixed test seed
  std::span<const std::byte> thumbnail;  // size 3648 or empty
};

// Emits {u32 size, u32 cksum} + SAVE + THMB + GAME + SEND exactly as
// FUN_00427ed4 + FUN_00426440 do for the header-only flag. Returns an
// empty vector on invalid input (modeField outside {3,6} — the
// original writer's (mode==6)?6:3 clamp is reproduced by clamping any
// other value to 3, matching "else 3").
std::vector<std::byte> saveGameWriteHeaderOnly(const SaveWriteInput& in);

// FUN_00426440's close-out: patch size+checksum over a finished stream
// (exposed for tests).
std::uint32_t saveEnvelopeChecksum(std::span<const std::byte> file,
                                   std::size_t streamOffset = 8);

// ---------------------------------------------------------------------------
// SAVES\ directory model — LASTGAME.SAV lifecycle
// ---------------------------------------------------------------------------

// The original's save-root seam: "<base>\SAVES\<name>.SAV" built by
// FUN_0041ab50 + "%s.SAV" append. This host keeps the same layout but
// never writes into the (read-only) data root — the caller supplies a
// writable directory.
class SaveStore {
public:
  explicit SaveStore(std::filesystem::path dir) : dir_(std::move(dir)) {}
  const std::filesystem::path& dir() const { return dir_; }

  std::filesystem::path pathFor(const std::string& stem) const;
  std::filesystem::path lastgamePath() const { return pathFor("LASTGAME"); }

  bool exists(const std::string& stem) const;
  bool writeLastgame(const SaveWriteInput& in) const;
  bool deleteLastgame() const;   // the FUN_0047d1a0 quit-path edge
  SaveError loadLastgame(SaveGame& out) const;

private:
  std::filesystem::path dir_;
};

} // namespace mdk
