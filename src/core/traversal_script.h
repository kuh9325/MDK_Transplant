// traversal_script.h — Phase 5H: the tr_alcmd arena script VM.
//
// Bounded reconstruction of the original interpreter FUN_004388d8 —
// the per-frame driver that runs each arena's CMI table-3 script
// record. All field offsets, opcodes and control-flow rules below are
// OBSERVED from MDK95.EXE disassembly unless marked otherwise; see
// docs/reverse-engineering/EVIDENCE_POLICY.md.
//
// Original layout (OBSERVED):
//   The interpreter context is the arena record + 0x118; the frame
//   driver calls FUN_004388d8(c48+0x118) for the current arena and
//   FUN_004388d8(ca4+0x118) for the active partner arena, each when
//   that arena's +0x220 field is nonzero.
//
//   ctx+0x108  == arena+0x220   script gate + persisted entry/resume
//                               PC (image-relative code offset; 0 =
//                               no script / stopped)
//   ctx+0x22c  == arena+0x344   wait timer in seconds, decremented by
//                               1/30 (0x49b6f4) per frame entry
//   ctx+0x230  == arena+0x348   wait-resume PC (image offset)
//   ctx+0x60   == arena+0x178   bound object (self — arena record)
//   ctx+0x0c   == arena+0x12c   arena name ptr
//   ctx+0x21d                   event byte (FUN_004546ac synthetic)
//   ctx+0x21e                   running flag (0xff opcode clears)
//   ctx+0x244                   ctx-local flag dword (group 2)
//   ctx+0x248                   call-stack depth (cap 4)
//   ctx+0x24c +0x25c            return-PC / saved-+0x108 stack slots
//   ctx+0x26c                   per-depth u16 marker (cleared)
//   ctx+0x2b8                   caller/parent context (var mode 4,
//                               flag group 'else')
//   ctx+0x312                   child flag dword (group 5)
//   ctx+0x30e                   f32 field written by opcode 0x99
//   ctx+0x11a                   u8 field written by 0x0b, tested by
//                               0x0a against a named object
//
//   Globals consumed by the proven corridor subset:
//     0x54c6bc  script image base (all PC/offsets image-relative)
//     0x540d88  f32 var array   (operand group 0)
//     boundObj+0x48  object f32 (operand group 1)
//     ctx+0x234  ctx f32 locals (operand group 2)
//     operand mode 3 = inline f32
//     0x540d98  global flag dword (flag group 0)
//     0x540b58  f32 written by opcode 0x05
//     0x541534  s8 written by opcode 0xca
//     0x54b5e0/0x5414e8  gate dwords for opcode 0x0d
//     0x540e24/0x540cbc  slide channel/aux cleared by 0xe0 flag==0
//
// Loop protection (OBSERVED): the dispatch loop counts instructions;
// exceeding 1000 in one invocation reports
//   "Alien %s looped %d commands, off %lx"
// Call-stack errors report "Gosub underflow/overflow on %s ID %d"
// (FUN_00438010) and clear the script gate.
//
// NATIVE SAFETY POLICY: the original indexes the script image with
// raw pointers and dereferences malformed offsets unconditionally.
// This port bounds-checks every fetch against the loaded image; a
// malformed stream halts the script (gate cleared) and records a
// diagnostic instead of reading out of range. Valid BUILD_A streams
// never reach the bound.

#ifndef MDK_CORE_TRAVERSAL_SCRIPT_H
#define MDK_CORE_TRAVERSAL_SCRIPT_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/cmi_directory.h"

namespace mdk {

struct TraversalArena;
struct TraversalRuntime;
struct RuntimeModel;
struct DynamicObject;

// ---------------------------------------------------------------------------
// CMI table-3 script record (FUN_00458550, OBSERVED)
// ---------------------------------------------------------------------------
// A table-3 record's value is an image offset into the CMI data
// region. At image+value lies:
//   {u8 len1, bytes1[len1]}  {u8 len2, bytes2[len2]}  {u32 codeOff}
// codeOff is the image-relative bytecode entry the VM reads as the
// arena's +0x220. codeOff==0 -> no script.
struct CmiScriptRecord {
  std::string name;      // arena/table-3 record name
  std::string sub;       // second length-prefixed string
  std::uint32_t codeOff = 0; // image-relative bytecode entry
};

// Resolve a table-3 record by name into its script-code image offset.
// Returns 0 when absent — the +0x220 gate stays cleared (OBSERVED
// FUN_00458550 semantics: no match or zero codeOff -> 0).
std::uint32_t cmiScriptCodeOffset(const CmiDirectory& cmi,
                                  std::span<const std::byte> cmiImage,
                                  const std::string& arenaName);

// ---------------------------------------------------------------------------
// CMI table-2 per-object init record (FUN_004566f0, OBSERVED)
// ---------------------------------------------------------------------------
// At spawn, FUN_004566f0 formats "%s$%s" (arenaName$className) and
// scans CMI table[2] for a matching record. On a hit the record's
// value IS the image-relative bytecode offset (no {str}{str}{u32}
// wrapper — unlike table-3) and the script runs immediately with the
// new object as the bound target, then FUN_0045612c rebuilds the
// transform (so the script's +0x58 scale applies). Returns 0 when no
// record matches — the object then keeps FUN_004566f0's defaults.
std::uint32_t cmiObjectScriptOffset(const CmiDirectory& cmi,
                                    const std::string& objectKey);

// ---------------------------------------------------------------------------
// VM context — the native equivalent of the arena +0x118 block's
// script state. Only the fields the proven corridor subset touches.
// ---------------------------------------------------------------------------
struct TraversalScriptState {
  std::uint32_t pcImageOff = 0;      // +0x108/+0x220 persisted PC+gate
  float waitSeconds = 0.0f;          // +0x22c
  std::uint32_t waitResumeImageOff = 0; // +0x230
  std::uint8_t running = 0;          // +0x21e (0xff clears)
  std::uint8_t eventByte = 0;        // +0x21d (FUN_004546ac)
  std::uint8_t var11a = 0;           // +0x11a (0x0b writes, 0x0a reads
                                     // on the named object)
  float field30e = 0.0f;             // +0x30e (0x99)
  std::uint32_t flagsLocal = 0;      // +0x244 flag group 2
  std::uint32_t flagsChild = 0;      // +0x312 flag group 5
  float locals[16] = {};             // +0x234 operand group 2
  int callDepth = 0;                 // +0x248 (cap 4)
  std::uint32_t retPc[4] = {};       // +0x24c
  std::uint32_t savedPc[4] = {};     // +0x25c (saved +0x108)
  std::uint16_t marker[4] = {};      // +0x26c

  // Native bookkeeping (not original fields):
  bool active = false;               // has a live script (pc!=0)
};

// Result of one interpreter invocation (one frame's run).
struct TraversalScriptResult {
  int instructions = 0;      // commands executed this invocation
  bool waited = false;       // 0x40 wrote the wait/resume pair
  bool stopped = false;      // 0x09 cleared the gate
  bool halted = false;       // 0xff end-of-frame reached
  bool error = false;        // diagnostic fired (see `diag`)
  std::string diag;          // first diagnostic, if any
};

// ---------------------------------------------------------------------------
// Host environment — the proven host operations the corridor subset
// can reach. Everything else is a counted seam.
// ---------------------------------------------------------------------------
struct TraversalScriptEnv {
  // Script byte space: the CMI file image (image base = file+4).
  std::span<const std::byte> image;
  std::uint32_t imageBase = 4;      // file offset of image base

  // Player position for 0x60/0x67 box tests (0x540bfc).
  const float* playerPos = nullptr; // [3]

  // Runtime + arena wiring.
  TraversalRuntime* rt = nullptr;
  TraversalArena* currentArena = nullptr;  // c48
  TraversalArena* selfArena = nullptr;     // ctx+0x60 bound object

  // FUN_004546ac synthetic context: when non-null the interpreter
  // runs this transient state instead of selfArena->script, and does
  // not persist the gate back to the arena. The surface-handler path
  // (event -> handlerOff script) builds a fresh state per invoke.
  TraversalScriptState* stateOverride = nullptr;

  // The interpreter's executing ctx (FUN_004388d8's record arg):
  // the arena VM uses &selfArena->eventLatch, the object VM the
  // object itself. Spawn ops write child+0x138 (leader) from it —
  // OBSERVED 0x43fc16 — so arena-spawned objects are born led by the
  // latch and object-spawned children by their spawner.
  DynamicObject* ctxObject = nullptr;

  // Mirrored globals the subset touches (0x541534, 0x540b58,
  // 0x540d98, 0x540d88[8], 0x54b5e0/0x5414e8 read-only, 0x540e24/cbc).
  std::int8_t g541534 = 0;
  float g540b58 = 0.0f;
  std::uint32_t gFlags = 0;           // 0x540d98 flag group 0
  float gVars[8] = {};                // 0x540d88 operand group 0
  std::uint32_t g54b5e0 = 0;
  std::uint32_t g5414e8 = 0;

  // 0xe0 slide/deflect plumbing (player_surface.h). `slideClear`
  // requests 0x540e24/0x540cbc = 0; `slideMode`/`hasContactNormal`/
  // `dt` feed slideZoneTrigger; `slideChannel`/`slideImpulse*`/
  // `deflectBounce` carry results back to the runtime.
  bool slideClear = false;
  bool slideMode = false;
  bool hasContactNormal = false;
  float dt = 0.0f;
  int frameStep = 1;                // DAT_0049b6e8 — the per-eval mark
                                    // quantum used by op 0x12's timer
  int slideChannel = 0;               // 0x540e24 (out)
  float slideImpulseX = 0.0f;
  float slideImpulseZ = 0.0f;
  bool deflectBounce = false;         // 0x540e28

  // FUN_004286c8 deferred-geometry model source — CMI enemy-table
  // index -> source RuntimeModel (the 0x95 spawn resolves its class
  // name to this index via the enemy table, then deep-copies it).
  // Wired by the traversal runtime; nullptr when models are absent.
  const RuntimeModel* (*modelFor)(int modelIndex, void* ctx) = nullptr;
  void* modelCtx = nullptr;

  // Spawn accounting — increments per created object (seam metric).
  int seamsSpawned = 0;

  // Diagnostics collected this invocation (bounded).
  std::vector<std::string>* diagLog = nullptr;
};

// Spawn one dynamic object for the 0x95/0x56/0xa1/0xce/0xe6 family.
// Creates a dormant DynamicObject on the destination arena's +0x68
// list (the arena whose name == `name`, else selfArena). Class/name/
// script metadata is captured on the record; the class's native
// behavior is a documented seam (Phase 5E object system).
void traversalScriptSpawn(TraversalScriptEnv& env, float x, float y,
                          float z, float yaw, std::uint32_t flags,
                          const std::string& cls, const std::string& name,
                          std::uint32_t scriptOff, int variant);

// One interpreter invocation — FUN_004388d8(ctx). Reads env.selfArena
// ->script, decodes from the persisted PC, runs to wait/halt/stop/
// error or the 1000-instruction cap. `image` is the full CMI file
// bytes; code offsets are image-relative (file offset = 4 + off).
TraversalScriptResult traversalScriptRun(TraversalScriptEnv& env);

// Persistent per-object script tick — FUN_004388d8(obj), called by
// FUN_004572ac for every list object whose +0x108 is nonzero (the
// table-0 "%s$%s_%d" script or the +0x110 death handoff). Honors the
// +0x22c wait/+0x230 resume pair; ckpt/call/goto write +0x108 (image
// pointers); 0xff suspends to the next frame. The op set is the
// object-bound union documented in traversal_script.cpp.
TraversalScriptResult traversalObjectScriptTick(TraversalScriptEnv& env,
                                                DynamicObject& obj);

// Run one table-2 object-init script synchronously — the bound object
// is `obj` (field ops write its +0xNN fields, not the arena ctx). The
// object-init run rebinds selfArena to the object's home arena (var
// mode 1 source) and uses a transient ctx for locals/call-stack, like
// the original's fresh FUN_004388d8 invocation from FUN_004566f0.
// Runs to 0xff/0x09/end or the 1000-instruction cap. OBSERVED subset
// implemented: the door opcodes (0x10/0x53/0x96/0x97/0x98/0x99/0xff)
// plus the common field/flag/element-init family; unknown opcodes halt
// with a diagnostic (native safety policy — the original desyncs).
TraversalScriptResult traversalObjectInitScript(
    TraversalScriptEnv& env, DynamicObject& obj, std::uint32_t codeOff);

// ---------------------------------------------------------------------------
// FUN_004546ac — the surface-contact handler seam. Matches the
// player_surface.h SurfaceScriptFn signature; wire it onto an arena's
// SurfaceObjectState::scriptFn (with scriptUser = &rt) so surface
// contact invokes the CMI-relative handler script through the VM.
// Stateless synthetic context per event — see the .cpp comment.
// ---------------------------------------------------------------------------
struct SurfaceObjectState;
struct SurfaceFxState;
void traversalScriptSurfaceHandler(SurfaceObjectState& ctx,
                                   std::uint32_t handlerOff,
                                   std::int32_t eventCode,
                                   std::uint8_t& result,
                                   const SurfaceFxState& fx,
                                   void* user);

// ---------------------------------------------------------------------------
// Diagnostics / disassembly (read-only)
// ---------------------------------------------------------------------------

// Decode one instruction at image offset `off`. Returns the byte
// length consumed (0 = not decodable/unknown opcode) and a mnemonic.
// Used by --script-disasm; never executes.
struct TraversalScriptInsn {
  std::uint32_t off = 0;
  std::uint8_t opcode = 0;
  int length = 0;
  std::string text;             // decoded form or "???"
  std::vector<std::uint32_t> linkTargets; // call/goto image offsets
};
TraversalScriptInsn traversalScriptDecode(std::span<const std::byte> image,
                                          std::uint32_t imageBase,
                                          std::uint32_t codeOff);

// Linear decode of one script's entry span until 0xff/stop or a
// bound. For inspector output; subroutine targets are exposed via
// linkTargets, not followed.
std::vector<TraversalScriptInsn> traversalScriptDisasm(
    std::span<const std::byte> image, std::uint32_t imageBase,
    std::uint32_t codeOff, int maxInsn = 256);

// Phase 15A — bounded spawn census for the combat harness. Appended
// by traversalScriptSpawn (in addition to the object landing in the
// arena's storage) so --traversal-runtime can list encounter
// participants. Process-local; not serialized.
struct TraversalSpawnRecord {
  std::string cls;
  std::string name;
  std::string arena;      // owning arena at spawn time
  int variant = 0;
};
std::vector<TraversalSpawnRecord>& traversalSpawnLog();

} // namespace mdk

#endif // MDK_CORE_TRAVERSAL_SCRIPT_H
