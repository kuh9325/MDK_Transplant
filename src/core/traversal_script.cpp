// traversal_script.cpp — Phase 5H tr_alcmd VM. See the header for the
// evidence map. All opcode semantics below are OBSERVED from
// MDK95.EXE handler disassembly; the operand grammars were validated
// against the real BUILD_A corridor scripts (LEVEL3–LEVEL8).

#include "core/traversal_script.h"

#include <cstdio>
#include <cstring>

#include "core/traversal_runtime.h"
#include "core/player_surface.h"
#include "core/player_projectiles.h"
#include "core/dynamic_objects.h"
#include "core/enemy_runtime.h"
#include "core/dti_structure.h"

namespace mdk {

namespace {

// ---------------------------------------------------------------------------
// Bounded byte reader. The original dereferences raw image pointers;
// this port fails safe — every read is checked against the image
// span. A failed read aborts the instruction (Reader::ok() == false)
// and the caller halts the script with a diagnostic.
// ---------------------------------------------------------------------------
struct Reader {
  std::span<const std::byte> image;
  std::uint32_t base = 4;   // file offset of image base (img = file+4)
  std::uint32_t pc = 0;     // image offset (image-relative)
  bool ok = true;

  const std::byte* ptr(std::uint32_t imgOff, std::size_t n) {
    std::uint64_t fo = static_cast<std::uint64_t>(base) + imgOff;
    if (fo + n > image.size()) { ok = false; return nullptr; }
    return image.data() + fo;
  }
  std::uint8_t u8() {
    const std::byte* p = ptr(pc, 1);
    if (!p) return 0;
    ++pc;
    return static_cast<std::uint8_t>(*p);
  }
  std::uint16_t u16() {
    const std::byte* p = ptr(pc, 2);
    if (!p) return 0;
    pc += 2;
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
  }
  std::uint32_t u32() {
    const std::byte* p = ptr(pc, 4);
    if (!p) return 0;
    pc += 4;
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
  }
  float f32() {
    std::uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  // Length-prefixed string (u8 count, count bytes incl. terminator).
  std::string str() {
    std::uint8_t n = u8();
    const std::byte* p = ptr(pc, n);
    if (!p) return {};
    pc += n;
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
      char c = static_cast<char>(p[i]);
      if (c == '\0') break;
      s += c;
    }
    return s;
  }
};

// ---------------------------------------------------------------------------
// Operand / flag resolvers (FUN_00438654 / FUN_00438744, OBSERVED)
// ---------------------------------------------------------------------------
// Var-operand: {u8 mode, payload}. mode 3 = inline f32; otherwise a
// u8 index selects a slot in a mode-selected f32 array via
// FUN_00438654(ctx,mode,index). OBSERVED: the resolver clamps index
// to [0,4) (out-of-range -> slot 0); each mode selects a 4-wide
// array: 0 -> globals 0x540d88, 1 -> boundObj+0x48, 2 -> ctx+0x234,
// >=3 -> caller ctx +0x234 (or the sink 0x49b81c when no caller —
// the arena ctx has none, so the bounded fallback targets locals).
// The ctx-bound slots the resolvers touch — +0x234 locals, +0x244
// flag dword, +0x312 flag dword. The arena ctx maps them to
// TraversalScriptState members; the object ctx maps them to the
// DynamicObject script block (same offsets — the ctx is the object).
struct ScriptCtxSlots {
  float* locals;
  std::uint32_t* flagsLocal;
  std::uint32_t* flagsChild;
};
ScriptCtxSlots ctxSlots(TraversalScriptState& st) {
  return {st.locals, &st.flagsLocal, &st.flagsChild};
}
ScriptCtxSlots ctxSlots(DynamicObject& o) {
  return {o.scriptLocals, &o.scriptFlagsLocal, &o.scriptFlagsChild};
}

static inline int varIndex(std::uint8_t idx) { return idx < 4 ? idx : 0; }
// Resolve a var-operand given an already-read mode byte. mode 3 =
// inline f32; otherwise a u8 index selects a slot via FUN_00438654.
float resolveVarMode(std::uint8_t mode, Reader& r,
                     TraversalScriptEnv& env, const ScriptCtxSlots& ctx) {
  if (mode == 3) return r.f32();          // inline f32 (read-op form)
  const int idx = varIndex(r.u8());
  switch (mode) {
  case 0: return env.gVars[idx];                 // 0x540d88 globals
  case 1:                                        // boundObj+0x48
    return env.selfArena ? env.selfArena->objVars48[idx] : 0.0f;
  case 2:                                        // ctx+0x234 locals
  default: return ctx.locals[idx];               // >=3: caller +0x234
  }                                              // -> local (bounded)
}
float resolveVar(Reader& r, TraversalScriptEnv& env,
                 const ScriptCtxSlots& ctx) {
  return resolveVarMode(r.u8(), r, env, ctx);
}

// Var-operand WRITE resolver — FUN_00438654's pointer form. Returns
// the 4-byte slot the mode/index pair selects (the same arrays
// resolveVar reads). A write op has no inline mode — mode selects
// the array, index the slot.
float* resolveVarRef(std::uint8_t mode, std::uint8_t idx,
                     TraversalScriptEnv& env, const ScriptCtxSlots& ctx) {
  const int i = varIndex(idx);
  switch (mode) {
  case 0: return &env.gVars[i];
  case 1:
    return env.selfArena ? &env.selfArena->objVars48[i] : nullptr;
  case 2:
  default: return &ctx.locals[i];
  }
}

// Flag-group resolver. Returns a reference to the flag dword the
// 0x44..0x48 family operates on.
std::uint32_t& resolveFlag(std::uint32_t group, TraversalScriptEnv& env,
                           const ScriptCtxSlots& ctx) {
  switch (group) {
  case 0: return env.gFlags;                    // 0x540d98
  case 1:                                        // boundObj+0x58
    if (env.selfArena) return env.selfArena->flags58;
    return env.gFlags;                           // bounded fallback
  case 5: return *ctx.flagsChild;               // ctx+0x312
  case 2:                                        // ctx+0x244
  default: return *ctx.flagsLocal;              // 'else' = caller ctx
  }                                             // +0x244 -> local here
}

// ---------------------------------------------------------------------------
// Linkage tail — the shared call/goto/return trailer read by every
// conditional opcode (OBSERVED). Returns false on a bounds fault.
//   0xfe  two-way:  {u32 trueOff, u32 elseOff}
//   0xfc  call:     {u32 off}
//   0x0c  goto:     {u32 off}
//   0xfd  return:   (no payload)
// A linkage of 0 (offset == image base -> null) is honored as "no
// target".
// ---------------------------------------------------------------------------
// FUN_0045ce58 — 6-float AABB overlap (same predicate as the
// collision_query copy; kept local to the script TU).
bool aabbOverlapLocal(const float* a, const float* b) {
  return !(a[3] < b[0] || b[3] < a[0]) && !(a[4] < b[1] || b[4] < a[1]) &&
         !(a[5] < b[2] || b[5] < a[2]);
}

// FUN_0045ad40 — the shared 8-kind compare used by the distance/var
// linkage ops (kinds 3/4/5/6 carry the OBSERVED +-0.05 epsilon).
bool cmpOp5ad40(std::uint8_t kind, float v, float a, float b) {
  switch (kind) {
  case 1: return v < a;
  case 2: return v > a;
  case 3: return v - 0.05f < a;
  case 4: return v + 0.05f > a;
  case 5: return std::fabs(v - a) < 0.05f;
  case 6: return std::fabs(v - a) >= 0.05f;
  case 7: return v >= a && v <= b;
  case 8: return v <= a || v >= b;
  default: return false;
  }
}

// FUN_0045dc18 — wrap-aware angle approach: step `cur` toward `target`
// along the shortest arc (|delta| vs +-180 picks the direction), with
// the OBSERVED clamp/wrap behaviour per branch.
float approachAngle5dc18(float target, float cur, float step) {
  const float delta = target - cur;
  if (delta < -180.0f) {
    float cand = cur + step;
    if (cand >= 360.0f) {
      cand += -360.0f;
      if (cand > target) cand = target;
    }
    return cand;
  }
  const float back = cur - step;
  if (delta < 0.0f) return (back >= target) ? back : target;
  if (delta < 180.0f) {
    const float fwd = cur + step;
    return (fwd <= target) ? fwd : target;
  }
  if (back >= 0.0f) return back;
  const float wrapped = back + 360.0f;
  return (wrapped >= target) ? wrapped : target;
}

struct Linkage {
  std::uint8_t mode = 0;
  std::uint32_t a = 0;   // first/only image offset
  std::uint32_t b = 0;   // second (0xfe else path)
};
bool readLinkage(Reader& r, Linkage& L) {
  L.mode = r.u8();
  if (!r.ok) return false;
  switch (L.mode) {
  case 0xfe: L.a = r.u32(); L.b = r.u32(); break;
  case 0xfc:
  case 0x0c: L.a = r.u32(); break;
  case 0xfd: break;
  default: break;   // unknown mode — consumed as mode byte only
  }
  return r.ok;
}

// FUN_0045d880 — player-in-cone + LOS test, shared by object ops
// 0x0e and 0x39 (OBSERVED).
bool coneLosTest(TraversalScriptEnv& env, const DynamicObject& obj,
                 float range, float arc) {
  bool cond = false;
  if (env.rt != nullptr && env.currentArena != nullptr) {
    const float* pp = env.rt->cs.pos;           // 0x540bfc
    const float dx = pp[0] - obj.pos[0];
    const float dy = pp[1] - obj.pos[1];
    const float dz = pp[2] - obj.pos[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= range) {
      float diff =
          std::fabs(obj.yawDeg - bearingDeg(dy, dx));  // |yaw - bearing|
      while (diff >= 360.0f) diff += -360.0f;          // OBSERVED fadd
      const float thresh = (arc - 90.0f) / range * dist + 90.0f;
      if (diff <= thresh || diff >= 360.0f - thresh) {
        const float a[3] = {pp[0], pp[1], pp[2] + 5.0f};
        const float b[3] = {obj.pos[0], obj.pos[1],
                            obj.pos[2] + 10.0f - obj.zBias};
        float hit[3];
        cond =
            collisionStab(env.currentArena->dyn.col, a, b, hit) == nullptr;
        if (cond && env.rt->cs.carrierBusy == 0 &&
            env.rt->partner != nullptr) {
          cond = collisionStab(env.rt->partner->dyn.col, a, b, hit) ==
                 nullptr;
        }
      }
    }
  }
  return cond;
}

} // namespace

// ---------------------------------------------------------------------------
// CMI table-3 script lookup — FUN_00458550 (OBSERVED)
// ---------------------------------------------------------------------------
std::uint32_t cmiScriptCodeOffset(const CmiDirectory& cmi,
                                  std::span<const std::byte> cmiImage,
                                  const std::string& arenaName) {
  if (cmi.tables.size() < 4) return 0;
  for (const auto& rec : cmi.tables[3].records) {
    if (rec.name() != arenaName) continue;
    // value -> image offset of {u8 l1,s1[l1]}{u8 l2,s2[l2]}{u32 code}.
    std::uint64_t fo = static_cast<std::uint64_t>(kCmiImageBaseOffset) +
                       rec.value;
    if (fo >= cmiImage.size()) return 0;
    Reader r{cmiImage, static_cast<std::uint32_t>(kCmiImageBaseOffset),
             static_cast<std::uint32_t>(rec.value)};
    (void)r.str();
    (void)r.str();
    if (!r.ok) return 0;
    return r.u32();
  }
  return 0;
}

// FUN_0047e02c — ASCII stricmp: A-Z folds +0x20 before compare;
// equality iff both terminate together (OBSERVED 0x47ed9c).
bool stricmpEq(const std::string& a, const std::string& b) {
  std::size_t i = 0;
  for (;; ++i) {
    unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    if (ca >= 'A' && ca <= 'Z') ca += 0x20;
    if (cb >= 'A' && cb <= 'Z') cb += 0x20;
    if (ca != cb || ca == 0) return ca == cb;
  }
}

// ---------------------------------------------------------------------------
// CMI table-2 object-init lookup — FUN_004566f0's "%s$%s" match
// ---------------------------------------------------------------------------
// Unlike table-3, the table-2 record's `value` is the image-relative
// code offset itself (code at file 4 + value); there is no
// {str}{str}{u32} indirection. OBSERVED: CHMO_2$XCORDOOR -> 0x204c0.
std::uint32_t cmiObjectScriptOffset(const CmiDirectory& cmi,
                                    const std::string& objectKey) {
  if (cmi.tables.size() < 3) return 0;
  for (const auto& rec : cmi.tables[2].records)
    if (rec.name() == objectKey)
      return static_cast<std::uint32_t>(rec.value);
  return 0;
}

// ---------------------------------------------------------------------------
// Broadcast dispatch — FUN_00438094 + FUN_004382e0 (OBSERVED). Shared
// by the arena VM (ctx = the controlalien record) and the object VM
// (ctx = the object): the handler is the same in both, reading
// ctx->+0x60 (home arena), +0x2b8, +0x11a, +0x4c and +0x10 off the
// VM's context record.
//
// {u8 outer, payload, u8 inner, operands}:
//   outer 7: {linkage} — remote call; linkage mode 0xfc re-tags the
//     dispatch as a remote GOSUB (frame push on the target).
//   outer 0x2b: {f32 fwd, f32 lat} — seek-order to a camera-relative
//     point (the op-0x2b math; z = 0x54c6cc).
//   outer 1: formation slot command.
//   inner: 9 -> ctx's bound object (+0x2b8) only; else the home arena
//     list is scanned with per-mode filters: 3 = all, 2/4/7 =
//     model-name match, 5 = spawnId, 6 = range+LOS, 0xa = pos.y >=
//     arg, 7/8 = direct subordinate, 4 = first match only. Outer-7
//     calls dedup on +0x10c.
// Returns a failure tag or nullptr.
// OBSERVED: object name comparisons in FUN_00438094 (inner filter)
// and FUN_0044d04a (op-0x77 count) read *(obj+0x0c) — a pointer to
// the enemy-table record (DAT_004edcc0; the spawn default). The
// compared string is the CLASS name ("XG"), not the geometry
// record's name-table[0] ("XG_BOD").
const std::string& objectEnemyName(const TraversalScriptEnv& env,
                                   const DynamicObject& o) {
  if (env.rt != nullptr &&
      o.enemyIndex < env.rt->level.enemies.entries.size())
    return env.rt->level.enemies.entries[o.enemyIndex].name;
  return o.scriptClass;
}
const char* broadcastDispatchOp(TraversalScriptEnv& env,
                                DynamicObject& ctxo, DynamicArena& home,
                                std::uint8_t ctxRank, Reader& r) {
  const std::uint8_t outer0 = r.u8();
  if (!r.ok) return "bcast mode";
  std::uint8_t outer = outer0;
  std::uint32_t pc = 0;                 // mode-7 target offset
  float point[3] = {0, 0, 0};           // mode-0x2b/1 anchor
  if (outer == 7) {
    Linkage L;
    if (!readLinkage(r, L)) return "bcast link";
    pc = L.a;
    if (L.mode == 0xfc) outer = 0xfc;   // OBSERVED re-tag
  } else if (outer == 0x2b) {
    const float fwd = r.f32();
    const float lat = r.f32();
    if (!r.ok || env.rt == nullptr) return "bcast pt";
    const float* cam = env.rt->camera.pose.pos;   // 0x54c6c4..cc
    const float dx = cam[0] - ctxo.pos[0];
    const float dy = cam[1] - ctxo.pos[1];
    const float dz = cam[2] - ctxo.pos[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float b = bearingDeg(dy, dx);           // FUN_0045acf0
    float sn = 0.0f, cs = 0.0f;
    sincosDeg(b, &sn, &cs);                       // FUN_00437f98
    const float s = d * 0.00999999978f;           // C(0x4979b0)
    point[0] = ctxo.pos[0] + fwd * s * cs + lat * s * sn;
    point[1] = ctxo.pos[1] + fwd * s * sn - lat * s * cs;
    point[2] = cam[2];                            // 0x54c6cc
  }
  const std::uint8_t inner = r.u8();
  std::uint32_t argRaw = 0;
  std::string nm;
  if (inner == 6 || inner == 0xa) {
    argRaw = r.u32();                             // f32 operand
    nm = r.str();
  } else if (inner == 2 || inner == 4 || inner == 5 || inner == 7) {
    nm = r.str();                                 // OBSERVED: inner 5
    if (inner == 5) argRaw = r.u32();             // is {lstr, u32}
  }                                             // (0x439a0e)
  if (!r.ok) return "bcast args";
  const float argf = std::bit_cast<float>(argRaw);
  const void* pcPtr = r.ptr(pc, 1);
  int hitFlag = 0;                                // FUN_00438094 -0xc

  auto execObj = [&](DynamicObject& o) {
    // FUN_004382e0 — per-target dispatch on the OUTER mode.
    if (outer == 7) {                             // remote call
      o.field22c = 0.0f;
      o.field108 = pcPtr;
      o.field230 = o.field108;
      o.field138 = &ctxo;                         // leader = ctx
      o.scriptCallDepth = 0;
      o.scriptMark[0] = 0;
      o.field10c = pcPtr;                         // dedup PC
    } else if (outer == 0xfc) {                   // remote gosub
      if (o.scriptCallDepth >= 4) {
        o.field108 = nullptr;                     // FUN_00438010
        return;
      }
      const int d = o.scriptCallDepth;
      o.scriptRetPc[d] = o.field108;              // +0x24c/+0x25c
      o.scriptSavedPc[d] = o.field108;            // both = +0x108
      ++o.scriptCallDepth;
      o.field22c = 0.0f;
      o.field108 = pcPtr;
      o.field230 = o.field108;
      o.field138 = &ctxo;
      o.scriptMark[o.scriptCallDepth] = 0;
    } else if (outer == 0x2b) {                   // seek order
      // OBSERVED: runs only while obj+0x60 == 0x540c48 (current).
      if (env.currentArena == nullptr ||
          o.arena != &env.currentArena->dyn) return;
      o.field120[0] = point[0];
      o.field120[1] = point[1];
      o.field120[2] = point[2];
      o.field138 = &ctxo;
      o.field11e = outer;                         // subtype = mode
      o.fieldEC = nullptr;
      objectWaypointReseek(o, home);              // FUN_00451ee8
      seekOpcodeTail(o);
    } else if (outer == 1) {                      // formation slot
      const bool xe = (o.model.modelName() == "XE");
      const float ss = xe ? 4.0f : 1.0f;          // 0x497980 cmp
      const float ff = xe ? 2.5f : 1.0f;
      const float side = (hitFlag & 1) ? 1.0f : -1.0f;
      const int ia = static_cast<int>(argRaw);
      const int n = ((ia - (ia >> 31)) >> 1) + 1; // OBSERVED shift
      o.field12c[0] = side * (n * 5) * ss;        // +0x12c side
      o.field12c[1] = ff * -4.0f;                 // +0x130 fwd
      o.field12c[2] = 8.0f;                       // +0x134 vert
      float sn = 0.0f, cs = 0.0f;
      sincosDeg(ctxo.yawDeg, &sn, &cs);           // ctx +0x4c
      o.field120[0] =
          ctxo.pos[0] - o.field12c[1] * sn + o.field12c[0] * cs;
      o.field120[1] =
          ctxo.pos[1] - o.field12c[1] * cs - o.field12c[0] * sn;
      o.field120[2] = ctxo.pos[2] + o.field12c[2];
      o.field138 = &ctxo;
      o.field11e = outer;                         // subtype = 1
      o.fieldEC = nullptr;
      ++hitFlag;
    }
  };

  if (inner == 9) {
    if (ctxo.field2b8 != nullptr) execObj(*ctxo.field2b8);
    return nullptr;
  }
  static const bool traceBcast = std::getenv("MDK_TRACE_BCAST") != nullptr;
  for (auto& up : home.storage) {
    DynamicObject& o = *up;
    if (traceBcast)
      std::fprintf(stderr,
                   "    [bc] ctx=%s outer=%02x inner=%02x nm=%s cand=%s model=%s "
                   "named=%d hp=%d f11b=%d f138=%p f10c=%p arenaOk=%d\n",
                   ctxo.scriptClass.c_str(),
                   outer, inner, nm.c_str(), o.scriptClass.c_str(),
                   o.model.modelName().c_str(), o.col.named ? 1 : 0,
                   (int)o.health, (int)o.field11b, (void*)o.field138,
                   (void*)o.field10c, o.arena == &home ? 1 : 0);
    if (!o.col.named || &o == &ctxo) continue;    // +0x06 / self
    if (o.arena != &home) continue;               // same arena
    if (inner != 3 && objectEnemyName(env, o) != nm) continue;  // +0x0c
    if (o.field138 != nullptr && o.field138 != &ctxo &&
        o.field138->field11a >= ctxRank) {
      if (traceBcast)
        std::fprintf(stderr,
                     "    [bc] skip %s: outrank f138=%s(%p)->f11a=%d "
                     "ctxRank=%d\n",
                     o.scriptClass.c_str(),
                     o.field138->scriptClass.c_str(),
                     (void*)o.field138,
                     (int)o.field138->field11a, (int)ctxRank);
      continue;                                   // outranked
    }
    if (o.field11b < ctxRank) {
      if (traceBcast)
        std::fprintf(stderr, "    [bc] skip %s: rank f11b=%d ctxRank=%d\n",
                     o.scriptClass.c_str(), (int)o.field11b,
                     (int)ctxRank);
      continue;                                   // rank gate
    }
    if ((inner == 7 || inner == 8) && o.field138 != &ctxo)
      continue;                                   // subordinate
    if (inner == 5 && o.spawnId != argRaw) continue;
    if (inner == 6) {
      const float dx = o.pos[0] - ctxo.pos[0];
      const float dy = o.pos[1] - ctxo.pos[1];
      const float dz = o.pos[2] - ctxo.pos[2];
      if (std::sqrt(dx * dx + dy * dy + dz * dz) > argf) continue;
      float from[3] = {ctxo.pos[0], ctxo.pos[1],
                       ctxo.pos[2] + 8.0f};       // C(0x49797c)=8
      float to[3] = {o.pos[0], o.pos[1], o.pos[2] + 8.0f};
      float hit[3];
      if (collisionStab(home.col, from, to, hit) != nullptr)
        continue;                                 // LOS blocked
    }
    if (inner == 0xa && o.pos[1] < argf) continue;
    if (outer == 7 && o.field10c == pcPtr) {
      if (traceBcast)
        std::fprintf(stderr, "    [bc] skip %s: dedup f10c==pcPtr\n",
                     o.scriptClass.c_str());
      continue;                                   // dedup
    }
    if (o.health == 0) {
      if (traceBcast)
        std::fprintf(stderr, "    [bc] skip %s: dead\n",
                     o.scriptClass.c_str());
      continue;                                   // +0x08 gate
    }
    if (traceBcast)
      std::fprintf(stderr, "    [bc] EXEC %s outer=%02x pc=%p\n",
                   o.scriptClass.c_str(), outer, pcPtr);
    execObj(o);
    if (inner == 4) break;                        // first only
  }
  return nullptr;
}

// Opcode 0x83 (handler 0x43d3e2, OBSERVED): {u8 subop} — the
// traversal-completion / cinematic-command family.
//   subop <= 0x32 : 0x540ebc = -1 (0x43d411) — the FUN_00436100
//                   end-level mailbox, consumed at the frame tail.
//   subop >  0x32 : FUN_0047baf4(ctx, subop) (0x43d40a) — OBSERVED
//                   arms: 0x35/0x5c -> 0x540e9c = 0; 0x51 ->
//                   0x540e9c = 0x51 then FUN_0047b038 (0x541492 = 8
//                   ending latch); 0x33/0x34/0x37/0x3d/0x5b/0x5d ->
//                   cinematic-rig calls (presentation, not ported);
//                   every other subop is a nop RET.
void completionOp83(TraversalScriptEnv& env, std::uint8_t subop) {
  if (env.rt == nullptr) return;
  if (subop <= 0x32) {
    env.rt->pendingViewSnap = -1;
    return;
  }
  ++env.rt->seams.cineDispatchCalls;
  switch (subop) {
  case 0x35:                                    // 0x47bb3e
  case 0x5c:                                    // 0x47bb85
    env.rt->fieldE9c = 0;
    break;
  case 0x51:                                    // 0x47bb5b
    env.rt->fieldE9c = 0x51;
    env.rt->endingRequest = 1;                  // FUN_0047b038 edge
    break;
  default: break;
  }
}

// ---------------------------------------------------------------------------
// The interpreter — FUN_004388d8(ctx)
// ---------------------------------------------------------------------------
TraversalScriptResult traversalScriptRun(TraversalScriptEnv& env) {
  TraversalScriptResult res;
  if (!env.selfArena) return res;
  // FUN_004546ac synthetic context override, else the arena's own
  // +0x118 state.
  TraversalScriptState& st =
      env.stateOverride ? *env.stateOverride : env.selfArena->script;
  if (st.pcImageOff == 0) return res;   // +0x220 gate cleared

  Reader r{env.image, env.imageBase, st.pcImageOff};

  // Wait gate: +0x22c counts seconds down by the 1/30 frame step;
  // while positive the body doesn't run. On expiry the resume PC
  // (+0x230) is entered. (OBSERVED: 0x49b6f4 = 1/30 s)
  if (st.waitSeconds > 0.0f) {
    st.waitSeconds -= (1.0f / 30.0f);
    if (st.waitSeconds > 0.0f) return res;   // still waiting
    st.waitSeconds = 0.0f;
    r.pc = st.waitResumeImageOff;            // resume at +0x230
    st.pcImageOff = st.waitResumeImageOff;
  }

  st.running = 1;                            // +0x21e
  const char* name = env.selfArena->name.c_str();
  // The arena VM's executing ctx is the controlalien record — spawn
  // ops born under it inherit it as leader (+0x138, OBSERVED 0x43fc16).
  env.ctxObject = &env.selfArena->eventLatch;

  auto fail = [&](const char* why) {
    res.error = true;
    st.pcImageOff = 0;
    st.active = false;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: %s", name, why);
    res.diag = buf;
    if (env.diagLog) env.diagLog->push_back(res.diag);
  };

  // The 1000-instruction loop cap — "Alien %s looped %d commands,
  // off %lx" (OBSERVED diagnostic; the original then exits).
  for (int i = 0; i < 1000; ++i) {
    if (!r.ok) { fail("script read out of bounds"); return res; }
    const std::uint32_t insnOff = r.pc;
    const std::uint8_t op = r.u8();
    ++res.instructions;
    if (!r.ok) { fail("opcode fetch out of bounds"); return res; }
    {
      static const char* tr = std::getenv("MDK_TRACE_ARENA");
      if (tr != nullptr && name != nullptr &&
          std::strcmp(name, tr) == 0)
        std::fprintf(stderr, "    [avm] %s +%05x op=%02x\n",
                     name, insnOff, op);
    }

    // 0xff — end of frame body: clear the running flag and exit.
    // The persisted +0x220 is NOT touched here — it holds whatever
    // the most recent checkpoint / call / goto left in it, so the
    // next frame re-enters at that point.
    if (op == 0xff) {
      st.running = 0;
      res.halted = true;
      return res;
    }

    // 0xfd — standalone return. Pop the 4-deep stack; underflow is a
    // gosub error and kills the script (FUN_00438010).
    if (op == 0xfd) {
      if (st.callDepth <= 0) {
        fail("Gosub underflow");
        return res;
      }
      --st.callDepth;
      r.pc = st.retPc[st.callDepth];
      st.pcImageOff = st.savedPc[st.callDepth];
      continue;
    }

    // Helpers shared by the conditional opcodes.
    auto doCall = [&](std::uint32_t target) -> bool {
      if (target == 0) return true;               // null -> no call
      if (st.callDepth >= 4) {
        fail("Gosub overflow");
        return false;
      }
      st.retPc[st.callDepth] = r.pc;
      st.savedPc[st.callDepth] = st.pcImageOff;
      st.marker[st.callDepth] = 0;
      ++st.callDepth;
      r.pc = target;
      st.pcImageOff = target;
      return true;
    };
    auto doGoto = [&](std::uint32_t target) {
      r.pc = target;
      st.pcImageOff = target;
    };
    // Two-way linkage dispatch: mode 0xfe picks a/b by `cond`;
    // 0xfc/0x0c call/goto on `cond`; 0xfd returns on `cond`.
    auto applyLink = [&](const Linkage& L, bool cond) -> bool {
      switch (L.mode) {
      case 0xfe:
        if (cond) return doCall(L.a);
        if (L.b != 0) return doCall(L.b);
        return true;                      // else-path: no target
      case 0xfc:
        if (cond) return doCall(L.a);
        return true;
      case 0x0c:
        if (cond) doGoto(L.a);
        return true;
      case 0xfd:
        if (!cond) return true;
        if (st.callDepth <= 0) { fail("Gosub underflow"); return false; }
        --st.callDepth;
        r.pc = st.retPc[st.callDepth];
        st.pcImageOff = st.savedPc[st.callDepth];
        return true;
      default:
        return true;
      }
    };

    switch (op) {
    // ---------------------------------------------------------------
    // VM control flow
    // ---------------------------------------------------------------
    case 0x01:                              // checkpoint: +0x108 = pc
      st.pcImageOff = r.pc;
      break;

    case 0x04: {                            // broadcast dispatch (0x439794)
      // {u8 outer, payload, u8 inner, operands}. OBSERVED
      // FUN_00438094 + FUN_004382e0 — see broadcastDispatchOp; the
      // arena VM's ctx is the controlalien record (eventLatch).
      if (const char* e = broadcastDispatchOp(
              env, env.selfArena->eventLatch, env.selfArena->dyn,
              st.var11a, r)) {
        fail(e);
        return res;
      }
      break;
    }

    case 0xaf: {                            // inventory-count link
      // {u8 id, u8 kind, f32 a, [f32 b if kind==7|8], linkage}.
      // OBSERVED (0x44112f): sums rec+0x04 over the 0x54155c
      // inventory records (0x24-stride, count 0x541610) whose
      // rec+0x00 == id, then FUN_0045ad40. The inventory table is
      // restored from PLAY (Phase 14C.1).
      const std::uint8_t id = r.u8();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { fail("invlink"); return res; }
      float invSum = 0.0f;
      if (env.rt != nullptr) {
        for (int i = 0; i < env.rt->inventoryCount; ++i) {
          const InventoryRecord& rec = env.rt->inventory[i];
          if (rec.id == id) invSum += rec.charges;
        }
      }
      applyLink(L, cmpOp5ad40(kind, invSum, va, vb));
      break;
    }

    case 0x77: {                            // model-count link
      // {lstr name, u8 kind, f32 a, [f32 b if kind==7|8], linkage}.
      // OBSERVED (0x44d04a): counts live objects of the ctx's home
      // arena whose model record name FUN_0042fa50-matches the
      // operand, then FUN_0045ad40 on the count.
      const std::string nm = r.str();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      if (!r.ok) { fail("cntlink args"); return res; }
      Linkage L;
      if (!readLinkage(r, L)) { fail("cntlink"); return res; }
      int count = 0;
      DynamicArena& home = env.selfArena->dyn;
      for (auto& up : home.storage) {
        DynamicObject& o = *up;
        if (!o.col.named || o.arena != &home) continue;   // +0x06
        if (objectEnemyName(env, o) != nm) continue;      // +0x0c
        if (o.health <= 0) continue;                      // +0x08 JG
        ++count;
      }
      applyLink(L, cmpOp5ad40(kind, static_cast<float>(count), va, vb));
      break;
    }

    case 0x43: {                            // var cmp link (0x447a1d)
      // {u8 mode, u8 idx, u8 kind, f32 a, [f32 b if kind==7|8],
      //  linkage}. OBSERVED: same handler as the object VM —
      // *FUN_00438654(mode,idx) compared via FUN_0045ad40.
      const std::uint8_t mode = r.u8();
      const std::uint8_t idx = r.u8();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      if (!r.ok) { fail("varcmp args"); return res; }
      Linkage L;
      if (!readLinkage(r, L)) { fail("varcmp"); return res; }
      const float* slot = resolveVarRef(mode, idx, env, ctxSlots(st));
      applyLink(L,
                slot != nullptr && cmpOp5ad40(kind, *slot, va, vb));
      break;
    }

    case 0xae: {                            // ammo-count link (0x440dd0)
      // {u8 idx, u8 kind, f32 a, [f32 b if kind==7|8], linkage}.
      // OBSERVED: FILD of 0x54161f[idx] (the ammo block) then
      // FUN_0045ad40 — e.g. "ammo[i] < x".
      const std::uint8_t idx = r.u8();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      if (!r.ok) { fail("ammolink args"); return res; }
      Linkage L;
      if (!readLinkage(r, L)) { fail("ammolink"); return res; }
      float cnt = 0.0f;
      if (env.rt != nullptr && idx < env.rt->ammo.size())
        cnt = static_cast<float>(env.rt->ammo[idx]);
      applyLink(L, cmpOp5ad40(kind, cnt, va, vb));
      break;
    }

    case 0xd8: {                            // var += f32*(1/30)
      // {u8 mode, u8 idx, f32 scale}. OBSERVED (0x447438) — same
      // handler as the object VM: the 4-byte operand is loaded with
      // `fld dword` (an f32, not an integer) — *varref(mode,idx)
      // += scale * C(0x49b6f4).
      const std::uint8_t mode = r.u8(), idx = r.u8();
      const float sc = r.f32();
      if (!r.ok) { fail("varacc"); return res; }
      if (float* slot = resolveVarRef(mode, idx, env, ctxSlots(st)))
        *slot += sc * 0.03333333507180214f;
      break;
    }

    case 0x09:                              // stop: +0x220 = 0, reset
      st.pcImageOff = 0;
      st.callDepth = 0;
      st.active = false;
      res.stopped = true;
      return res;

    case 0x40: {                            // wait: +0x22c=secs,
      float secs = resolveVar(r, env, ctxSlots(st));  // +0x230=resume, exit
      if (!r.ok) { fail("wait operand"); return res; }
      if (secs <= 0.0f) secs = 1.0e-5f;     // 0x3727c5ac epsilon
      st.waitSeconds = secs;
      st.waitResumeImageOff = r.pc;
      st.pcImageOff = r.pc;
      res.waited = true;
      return res;
    }

    case 0x0c: {                            // standalone rgoto:
      std::uint8_t n = r.u8();              // {u8 n, n×u32} random-pick
      if (!r.ok || n == 0) { fail("rgoto"); return res; }
      std::uint32_t tgt = 0;
      for (std::uint8_t k = 0; k < n; ++k) {
        std::uint32_t o = r.u32();
        if (k == 0) tgt = o;                // n=1 deterministic;
      }                                     // n>1 = rand pick (seam:
      if (!r.ok) { fail("rgoto offs"); return res; } // rand() not
      doGoto(tgt);                          // modelled — first entry)
      break;
    }

    case 0xfc: {                            // standalone rcall:
      std::uint8_t n = r.u8();              // {u8 n, n×u32} random call
      if (!r.ok || n == 0) { fail("rcall"); return res; }
      std::uint32_t tgt = 0;
      for (std::uint8_t k = 0; k < n; ++k) {
        std::uint32_t o = r.u32();
        if (k == 0) tgt = o;
      }
      if (!r.ok) { fail("rcall offs"); return res; }
      if (!doCall(tgt)) return res;
      break;
    }

    // ---------------------------------------------------------------
    // Flag ops (FUN_00438744 group resolve)
    // ---------------------------------------------------------------
    case 0x44: case 0x45: {                 // bitset / bitclr {grp,bit}
      std::uint8_t grp = r.u8(), bit = r.u8();
      if (!r.ok) { fail("bit op"); return res; }
      std::uint32_t& f = resolveFlag(grp, env, ctxSlots(st));
      if (op == 0x44) f |= (1u << (bit & 31));
      else f &= ~(1u << (bit & 31));
      break;
    }
    case 0x46: case 0x47: case 0x48: {      // branch if bit set/clr
      std::uint8_t grp = r.u8(), bit = r.u8();
      Linkage L;
      if (!readLinkage(r, L)) { fail("flag branch"); return res; }
      const std::uint32_t f = resolveFlag(grp, env, ctxSlots(st));
      const bool set = (f >> (bit & 31)) & 1u;
      // 0x46/0x47 fire the linkage on SET; 0x48 on CLEAR.
      const bool cond = (op == 0x48) ? !set : set;
      if (!applyLink(L, cond)) return res;
      break;
    }

    // ---------------------------------------------------------------
    // Player box conditionals
    // ---------------------------------------------------------------
    case 0x60: {                            // box2d {x0,y0,x1,y1,link}
      float x0 = r.f32(), y0 = r.f32();
      float x1 = r.f32(), y1 = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { fail("box2d"); return res; }
      bool in = false;
      if (env.playerPos) {
        const float px = env.playerPos[0], py = env.playerPos[1];
        in = (px >= x0 && px <= x1 && py >= y0 && py <= y1);
      }
      if (!applyLink(L, in)) return res;
      break;
    }
    case 0x67: {                            // box3d {6×f32,link}
      float b[6];
      for (int k = 0; k < 6; ++k) b[k] = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { fail("box3d"); return res; }
      bool in = false;
      if (env.playerPos) {
        const float* p = env.playerPos;
        in = (p[0] >= b[0] && p[0] <= b[3] && p[1] >= b[1] &&
              p[1] <= b[4] && p[2] >= b[2] && p[2] <= b[5]);
      }
      if (!applyLink(L, in)) return res;
      break;
    }

    // ---------------------------------------------------------------
    // Context / global setters
    // ---------------------------------------------------------------
    case 0x0b: st.var11a = r.u8(); break;         // ctx+0x11a
    case 0x61: {                                  // boundObj+0x148
      std::uint8_t v = r.u8();
      if (!r.ok) { fail("setObj148"); return res; }
      if (env.selfArena) env.selfArena->objFlag148 = v;
      break;
    }
    case 0x41: {                          // setVar {mode,index,u32} —
      std::uint8_t mode = r.u8();         // *FUN_00438654(mode,idx)=v
      std::uint8_t idx = r.u8();
      std::uint32_t v = r.u32();
      if (!r.ok) { fail("setVar"); return res; }
      if (float* slot = resolveVarRef(mode, idx, env, ctxSlots(st))) {
        float f;
        std::memcpy(&f, &v, 4);           // raw u32 -> f32 slot
        *slot = f;
      }
      break;
    }
    case 0x99: st.field30e = r.f32(); break;      // ctx+0x30e
    case 0x05: env.g540b58 = r.f32(); break;      // 0x540b58
    case 0xca:                                    // 0x541534 (s8)
      env.g541534 = static_cast<std::int8_t>(r.u8());
      break;

    // ---------------------------------------------------------------
    // Conditional linkage on named-object state / partner / globals
    // ---------------------------------------------------------------
    case 0x0a: {                            // brObj11a {name,val,link}
      std::string nm = r.str();
      std::uint8_t val = r.u8();
      Linkage L;
      if (!readLinkage(r, L)) { fail("brObj11a"); return res; }
      // Named-object lookup: scan arenas for a matching name and
      // compare its +0x11a byte. (CORROBORATED: original resolves
      // the name through the object table FUN_0042fa50.)
      bool eq = false;
      if (env.rt) {
        for (const auto& ap : env.rt->arenas) {
          if (ap && ap->name == nm) { eq = (ap->script.var11a == val);
                                        break; }
        }
      }
      if (!applyLink(L, eq)) return res;
      break;
    }
    case 0x7b: {                            // ifPartner {link} — fires
      Linkage L;                            // when boundObj != c48
      if (!readLinkage(r, L)) { fail("ifPartner"); return res; }
      const bool isPartner =
          (env.selfArena && env.currentArena &&
           env.selfArena != env.currentArena);
      if (!applyLink(L, isPartner)) return res;
      break;
    }
    case 0x0d: {                            // linkGate {link} — fires
      Linkage L;                            // when both globals set
      if (!readLinkage(r, L)) { fail("linkGate"); return res; }
      const bool go = (env.g54b5e0 != 0 && env.g5414e8 != 0);
      if (!applyLink(L, go)) return res;
      break;
    }

    // ---------------------------------------------------------------
    // Surface opcodes — Phase 5F SurfaceObjectState
    // ---------------------------------------------------------------
    case 0x62: {                            // surfop {surfId, op}
      std::uint8_t sid = r.u8(), sop = r.u8();
      if (!r.ok) { fail("surfop"); return res; }
      if (!env.selfArena) break;
      SurfaceObjectState& s = env.selfArena->surface;
      surfacePolyOp(s.polys, s.polyCount, sid, sop);
      // opMaskA/B updates (OBSERVED writes at boundObj+0x10c/+0x110).
      const std::uint32_t m = 1u << (sid & 31);
      if (sop == 1) s.opMaskA |= m;
      else if (sop == 2) s.opMaskB |= m;
      break;
    }
    case 0x63: {                            // surfbind {mask,sid,off}
      std::uint8_t mask = r.u8(), sid = r.u8();
      std::uint32_t hoff = r.u32();
      if (!r.ok) { fail("surfbind"); return res; }
      if (!env.selfArena) break;
      SurfaceObjectState& s = env.selfArena->surface;
      const std::size_t i = (sid - 1) & 0xf;
      s.handlerMask[i] = mask;
      s.handlerOff[i] = hoff;
      break;
    }
    case 0xa8: {                            // surfcfg {sid, mask}
      std::uint8_t sid = r.u8(), mask = r.u8();
      if (!r.ok) { fail("surfcfg"); return res; }
      if (!env.selfArena) break;
      SurfaceObjectState& s = env.selfArena->surface;
      const std::size_t i = (sid - 1) & 0xf;
      s.config[i] = mask;
      if (mask & 0x80) {
        surfacePolyOp(s.polys, s.polyCount, sid, 2);
        s.opMaskB |= (1u << (sid & 31));
      }
      break;
    }

    // ---------------------------------------------------------------
    // Type-7 volume activation — FUN_00412d04
    // ---------------------------------------------------------------
    case 0x8e: {                      // volact {u8 id, str, u8, u8, f32}
      std::uint8_t id = r.u8();
      std::string nm = r.str();       // name -> record +0x8 (stricmp'd
      std::uint8_t k0 = r.u8(), k1 = r.u8();  //  by op 0xbe)
      float rate = r.f32();
      if (!r.ok) { fail("volact"); return res; }
      if (env.selfArena) {
        // OBSERVED call shape: name/kind/rate/mask feed
        // surfaceVolumeCreate (FUN_00412d04 stores the name at +0x8).
        traversalVolumeActivate(*env.selfArena, id, nm, k1, rate,
                                static_cast<std::uint32_t>(k0),
                                /*noFalloff=*/false);
      }
      break;
    }

    // ---------------------------------------------------------------
    // Named record flag — FUN_00413354 (0x44ace6): find the arena's
    // +0x45e record by name (stricmp, FUN_0047e02c) and set/clear bit0
    // of its +0x1c (queryMask). No record -> silent no-op.
    // ---------------------------------------------------------------
    case 0xbe: {                      // recEnable {u8 flag, lstr name}
      std::uint8_t flag = r.u8();
      std::string nm = r.str();
      if (!r.ok) { fail("recEnable"); return res; }
      if (env.selfArena) {
        for (SurfaceRecord* rec = env.selfArena->surface.records; rec;
             rec = rec->next) {
          if (stricmpEq(rec->nameText, nm)) {
            if (flag) rec->queryMask |= 1u;
            else rec->queryMask &= ~1u;
            break;
          }
        }
      }
      break;
    }

    // ---------------------------------------------------------------
    // Object spawn family — FUN_00454894 / FUN_00454af8. The object is
    // created on the named arena's +0x68 list (or self when the name
    // isn't an arena); class/name/script metadata is preserved on the
    // record. The class's native behavior is a documented seam.
    // ---------------------------------------------------------------
    case 0x95: {              // spawn {x,y,z,yaw,flags,class,name,scOff}
      float x = r.f32(), y = r.f32(), z = r.f32(), yaw = r.f32();
      std::uint32_t flags = r.u32();
      std::string cls = r.str(), nm = r.str();
      std::uint32_t scOff = r.u32();
      if (!r.ok) { fail("spawn"); return res; }
      traversalScriptSpawn(env, x, y, z, yaw, flags, cls, nm, scOff,
                           0);
      break;
    }
    case 0x56: {              // spawn2 {x,y,z,class,scOff}
      float x = r.f32(), y = r.f32(), z = r.f32();
      std::string cls = r.str();
      std::uint32_t scOff = r.u32();
      if (!r.ok) { fail("spawn2"); return res; }
      traversalScriptSpawn(env, x, y, z, 0.0f, 0, cls, "", scOff, 1);
      break;
    }
    case 0xe6: {              // spawn3 {x,y,z,yaw,spawnId,class,scOff}
      // OBSERVED (0x44a0a9): operand 4 is an f32 copied raw to +0x4c
      // (yaw); operand 5 is the spawn-id arg to FUN_00454af8 (>=0 kept,
      // <0 -> FUN_00454810 name-instance counter+1). No +0x148 flag
      // tail — spawn3 objects are not movers.
      float x = r.f32(), y = r.f32(), z = r.f32();
      float yaw = r.f32();
      std::uint32_t spawnId = r.u32();
      std::string cls = r.str();
      std::uint32_t scOff = r.u32();
      if (!r.ok) { fail("spawn3"); return res; }
      traversalScriptSpawn(env, x, y, z, yaw, spawnId, cls, "", scOff,
                           3);
      break;
    }
    case 0xa1: {              // spawnNamed {x,y,z,name,scOff}
      float x = r.f32(), y = r.f32(), z = r.f32();
      std::string nm = r.str();
      std::uint32_t scOff = r.u32();
      if (!r.ok) { fail("spawnNamed"); return res; }
      traversalScriptSpawn(env, x, y, z, 0.0f, 0, nm, "", scOff, 2);
      break;
    }
    case 0xce: {              // imgobj {w imgCell, f32, s name, w scOff}
      (void)r.u32();
      (void)r.f32();
      std::string nm = r.str();
      std::uint32_t scOff = r.u32();
      if (!r.ok) { fail("imgobj"); return res; }
      traversalScriptSpawn(env, 0, 0, 0, 0.0f, 0, nm, "", scOff, 4);
      break;
    }

    // ---------------------------------------------------------------
    // Type-9 slide/deflect — opcode 0xe0 (slideZoneTrigger)
    // ---------------------------------------------------------------
    case 0xe0: {              // deflect {u8 flag; if!=0 f32 yaw, f32
      std::uint8_t flag = r.u8();   // speed}
      if (!r.ok) { fail("deflect"); return res; }
      if (flag == 0) {
        env.slideClear = true;      // clears 0x540e24 + 0x540cbc
        break;
      }
      float yaw = r.f32(), speed = r.f32();
      if (!r.ok) { fail("deflect args"); return res; }
      // Type-9 scan over the CURRENT arena's records (c48 +0x38/0x3c).
      if (env.rt && env.currentArena && env.currentArena->rec &&
          env.playerPos) {
        SlideZoneResult sz = slideZoneTrigger(
            env.currentArena->rec->subRecords.data(),
            env.currentArena->rec->subRecords.size(), flag,
            env.playerPos, env.slideMode, env.hasContactNormal, yaw,
            speed, env.dt);
        if (sz.clearedSlide) env.slideClear = true;
        if (sz.slideRedirect) {
          env.slideChannel = 1;     // enter slide (0x540e24 nonzero)
          env.slideImpulseX = sz.impulseX;
          env.slideImpulseZ = sz.impulseZ;
        }
        if (sz.setBounceFlag) env.deflectBounce = true;
      }
      break;
    }

    case 0x83: {                            // completion/cine (0x43d3e2)
      const std::uint8_t subop = r.u8();    // {u8 subop}
      if (!r.ok) { fail("0x83"); return res; }
      completionOp83(env, subop);
      break;
    }

    default:
      // Unknown / unimplemented opcode — OBSERVED behavior is the
      // "Unrecognised controlalien" diagnostic + exit. We halt the
      // script so the stream can't desynchronize silently.
      {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "Unrecognised controlalien op 0x%02x at +%x",
                      op, insnOff);
        fail(buf);
      }
      return res;
    }
  }

  // Loop cap hit — OBSERVED diagnostic, gate cleared.
  {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "Alien %s looped %d commands, off %lx", name,
                  res.instructions,
                  static_cast<unsigned long>(r.pc));
    res.diag = buf;
    if (env.diagLog) env.diagLog->push_back(res.diag);
    res.error = true;
    st.pcImageOff = 0;
    st.active = false;
  }
  return res;
}

// ---------------------------------------------------------------------------
// Object-bound script pass — FUN_004388d8(ctx=object). The same VM
// bytecode + linkage rules as the arena form, but the bound object is
// a DynamicObject: field ops write its +0xNN fields and the persistent
// ctx block is the object's own (+0x234 locals, +0x244/+0x312 flag
// dwords, +0x248..+0x26c call stack, +0x108/+0x22c/+0x230 PCs/wait —
// image POINTERS, matching the original's cmiBase+offset storage).
//
// Two drivers share the instruction set:
//   traversalObjectInitScript — the FUN_004566f0 table-2 "%s$%s"
//     record, run synchronously at spawn; +0x108 is cleared on exit
//     (OBSERVED 0x4567f9) so the script never persists.
//   traversalObjectScriptTick — the persistent per-object script
//     (table-0 "%s$%s_%d" record or the +0x110 death handoff): the
//     +0x108 gate/+0x22c wait are honored, ckpt/call/goto write +0x108
//     and 0xff suspends to the next frame.
//
// OBSERVED opcode set (MDK95.EXE handler disassembly):
//   control:  0x01 ckpt(+0x108=pc)  0x09 stop(+0x108=0,depth=0,
//             mark[0]=0)  0xff suspend(+0x21e=0)  0xfd return
//             0x0c rgoto / 0xfc rcall {u8 n,n×u32} (RNG -> first, seam)
//             0x5f wcall {u8 n,n×{u8 w,u32}} (weighted RNG -> first
//             positive weight, seam)  0x66 condlink {u8 mode,...}
//             gated on +0xec==0   0x40 wait {varop} -> +0x22c/+0x230
//   fields:   0x08 +0x4c yaw(i16,neg+360)  0x0b +0x11a  0x49 +0x11b
//             0x10 +0x8/+0x2a2/+0x21f health(u16)  0x6f +0x146(u16)
//             0x32/0x33/0x34 +0x38/+0x3c/+0x40     0x53 +0x58 scale
//             0x54 +0x5c zBias   0x5a +0xe8        0xc7 +0x104
//             0x75 +0x148 &= ~u32               0x76 +0x118 tgt(u16-1)
//             0x03/0x3b +0x114 anim bind (one-shot/loop, reset state)
//             0x18 +0x140/+0x144 anim-sound marker   0x4c +0x110 imgref
//             0xc6 elem-set decl -> +0x149|0x20, +0x302 prefix,
//                +0x306 digitOfs, +0x30a extra, +0x30e/+0x31e pools
//             0x02 path bind -> +0xec/+0x149/+0x14b/+0xe8/+0xf0/+0xf4
//             0x4e subtype set -> +0x120..+0x128, +0x11e=0x4e
//             0xcd +0x21f = u8
//   flags:    0x23 +0x148|2   0x24 +0x148|1  0x29 +0x149|1/+0x14a|0x80
//             0x3f +0x148^0x10(inv)  0x61 +0x148^0x80(inv,+0x54=0)
//             0x74 +0x148 dword |=   0x44/45/46 +0xNN group |=/&=~/^=
//             1<<(bit&31)          0x47 test bit + linkage
//   masks:    0x1f {count,strings} -> +0x2c8 element-name/"ALL" mask
//   vars:     0x41 {mode,idx,u32} -> *FUN_00438654 slot
//   connect:  0x96 {u32,u32}->+0x306/+0x30a anim recs
//             0x97 {4 strs}->+0x316/31a/31e/322 sound names
//             0x98 +0x312 hi nibble  0x99 +0x30e radius
// Unknown/other opcodes halt with a diagnostic (native safety policy —
// the original would desync on a mis-framed stream).
// ---------------------------------------------------------------------------
namespace {

struct ObjScriptPass {
  TraversalScriptEnv& env;
  DynamicObject& obj;
  Reader& r;
  TraversalScriptResult& res;
  const char* name;
  bool done = false;            // pass must return to caller

  void fail(const char* why) {
    res.error = true;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: %s", name, why);
    res.diag = buf;
    if (env.diagLog) env.diagLog->push_back(res.diag);
    obj.field108 = nullptr;            // error path: +0x108 = 0
    done = true;
  }
  // Image-relative ref (cmiBase + off) — pointer form, matching the
  // original's arithmetic. off==0 -> nullptr (bounded; the original
  // would point at the image base).
  const void* ptrAt(std::uint32_t off, std::size_t n = 1) {
    if (off == 0) return nullptr;
    return r.ptr(off, n);
  }
  std::uint32_t offAt(const void* p) const {
    const std::byte* base = env.image.data() + env.imageBase;
    const std::byte* q = static_cast<const std::byte*>(p);
    if (!p || q < base || q >= env.image.data() + env.image.size())
      return 0;
    return static_cast<std::uint32_t>(q - base);
  }
  // OBSERVED call/return (0x43bb57..0x43bc2c shared tail): push
  // {retPc=pc, savedPc=+0x108}, +0x248++, +0x108=pc=target,
  // mark[depth+1]=0 (the tail reads +0x248 after the increment);
  // return pops the frame and restores +0x108.
  bool doCall(std::uint32_t target) {
    if (obj.scriptCallDepth >= 4) { fail("Gosub overflow"); return false; }
    const int d = obj.scriptCallDepth;
    obj.scriptRetPc[d] = ptrAt(r.pc);
    obj.scriptSavedPc[d] = obj.field108;
    ++obj.scriptCallDepth;
    obj.scriptMark[d + 1] = 0;
    obj.field108 = ptrAt(target);
    r.pc = target;
    return true;
  }
  bool doReturn() {
    if (obj.scriptCallDepth <= 0) { fail("Gosub underflow"); return false; }
    --obj.scriptCallDepth;
    r.pc = offAt(obj.scriptRetPc[obj.scriptCallDepth]);
    obj.field108 = obj.scriptSavedPc[obj.scriptCallDepth];
    return true;
  }
  // OBSERVED goto (0x4393b6): +0x108 = target AND pc = target — the
  // goto checkpoints as it jumps — then mark[depth]=0 (the inline
  // tails read +0x248 without incrementing it).
  void doGoto(std::uint32_t target) {
    obj.field108 = ptrAt(target);
    obj.scriptMark[obj.scriptCallDepth] = 0;
    r.pc = target;
  }
  // Random-pick seam: FUN_00401ed4's RNG is deterministic here —
  // index 0 / first positive weight, matching the arena VM's rcall
  // convention.
  std::uint32_t pickList(std::uint8_t n) {
    std::uint32_t tgt = 0;
    for (std::uint8_t k = 0; k < n; ++k) {
      std::uint32_t o = r.u32();
      if (k == 0) tgt = o;
    }
    return tgt;
  }
};

void objScriptInsn(ObjScriptPass& v) {
  Reader& r = v.r;
  DynamicObject& obj = v.obj;
  TraversalScriptEnv& env = v.env;
  TraversalScriptResult& res = v.res;
  const ScriptCtxSlots ctx = ctxSlots(obj);
  auto imageRef = [&](std::uint32_t off, std::size_t n) -> const void* {
    return r.ptr(off, n);
  };
  // FUN_00438898 — named-indirect record fixup (OBSERVED: called by
  // ops 0x03/0x3b/0x96 right after the u32 image-ref operand). When
  // the bound record's first dword is 0, the record is a name stub —
  // the original resolves the 8-char name at rec+4 through the level
  // stream's region-A array-A {name[8], tA-relative off} table
  // (FUN_004387ec; 0x54b744 = the stream cursor, t = cursor+[cursor]+4
  // = the block's region-A base). The port scans the object's
  // home-arena MTO block first, then every block.
  auto animRecResolve = [&](const void* p) -> const void* {
    if (p == nullptr || env.rt == nullptr) return p;
    const auto* b = static_cast<const std::uint8_t*>(p);
    if (b[0] != 0 || b[1] != 0 || b[2] != 0 || b[3] != 0) return p;
    char nm9[9]{};
    std::memcpy(nm9, b + 4, 8);
    const std::string nm(nm9);
    if (nm.empty()) return p;
    const TraversalLevel& lv = env.rt->level;
    if (lv.mtoBytes.empty()) return p;
    const auto* mb =
        reinterpret_cast<const std::uint8_t*>(lv.mtoBytes.data());
    const auto match = [&](const MtoBlock& blk) -> const void* {
      for (const auto& nr : blk.regionAArrayA) {
        if (nr.name() != nm) continue;
        const std::uint64_t off =
            blk.regionAOffset + nr.fieldAt0x08;
        if (off < lv.mtoBytes.size()) return mb + off;
      }
      return nullptr;
    };
    const std::string& arenaNm =
        (obj.arena != nullptr && obj.arena->owner != nullptr)
            ? obj.arena->owner->name
            : std::string();
    if (!arenaNm.empty()) {
      for (const auto& blk : lv.mto.blocks) {
        if (blk.innerName().compare(0, arenaNm.size(), arenaNm) == 0) {
          if (const void* hit = match(blk)) return hit;
        }
      }
    }
    for (const auto& blk : lv.mto.blocks) {
      if (const void* hit = match(blk)) return hit;
    }
    return p;   // unresolved — keep the stub (bounded inert record)
  };

  if (!r.ok) { v.fail("script read out of bounds"); return; }
  const std::uint32_t insnOff = r.pc;
  const std::uint8_t op = r.u8();
  ++res.instructions;
  if (!r.ok) { v.fail("opcode fetch out of bounds"); return; }
  static const char* traceCls = std::getenv("MDK_TRACE_OBJ");
  if (traceCls != nullptr && obj.scriptClass == traceCls) {
    std::fprintf(stderr, "    [vm] %s +%05x op=%02x mark=%02x f=%d\n",
                 obj.scriptClass.c_str(), insnOff, op,
                 (unsigned)obj.field21e, (int)obj.animFrame);
  }

  switch (op) {
  case 0xff:                              // suspend — end of pass
    obj.field21e = 0;                     // +0x21e = 0 (OBSERVED
    res.halted = true;                    //  0x451e7c)
    v.done = true; return;
  case 0x09:                              // stop — +0x108=0, depth=0,
    obj.field108 = nullptr;               // mark[0]=0 (0x43f57a)
    obj.scriptCallDepth = 0;
    obj.scriptMark[0] = 0;
    res.stopped = true; v.done = true; return;
  case 0xfd:                              // return — pop event frame
    v.doReturn(); return;
  case 0x01:                              // ckpt: +0x108 = pc
    obj.field108 = v.ptrAt(r.pc);         // (OBSERVED 0x43bcbc)
    return;
  case 0x04: {                            // broadcast dispatch (0x439794)
    // Same handler as the arena VM — the ctx is this object:
    // ctx->+0x60 = obj's arena (+0x60), +0x2b8/+0x11a/+0x4c/+0x10
    // read the object's own fields.
    if (obj.arena == nullptr) return;
    if (const char* e = broadcastDispatchOp(env, obj, *obj.arena,
                                          obj.field11a, r)) {
      v.fail(e);
      return;
    }
    return;
  }
  case 0x3a: {                            // varop -> +0xe0 (0x43aed2)
    // {u8 mode, f32 | u8 idx}. OBSERVED: mode 3 reads an inline f32,
    // else idx resolves a FUN_00438654 slot; the value lands in
    // +0xe0 (animRate).
    const std::uint8_t mode = r.u8();
    const float val = resolveVarMode(mode, r, env, ctx);
    if (!r.ok) { v.fail("animrate"); return; }
    obj.animRate = val;
    return;
  }
  case 0xb0: {                            // flag14a&4 link (0x44c76a)
    // {linkage}. cond: ctx+0x14a & 4.
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("f14alink"); return; }
    const bool cond = (obj.col.flags14a & 4) != 0;
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x25: {                            // flag14c&2 link (0x44040d)
    // {linkage}. cond: ctx+0x14c & 2 (collision state bit).
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("f14clink"); return; }
    const bool cond = (obj.col.flags14c & 2) != 0;
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0xb1: {                            // varop -> +0x2c4 (0x449432)
    // {u8 mode, f32 | u8 idx} — same varop shape as 0x3a, stored to
    // +0x2c4 (the aux-distance gate, default 1000).
    const std::uint8_t mode = r.u8();
    const float val = resolveVarMode(mode, r, env, ctx);
    if (!r.ok) { v.fail("auxdist"); return; }
    obj.field2c4 = val;
    return;
  }
  case 0xbb: {                            // random impulse (0x448f7a)
    // {f32 horiz, f32 vert}. OBSERVED: deg = rand(360); sn/cs =
    // FUN_00437f98(deg); s1,s2 = (rand()+0x4000)*C(0x497c2c=1/32768,
    // double muls truncated to f32); vel += (sn*horiz*s1,
    // cs*horiz*s1, vert*s2).
    const float mag = r.f32(), vert = r.f32();
    if (!r.ok) { v.fail("randImpulse"); return; }
    if (env.rt != nullptr) {
      const double k = 3.0517578125e-5;    // 0x497c2c = 1/32768
      const float deg = static_cast<float>(
          enemyRandBelow(env.rt->rngState, 0x168));
      float sn = 0.0f, cs = 0.0f;
      sincosDeg(deg, &sn, &cs);
      const float m = static_cast<float>(
          (enemyRandNext(env.rt->rngState) + 0x4000) * k *
          static_cast<double>(mag));
      obj.field28 += sn * m;
      obj.field2c += cs * m;
      obj.field30 += static_cast<float>(
          (enemyRandNext(env.rt->rngState) + 0x4000) * k *
          static_cast<double>(vert));
    }
    return;
  }
  case 0x40: {                            // wait {varop} — +0x22c secs,
    float secs = resolveVar(r, env, ctx); // +0x230=resume (0x43bcd0)
    if (!r.ok) { v.fail("wait"); return; }
    std::uint32_t bits;
    std::memcpy(&bits, &secs, 4);
    if ((bits & 0x7fffffffu) == 0) {      // zero -> 1-frame nudge
      std::uint32_t nudge = 0x3727c5acu;  //   (OBSERVED constant)
      std::memcpy(&secs, &nudge, 4);
    }
    obj.field22c = secs;
    obj.field230 = v.ptrAt(r.pc);
    res.waited = true; v.done = true; return;
  }
  case 0x0c: {                            // rgoto {u8 n, n×u32}
    std::uint8_t n = r.u8();
    if (!r.ok || n == 0) { v.fail("rgoto"); return; }
    const std::uint32_t tgt = v.pickList(n);
    if (!r.ok) { v.fail("rgoto offs"); return; }
    v.doGoto(tgt);
    return;
  }
  case 0xfc: {                            // rcall {u8 n, n×u32}
    std::uint8_t n = r.u8();
    if (!r.ok || n == 0) { v.fail("rcall"); return; }
    const std::uint32_t tgt = v.pickList(n);
    if (!r.ok) { v.fail("rcall offs"); return; }
    v.doCall(tgt);
    return;
  }
  case 0x5f: {                            // wcall {u8 n, n×{u8 w,u32}}
    // OBSERVED (0x43b9c7): weights summed, FUN_00401ed4(sum) picks a
    // weight index, the first entry whose cumulative weight exceeds
    // the pick is called. Deterministic seam: pick = 0 -> the first
    // positive-weight entry (all-zero -> no call).
    std::uint8_t n = r.u8();
    if (!r.ok || n == 0) { v.fail("wcall"); return; }
    std::uint32_t tgt = 0;
    std::uint32_t accum = 0;
    for (std::uint8_t k = 0; k < n; ++k) {
      std::uint8_t w = r.u8();
      std::uint32_t o = r.u32();
      accum += w;
      if (tgt == 0 && accum > 0) tgt = o;  // first cum weight > pick
    }
    if (!r.ok) { v.fail("wcall offs"); return; }
    if (tgt != 0) v.doCall(tgt);
    return;
  }
  case 0x66: {                            // condlink — +0xec gate
    // OBSERVED (0x4391db..0x43949e): {u8 mode; 0xfe -> two u32,
    // 0xfc/0x0c -> one u32}. With NO path bound (+0xec==0): 0xfe/0xfc
    // call the first target, 0xfd returns, 0x0c goto-checkpoints it.
    // With a path bound only 0xfe acts — and calls the SECOND (else)
    // target.
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("condlink"); return; }
    const bool pathFree = (obj.fieldEC == nullptr);
    if (pathFree) {
      if (L.mode == 0xfe || L.mode == 0xfc) v.doCall(L.a);
      else if (L.mode == 0xfd) v.doReturn();
      else if (L.mode == 0x0c) v.doGoto(L.a);
    } else if (L.mode == 0xfe) {
      v.doCall(L.b);
    }
    return;
  }
  case 0x02: {                            // path bind (0x438e7c)
    // {u32 pathRef, u8 f1, u8 f2, u16 frame, u8 mode, [f32×3]}
    std::uint32_t ref = r.u32();
    std::uint8_t f1 = r.u8(), f2 = r.u8();
    std::uint16_t frame = r.u16();
    std::uint8_t mode = r.u8();
    if (!r.ok) { v.fail("pathbind"); return; }
    obj.fieldEC = v.ptrAt(ref, 8);
    // +0x149 is byte1 of the +0x148 dword — write through flags148 so
    // the flags149 mirror can't be wiped by a later dword op resync.
    if (f1 & 1) obj.col.flags148 |= 0x0200u;
    else        obj.col.flags148 &= ~0x0200u;
    // f1&2 -> +0x14b|8 (ghost); +0x14b byte is unmodelled.
    if (f2 & 1) obj.col.flags148 |= 0x0400u;
    else        obj.col.flags148 &= ~0x0400u;
    obj.col.flags149 = static_cast<std::uint8_t>(obj.col.flags148 >> 8);
    const bool marked = (f2 & 2) != 0;
    if (marked) obj.fieldE8 = -1.0f;       // +0xe8 = -1.0
    if (frame != 0) obj.fieldF0 = static_cast<float>(frame);
    else if (marked && obj.fieldEC) {
      // OBSERVED (0x438f96): +0xf0 = last path entry's u32 - 1 —
      // path {u32 count, u32, entry[count]×10 dwords}.
      const std::byte* p =
          static_cast<const std::byte*>(obj.fieldEC);
      std::uint32_t cnt;
      std::memcpy(&cnt, p, 4);
      const std::byte* base = env.image.data() + env.imageBase;
      const std::byte* lim = env.image.data() + env.image.size();
      const std::byte* slot = p + 4 + (cnt ? cnt - 1 : 0) * 0x28;
      if (cnt && slot + 4 <= lim && slot >= base) {
        std::uint32_t v0;
        std::memcpy(&v0, slot, 4);
        obj.fieldF0 = static_cast<float>(v0 - 1);
      } else {
        obj.fieldF0 = 0.0f;
      }
    } else {
      obj.fieldF0 = 0.0f;
    }
    if (mode == 0) {
      for (int i = 0; i < 3; ++i) obj.fieldF4[i] = r.f32();
    } else {
      // FUN_00456bc8(path, +0xf0, &pt); +0xf4 = pos - pt — the path
      // sampler is a seam; keep the offset zeroed.
      obj.fieldF4[0] = obj.fieldF4[1] = obj.fieldF4[2] = 0.0f;
    }
    obj.fieldE6 = -1;                      // +0xe6 = 0xffff
    // FUN_00457264(obj) — path-start snap; a seam.
    return;
  }
  case 0x44: case 0x45: case 0x46: {      // flag bit ops (0x447d99+)
    std::uint8_t grp = r.u8(), bit = r.u8();
    if (!r.ok) { v.fail("flagop"); return; }
    std::uint32_t& f = resolveFlag(grp, env, ctx);
    const std::uint32_t m = 1u << (bit & 0x1f);
    if (op == 0x44) f |= m;
    else if (op == 0x45) f &= ~m;
    else f ^= m;
    return;
  }
  case 0x47: {                            // flag bit test + linkage
    std::uint8_t grp = r.u8(), bit = r.u8();
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("flagtest"); return; }
    const bool cond = (resolveFlag(grp, env, ctx) >> (bit & 0x1f)) & 1;
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x2a: {                            // name-cond link (0x43cb95)
    // {lstr name, [lstr fallback], linkage}. OBSERVED operand quirk:
    // the second string is only consumed when the first's text is
    // EMPTY (handler skips it otherwise) — so `nm` = s1 when
    // non-empty, else the fallback. cond: ctx+0x21e (signed) > 0, the
    // bound index within the model's ELEMENT array (model+0x20,
    // 0x5c-stride records, name at +0x00; bound = +0x1c elem count),
    // and elemName[idx-1] == nm OR nm == "ANY" (0x4979f4). The -0x258
    // flag survives
    // only when s1 was non-empty: a fired linkage clears +0x21e iff
    // the primary string was used (the dispatch itself always runs;
    // the else-call is not gated on it either).
    const std::string s1 = r.str();
    const std::string nm = s1.empty() ? r.str() : s1;
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("namelink"); return; }
    const int idx = static_cast<std::int8_t>(obj.field21e);
    bool cond = false;
    if (idx > 0 &&
        static_cast<std::size_t>(idx) <= obj.model.elems.size()) {
      cond = (obj.model.elemName(idx - 1) == nm) || (nm == "ANY");
    }
    bool fired = false;
    switch (L.mode) {
    case 0xfe: if (cond) { v.doCall(L.a); fired = true; }
               else if (L.b) v.doCall(L.b); break;
    case 0xfc: if (cond) { v.doCall(L.a); fired = true; } break;
    case 0x0c: if (cond) { v.doGoto(L.a); fired = true; } break;
    case 0xfd: if (cond) { v.doReturn(); fired = true; } break;
    default: break;
    }
    if (fired && !s1.empty()) obj.field21e = 0;  // the -0x258 gate —
                                                 // fallback names never
                                                 // consume the mark
    return;
  }
  case 0x15: {                            // path-index set (0x439187)
    // {u32 v — only lo16 is read, pc still advances 4}. OBSERVED:
    // +0xe6 = (i16)v; the 0xfffe (-2) sentinel rewrites it to
    // trunc(+0xf0) via FUN_0047d59a (FRNDINT under RC=11 —
    // truncate-toward-zero).
    obj.fieldE6 = static_cast<std::int16_t>(r.u32() & 0xffff);
    if (obj.fieldE6 == -2)
      obj.fieldE6 = static_cast<std::int16_t>(std::trunc(obj.fieldF0));
    return;
  }
  case 0x16: {                            // mark!=0,-3 link (0x43c8b1)
    // {linkage}. OBSERVED: fires while +0x21e is nonzero and not -3
    // (0xfd) — i.e. a bound name index the 0x2a family hasn't
    // consumed and not the -3 sentinel. A fired arm (call/goto/ret)
    // clears +0x21e — all three dispatch tails converge on the shared
    // clear at 0x43cad3. The gate-off path dispatches only mode 0xfe's
    // else target.
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("marklink"); return; }
    const bool cond = obj.field21e != 0 && obj.field21e != 0xfd;
    bool fired = false;
    switch (L.mode) {
    case 0xfe: if (cond) { v.doCall(L.a); fired = true; }
               else if (L.b) v.doCall(L.b); break;
    case 0xfc: if (cond) { v.doCall(L.a); fired = true; } break;
    case 0x0c: if (cond) { v.doGoto(L.a); fired = true; } break;
    case 0xfd: if (cond) { v.doReturn(); fired = true; } break;
    default: break;
    }
    if (fired) obj.field21e = 0;
    return;
  }
  case 0x17: {                            // +0x148 bit0 (0x43ac02)
    // {u8 v}. OBSERVED: v!=0 -> +0x148|=1, +0x302(dword)=1;
    // v==0 -> +0x148&=~1, +0x54(pitch)=0.
    if (r.u8()) {
      obj.col.flags148 |= 0x1;
      obj.field302 = 1;
    } else {
      obj.col.flags148 &= ~0x1u;
      obj.pitchDeg = 0.0f;
    }
    return;
  }
  case 0x0a: {                            // same-name count link
    // {lstr name, u8 n, linkage}. OBSERVED (0x43f5b0): counts objects
    // in the ctx's home arena (+0x60) that are alive (+0x06), whose
    // model name (+0x0c->+0x00) == name, and that pass the
    // leader/var gates: +0x138 null or ==ctx or leader's +0x11a below
    // ctx's, and the object's +0x11b >= ctx's +0x11a. Fires when the
    // count equals n.
    const std::string nm = r.str();
    const std::uint8_t want = r.u8();
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("countlink"); return; }
    int count = 0;
    if (obj.arena != nullptr) {
      for (auto& up : obj.arena->storage) {
        DynamicObject* c = up.get();
        if (!c->col.named || c->arena != obj.arena) continue;
        if (c->model.modelName() != nm) continue;
        if (c->field138 != nullptr && c->field138 != &obj &&
            c->field138->field11a >= obj.field11a)
          continue;
        if (c->field11b < obj.field11a) continue;
        ++count;
      }
    }
    const bool cond = (count == want);
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x0e: {                            // view-cone+LOS link
    // {u16 range, u8 arc, linkage}. OBSERVED (0x43ec02 ->
    // FUN_0045d880): player within `range`, inside the yaw cone whose
    // half-angle scales with distance —
    // thresh = (arc-90)/range*dist + 90 — and with a stab-clear LOS
    // (ctx.z+10-zBias -> player.z+5) in the current arena and the
    // partner when attached with carrierBusy clear.
    const float range = static_cast<float>(r.u16());
    const float arc = static_cast<float>(r.u8());
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("conelink"); return; }
    const bool cond = coneLosTest(env, obj, range, arc);
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x36: {                            // seek-dist cmp link
    // {u8 kind, f32 a, [f32 b if kind==7|8], linkage}. OBSERVED
    // (0x4459d6 -> FUN_0045ad40): dist = pos->+0x120 (XY when
    // +0x14c&2 else 3D); kind: 1=d<a 2=d>a 3=d-0.05<a 4=d+0.05>a
    // 5=|d-a|<0.05 6=|d-a|>=0.05 7=a<=d<=b 8=d<=a||d>=b.
    const std::uint8_t kind = r.u8();
    const float va = r.f32();
    float vb = 0.0f;
    if (kind == 7 || kind == 8) vb = r.f32();
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("distlink"); return; }
    const float dx = obj.field120[0] - obj.pos[0];
    const float dy = obj.field120[1] - obj.pos[1];
    float dist;
    if (obj.col.flags14c & 0x2) {
      dist = std::sqrt(dx * dx + dy * dy);        // FUN_004301bc
    } else {
      const float dz = obj.field120[2] - obj.pos[2];
      dist = std::sqrt(dx * dx + dy * dy + dz * dz); // FUN_00430160
    }
    bool cond;
    switch (kind) {                             // FUN_0045ad40
    case 1: cond = dist < va; break;
    case 2: cond = dist > va; break;
    case 3: cond = dist - 0.05f < va; break;    // C(0x4981cc) = -0.05
    case 4: cond = dist + 0.05f > va; break;    // C(0x4981c4) = +0.05
    case 5: cond = std::fabs(dist - va) < 0.05f; break;
    case 6: cond = std::fabs(dist - va) >= 0.05f; break;
    case 7: cond = dist >= va && dist <= vb; break;
    case 8: cond = dist <= va || dist >= vb; break;
    default: cond = false; break;
    }
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x3e: {                            // facing-angle link (0x445d4d)
    // {u8 kind, f32 a, [f32 b if kind==7|8], linkage}. OBSERVED:
    // angle = bearing(camXY - pos) - +0x4c, normalized to [0,180]
    // (+=360 while <0, -=360 while >360, 360-angle when >180); cond =
    // FUN_0045ad40(kind, angle, a, b) — the angle is the comparator's
    // first stack arg (x), so kind 1 = angle>=a, 2 = angle>a,
    // 3 = angle-0.05>a, 4 = angle+0.05<a, 5 = |angle-a|<0.05,
    // 6 = |angle-a|>=0.05, 7 = a<=angle<=b, 8 = angle<=a||angle>=b.
    const std::uint8_t kind = r.u8();
    const float va = r.f32();
    float vb = 0.0f;
    if (kind == 7 || kind == 8) vb = r.f32();
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("anglelink"); return; }
    bool cond = false;
    if (env.rt != nullptr) {
      const float* cam = env.rt->camera.pose.pos;  // 0x54c6c4..c8
      float ang = bearingDeg(cam[1] - obj.pos[1],
                             cam[0] - obj.pos[0]) - obj.yawDeg;
      while (ang < 0.0f) ang += 360.0f;
      while (ang > 360.0f) ang += -360.0f;
      if (ang > 180.0f) ang = 360.0f - ang;
      switch (kind) {                           // FUN_0045ad40
      case 1: cond = ang >= va; break;
      case 2: cond = ang > va; break;
      case 3: cond = ang - 0.05f > va; break;   // C(0x4981cc) = -0.05
      case 4: cond = ang + 0.05f < va; break;   // C(0x4981c4) = +0.05
      case 5: cond = std::fabs(ang - va) < 0.05f; break;
      case 6: cond = std::fabs(ang - va) >= 0.05f; break;
      case 7: cond = ang >= va && ang <= vb; break;
      case 8: cond = ang <= va || ang >= vb; break;
      default: break;
      }
    }
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x3c: {                            // face camera (0x441e46)
    // No operands: +0x4c = bearing(0x54c6c4 - pos) wrapped [0,360).
    if (env.rt != nullptr) {
      const float* cam = env.rt->camera.pose.pos;
      float yaw = bearingDeg(cam[1] - obj.pos[1],
                             cam[0] - obj.pos[0]);
      while (yaw >= 360.0f) yaw += -360.0f;       // OBSERVED loops
      while (yaw < 0.0f) yaw += 360.0f;
      obj.yawDeg = yaw;
    }
    return;
  }
  case 0x48: {                            // flag bit CLEAR + linkage
    // {u8 grp, u8 bit, linkage}. OBSERVED (0x4481b2): the fire path
    // runs when the resolved flag dword's bit is CLEAR (0x47's
    // inverse).
    std::uint8_t grp = r.u8(), bit = r.u8();
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("flagtest"); return; }
    const bool cond =
        ((resolveFlag(grp, env, ctx) >> (bit & 0x1f)) & 1) == 0;
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x52: {                            // +0x44 var write (0x43b018)
    // {u8 mode; mode==3 -> f32 imm, else u8 idx -> var slot}.
    const std::uint8_t mode = r.u8();
    if (mode == 3) {
      obj.field44 = r.f32();
    } else {
      const std::uint8_t idx = r.u8();
      if (float* slot = resolveVarRef(mode, idx, env, ctx))
        obj.field44 = *slot;
    }
    if (!r.ok) { v.fail("var44"); return; }
    return;
  }
  case 0xc8: {                            // move-toward + arrive link
    // {f32 rate, f32 x,y,z, linkage}. OBSERVED (0x451261): writes the
    // +0x294/+0x298/+0x29c per-axis approach values —
    // field = (1/30) / clamp(rate*(1/30)*delta/manhattan, delta) —
    // with manhattan = |dx|+|dy| (+|dz| unless +0x148&2) floored at
    // 0.1; z skipped when +0x148&2. The linkage fires when
    // manhattan < 0.5 ("arrived").
    float rate = r.f32();
    const float pt[3] = {r.f32(), r.f32(), r.f32()};
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("movelink"); return; }
    float d[3] = {pt[0] - obj.pos[0], pt[1] - obj.pos[1],
                  pt[2] - obj.pos[2]};
    float man = std::fabs(d[0]) + std::fabs(d[1]);
    if (!(obj.col.flags148 & 0x2)) man += std::fabs(d[2]);
    if (man <= 0.1f) man = 0.1f;                  // C(0x497cfc)
    rate *= 0.03333333507180214f;                 // C(0x49b6f4)=1/30
    for (int i = 0; i < 3; ++i) {
      if (i == 2 && (obj.col.flags148 & 0x2)) break;
      if (d[i] == 0.0f) continue;
      float t = rate * d[i] / man;
      if ((d[i] > 0.0f && t > d[i]) || (d[i] < 0.0f && t < d[i]))
        t = d[i];
      obj.animImpulse[i] = 0.03333333507180214f / t;
    }
    const bool cond = man < 0.5f;                 // C(0x497d04)
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0xd8: {                            // var += f32*(1/30)
    // {u8 mode, u8 idx, f32 scale}. OBSERVED (0x447438):
    // the operand is loaded with `fld dword` (f32, not integer) —
    // *varref(mode,idx) += scale * C(0x49b6f4).
    const std::uint8_t mode = r.u8(), idx = r.u8();
    const float sc = r.f32();
    if (!r.ok) { v.fail("varacc"); return; }
    if (float* slot = resolveVarRef(mode, idx, env, ctx))
      *slot += sc * 0.03333333507180214f;
    return;
  }
  case 0xa6: {                            // LOS-to-cmd2 link (0x442a11)
    // {linkage}. cond: a cmd-2 object is registered (0x540e60) AND
    // the z+5-lifted segment ctx.pos -> cmdObj60.pos stabs clear in
    // the current arena, and in the partner when attached while
    // 0x540d3c (carrierBusy) is clear.
    Linkage L;
    if (!readLinkage(r, L)) { v.fail("loslink"); return; }
    bool cond = false;
    if (env.rt != nullptr && env.currentArena != nullptr &&
        env.rt->cmdObj60 != nullptr) {
      const DynamicObject* t = env.rt->cmdObj60;
      const float a[3] = {obj.pos[0], obj.pos[1], obj.pos[2] + 5.0f};
      const float b[3] = {t->pos[0], t->pos[1], t->pos[2] + 5.0f};
      float hit[3];
      cond = collisionStab(env.currentArena->dyn.col, a, b, hit) ==
             nullptr;
      if (cond && env.rt->cs.carrierBusy == 0 &&
          env.rt->partner != nullptr) {
        cond = collisionStab(env.rt->partner->dyn.col, a, b, hit) ==
               nullptr;
      }
    }
    switch (L.mode) {
    case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
               break;
    case 0xfc: if (cond) v.doCall(L.a); break;
    case 0x0c: if (cond) v.doGoto(L.a); break;
    case 0xfd: if (cond) v.doReturn(); break;
    default: break;
    }
    return;
  }
  case 0x2b: {                            // cam-relative seek (0x439aef)
    const float fwd = r.f32(), lat = r.f32();
    if (!r.ok) { v.fail("seekcam"); return; }
    // Runs only while obj+0x60 == 0x540c48 (the current arena).
    if (env.rt != nullptr && env.currentArena != nullptr)
      objectOpSeekCamera(*env.rt, obj, env.currentArena->dyn, fwd, lat);
    return;
  }
  case 0xa7: {                            // flee cmd2 obj (0x442de1)
    const float dist = r.f32();
    if (!r.ok) { v.fail("seekaway"); return; }
    if (env.rt != nullptr)
      objectOpSeekAway(*env.rt, obj, dist);
    return;
  }
  case 0x4e: {                            // subtype set (0x448741)
    for (int i = 0; i < 3; ++i)
      obj.field120[i] = std::bit_cast<float>(r.u32());
    if (!r.ok) { v.fail("subtype4e"); return; }
    obj.field11e = 0x4e;
    obj.fieldEC = nullptr;                 // unbind path
    obj.field2a0 = 0; obj.field2a1 = 0;
    obj.field2a4 = 0.0f;
    obj.field2a8 = 0.0f; obj.field2ac = 0.0f;
    // FUN_00451ee8(obj) — the bind-time waypoint re-seek.
    if (obj.arena != nullptr)
      objectWaypointReseek(obj, *obj.arena);
    obj.col.flags14c &= 0xf7;              // +0x14c &= ~8
    return;
  }
  case 0xcd:                              // +0x21f = u8 (0x451151)
    obj.flag21f = r.u8();
    return;

    // ----- object field writes -----
    // ----- object field writes -----
    case 0x08: {                            // +0x4c yaw (MOVSX u16)
      std::int16_t v = static_cast<std::int16_t>(r.u16());
      obj.yawDeg = static_cast<float>(v);
      if (obj.yawDeg < 0.0f) obj.yawDeg += 360.0f;   // normalize to [0,360)
      return;
    }
    case 0x0b: obj.field11a = r.u8(); return;         // +0x11a
    case 0x49: obj.field11b = r.u8(); return;         // +0x11b
    case 0x10: {                            // +0x8 health (MOVZX u16)
      std::uint16_t hv = r.u16();
      obj.health = static_cast<int>(hv);
      obj.healthMirror2a2 = hv;                      // +0x2a2 = low16(+0x8)
      if (hv >= 0xfde8) {                            // +0x21f = 1 sentinel
        obj.flag21f = 1;
        return;
      }
      if (hv == 0) {                           // +0x8==0 -> FUN_004581a4
        // OBSERVED (0x439fbc): the health-zero arm calls the death
        // boundary and ends the pass. FUN_00458140: when +0x110 is
        // set the object keeps a deferred script — +0x11e=0, +0x8=0,
        // +0x22c=0, +0x148|=0x20, +0x110 -> +0x108 AND +0x230,
        // +0x110=0 (the death fragment runs from the next pass).
        // Otherwise FUN_00457cf4 destroys the record outright — the
        // same +0x6e teardown family the port models as
        // objectTeardownNow.
        if (obj.field110 != nullptr) {
          obj.field11e = 0;
          obj.field22c = 0.0f;
          obj.col.flags148 |= 0x20;
          obj.field108 = obj.field230 = obj.field110;
          obj.field110 = nullptr;
        } else if (env.rt != nullptr) {
          objectTeardownNow(*env.rt, obj);
        }
        v.done = true;
      }
      return;
    }
    case 0x6f: obj.spawnId =                    // +0x146 (u16 of u32)
        static_cast<std::uint16_t>(r.u32() & 0xffff); return;
    case 0x32: obj.field38 = resolveVar(r, env, ctx); return;  // +0x38
    case 0x33: obj.field3c = resolveVar(r, env, ctx); return;  // +0x3c
    case 0x34: obj.field40 = resolveVar(r, env, ctx); return;  // +0x40
    case 0x53: {                              // +0x58 scale
      std::uint8_t mode = r.u8();
      if (mode == 0xff) {                     // ramp form {u8,u32,u32}:
        (void)r.u8(); (void)r.u32(); (void)r.u32();   // per-frame ease of
        // +0x58 toward a target — a runtime behavior; no init script
        // uses it, so consume operands only (HYPOTHESIS: single step).
      } else {
        obj.col.scale = resolveVarMode(mode, r, env, ctx);
      }
      return;
    }
    case 0x54: obj.zBias = resolveVar(r, env, ctx); return;    // +0x5c
    case 0xc6: {                            // element-set declaration
      // OBSERVED (handler 0x4394d0, dispatch-table slot 0xc6):
      //   {str8 prefix, u8 digitOfs, u32 hpThresh, u32 extra}
      // +0x149 |= 0x20; +0x302 = prefix chars (the original stores a
      // char* into the bytecode stream — past the len byte, or at it
      // for the empty string which reads as ""); +0x306 = digitOfs
      // (dword-stored u8); +0x30a = extra; then all eight +0x31e[i]
      // and +0x30e[i] = low16(hpThresh). The only real user in
      // LEVEL3..8 is LEVEL3 HMO_1$XH1_DOOR {XH1_KEY, 7, 120, 0}.
      obj.col.flags149 |= 0x20;
      obj.homingPrefix = r.str();
      obj.homingDigitOfs = r.u8();
      const std::uint32_t hp = r.u32();
      obj.field30a = r.u32();
      if (!r.ok) { v.fail("elemset"); return; }
      const std::int16_t v = static_cast<std::int16_t>(hp & 0xffffu);
      obj.elemThresh.assign(8, v);
      obj.elemHp.assign(8, v);
      return;
    }
    case 0xc7: obj.field104 = resolveVar(r, env, ctx); return; // +0x104
    case 0x4c: {                              // +0x110 image ref (0->null)
      std::uint32_t off = r.u32();
      obj.field110 = (off == 0) ? nullptr : imageRef(off, 4);
      return;
    }
    case 0x75: {                              // +0x148 &= ~u32
      // OBSERVED (handler 0x44d025): reads a u32 mask and ANDs the
      // complement into the +0x148 flag dword. (An earlier draft
      // mislabeled this slot as the +0x118 writer — that handler is
      // op 0x76.)
      std::uint32_t v = r.u32();
      std::uint32_t nv = ~v;
      obj.col.flags148 = static_cast<std::uint16_t>(
          obj.col.flags148 & (nv & 0xffffu));
      obj.col.flags149 = static_cast<std::uint8_t>(obj.col.flags148 >> 8);
      obj.col.flags14a &= static_cast<std::uint8_t>((nv >> 16) & 0xff);
      return;                                  // +0x14b byte unmodelled
    }
    case 0x76:                                // +0x118 anim target word
      // OBSERVED (handler 0x43976b): {u32 slot, low u16 used} — writes
      // +0x118 = (i16)low16 - 1. Shared anim-status word: pre-loads
      // the frame the anim player (FUN_004555bc) runs toward before
      // latching done.
      obj.animLatch = static_cast<std::int16_t>(
          static_cast<std::int16_t>(r.u32() & 0xffff) - 1);
      return;
    case 0x03: case 0x3b: {                   // bind +0x114 anim record
      // OBSERVED (handlers 0x4395bb/0x439663): {u32 imgref} resolved
      // through FUN_00438898 (lazy image ref). On rebind — or when the
      // done latch is set — resets +0xe4=0xffff, +0xdc=-1.0f,
      // +0x118=0xffff; then 0x03 clears +0x148 bit3 (one-shot) and
      // 0x3b sets it (loop).
      std::uint32_t off = r.u32();
      const void* rec = animRecResolve(imageRef(off, 12));
      if (!r.ok) { v.fail("animbind"); return; }
      if (obj.animRec != rec ||
          static_cast<std::uint16_t>(obj.animLatch) == 0xff00u) {
        obj.animRec = rec;
        obj.animFrame = -1;                    // +0xe4 = 0xffff
        obj.animAcc = -1.0f;                   // +0xdc = -1.0
        obj.animLatch = -1;                    // +0x118 = 0xffff
      }
      if (op == 0x03) obj.col.flags148 &= ~0x8u;
      else            obj.col.flags148 |= 0x8u;
      return;
    }
    case 0x18: {                              // anim sound marker
      // OBSERVED (handler 0x43a01f): {u8 mark, str name} ->
      // +0x144 = mark-1, +0x140 = name pointer. FUN_004555bc emits
      // the sound once when +0xdc crosses the mark (seam) — we keep
      // the text + marker so the consume state stays faithful.
      obj.animSoundMark = static_cast<std::int16_t>(r.u8() - 1);
      obj.animSoundName = r.str();
      if (!r.ok) { v.fail("animsnd"); return; }
      return;
    }
    case 0x41: {                              // setVar {mode,idx,u32}
      std::uint8_t mode = r.u8(), idx = r.u8();
      std::uint32_t bits = r.u32();
      if (!r.ok) { v.fail("setVar"); return; }
      if (float* slot = resolveVarRef(mode, idx, env, ctx)) {
        float f; std::memcpy(&f, &bits, 4); *slot = f;
      }
      return;
    }

    // ----- +0x148/+0x149/+0x14a flag ops (byte-addressed) -----
    case 0x23: {                              // +0x148 bit2
      if (r.u8()) obj.col.flags148 |= 0x4; else obj.col.flags148 &= ~0x4u;
      return;
    }
    case 0x24: {                              // +0x148 bit1
      if (r.u8()) obj.col.flags148 |= 0x2; else obj.col.flags148 &= ~0x2u;
      return;
    }
    case 0x3f: {                              // +0x148 bit4 INVERTED
      if (r.u8()) obj.col.flags148 &= ~0x10u; else obj.col.flags148 |= 0x10u;
      return;
    }
    case 0x61: {                              // +0x148 bit7 INVERTED
      if (r.u8()) { obj.col.flags148 &= ~0x80u; }
      else { obj.col.flags148 |= 0x80u; obj.pitchDeg = 0.0f; }  // +0x54=0
      return;
    }
    case 0x29: {                              // +0x149 bit0 / +0x14a bit7
      std::uint8_t v = r.u8();
      if (v) { obj.col.flags148 |= 0x100u; obj.col.flags149 |= 0x1; }
      else   { obj.col.flags148 &= ~0x100u; obj.col.flags149 &= ~0x1; }
      if (v == 2) obj.col.flags14a |= 0x80; else obj.col.flags14a &= ~0x80;
      // +0x149 bit0 clear while ridden -> FUN_00461878 dismount (seam).
      return;
    }
    case 0x74: {                              // +0x148 dword |= operand
      std::uint32_t v = r.u32();
      obj.col.flags148 = static_cast<std::uint16_t>(
          obj.col.flags148 | (v & 0xffffu));
      obj.col.flags149 = static_cast<std::uint8_t>(obj.col.flags148 >> 8);
      obj.col.flags14a |= static_cast<std::uint8_t>((v >> 16) & 0xff);
      return;                                  // +0x14b byte unmodelled
    }

    // ----- element-name mask -> +0x2c8 -----
    case 0x1f: {                              // {u8 count, count×str}
      std::uint8_t n = r.u8();
      for (std::uint8_t k = 0; k < n; ++k) {
        std::string s = r.str();
        if (!r.ok) return;
        for (int e = 0; e < obj.elemSet.count; ++e) {
          const std::string en = obj.model.elemName(e);
          if (en == s || s == "ALL")            // FUN_0042fa50 match /
            obj.col.elemMaskB |= (1u << (e & 31)); //  "ALL" wildcard
        }
      }
      if (!r.ok) { v.fail("elemmask"); return; }
      return;
    }

    // ----- element-name unmask -> clear +0x2c8 -----
    case 0x20: {                              // {u8 count, count×str}
      // OBSERVED (handler 0x43fd39): for each lstr, scan the model's
      // element records (+0x20 array, +0x1c count, 0x5c stride, name at
      // +0). On strcmp(elem.name, s)==0 or strcmp(s,"ALL")==0:
      //   if (+0x2cc & (1<<j)) -> skip (latched)
      //   else +0x2c8 &= ~(1<<j)   (re-enable the element)
      std::uint8_t n = r.u8();
      for (std::uint8_t k = 0; k < n; ++k) {
        std::string s = r.str();
        if (!r.ok) return;
        for (int e = 0; e < obj.elemSet.count; ++e) {
          const std::string en = obj.model.elemName(e);
          if (en == s || s == "ALL") {
            const std::uint32_t bit = 1u << (e & 31);
            if ((obj.col.elemMaskLatch & bit) == 0)
              obj.col.elemMaskB &= ~bit;
          }
        }
      }
      if (!r.ok) { v.fail("elemunmask"); return; }
      return;
    }

    // ----- velocity-compare linkage -----
    case 0x26: {                              // {u8 sel, f32 a, [f32 b], link}
      // OBSERVED (handler 0x4414dd -> comparator FUN_0045ad40):
      //   x = ctx+0x30 (vel.z). sel: 1 = x<a, 2 = x>a,
      //   3 = x-0.05>a, 4 = x+0.05<a, 5 = |x-a|<0.05,
      //   6 = |x-a|>=0.05, 7 = a<=x<=b, 8 = x<=a || x>=b.
      //   result != 0 -> linkage (fc/fe=call, fd=ret, else=goto).
      const std::uint8_t sel = r.u8();
      const float a = r.f32();
      float b = 0.0f;
      if (sel == 7 || sel == 8) b = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("velcmp"); return; }
      const float x = obj.field30;
      bool hit = false;
      switch (sel) {
        case 1: hit = x < a; break;
        case 2: hit = x > a; break;
        case 3: hit = x - 0.05f > a; break;
        case 4: hit = x + 0.05f < a; break;
        case 5: hit = std::fabs(x - a) < 0.05f; break;
        case 6: hit = std::fabs(x - a) >= 0.05f; break;
        case 7: hit = x >= a && x <= b; break;
        case 8: hit = x <= a || x >= b; break;
        default: break;
      }
      if (hit) {
        switch (L.mode) {
        case 0xfe: case 0xfc: v.doCall(L.a); break;
        case 0xfd: v.doReturn(); break;
        default: v.doGoto(L.a); break;
        }
      } else if (L.mode == 0xfe && L.b != 0) {
        v.doCall(L.b);                        // 0x441764 — else-call
      }
      return;
    }

    // ----- refpoint escape + LoS linkage -----
    case 0xf3: {                              // {u8 ref, u16 val, linkage}
      // OBSERVED (handler 0x451aef -> FUN_0045d7c8): the linkage fires
      // when the object's world refpoint[idx] is FARTHER than `val`
      // from 0x540bfc (the chase anchor — cs.pos) AND the segment
      // cs.pos+(0,0,5) -> refpoint crosses no non-skip polygon in the
      // current arena's space (0x540c48) nor the partner world
      // (0x540ca4, skipped when 0x540ca4==0 or 0x540d3c==0 — the
      // carrierBusy gate coneLosTest already models).
      const std::uint8_t idx = r.u8();
      const float val = static_cast<float>(r.u16());
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("refesc"); return; }
      bool hit = false;
      if (env.rt != nullptr && env.currentArena != nullptr && idx < 8) {
        const float* pp = env.rt->cs.pos;
        const float* rp = obj.worldRef[idx];
        const float dx = pp[0] - rp[0], dy = pp[1] - rp[1],
                    dz = pp[2] - rp[2];
        if (dx * dx + dy * dy + dz * dz > val * val) {
          const float a[3] = {pp[0], pp[1], pp[2] + 5.0f};
          float hp[3];
          hit = collisionStab(env.currentArena->dyn.col, a, rp, hp) ==
                nullptr;
          if (hit && env.rt->cs.carrierBusy == 0 &&
              env.rt->partner != nullptr) {
            hit = collisionStab(env.rt->partner->dyn.col, a, rp, hp) ==
                  nullptr;
          }
        }
      }
      if (hit) {
        switch (L.mode) {
        case 0xfe: case 0xfc: v.doCall(L.a); break;
        case 0xfd: v.doReturn(); break;
        default: v.doGoto(L.a); break;
        }
      } else if (L.mode == 0xfe && L.b != 0) {
        v.doCall(L.b);
      }
      return;
    }

    // ----- connector config family -----
    case 0x62: {                              // surf-flag {u8 bit, u8 subop}
      // OBSERVED (handler 0x44ba80): FUN_0040a704(obj->+0x28,
      //   obj->+0x10, bit, subop) tags the element whose +0x20 id-byte
      //   matches (seam — no element flag array yet); then the +0x10c
      //   and +0x110 mask registers are armed per subop:
      //     0 = set both, 1 = clear both, 2/3 = +0x110 set/clear,
      //     4/5 = +0x10c set/clear, >5 = clear both.
      //   +0x10c/+0x110 are shared dwords in the original (mode-7
      //   remote PC / 0x4c deathRef vs these masks) — bit ops apply
      //   to the stored value verbatim.
      const std::uint32_t m = 1u << (r.u8() & 0x1f);
      const std::uint8_t so = r.u8();
      if (!r.ok) { v.fail("surfFlag"); return; }
      ++env.rt->seams.surfFlagElemTags;
      auto orM = [](const void*& p, std::uint32_t bits) {
        p = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(p) | bits);
      };
      auto andM = [](const void*& p, std::uint32_t bits) {
        p = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(p) & ~bits);
      };
      switch (so) {
        case 0: orM(obj.field10c, m);  orM(obj.field110, m);  break;
        case 2: orM(obj.field110, m);  break;
        case 3: andM(obj.field110, m); break;
        case 4: orM(obj.field10c, m);  break;
        case 5: andM(obj.field10c, m); break;
        default: andM(obj.field10c, m); andM(obj.field110, m); break;
      }
      return;
    }
    case 0x96: {                              // {u32,u32}->+0x306/+0x30a
      std::uint32_t a = r.u32(), b = r.u32();
      obj.animRecNear = animRecResolve(imageRef(a, 12)); // +0x306 (open anim)
      obj.animRecFar  = animRecResolve(imageRef(b, 12)); // +0x30a (close anim)
      // OBSERVED (handler 0x44a652): both operands pass through
      // FUN_00438898 — first-dword-0 stubs resolve through the
      // region-A array-A name table (animRecResolve above).
      return;
    }
    case 0x97: {                              // {4 strs}->+0x316..+0x322
      std::string s[4];
      for (auto& x : s) { x = r.str(); if (x == "NONE") x.clear(); }
      // OBSERVED field order: op0->+0x31a op1->+0x322 op2->+0x316
      // op3->+0x31e (scrambled). "NONE" sentinel clears the slot.
      obj.connSound31a = s[0]; obj.connSound322 = s[1];
      obj.connSound316 = s[2]; obj.connSound31e = s[3];
      return;
    }
    case 0x98: {                              // +0x312 hi nibble
      std::uint32_t v = r.u32();
      obj.connState = static_cast<std::uint8_t>(
          (obj.connState & 0x0f) | (v & 0xf0));
      return;
    }
    case 0x99: obj.connRadius = r.f32(); return; // +0x30e
    case 0x9a: {                            // anim-frame wait (0x43db7b)
      // {i16 v}. OBSERVED: +0x108 = this insn — the pass suspends and
      // the op re-tests each tick. Proceeds when: the anim is done
      // (+0x114 != 0 && animLatch == 0xff00), or v>=0 && animFrame>=v,
      // or v==-1 && (animFrame==animLatch || animLatch<0). v<=-2
      // blocks indefinitely. The halt jumps straight to the 0x451e83
      // epilogue — SKIPPING the 0x451e79 +0x21e clear — so a pending
      // event mark survives the wait.
      obj.field108 = v.ptrAt(insnOff);
      const std::int16_t val = static_cast<std::int16_t>(r.u16());
      bool proceed;
      if (obj.animRec != nullptr &&
          static_cast<std::uint16_t>(obj.animLatch) == 0xff00u) {
        proceed = true;
      } else if (val >= 0) {
        proceed = obj.animFrame >= val;
      } else if (val == -1) {
        proceed = (obj.animFrame == obj.animLatch) || (obj.animLatch < 0);
      } else {
        proceed = false;
      }
      if (!proceed) { res.halted = true; v.done = true; }
      return;
    }

    // ----- late-census conditional linkage family -----
    case 0x12: {                              // timed link (0x43df44)
      // {f32 wait, linkage}. OBSERVED: while mark[depth] <
      // wait*30.0 (C 0x497a04 = 30.0f) the handler adds DAT_0049b6e8
      // (the u16 frame delta — frameStep here) to mark[depth] and
      // applies the else arm (0xfe calls linkB, others fall through).
      // Once mark[depth] >= wait*30.0 it clears mark[depth] and the
      // linkage dispatches.
      const float wait = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("timelink"); return; }
      const bool cond =
          static_cast<float>(obj.scriptMark[obj.scriptCallDepth]) >=
          wait * 30.0f;
      if (cond) {
        obj.scriptMark[obj.scriptCallDepth] = 0;
      } else {
        obj.scriptMark[obj.scriptCallDepth] =
            static_cast<std::uint16_t>(
                obj.scriptMark[obj.scriptCallDepth] +
                env.frameStep);
      }
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x21: {                              // path-frame link (0x43dc44)
      // {u32 n, linkage}. OBSERVED: while +0xec (path record) is
      // bound and (n-1) > FRNDINT(+0xf0) — the path frame truncated
      // toward zero via FUN_0047d59a — the else arm applies
      // (0xfe calls linkB, others fall through). Otherwise the
      // linkage dispatches.
      const std::uint32_t n = r.u32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("pathlink"); return; }
      const bool cond = obj.fieldEC == nullptr ||
          static_cast<std::int64_t>(n) - 1 <=
              static_cast<std::int64_t>(std::trunc(obj.fieldF0));
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x11: {                              // anim-done link (0x43d594)
      // {linkage}. OBSERVED: fires when +0x114 == 0 OR
      // (s16)+0x118 == 0xff00 — exactly animDone().
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("animlink"); return; }
      const bool cond = obj.animDone();
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x2c: {                              // no-subtype link (0x43c312)
      // {linkage}. OBSERVED: fires when +0x11e == 0.
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("subtlink"); return; }
      const bool cond = (obj.field11e == 0);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x2f: {                              // probability link (0x43e606)
      // {f32 prob, linkage}. OBSERVED: fires when
      // FUN_00401ed4(10000) < prob*100 — percent chance; the rand
      // draw is unconditional.
      const float prob = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("problink"); return; }
      bool cond = false;
      if (env.rt != nullptr) {
        cond = static_cast<float>(
                   enemyRandBelow(env.rt->rngState, 10000)) <
               prob * 100.0f;
      }
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x30: {                              // period link (0x43e8fe)
      // {f32 period, linkage}. OBSERVED: fires when
      // FUN_00401ed4(10000) < (1/30)/period * 10000 — i.e. `period` is
      // the mean seconds between events (per-frame chance
      // 1/(30*period)); the rand draw is unconditional.
      const float period = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("periodlink"); return; }
      bool cond = false;
      if (env.rt != nullptr) {
        cond = static_cast<float>(
                   enemyRandBelow(env.rt->rngState, 10000)) <
               10000.0f / (30.0f * period);
      }
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x77: {                              // model-count link (0x44d04a)
      // {lstr name, u8 kind, f32 a, [f32 b if kind==7|8], linkage}.
      // OBSERVED: counts objects on the ctx's home-arena +0x68 list
      // whose +0x6 (named) byte is set, +0xc name FUN_0042fa50-matches
      // the operand (case-sensitive) and +0x8 (health) > 0, then
      // FUN_0045ad40 on the count.
      const std::string nm = r.str();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("cntlink"); return; }
      int count = 0;
      DynamicArena* home = obj.arena;
      if (home != nullptr) {
        for (auto& up : home->storage) {
          DynamicObject& o = *up;
          if (!o.col.named || o.arena != home) continue;    // +0x06
          if (objectEnemyName(env, o) != nm) continue;      // +0x0c
          if (o.health <= 0) continue;                      // +0x08 JG
          ++count;
        }
      }
      const bool cond = cmpOp5ad40(kind, static_cast<float>(count),
                                   va, vb);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x7b: {                              // ifPartner {link} (0x44d8af)
      // OBSERVED: fires when ctx->+0x60 != [0x540c48] — the executing
      // object's bound arena differs from the current arena (i.e. it
      // is attached to the partner arena).
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("ifPartner"); return; }
      const DynamicArena* bound =
          (env.currentArena != nullptr) ? &env.currentArena->dyn
                                        : nullptr;
      const bool cond = (obj.arena != bound);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0xde: {                              // insn-count link (0x44f842)
      // {u8 kind, f32 a, [f32 b if kind==7|8], linkage}. OBSERVED:
      // FUN_0045ad40(kind, fild(passInsnCounter), a, b) — the
      // interpreter's per-invocation instruction counter (its
      // loop-guard local), compared as f32. Scripts use it as a
      // watchdog: a busy block bails via the linkage once the pass
      // exceeds the operand (LEVEL8 GUNT_10$XG +223c6: >200).
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("insnlink"); return; }
      const bool cond = cmpOp5ad40(kind,
          static_cast<float>(res.instructions), va, vb);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x71: {                              // spawn at rotated offset
      // {f32 a, f32 b, f32 c, lstr name, u32 scOff} (0x44a205).
      // OBSERVED: pos = (x - a*cos - b*sin, y - b*cos - a*sin, z + c)
      // rotated by ctx +0x4c (FUN_00437f98), then the FUN_00454af8
      // spawn tail — the flank-spawn used for boss helper objects.
      const float a = r.f32(), b = r.f32(), c = r.f32();
      const std::string nm = r.str();
      const std::uint32_t scOff = r.u32();
      if (!r.ok) { v.fail("spawnOfs"); return; }
      float sn = 0.0f, cs = 0.0f;
      sincosDeg(obj.yawDeg, &sn, &cs);
      traversalScriptSpawn(env,
                           obj.pos[0] - a * cs - b * sn,
                           obj.pos[1] - b * cs - a * sn,
                           obj.pos[2] + c,
                           0.0f, 0, nm, "", scOff, 2);
      return;
    }
    case 0xa1: {                              // spawnNamed (0x4497f8)
      // {f32 x,y,z, lstr name, u32 scOff}. OBSERVED: absolute position;
      // the SW_EWJ/BONEFLC names route to the scaled FUN_004549b4
      // spawn arm in the original (not yet modelled — plain path).
      const float x = r.f32(), y = r.f32(), z = r.f32();
      const std::string nm = r.str();
      const std::uint32_t scOff = r.u32();
      if (!r.ok) { v.fail("spawnNamed"); return; }
      traversalScriptSpawn(env, x, y, z, 0.0f, 0, nm, "", scOff, 2);
      return;
    }
    case 0xe6: {                              // spawn3 (0x44a0a9)
      // {f32 x,y,z, f32 +0x4c, u32 spawnId, lstr class, u32 scOff}.
      // OBSERVED: same handler as the arena VM's case — operand 4
      // lands raw in the spawn's +0x4c; operand 5 <0 resolves to the
      // class-instance count +1 (FUN_00454810).
      const float x = r.f32(), y = r.f32(), z = r.f32();
      const float yaw = r.f32();
      const std::uint32_t spawnId = r.u32();
      const std::string cls = r.str();
      const std::uint32_t scOff = r.u32();
      if (!r.ok) { v.fail("spawn3"); return; }
      traversalScriptSpawn(env, x, y, z, yaw, spawnId, cls, "", scOff,
                           3);
      return;
    }
    case 0xd3: {                              // zero velocity (0x45073e)
      // OBSERVED: +0x30 = +0x2c = +0x28 = 0 — clears the velocity
      // vector in place (no operands).
      obj.field30 = 0.0f;
      obj.field2c = 0.0f;
      obj.field28 = 0.0f;
      return;
    }
    case 0x50: {                              // rotated velocity add
      // {f32 a, f32 b, f32 c} (0x4489f6). OBSERVED: same FUN_00437f98
      // yaw-rotate as op 0x71, applied to the velocity vector —
      // +0x28 += -(a*cos + b*sin), +0x2c += -(b*cos + a*sin),
      // +0x30 += c. The XG chute's deploy impulse.
      const float a = r.f32(), b = r.f32(), c = r.f32();
      if (!r.ok) { v.fail("velrot"); return; }
      float sn = 0.0f, cs = 0.0f;
      sincosDeg(obj.yawDeg, &sn, &cs);
      obj.field28 += -(a * cs + b * sn);
      obj.field2c += -(b * cs + a * sn);
      obj.field30 += c;
      return;
    }
    case 0x7c: {                              // +0x100 write (0x43916a)
      // {f32} — face-travel yaw bias.
      const float v100 = r.f32();
      if (!r.ok) { v.fail("f100"); return; }
      obj.field100 = v100;
      return;
    }
    case 0x4f: {                              // set pos (0x4489a0)
      // {f32,f32,f32} — raw +0x10/+0x14/+0x18 stores (no baseZ
      // mirror — the original writes only the pos triple).
      const float x = r.f32(), y = r.f32(), z = r.f32();
      if (!r.ok) { v.fail("setpos"); return; }
      obj.pos[0] = x; obj.pos[1] = y; obj.pos[2] = z;
      return;
    }
    case 0x72: {                              // ride-mount link (0x44c4a4)
      // {linkage}. cond: ctx == 0x540dc0 — the object the player is
      // currently standing on (cs.rideObj).
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("ridelink"); return; }
      const bool cond =
          env.rt != nullptr && env.rt->cs.rideObj == &obj.col;
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x7d:                                // clear event stack (0x43bcaa)
      // No operands. OBSERVED: +0x248 = 0 — drops every pending
      // event-call frame.
      obj.scriptCallDepth = 0;
      return;
    case 0x4b:                                // subtype clear (0x4485ea)
      // No operands. OBSERVED: +0x11e = 0.
      obj.field11e = 0;
      return;
    case 0x58: {                              // speed-ramp arm (0x44a801)
      // {u8 mode}. OBSERVED: mode!=2 -> +0x11e=0x58, +0x11f=mode;
      // mode==2 -> +0x11e=0, +0x34=0. +0xec=0 either way (path
      // unbound — the speed-ramp subtype owns movement).
      const std::uint8_t rampMode = r.u8();
      if (!r.ok) { v.fail("speedramp"); return; }
      if (rampMode != 2) {
        obj.field11e = 0x58;
        obj.field11f = rampMode;
      } else {
        obj.field11e = 0;
        obj.field34 = 0.0f;
      }
      obj.fieldEC = nullptr;
      return;
    }
    case 0x0f:                                // subtype 0xf (0x439f2b)
      // No operands. OBSERVED: +0x11e = 0xf.
      obj.field11e = 0x0f;
      return;
    case 0xf9: {                              // seam-child link (0x43a4fe)
      // {u8 mode, [lstr name if mode==1], linkage}. OBSERVED:
      //   mode 0      — cond = +0x158 != 0 (seam child exists)
      //   mode 1      — +0x158 != 0 AND FUN_0042fa50(+0x15c, name)
      //   mode >=2    — cond = +0x158 == 0 (no seam child)
      const std::uint8_t mode = r.u8();
      std::string nm;
      if (mode == 1) nm = r.str();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("childlink"); return; }
      bool cond;
      if (mode == 0)      cond = (obj.field158 != nullptr);
      else if (mode == 1) cond = (obj.field158 != nullptr &&
                                  obj.field15c == nm);
      else                cond = (obj.field158 == nullptr);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0xb7: {                              // computed call (0x447754)
      // {u8 mode, u8 idx, u8 n, u32 target[n]}. OBSERVED: iv =
      // trunc(*FUN_00438654(mode,idx)); the table is always consumed;
      // when 0 <= iv < n the handler dispatches targets[iv] as a 0xfc
      // call (push +0x248 stack, pc=target, +0x26c[depth]=0).
      const std::uint8_t mode = r.u8(), idx = r.u8(), n = r.u8();
      if (!r.ok) { v.fail("swlink"); return; }
      int iv = -1;
      if (const float* slot = resolveVarRef(mode, idx, env, ctx))
        iv = static_cast<int>(*slot);         // trunc toward zero
      std::uint32_t target = 0;
      bool hit = false;
      for (int i = 0; i < n; ++i) {
        const std::uint32_t o = r.u32();
        if (i == iv) { target = o; hit = (o != 0); }
      }
      if (!r.ok) { v.fail("swlink tbl"); return; }
      if (hit) v.doCall(target);
      return;
    }
    case 0xbe: {                              // recEnable {u8 flag, lstr}
      // OBSERVED (0x44ace6): same FUN_00413354 named-record op as the
      // arena VM — set/clear bit0 of the record's +0x1c queryMask.
      const std::uint8_t flag = r.u8();
      const std::string nm = r.str();
      if (!r.ok) { v.fail("recEnable"); return; }
      if (env.selfArena) {
        for (SurfaceRecord* rec = env.selfArena->surface.records; rec;
             rec = rec->next) {
          if (stricmpEq(rec->nameText, nm)) {
            if (flag) rec->queryMask |= 1u;
            else rec->queryMask &= ~1u;
            break;
          }
        }
      }
      return;
    }
    case 0xdc: {                              // spawn-x dedupe (0x44fb7f)
      // {f32 lo, f32 hi, f32 aux}. OBSERVED (FUN_0045e1b0): pick x in
      // [lo,hi) biased by player position, then dedupe against same-y
      // objects (|dx|<11.5 -> x = obj.x - 12.0, wrap into [lo,hi)) and
      // write +0x10. NOTE (quirk): the original's first rand bound is
      // a STALE EDX — the register holds whatever the interpreter
      // left; effectively garbage. The port uses bound 100 so the
      // OBSERVED <25/<50 thresholds read as ~25%/25%/50% branch
      // weights (HYPOTHESIS).
      const float lo = r.f32(), hi = r.f32(), aux = r.f32();
      if (!r.ok) { v.fail("spawnx"); return; }
      if (env.rt == nullptr || obj.arena == nullptr) return;
      const float px = env.rt->cs.pos[0];       // 0x540bfc
      const float pz = env.rt->cs.pos[2];       // 0x540c00
      const float ex = env.rt->cs.entryPos[0];  // 0x540c08
      const int roll = enemyRandBelow(env.rt->rngState, 100);
      float xv;
      if (roll < 25 || px < lo || hi < px || aux < pz) {
        xv = lo + (hi - lo) * static_cast<float>(
            enemyRandBelow(env.rt->rngState, roll)) * 1.0e-4f;
      } else if (roll < 50) {
        xv = px;
      } else {
        xv = px + ((px - ex) / 0.03333333507180214f) * 2.6733334f;
      }
      // Dedupe: shift left 12 while a same-y object sits within 11.5.
      for (;;) {
        if (xv < lo) xv += hi - lo;
        DynamicObject* hit = nullptr;
        for (auto& up : obj.arena->storage) {
          DynamicObject& o = *up;
          if (!o.col.named || &o == &obj) continue;
          if (o.pos[1] == obj.pos[1] &&
              std::fabs(xv - o.pos[0]) < 11.5f) { hit = &o; break; }
        }
        if (hit == nullptr) break;
        xv = hit->pos[0] + -12.0f;
      }
      obj.pos[0] = xv;                          // +0x10 direct write
      return;
    }
    case 0x2d: {                              // camera-dist link (0x443e56)
      // {u8 kind, f32 a, [f32 b if kind==7|8], linkage}. OBSERVED:
      // dist = 3D |pos - 0x54c6c4| (FUN_00430160) then FUN_0045ad40.
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("camdist"); return; }
      bool cond = false;
      if (env.rt != nullptr) {
        const float* cam = env.rt->camera.pose.pos;
        const float dx = obj.pos[0] - cam[0];
        const float dy = obj.pos[1] - cam[1];
        const float dz = obj.pos[2] - cam[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        cond = cmpOp5ad40(kind, dist, va, vb);
      }
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x42: {                              // var += operand (0x4473d4)
      // {u8 mode, u8 idx, f32}. OBSERVED: *FUN_00438654(mode,idx) +=
      // operand — the unscaled sibling of 0xd8.
      const std::uint8_t mode = r.u8(), idx = r.u8();
      const float operand = r.f32();
      if (!r.ok) { v.fail("varadd"); return; }
      if (float* slot = resolveVarRef(mode, idx, env, ctx))
        *slot += operand;
      return;
    }
    case 0x43: {                              // var cmp link (0x447a1d)
      // {u8 mode, u8 idx, u8 kind, f32 a, [f32 b if kind==7|8],
      //  linkage}. OBSERVED: *FUN_00438654(mode,idx) compared via
      // FUN_0045ad40.
      const std::uint8_t mode = r.u8();
      const std::uint8_t idx = r.u8();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("varcmp"); return; }
      const float* slot = resolveVarRef(mode, idx, env, ctx);
      const bool cond =
          slot != nullptr && cmpOp5ad40(kind, *slot, va, vb);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0xae: {                            // ammo-count link (0x440dd0)
      // {u8 idx, u8 kind, f32 a, [f32 b if kind==7|8], linkage}.
      // OBSERVED: FILD of 0x54161f[idx*4] (the ammo block) then
      // FUN_0045ad40 — e.g. "ammo[i] < x". Same handler as the arena
      // VM's case; object scripts reach it via weapon branches.
      const std::uint8_t idx = r.u8();
      const std::uint8_t kind = r.u8();
      const float va = r.f32();
      float vb = 0.0f;
      if (kind == 7 || kind == 8) vb = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("ammolink"); return; }
      float cnt = 0.0f;
      if (env.rt != nullptr && idx < env.rt->ammo.size())
        cnt = static_cast<float>(env.rt->ammo[idx]);
      const bool cond = cmpOp5ad40(kind, cnt, va, vb);
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x6c: {                              // player-touch link (0x443924)
      // {linkage}. OBSERVED: subtype 0x3d -> cond = +0x14c&4; else
      // copy +0x198 AABB, re-scale about its centre by +0x2c0
      // (FUN_0045c138), must overlap the player box (FUN_0045ce58)
      // AND an unmasked element's world AABB must overlap it
      // (FUN_0045c1b8; +0x2c8 bit SET skips the element).
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("touchlink"); return; }
      bool cond = false;
      if (obj.field11e == 0x3d) {
        cond = (obj.col.flags14c & 0x4) != 0;
      } else if (env.rt != nullptr) {
        float box[6];
        std::memcpy(box, obj.col.aabb, sizeof box);
        for (int i = 0; i < 3; ++i) {
          const float half =
              (box[3 + i] - box[i]) * 0.5f * obj.field2c0;
          const float mid = (box[3 + i] + box[i]) * 0.5f;
          box[i] = mid - half;
          box[3 + i] = mid + half;
        }
        if (aabbOverlapLocal(box, env.rt->cs.playerBox)) {
          for (std::size_t e = 0; e < obj.model.elems.size(); ++e) {
            if ((obj.col.elemMaskB & (1u << (e & 31))) != 0) continue;
            if (aabbOverlapLocal(env.rt->cs.playerBox,
                                 obj.model.elems[e].aabb)) {
              cond = true;
              break;
            }
          }
        }
      }
      switch (L.mode) {
      case 0xfe: if (cond) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (cond) v.doCall(L.a); break;
      case 0x0c: if (cond) v.doGoto(L.a); break;
      case 0xfd: if (cond) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0xcf: {                              // yaw morph link (0x450d75)
      // {f32 rate, f32 target, linkage | 0xff u8}. OBSERVED:
      // +0x4c = FUN_0045dc18(target, +0x4c, rate*(1/30)); arrived
      // (+0x4c==target) -> linkage, not-arrived + 0xfe -> else call,
      // otherwise the +0x4c >= 360 wrap loop runs (the dead
      // positive +360 loop is not reproduced). 0xff escapes to a
      // flag byte; flag==0xff -> wrap only, no linkage.
      const float rate = r.f32();
      const float target = r.f32();
      Linkage L{};
      const std::uint8_t esc = r.u8();
      if (esc == 0xff) {
        const std::uint8_t flag = r.u8(); // no linkage follows
        (void)flag;   // flag!=0xff dispatches on the original's STALE
                      // linkage slot — unmodelled (treated as mode 0).
      } else {
        L.mode = esc;
        switch (L.mode) {                 // readLinkage tail
        case 0xfe: L.a = r.u32(); L.b = r.u32(); break;
        case 0xfc:
        case 0x0c: L.a = r.u32(); break;
        default: break;
        }
      }
      if (!r.ok) { v.fail("yawmorph"); return; }
      obj.yawDeg = approachAngle5dc18(target, obj.yawDeg,
                                      rate * (1.0f / 30.0f));
      const bool arrived = (obj.yawDeg == target);
      if (esc == 0xff || !arrived) {
        if (esc != 0xff && L.mode == 0xfe && L.b != 0) {
          v.doCall(L.b);                  // not-arrived else path
          return;
        }
        while (obj.yawDeg >= 360.0f) obj.yawDeg += -360.0f;
        return;
      }
      switch (L.mode) {
      case 0xfe:
      case 0xfc: v.doCall(L.a); break;
      case 0x0c: v.doGoto(L.a); break;
      case 0xfd: v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0x3d: {                            // model spawn (0x4424b3)
      // {u8 mode, [u8 refIdx | lstr refName], lstr model, u32 pc}.
      // OBSERVED: mode==0 -> spawn pos = ctx worldRef[refIdx]
      // (+0x1b0 + idx*0xc); mode!=0 -> FUN_0045db60 named-element
      // world centroid (default = pos when no element matches).
      // Model resolved by scanning the level model table by name
      // (0x4edcc0 records, FUN_0042fa50 match; no match = fatal).
      // Spawn (FUN_0045cffc): home-arena alloc + FUN_00403720 model
      // copy + FUN_004566f0 init, then +0x11e=0x3d, +0x148|=0x80820,
      // +0x4c/+0x13c inherited, +0x108 = pc operand.
      const std::uint8_t smode = r.u8();
      std::uint8_t refIdx = 0;
      std::string refName;
      if (smode == 0) refIdx = r.u8();
      else refName = r.str();
      const std::string modelName = r.str();
      const std::uint32_t pc = r.u32();
      if (!r.ok) { v.fail("spawn-args"); return; }
      if (env.rt == nullptr || obj.arena == nullptr) {
        v.fail("spawn-env");
        return;
      }
      {
        // OBSERVED: the original scans the 0x4edcc0 model table by
        // C-string (FUN_0042fa50); the port resolves the index through
        // level.enemies then lazy-loads via env.modelFor.
        const int midx = env.rt->level.enemies.indexOf(modelName);
        const RuntimeModel* model =
            (midx >= 0 && env.modelFor)
                ? env.modelFor(midx, env.modelCtx)
                : nullptr;
        if (model == nullptr) { v.fail("spawn-model"); return; }
        float pos[3] = {obj.pos[0], obj.pos[1], obj.pos[2]};
        if (smode == 0) {
          // OBSERVED: raw +0x1b0 + idx*0xc read (no bound check);
          // the port clamps to the 8-entry refpoint block.
          if (refIdx < 8) {
            pos[0] = obj.worldRef[refIdx][0];
            pos[1] = obj.worldRef[refIdx][1];
            pos[2] = obj.worldRef[refIdx][2];
          }
        } else {
          // FUN_0045db60 — named-element world-space vertex centroid.
          for (std::size_t e = 0; e < obj.model.elems.size(); ++e) {
            if (obj.model.elemName(e) != refName) continue;
            const auto& vv = obj.model.elemVerts[e];
            float c[3] = {0, 0, 0};
            const std::size_t nv = vv.size() / 3;
            for (std::size_t k = 0; k < nv; ++k) {
              c[0] += vv[k * 3];
              c[1] += vv[k * 3 + 1];
              c[2] += vv[k * 3 + 2];
            }
            if (nv != 0) {
              c[0] /= static_cast<float>(nv);
              c[1] /= static_cast<float>(nv);
              c[2] /= static_cast<float>(nv);
            }
            transformPoint(obj.col.xform, obj.col.origin, c, pos);
            break;
          }
        }
        DynamicObject& o = obj.arena->allocFront();
        o.model = deepCopyModel(*model);            // FUN_00403720
        o.enemyIndex = static_cast<std::uint16_t>(midx);
        o.field1c[0] = pos[0];                      // +0x1c anchor
        o.field1c[1] = pos[1];
        o.field1c[2] = pos[2];
        o.setPosition(pos[0], pos[1], pos[2]);      // +0x10
        o.prevPos[0] = pos[0];                      // +0x180
        o.prevPos[1] = pos[1];
        o.prevPos[2] = pos[2];
        initObjectCollision(o);                     // FUN_004566f0
        o.field11e = 0x3d;                          // +0x11e
        o.col.flags148 = static_cast<std::uint16_t>(
            o.col.flags148 | 0x0820);               // +0x148 |= 0x80820
        o.col.flags149 =
            static_cast<std::uint8_t>(o.col.flags148 >> 8);
        o.col.flags14a |= 0x08;
        o.yawDeg = obj.yawDeg;                      // +0x4c inherit
        o.bankDeg = obj.bankDeg;                    // +0x13c inherit
        o.field108 = v.ptrAt(pc);                   // +0x108 script PC
      }
      return;
    }
    case 0x35: {                            // varop -> +0x34 (0x43ae5f)
      // {u8 mode, f32 | u8 idx}. OBSERVED: mode 3 reads an inline f32,
      // else idx resolves a FUN_00438654 slot; the value lands in
      // +0x34 (steer/projectile rate read by FUN_0045b6f8/subtype-0x3d).
      const std::uint8_t mode = r.u8();
      const float val = resolveVarMode(mode, r, env, ctx);
      if (!r.ok) { v.fail("rate34"); return; }
      obj.field34 = val;
      return;
    }
    case 0x5b: {                            // varop -> +0xe8 (0x43af9f)
      // {u8 mode, f32 | u8 idx}. OBSERVED: same FUN_00438654 varop
      // form as 0x35 — mode 3 reads an inline f32, else idx selects
      // a slot; the value lands in +0xe8.
      obj.fieldE8 = resolveVar(r, env, ctx);
      if (!r.ok) { v.fail("setFe8"); return; }
      return;
    }
    case 0xd2: {                            // varop -> +0x2c0 (0x44f54d)
      // {u8 mode, f32 | u8 idx}. OBSERVED: FUN_00438654 varop form;
      // writes +0x2c0 (element-box scale — init default 1.0).
      obj.field2c0 = resolveVar(r, env, ctx);
      if (!r.ok) { v.fail("setF2c0"); return; }
      return;
    }
    case 0xa9: {                            // varop -> +0x326 (0x4494ab)
      // {u8 mode, f32 | u8 idx}. OBSERVED: FUN_00438654 varop form;
      // the f32 bit pattern lands in +0x326 — the connMaskLock union
      // (consumed as f32 by enemy_runtime, as a LOCK elem mask by
      // the connect paths).
      obj.connMaskLock =
          std::bit_cast<std::uint32_t>(resolveVar(r, env, ctx));
      if (!r.ok) { v.fail("setF326"); return; }
      return;
    }
    case 0x5a: {                            // named-anim angle (0x44a861)
      // {lstr name, f32 val}. OBSERVED: scans ctx->model (+0x0c)
      // record array +0x10 (count +0x18) for a +0x28-named match;
      // on hit calls FUN_00425190(rec,0,val+55.835) — a fixed-point-
      // wrapped angle store + vert propagation on the named anim/
      // rotator record. The port does not yet model the model's
      // anim-record array — consume operands and count the call
      // (seam). If a boss path reaches this op the array must be
      // populated.
      const std::string nm = r.str();
      (void)r.f32();
      if (!r.ok) { v.fail("animangle"); return; }
      if (env.rt != nullptr) {
        ++env.rt->seams.namedAnimAngleCalls;
        env.rt->seams.namedAnimAngleName = nm;
      }
      return;
    }
    case 0x39: {                            // cone+LOS link (0x43f260)
      // {u16 range, u8 arc, u8 mode, targets per mode}. OBSERVED: the
      // same FUN_0045d880 cone+LOS test as op-0x0e, but the targets
      // precede the test and the polarity is INVERTED — target 1 is
      // the FALSE branch, target 2 (0xfe only) the TRUE branch.
      // False: 0xfe/0xfc -> call t1, 0x0c -> goto t1, 0xfd -> return.
      // True: 0xfe -> call t2; other modes fall through.
      const float range = static_cast<float>(r.u16());
      const float arc = static_cast<float>(r.u8());
      const std::uint8_t mode = r.u8();
      std::uint32_t t1 = 0, t2 = 0;
      if (mode == 0xfe) {
        t1 = r.u32();
        t2 = r.u32();
      } else if (mode == 0xfc || mode == 0x0c) {
        t1 = r.u32();
      }
      if (!r.ok) { v.fail("cone39"); return; }
      const bool cond = coneLosTest(env, obj, range, arc);
      if (!cond) {
        switch (mode) {
        case 0xfe:
        case 0xfc: v.doCall(t1); break;
        case 0x0c: v.doGoto(t1); break;
        case 0xfd: v.doReturn(); break;
        default: break;
        }
      } else if (mode == 0xfe) {
        v.doCall(t2);
      }
      return;
    }
    case 0xec: {                            // yaw-offset floor probe
      // {f32 x, f32 y, f32 z, linkage} (0x44e657 -> FUN_0045d71c).
      // OBSERVED: builds pt = pos - R(yaw)*(x,y) and stabs the bound
      // arena (+0x60) mode-1 vertically: from z0 = pos.z + 3.0
      // (C(0x49835c)) down to z0 - 6.0 (C(0x498364)) when the z
      // operand is 0, else z0 + z - 3.0 (C(0x498360)). The linkage
      // fires on NO floor (inverted polarity, below).
      const float lx = r.f32(), ly = r.f32(), lz = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("floorec"); return; }
      bool cond = false;
      const DynamicArena* bound =
          (obj.arena != nullptr) ? obj.arena
                                 : (env.currentArena != nullptr
                                        ? &env.currentArena->dyn
                                        : nullptr);
      if (bound != nullptr) {
        float sn, cs;
        sincosDeg(obj.yawDeg, &sn, &cs);
        const float px = obj.pos[0] - lx * cs - ly * sn;
        const float py = obj.pos[1] - lx * sn - ly * cs;
        const float z0 = obj.pos[2] + 3.0f;
        const float z1 = (lz == 0.0f) ? z0 - 6.0f : z0 + lz - 3.0f;
        const float a[3] = {px, py, z0};
        const float b[3] = {px, py, z1};
        cond = collisionStabMode1(bound->col, a, b) != nullptr;
      }
      // OBSERVED polarity (0x44e754/0x44e810/0x44e8bd): target 1 is
      // the NO-FLOOR branch — false: fe/fc -> call t1, 0x0c -> goto
      // t1, fd -> return; true: fe -> call t2, other modes fall
      // through to the next instruction.
      if (!cond) {
        switch (L.mode) {
        case 0xfe:
        case 0xfc: v.doCall(L.a); break;
        case 0xfd: v.doReturn(); break;
        default:                          // 0x0c goto; a null target
          if (L.a != 0) v.doGoto(L.a);    // writes +0x108=0 (halt)
          else { obj.field108 = nullptr; v.done = true;
                 res.stopped = true; }
          break;
        }
      } else if (L.mode == 0xfe) {
        v.doCall(L.b);
      }
      return;
    }
    case 0x28: {                            // yaw accumulate (0x441c6a)
      // {varop}: +0x4c += operand*(1/30) — OBSERVED. Same dead-wrap
      // quirk as op-0x68/0x86: the post-add FLDZ/FCOMPP split sends
      // >0 to a `<0:+=360` loop and <=0 to a `>=360:-=360` loop — both
      // unreachable, so +0x4c is stored raw (unbounded spin
      // accumulator — the XGREN/XGATT/XHOME turn-rate op).
      const float val = resolveVar(r, env, ctx);
      if (!r.ok) { v.fail("yawacc"); return; }
      obj.yawDeg += val * (1.0f / 30.0f);
      return;
    }
    case 0x59: {                            // sfx bind (0x43a112)
      // {u8 mode, [mode&0x10|0x40 -> 3f32 | mode&0x20 -> u8], lstr sfx}.
      // OBSERVED: the position locals (own +0x10 / literal / refpoint
      // +0x1b0) are computed then DISCARDED — the op's live effects are
      // +0x15c = sfx-name string (mode&4) and the 0x4a1220 sfx-record
      // resolve + state call (mode&0x80, sub-mode mode&3 ->
      // FUN_004022b8 / FUN_00402388(rec,{1,0})). SFX records are a
      // presentation seam in the port — counted, not resolved; the
      // "%s #%d sfx not found" error path stays inert.
      const std::uint8_t mode = r.u8();
      if ((mode & 0x10) != 0) {
        r.f32(); r.f32(); r.f32();
      } else if ((mode & 0x20) != 0) {
        r.u8();
      } else if ((mode & 0x40) != 0) {
        r.f32(); r.f32(); r.f32();
      }
      const std::string sfx = r.str();
      if (!r.ok) { v.fail("sfx-args"); return; }
      if ((mode & 4) != 0) {
        obj.field15c = sfx;                 // +0x15c string slot
      }
      if ((mode & 0x80) != 0 && env.rt != nullptr) {
        ++env.rt->seams.fireSoundCalls;     // FUN_004022b8/402388 seam
      }
      return;
    }
    case 0x81: {                            // element kill (0x43d363)
      // {u8 sel, [u8 n, n×lstr when sel<=2]}. OBSERVED:
      //   sel 0 -> FUN_0045421c(ctx, names, 0)
      //   sel 1 -> FUN_0045421c(ctx, names, 1)
      //   sel 2 -> FUN_004541a4: same call gated on debris-enable
      //            0x54150c (operand strings always consumed)
      //   sel>2 -> consumes only the sel byte.
      // FUN_0045421c per name: element-name lookup (FUN_0045c0f0);
      // +0x2c8 |= 1<<idx AND +0x2cc |= 1<<idx (mask AND latch — the
      // 0x20 unmask op can never re-arm it), then detaches the
      // element's geometry into a centroid-relativized debris shard
      // record (FUN_0041c420 alloc + vertex/tri remap — cosmetic).
      // The port applies the mask+latch and counts the shard emit.
      const std::uint8_t sel = r.u8();
      if (sel > 2) { if (!r.ok) v.fail("elkill"); return; }
      const std::uint8_t n = r.u8();
      for (std::uint8_t k = 0; k < n; ++k) {
        const std::string s = r.str();
        if (!r.ok) break;
        for (int e = 0; e < obj.elemSet.count; ++e) {
          if (obj.model.elemName(e) == s) {
            obj.col.elemMaskB |= (1u << (e & 31));
            obj.col.elemMaskLatch |= (1u << (e & 31));
          }
        }
      }
      if (!r.ok) { v.fail("elkill"); return; }
      if (env.rt != nullptr) ++env.rt->seams.elemShardCalls;
      return;
    }
    case 0x82: {                            // impact decal (0x43d420)
      // No operands. OBSERVED: handler calls FUN_00454e4c(ctx),
      // which early-outs when +0x21c (hit element index+1) is zero;
      // otherwise it transforms the +0x210 hit point into model
      // space, derives tri-local decal UVs and submits the decal
      // record to FUN_00453ba4 — pure presentation. The port counts
      // the emit request only when the +0x21c gate would pass.
      if (obj.field21c != 0 && env.rt != nullptr) {
        ++env.rt->seams.impactDecalCalls;   // FUN_00453ba4 seam
      }
      return;
    }
    case 0x84: {                            // sfxPee (0x43d42d)
      // {u8 chance, u8 ref | fff pos iff ref==0xff} — OBSERVED.
      // ref!=0xff copies a 12-byte named ref-point record from
      // ctx(+0x70)+0x1b0+ref*12; ref==0xff reads the position inline.
      // Both arms then draw rand() once (FUN_0047d2b5) and compare
      // rand%100 against chance (chance<150) or chance-150
      // (chance>=150): the pass spawns a positional one-shot FX
      // record via FUN_00405270 (arm A) or 0x403f6c+0x404108 (arm B —
      // at the bound object's pos + a z offset constant). The FX is
      // presentation-only; the port consumes the operand stream and
      // the rand draw for parity and counts the request.
      const std::uint8_t chance = r.u8();
      const std::uint8_t ref = r.u8();
      if (ref == 0xff) { r.f32(); r.f32(); r.f32(); }
      if (!r.ok) { v.fail("sfxPee-args"); return; }
      if (env.rt != nullptr) {
        const std::uint32_t roll = enemyRandNext(env.rt->rngState) % 100u;
        const int gate = (chance < 0x96) ? chance : (chance - 0x96);
        if (static_cast<int>(roll) < gate) {
          ++env.rt->seams.sfxPeeCalls;      // FUN_00405270/0x404108 seam
        }
      }
      return;
    }
    case 0x65: {                            // face camera + pitch aim
      // No operands (0x4421ea). OBSERVED: dx/dy = camXY - pos;
      // dz = (camZ + 3.0) - pos; xyDist = FUN_004301bc (XY sqrt).
      // +0x4c yaw = bearing(dy,dx) ONLY when xyDist > 2.0; +0x13c
      // bank = bearing(dz, xyDist) unconditionally. Yaw wrap: the
      // positive-side loop is dead (entry tests +0x4c>0 but iterates
      // only while +0x4c<0 — OBSERVED quirk), then +0x4c -= 360
      // while +0x4c >= 360.
      if (env.rt != nullptr) {
        const float* cam = env.rt->camera.pose.pos;  // 0x54c6c4..cc
        const float dx = cam[0] - obj.pos[0];
        const float dy = cam[1] - obj.pos[1];
        const float dz = cam[2] + 3.0f - obj.pos[2];
        const float xyDist = std::sqrt(dx * dx + dy * dy);
        if (xyDist > 2.0f) {
          float yaw = bearingDeg(dy, dx);
          // Dead first loop omitted on purpose (never iterates).
          while (yaw >= 360.0f) yaw += -360.0f;
          obj.yawDeg = yaw;
        }
        obj.bankDeg = bearingDeg(dz, xyDist);          // +0x13c
      }
      return;
    }
    case 0xac: {                            // arena emit (0x44b26b)
      // {u8 sel, [u8 refIdx iff sel==3 | f32 xyz otherwise], f32 arg}.
      // OBSERVED: sel 3 copies the 12-byte ref point at
      // ctx+0x1b0+idx*12 (worldRef); other sels read an inline f32
      // triple — sel 1 offsets it by obj pos (+0x10), sel 2 rotates
      // it through the object transform (FUN_0045546c). The tail
      // calls FUN_004575fc(arena, pos, arg) — a camera-facing
      // detonation remnant emit (cosmetic). The port resolves the
      // point and counts the request via the remnant seam.
      const std::uint8_t sel = r.u8();
      float pos[3];
      if (sel == 3) {
        const std::uint8_t idx = r.u8();
        if (!r.ok) { v.fail("emit-ref"); return; }
        const int i = (idx < 8) ? idx : 0;
        pos[0] = obj.worldRef[i][0];
        pos[1] = obj.worldRef[i][1];
        pos[2] = obj.worldRef[i][2];
      } else {
        pos[0] = r.f32(); pos[1] = r.f32(); pos[2] = r.f32();
        if (!r.ok) { v.fail("emit-pos"); return; }
        if (sel == 1) {
          pos[0] += obj.pos[0];
          pos[1] += obj.pos[1];
          pos[2] += obj.pos[2];
        } else if (sel == 2) {
          float t[3];
          transformPoint(obj.col.xform, obj.col.origin, pos, t);
          pos[0] = t[0]; pos[1] = t[1]; pos[2] = t[2];
        }
      }
      r.f32();                                   // emit magnitude arg
      if (!r.ok) { v.fail("emit-arg"); return; }
      if (env.rt != nullptr) {
        ++env.rt->seams.remnantSpawnCalls;       // FUN_004575fc seam
      }
      return;
    }
    case 0x68: {                            // aim + spread (0x4422f9)
      // {f32 spread}. OBSERVED: +0x4c = bearing(camXY - pos),
      // +0x13c = bearing((camZ+3) - posZ, xyDist); when spread != 100,
      // +0x4c += (rand(0x14)-10)*(100-spread)/d3 and +0x13c += the same
      // draw scaled by (d3*4) — jitter shrinks with distance.
      // FUN_004301bc = XY dist, FUN_00430160 + 0.1 = 3D dist. Both
      // post-store wrap loops are dead (same quirk as op-0x65).
      const float spread = r.f32();
      if (!r.ok) { v.fail("aim68"); return; }
      if (env.rt != nullptr) {
        const float* cam = env.rt->camera.pose.pos;  // 0x54c6c4..cc
        const float dx = cam[0] - obj.pos[0];
        const float dy = cam[1] - obj.pos[1];
        const float dz = cam[2] + 3.0f - obj.pos[2];
        const float xyDist = std::sqrt(dx * dx + dy * dy);
        const float d3 =
            std::sqrt(dx * dx + dy * dy + dz * dz) + 0.1f;
        float yaw = bearingDeg(dy, dx);
        float bank = bearingDeg(dz, xyDist);
        if (spread != 100.0f) {
          const float s = 100.0f - spread;
          yaw += static_cast<float>(
                     enemyRandBelow(env.rt->rngState, 0x14) - 10) *
                 s / d3;
          bank += static_cast<float>(
                      enemyRandBelow(env.rt->rngState, 0x14) - 10) *
                  s / (d3 * 4.0f);
        }
        obj.yawDeg = yaw;                            // +0x4c, raw store
        obj.bankDeg = bank;                          // +0x13c
      }
      return;
    }
    case 0x6b: {                            // voice rebind (0x43848)
      // {lstr sfx}: +0x15c = name; +0x158 voice handle released via
      // FUN_004020b4 when bound, then FUN_00402fe8 resolve +
      // FUN_00402160 respawn when the name is nonempty. Voice/sfx are
      // presentation — counted in seams.fireSoundCalls, not resolved.
      const std::string sfx = r.str();
      if (!r.ok) { v.fail("voicebind"); return; }
      obj.field15c = sfx;                            // +0x15c
      if (obj.field158 != nullptr) {
        if (env.rt != nullptr) ++env.rt->seams.fireSoundCalls;
        obj.field158 = nullptr;
      }
      if (!sfx.empty() && env.rt != nullptr)
        ++env.rt->seams.fireSoundCalls;              // FUN_00402160
      return;
    }
    case 0x6d: {                            // player damage (0x443c73)
      // {u8 dmg}: FUN_00467888(dmg, +0x4c) — OBSERVED: FUN_00467888
      // is the player-damage function (gates 0x541554/0x541510,
      // 0x540e10 suppress + 0x540d5c clear, loco-state 0x326/0x385/
      // 0x3ea, 0x540eb8; Skill scaling; 0x540dac += dmg·25 clamp
      // [75,180]; mount redirect 0x540e6c + 0x540e70&1; health
      // 0x541554 -= dmg floored 0; landingAccum += dmg). Identical
      // body to FUN_0046771c = playerDamageApply; the pushed +0x4c
      // arg is never read by the callee (vestigial RET 0x4 slot).
      const std::uint8_t dmg = r.u8();
      if (!r.ok) { v.fail("dmg"); return; }
      if (env.rt != nullptr)
        playerDamageApply(*env.rt, dmg, obj.pos);
      return;
    }
    case 0xf0: {                            // damage-accum set (0x443c9f)
      // {u8 dmg}. OBSERVED: operand 1 is remapped to 5, then
      // `0 >= 0x540e10` (the player-damage suppress window idle)
      // gates `fild(dmg) -> 0x540d5c` (the landing/damage
      // accumulator — a direct set, not an add). When the suppress
      // window is positive the store is skipped entirely.
      std::uint8_t dmg = r.u8();
      if (!r.ok) { v.fail("dmgset"); return; }
      if (dmg == 1) dmg = 5;
      if (env.rt != nullptr && env.rt->fieldE10 <= 0.0f)
        env.rt->vert.landingAccum = static_cast<float>(dmg);
      return;
    }
    case 0x86: {                            // pitch drift (0x441d58)
      // {varop}: +0x54 += operand * (1/30) — OBSERVED 0x49b6f4 is the
      // frame-time constant. Dead-wrap quirk (OBSERVED 0x441dcf..):
      // >0 routes to a `<0:+=360` loop and <=0 to a `>=360:-=360` loop
      // — both unreachable, so +0x54 is stored raw.
      const float val = resolveVar(r, env, ctx);
      if (!r.ok) { v.fail("pitchdrift"); return; }
      obj.pitchDeg += val * (1.0f / 30.0f);
      return;
    }
    case 0x6a: {                            // f32 -> +0x302 (0x44382b)
      // {f32} written straight into the +0x302 dword (rawMatrix[0] /
      // connector field302 view — OBSERVED 0x44382b).
      const float val = r.f32();
      if (!r.ok) { v.fail("f302"); return; }
      obj.field302 = std::bit_cast<std::uint32_t>(val);
      return;
    }
    case 0x6e: {                            // teardown now (0x443ceb)
      // No operands — FUN_0045828c(ctx): frees the +0x160 slot table,
      // releases +0x158/+0x15c, clears the 0x49b85c view-anchor when it
      // is ctx, frees the deep-copied +0x0c model, memsets the record
      // (link +0x00 and +0x60 preserved), unlinks. Ported as
      // objectTeardownNow.
      if (env.rt != nullptr) objectTeardownNow(*env.rt, obj);
      return;
    }
    case 0x55: {                            // xform snapshot (0x449524)
      // {u8 v}. OBSERVED: v!=0 && +0x148 bit6 clear -> +0x148|=0x40 and
      // +0x302..0x322 (rawMatrix) = the col 3x3 rotation; the original
      // reads +0xac/+0xb0/+0xb4, +0xbc/+0xc0/+0xc4, +0xcc/+0xd0/+0xd4 —
      // a 3x3 sliced out of the 3x4 world xform (origin column +0xb8/
      // +0xc8/+0xd8 skipped, held in col.origin). v==0 -> clears
      // +0x148 bit6 only.
      if (r.u8()) {
        if (!(obj.col.flags148 & 0x40)) {
          obj.col.flags148 |= 0x40;
          for (int i = 0; i < 9; ++i) obj.rawMatrix[i] = obj.col.xform[i];
        }
      } else {
        obj.col.flags148 &= ~0x40u;
      }
      return;
    }
    case 0x60: {                            // box2d link (0x444bb0)
      // {x0,y0,x1,y1 f32, linkage} — same shared handler as the arena
      // VM: player XY inside the box dispatches the linkage.
      const float x0 = r.f32(), y0 = r.f32();
      const float x1 = r.f32(), y1 = r.f32();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("box2d"); return; }
      bool in = false;
      if (env.playerPos) {
        const float px = env.playerPos[0], py = env.playerPos[1];
        in = (px >= x0 && px <= x1 && py >= y0 && py <= y1);
      }
      switch (L.mode) {
      case 0xfe: if (in) v.doCall(L.a); else if (L.b) v.doCall(L.b);
                 break;
      case 0xfc: if (in) v.doCall(L.a); break;
      case 0x0c: if (in) v.doGoto(L.a); break;
      case 0xfd: if (in) v.doReturn(); break;
      default: break;
      }
      return;
    }
    case 0xf2: {                            // orbit state block (0x451987)
      // {u8 flag, [f32 x12 iff flag!=0]}. OBSERVED: flag!=0 ->
      // +0x2d0=1, +0x2d1=0xff, 12 floats land at +0x2d2..+0x301
      // (field2d2 + field2ea). flag==0 -> +0x2d1=0 only (asymmetric —
      // +0x2d0 survives).
      if (r.u8()) {
        obj.field2d0 = 1;
        obj.field2d1 = 0xff;
        for (int i = 0; i < 6; ++i) obj.field2d2[i] = r.f32();
        for (int i = 0; i < 6; ++i) obj.field2ea[i] = r.f32();
      } else {
        obj.field2d1 = 0;
      }
      return;
    }
    case 0x2e: {                            // mounted linkage (0x4464cb)
      // {u8 mode | links}. OBSERVED: the handler first calls
      // FUN_0045ae5c(ctx) — the vehicle-mount scan over the arena's
      // type-5 connect records that boards/refreshes the object's
      // mounted state (subtype +0x11e = 0x2b while mounted). The
      // mount/boarding math is an unported seam — objects keep
      // their current subtype here.
      // Mounted (0x44657f): 0xfc/0xfe -> call linkA, 0xfd -> return,
      //   any other mode -> goto linkA (+0x108=linkA, mark[depth]=0).
      // Not mounted (0x4466e8): 0xfe -> call linkB (else branch),
      //   other modes fall through with no linkage.
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("mountLink"); return; }
      if (obj.field11e == 0x2b) {
        switch (L.mode) {
        case 0xfc: case 0xfe:
          if (!v.doCall(L.a)) return;
          break;
        case 0xfd: if (!v.doReturn()) return; break;
        default: v.doGoto(L.a); break;
        }
      } else if (L.mode == 0xfe) {
        if (!v.doCall(L.b)) return;         // linkB==0 -> +0x108=0
      }
      return;
    }
    case 0x5c: {                            // anim-frame link (0x43d86f)
      // {u16 frame, u8 mode|links} — OBSERVED. Pass condition
      // (0x43d923): +0x114 animRec==0 OR (+0x116>>16)==-256
      // (animLatch==0xff00 — the animDone test) OR
      // (+0xe2>>16) >= frame-1 — the +0xe2 dword's high word is
      // +0xe4 animFrame. Pass: 0xfc/0xfe -> call linkA, 0xfd ->
      // return, any other mode -> goto linkA. Fail (0x43dac8):
      // 0xfe -> call linkB only; other modes fall through.
      const std::uint16_t frame = r.u16();
      Linkage L;
      if (!readLinkage(r, L)) { v.fail("animLink2"); return; }
      const bool cond = obj.animDone() ||
          obj.animFrame >= static_cast<int>(frame) - 1;
      switch (L.mode) {
      case 0xfc:
        if (cond && !v.doCall(L.a)) return;
        break;
      case 0xfe:
        if (!v.doCall(cond ? L.a : L.b)) return;
        break;
      case 0xfd:
        if (cond && !v.doReturn()) return;
        break;
      default:
        if (cond) v.doGoto(L.a);
        break;
      }
      return;
    }
    case 0x83: {                            // completion/cine (0x43d3e2)
      // {u8 subop} — same handler as the arena VM.
      const std::uint8_t subop = r.u8();
      if (!r.ok) { v.fail("0x83"); return; }
      completionOp83(env, subop);
      return;
    }

    default: {
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "Unrecognised object op 0x%02x at +%x [%s/%s]",
                    op, insnOff, obj.scriptClass.c_str(),
                    obj.model.modelName().c_str());
      v.fail(buf);
      return;
    }
  }
}

} // namespace

TraversalScriptResult traversalObjectInitScript(
    TraversalScriptEnv& env, DynamicObject& obj, std::uint32_t codeOff) {
  TraversalScriptResult res;
  if (codeOff == 0) return res;
  // Var mode 1 resolves the bound object's home arena (+0x48). Rebind
  // selfArena to the object's actual home so resolveVar/resolveVarRef
  // hit the right arena even if env.selfArena differs.
  TraversalScriptEnv oenv = env;
  if (obj.arena && obj.arena->owner) oenv.selfArena = obj.arena->owner;
  oenv.ctxObject = &obj;       // object-init ctx = the object itself
  Reader r{oenv.image, oenv.imageBase, codeOff};
  const char* name =
      oenv.selfArena ? oenv.selfArena->name.c_str() : "<obj>";
  ObjScriptPass v{oenv, obj, r, res, name};
  for (int i = 0; i < 1000 && !v.done; ++i) objScriptInsn(v);
  if (!v.done && !res.error) {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "Objinit %s looped %d commands, off %lx", name,
                  res.instructions, static_cast<unsigned long>(r.pc));
    res.diag = buf;
    if (oenv.diagLog) oenv.diagLog->push_back(res.diag);
    res.error = true;
  }
  obj.field108 = nullptr;   // FUN_004566f0 clears +0x108 post-run
  return res;
}

// ---------------------------------------------------------------------------
// Persistent per-object script tick — FUN_004388d8(obj) gated on
// +0x108 (OBSERVED FUN_004572ac body: the VM runs before the path
// follower / subtype / gravity / anim driver each frame). The wait
// gate at 0x438971..0x4389be: +0x22c decrements by 1/30 per entry;
// while positive the fetch is skipped entirely; on crossing zero the
// pass resumes at +0x230. Entry pc = +0x108 — the last checkpoint
// (0x01), call target (0xfc/0x5f/0x66) or wait-resume target.
// ---------------------------------------------------------------------------
TraversalScriptResult traversalObjectScriptTick(
    TraversalScriptEnv& env, DynamicObject& obj) {
  TraversalScriptResult res;
  if (obj.field108 == nullptr) return res;
  TraversalScriptEnv oenv = env;
  if (obj.arena && obj.arena->owner) oenv.selfArena = obj.arena->owner;
  oenv.ctxObject = &obj;       // object VM ctx = the object itself
  // oenv is a copy made for the selfArena override — but group-0 flag
  // ops (0x44..0x48) write 0x540d98, a GLOBAL in the original. The
  // flag dword must reach the caller's env on every exit, else
  // restored object scripts silently drop global flag writes.
  auto sync = [&] { env.gFlags = oenv.gFlags; };

  const std::byte* base = oenv.image.data() + oenv.imageBase;
  auto offIn = [&](const void* p) -> std::uint32_t {
    const std::byte* q = static_cast<const std::byte*>(p);
    if (!q || q < base ||
        q >= oenv.image.data() + oenv.image.size())
      return 0;
    return static_cast<std::uint32_t>(q - base);
  };

  std::uint32_t entryOff;
  if (obj.field22c > 0.0f) {
    obj.field22c -= 1.0f / 30.0f;       // 0x49b6f4 (OBSERVED)
    if (obj.field22c > 0.0f) {
      res.waited = true;
      sync();
      return res;
    }
    obj.field22c = 0.0f;
    entryOff = offIn(obj.field230);
  } else {
    entryOff = offIn(obj.field108);
  }
  if (entryOff == 0) {                  // pc outside the image — the
    res.error = true;                   // original would fault; the
    res.diag = "object script PC out of bounds";  // port kills it
    if (oenv.diagLog) oenv.diagLog->push_back(res.diag);
    obj.field108 = nullptr;
    sync();
    return res;
  }

  Reader r{oenv.image, oenv.imageBase, entryOff};
  const char* name =
      oenv.selfArena ? oenv.selfArena->name.c_str() : "<obj>";
  ObjScriptPass v{oenv, obj, r, res, name};
  for (int i = 0; i < 1000 && !v.done; ++i) objScriptInsn(v);
  if (!v.done && !res.error) {
    // "Alien %s looped %d commands, off %lx" (OBSERVED diagnostic);
    // the original then clears +0x108.
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "Alien %s looped %d commands, off %lx", name,
                  res.instructions, static_cast<unsigned long>(r.pc));
    res.diag = buf;
    if (oenv.diagLog) oenv.diagLog->push_back(res.diag);
    res.error = true;
    obj.field108 = nullptr;
  }
  sync();
  return res;
}

// ---------------------------------------------------------------------------
// FUN_00432ec4 — arena-name lookup. The arena record's name is at the
// record start; the original logs "arena %s not found" on a miss and
// the 0x95 handler aborts the spawn when it returns 0.
// ---------------------------------------------------------------------------
static TraversalArena* traversalFindArena(TraversalRuntime& rt,
                                          const std::string& name) {
  for (const auto& ap : rt.arenas)
    if (ap && ap->name == name) return ap.get();
  return nullptr;
}

// ---------------------------------------------------------------------------
// Spawn helper — FUN_00454af8 / the tr_alcmd spawn opcodes. The object
// is created on the SCRIPT's own arena +0x68 list (the original's
// in_EAX is the bound arena, not the destination). For the 0x95
// connector form (variant 0) it additionally records the resolved
// class index (+0x04), spawn id (+0x146), destination arena (+0x302),
// connector flag (+0x148 = 0x1108000 -> +0x14a bit4), and the closed
// connector state (+0x312 = 8, radius +0x30e = 20) — see
// FUN_00454af8's param_9 != 0 block.
// ---------------------------------------------------------------------------
void traversalScriptSpawn(TraversalScriptEnv& env, float x, float y,
                          float z, float yaw, std::uint32_t flags,
                          const std::string& cls, const std::string& name,
                          std::uint32_t scriptOff, int variant) {
  if (!env.rt || !env.selfArena) return;
  TraversalRuntime& rt = *env.rt;
  TraversalArena* self = env.selfArena;

  // FUN_00454794 — class/model name -> enemy-table index. The original
  // scans DAT_004edcc0 records by C-string; a miss logs "ENEMY name %s
  // not found" and aborts the spawn (no object created).
  int enemyIdx = -1;
  if (!cls.empty()) enemyIdx = rt.level.enemies.indexOf(cls);
  if (!cls.empty() && enemyIdx < 0) return;

  TraversalArena* dst = nullptr;
  if (variant == 0) {
    // 0x95 only: the second string is the destination arena name,
    // resolved by FUN_00432ec4. A miss aborts the spawn.
    dst = traversalFindArena(rt, name);
    if (dst == nullptr) return;

    // FUN_00454894 — connector dedup: reuse an existing object already
    // bridging self<->dst with this class index (+0x04) and the
    // {+0x60,+0x302} pair in either order. The original then migrates
    // it toward the current arena instead of spawning a duplicate.
    auto isConn = [&](DynamicObject& o) {
      if (!o.col.named || o.enemyIndex != enemyIdx) return false;
      const bool fwd = (o.arena == &self->dyn && o.connDest == dst);
      const bool rev = (o.connDest == self && o.arena == &dst->dyn);
      return fwd || rev;
    };
    for (DynamicArena* la : {&self->dyn, &dst->dyn})
      for (auto& up : la->storage)
        if (isConn(*up)) { ++env.seamsSpawned; return; }
  }

  DynamicObject& o = self->dyn.allocFront();   // FUN_0045cffc
  o.scriptClass = cls;
  o.scriptName = name;
  o.scriptOff = scriptOff;
  o.scriptVariant = variant;
  o.enemyIndex = static_cast<std::uint16_t>(enemyIdx < 0 ? 0 : enemyIdx);
  o.spawnId = static_cast<std::uint16_t>(flags & 0xffffu);   // +0x146
  o.setPosition(x, y, z);                    // +0x10..0x18 / +0x1c..0x24
  o.prevPos[0] = x; o.prevPos[1] = y; o.prevPos[2] = z;      // +0x180..
  o.yawDeg = yaw;                            // +0x4c
  o.prevYawDeg = yaw;                        // +0x50
  o.behaviorByte = 7;                        // +0x11c
  // OBSERVED: generic spawns are born leaderless — FUN_00454af8
  // writes only +0x146/+0x11c. The +0x11e=0x1e/+0x138=ctx tail at
  // 0x43fb0e..0x43fc16 belongs to opcode 0x1d's chain-spawn arm
  // (sequential spawnIds), not to every script spawn.

  // FUN_00403720 — deep-copy the resolved model into +0x0c.
  if (env.modelFor && enemyIdx >= 0) {
    if (const RuntimeModel* src = env.modelFor(enemyIdx, env.modelCtx))
      o.model = deepCopyModel(*src);
  }

  if (variant == 0) {
    // Connector fields — the param_9 != 0 block of FUN_00454af8 plus
    // the handler tail (+0x302 dest, +0x148 = 0x1108000). OBSERVED
    // dword 0x1108000 -> +0x148=0x00 +0x149=0x80 +0x14a=0x10 +0x14b=0x01:
    // flags148=0x8000 (its high byte mirrors flags149=0x80),
    // flags14a=0x10 (connector bit). No sweep-skip bits — the door is
    // born solid and opens via the +0x312-state collision toggle.
    o.connDest = dst;                        // +0x302
    o.connRadius = 20.0f;                    // +0x30e
    o.connState = 8;                         // +0x312 = closed
    o.connStateHi = 0;                       // +0x313
    o.animRecNear = nullptr;                // +0x306
    o.animRecFar = nullptr;                 // +0x30a
    o.col.flags148 = 0x8000;                 // +0x148=0x00 +0x149=0x80
    o.col.flags149 = 0x80;                   // +0x149
    o.col.flags14a = 0x10;                   // +0x14a (connector)
  } else if (variant == 2 || variant == 4) {
    // OBSERVED: the op-0xa1 and op-0xce handlers share the tail
    // `+0x148 dword = 0x2008a6` (0x4499d2 / 0x449b4b) — flags148=0x08a6
    // (incl. the +0x148&2 gravity bit), flags14a=0x20. Every named
    // pickup spawn through these ops is a FUN_004585c4 mover: SW_H150
    // takes the flee branch, SW_SEAL/SW_SBONE are explicit no-ops, and
    // all other SW_* models take the default yaw spin.
    o.col.flags148 = 0x08a6;
    o.col.flags149 = 0x08;
    o.col.flags14a = 0x20;
  }
  // Variants 1 (op-0x56) and 3 (op-0xe6) have no +0x148 flag tail in
  // the original — flags14a keeps its alloc-zero value (no mover bit).

  // FUN_004566f0 — generic object init, common to every spawned object:
  // default block (health/scale/fields/identity) -> bind the model ->
  // the table-2 "%s$%s" init script -> FUN_0045612c transform rebuild
  // (applies the script's +0x58 scale / +0x5c zBias).
  initObjectDefaults(o);
  o.syncCollisionView();                     // bind +0x0c (elemSet+gate)
  {
    // Key "%s$%s" = homeArena$className. OBSERVED (FUN_004566f0):
    // sprintf("%s$%s", *+0x60 /*arena*/, *+0xc /*enemy-table record*/)
    // — the second half is the ENEMY-TABLE name (the class operand),
    // not the geometry record's name-table[0] (XGUNTAM resolves to
    // "XGU_HEAD" there while its init is keyed "GUNT_10$XGUNTAM").
    std::string cn = !cls.empty() ? cls : name;
    if (cn.empty()) cn = o.model.modelName();
    const std::string key = self->name + "$" + cn;
    const std::uint32_t initOff = cmiObjectScriptOffset(rt.level.cmi, key);
    if (initOff != 0) {
      TraversalScriptResult ir = traversalObjectInitScript(env, o, initOff);
      rt.scriptInsnTotal += ir.instructions;
    }
    o.syncCollisionView();                   // presence-gate re-check
    rebuildObjectTransform(o);               // FUN_0045612c (+0x58/+0x5c)
  }

  // FUN_00456808 tail — the spawn's script operand binds +0x108, the
  // persistent per-object VM PC (image pointer; the object-script tick
  // runs it each frame while nonzero). OBSERVED write order: the
  // init script runs first (its own +0x108 use is transient).
  if (scriptOff != 0 && !env.image.empty()) {
    const std::uint64_t fo =
        static_cast<std::uint64_t>(env.imageBase) + scriptOff;
    if (fo < env.image.size())
      o.field108 = env.image.data() + fo;
  }

  if (variant == 0) {
    // Handler tail (0x44a55a..0x44a648): build the element-name masks
    // the connector update ORs into +0x2c8. +0x326 = elements named
    // "LOCK" (FUN_0042fa50 exact compare); +0x32a = elements whose
    // name starts with "HC". For XCORDOOR both resolve to 0 — its
    // leaves are XCORD_L*/XCORD_R*, so the mask path is inert.
    for (int e = 0; e < o.elemSet.count; ++e) {
      const std::string en = o.model.elemName(e);
      if (en == "LOCK")
        o.connMaskLock |= (1u << (e & 31));        // +0x326
      else if (en.size() >= 2 && en[0] == 'H' && en[1] == 'C')
        o.connMaskHC |= (1u << (e & 31));          // +0x32a
    }
    o.col.elemMaskA = o.connMaskLock;              // +0x326 alias
  }
  traversalSpawnLog().push_back(
      {cls, name, self->name, variant});
  ++env.seamsSpawned;
}

// Phase 15A — bounded spawn census for the combat harness: records
// every script-spawned class/name/arena so --traversal-runtime can
// list encounter participants without a proprietary data dump.
std::vector<TraversalSpawnRecord>& traversalSpawnLog() {
  static std::vector<TraversalSpawnRecord> log;
  return log;
}

namespace {

// Operand grammars (identical to the executor's fetch order).
const char* opcodeGrammar(std::uint8_t op) {
  switch (op) {
  case 0x01: case 0x09: case 0xff: case 0xfd: case 0x65: case 0x6e:
  case 0x7d: case 0x4b: case 0x0f: case 0x3c: case 0x82:
    return "";
  case 0x08: case 0x10: return "h";         // i16 yaw / u16 health
  case 0x28: case 0x35: case 0x40: case 0x86: case 0xb1: case 0x3a:
  case 0x52: case 0x54: case 0x5b: case 0x32: case 0x33: case 0x34:
  case 0xa9: case 0xd2:
    return "v";
  case 0x5a: return "sf";                   // {lstr name, f32 angle}
  case 0x17: case 0x23: case 0x24: case 0x29: case 0x3f:
  case 0x49: case 0x98: case 0xcd: case 0x58:
    return "b";
  case 0x4c: case 0x6f: case 0x74: return "w";
  case 0x4e: return "www"; case 0x96: return "ww";
  case 0x97: return "ssss"; case 0xa7: return "f";
  case 0x2c: case 0x66: case 0x6c: case 0xa6: case 0xb0: case 0x11:
  case 0x7b: case 0x0d: case 0x2e:
    return "l";
  case 0x12: return "fl";
  case 0x0e: return "hbl";                    // {u16,u8,link}
  case 0x5c: return "hl";                     // {u16, linkage}
  case 0x2d: case 0x36: return "kl";
  case 0x2f: return "fl";
  case 0xaf: return "bkl";
  case 0x3e: case 0x7f: return "kl";
  case 0x68: case 0x6a: case 0x7c: return "f";
  case 0xbb: return "ff"; case 0xdc: case 0x4f: return "fff";
  case 0x42: return "bbf"; case 0x04: return "b";
  case 0x02: return "wbbhb";                  // + fff iff mode==0
  case 0xc8: return "ffffl";
  case 0xec: return "fffl";
  case 0x39: return "hbb";                    // + targets per mode
  case 0xcf: return "ff";                     // + link | ff u8
  case 0xd8: return "bbw";
  case 0x3d: case 0x5f: case 0x84: case 0x1f: case 0x20: case 0x26:
  case 0x81: case 0xac:
    return "b";                               // lead u8 + shaped tail
  case 0xae: return "bkl"; case 0x43: return "bbkl";
  case 0xb7: return "bbn"; case 0xf9: return "b";
  case 0x6b: return "s";
  case 0x6d: case 0x83: case 0x55: case 0xf2: return "b";
  case 0x44: case 0x45: case 0x61: case 0x0b: case 0xca:
    return (op == 0x44 || op == 0x45) ? "bb" : "b";
  case 0x46: case 0x47: case 0x48: return "bbl";
  case 0x15: return "w"; case 0x16: return "l"; case 0x2a: return "s";
  case 0x9a: return "h";
  case 0x25: case 0x72: return "l";
  case 0xf3: return "bhl";                     // refEsc {u8, u16, linkage}
  case 0x21: return "wl";                      // pathLink {u32, linkage}
  case 0x30: return "fl"; case 0xbe: return "bs";
  case 0x60: return "ffffl";
  case 0x67: return "ffffffl";
  case 0x62: case 0xa8: return "bb";
  case 0x41: case 0x63: return "bbw";
  case 0x0c: case 0xfc: return "n";
  case 0x95: return "ffffwssw";
  case 0x56: case 0xa1: case 0x71: return "fffsw";
  case 0x77: return "skl"; case 0xd3: return "";
  case 0x50: return "fff";                     // velRotAdd
  case 0xe6: return "ffffwsw";
  case 0xce: return "wfsw";
  case 0x8e: return "bsbbf";
  case 0x99: case 0x05: return "f";
  case 0xc6: return "sbww";        // obj-init elem-set declaration
  case 0xc7: return "v";           // obj-init +0x104 var write
  case 0xe0: return "b";           // + conditional ff when flag != 0
  case 0x0a: return "sbl";
  case 0x03: case 0x3b: return "w";    // anim bind {u32 imgref}
  case 0x18: return "bs";              // anim sound {u8 mark, str}
  case 0x75: case 0x76: return "w";    // flag-mask / +0x118 target
  default: return nullptr;
  }
}
const char* opcodeName(std::uint8_t op) {
  switch (op) {
  case 0x01: return "ckpt";   case 0x09: return "stop";
  case 0xff: return "end";    case 0xfd: return "ret";
  case 0x28: return "yawAcc"; case 0x35: return "rate34";
  case 0x3e: return "angleLink"; case 0x65: return "faceCam";
  case 0x68: return "aim68";  case 0x6a: return "life302";
  case 0x6b: return "voiceBind"; case 0x6d: return "dmg";
  case 0x83: return "endlvl/cine";
  case 0x6e: return "teardown"; case 0x7f: return "cmpLink";
  case 0x86: return "pitchDrift";
  case 0x40: return "wait";   case 0x44: return "bitset";
  case 0x45: return "bitclr"; case 0x46: return "brSet2";
  case 0x47: return "brSet";  case 0x48: return "brClr";
  case 0x60: return "box2d";  case 0x67: return "box3d";
  case 0x61: return "setObj148"; case 0x62: return "surfop";
  case 0x63: return "surfbind";  case 0xa8: return "surfcfg";
  case 0x0c: return "rgoto";  case 0xfc: return "rcall";
  case 0x95: return "spawn";  case 0x56: return "spawn2";
  case 0xe6: return "spawn3"; case 0xa1: return "spawnNamed";
  case 0x71: return "spawnOfs"; case 0x77: return "cntLink";
  case 0xd3: return "velZero"; case 0xb1: return "auxDist";
  case 0xbb: return "randImpulse"; case 0x25: return "f14cLink";
  case 0x7c: return "faceBias"; case 0xdc: return "spawnX";
  case 0xae: return "ammoLink"; case 0x43: return "varcmpLink";
  case 0x4f: return "setPos"; case 0x72: return "rideLink";
  case 0x42: return "varAdd"; case 0x7d: return "clrEvtStk";
  case 0x4b: return "subClr"; case 0x0f: return "sub0f";
  case 0x58: return "spdRamp"; case 0xec: return "floorL";
  case 0xf9: return "childLink"; case 0xb7: return "swCall";
  case 0x04: return "bcast";
  case 0xce: return "imgobj"; case 0x8e: return "volact";
  case 0xca: return "setG";   case 0x99: return "ctx30e";
  case 0x05: return "gFloat"; case 0x7b: return "ifPartner";
  case 0x0d: return "linkGate"; case 0xe0: return "deflect";
  case 0x0a: return "brObj11a";  case 0x0b: return "setVar11a";
  case 0x15: return "pathIdx"; case 0x16: return "markLink";
  case 0x2a: return "namedLink"; case 0x55: return "xformSnap";
  case 0x9a: return "animWait"; case 0xf2: return "orbitBlk";
  case 0x30: return "periodLink"; case 0xbe: return "recEnable";
  case 0x41: return "setVar";
  case 0xc6: return "elemset"; case 0xc7: return "setF104";
  case 0x03: return "animBind"; case 0x3b: return "animLoop";
  case 0x18: return "animSnd";  case 0x75: return "clr148";
  case 0x76: return "animTgt";
  case 0x08: return "setYaw";  case 0x10: return "health";
  case 0x17: return "f148b0";  case 0x23: return "f148b2";
  case 0x24: return "f148b1";  case 0x29: return "f149b0";
  case 0x3f: return "f148b4n";
  case 0x49: return "setF11b"; case 0x98: return "f312hi";
  case 0xcd: return "setF21f"; case 0x4c: return "deathRef";
  case 0x6f: return "spawnId"; case 0x74: return "f148or";
  case 0x4e: return "sub4e";   case 0x96: return "set306";
  case 0x97: return "set316";  case 0xa7: return "flee";
  case 0x2c: return "subtLink"; case 0x66: return "condLink";
  case 0x6c: return "touchLink"; case 0xa6: return "losLink";
  case 0xb0: return "f14aLink"; case 0x2d: return "camDistLink";
  case 0x36: return "seekDistLink"; case 0x2f: return "probLink";
  case 0xaf: return "invLink"; case 0x3a: return "animRate";
  case 0x52: return "setF44";  case 0x54: return "setZbias";
  case 0x50: return "velRotAdd";
  case 0x5b: return "setFe8";  case 0x3c: return "faceCam2";
  case 0x5a: return "animAngle"; case 0xa9: return "setF326"; case 0xd2: return "setF2c0";
  case 0x32: return "setF38";  case 0x33: return "setF3c";
  case 0x34: return "setF40";  case 0x02: return "pathBind";
  case 0xc8: return "moveTo";  case 0x11: return "animLink";
  case 0x21: return "pathLink";
  case 0x12: return "timeLink"; case 0x5c: return "animLink2";
  case 0x82: return "decal";   case 0x84: return "sfxPee";
  case 0x81: return "elKill";  case 0xac: return "emitAt";
  case 0x2e: return "mountLink"; case 0x5f: return "wcall";
  case 0x3d: return "mdlSpawn"; case 0x39: return "coneLink";
  case 0xcf: return "yawMorph"; case 0x1f: return "elemBind";
  case 0x20: return "elemUnmask"; case 0x26: return "velCmp";
  case 0xf3: return "refEsc";
  case 0xd8: return "varAcc";  case 0x59: return "sfxBind";
  case 0x0e: return "viewLink";
  default: return nullptr;
  }
}

} // namespace

TraversalScriptInsn traversalScriptDecode(std::span<const std::byte> image,
                                          std::uint32_t imageBase,
                                          std::uint32_t codeOff) {
  TraversalScriptInsn out;
  out.off = codeOff;
  Reader r{image, imageBase, codeOff};
  out.opcode = r.u8();
  if (!r.ok) { out.text = "???"; return out; }

  const char* g = opcodeGrammar(out.opcode);
  const char* nm = opcodeName(out.opcode);
  if (!g || !nm) {
    out.length = 1;
    char b[32];
    std::snprintf(b, sizeof b, "??? op%02x", out.opcode);
    out.text = b;
    return out;
  }
  std::string text = nm;
  char arg[64];
  for (const char* t = g; *t; ++t) {
    switch (*t) {
    case 'b': std::snprintf(arg, sizeof arg, " %u", r.u8()); text += arg; break;
    case 'h': {                              // i16 operand
      std::snprintf(arg, sizeof arg, " %d",
                    static_cast<std::int16_t>(r.u16())); text += arg; break;
    }
    case 'f': std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg; break;
    case 'w': {
      std::uint32_t o = r.u32();
      std::snprintf(arg, sizeof arg, " 0x%x", o); text += arg; break;
    }
    case 's': { std::string s = r.str(); text += " \"" + s + "\""; break; }
    case 'v': {
      std::uint8_t m = r.u8();
      if (m == 3) { std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg; }
      else { std::snprintf(arg, sizeof arg, " v%u:%u", m, r.u8()); text += arg; }
      break;
    }
    case 'k': {                              // kind-cmp operands
      std::uint8_t kind = r.u8();
      std::uint32_t a = r.u32();
      std::snprintf(arg, sizeof arg, " %u 0x%x", kind, a); text += arg;
      if (kind == 7 || kind == 8) {
        std::uint32_t b = r.u32();
        std::snprintf(arg, sizeof arg, " 0x%x", b); text += arg;
      }
      break;
    }
    case 'n': {
      std::uint8_t n = r.u8();
      std::snprintf(arg, sizeof arg, " n%u", n); text += arg;
      for (std::uint8_t k = 0; k < n; ++k) {
        std::uint32_t o = r.u32();
        std::snprintf(arg, sizeof arg, "->%x", o); text += arg;
        out.linkTargets.push_back(o);
      }
      break;
    }
    case 'l': {
      std::uint8_t m = r.u8();
      std::snprintf(arg, sizeof arg, " link%02x", m); text += arg;
      if (m == 0xfe) {
        std::uint32_t a = r.u32(), b = r.u32();
        std::snprintf(arg, sizeof arg, ":%x,%x", a, b); text += arg;
        out.linkTargets.push_back(a); out.linkTargets.push_back(b);
      } else if (m == 0xfc || m == 0x0c) {
        std::uint32_t a = r.u32();
        std::snprintf(arg, sizeof arg, ":%x", a); text += arg;
        out.linkTargets.push_back(a);
      }
      break;
    }
    }
    if (!r.ok) { out.text += " <BOUNDS>"; break; }
  }
  if (out.opcode == 0xe0) {                  // conditional f32 pair
    // the flag byte was consumed as 'b'; refetch at codeOff+1
    if (imageBase + codeOff + 1 < image.size() &&
        static_cast<std::uint8_t>(image[imageBase + codeOff + 1]) != 0) {
      float y = r.f32(), s = r.f32();
      std::snprintf(arg, sizeof arg, " %.3g %.3g", y, s); text += arg;
    }
  }
  if (out.opcode == 0xf2) {                  // conditional f32 x12
    if (imageBase + codeOff + 1 < image.size() &&
        static_cast<std::uint8_t>(image[imageBase + codeOff + 1]) != 0) {
      for (int k = 0; k < 12; ++k) {
        std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
      }
    }
  }
  if (out.opcode == 0x04) {                  // broadcast operands
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t outer = r2.u8();
    if (outer == 7) {
      std::uint8_t m = r.u8();
      std::snprintf(arg, sizeof arg, " link%02x", m); text += arg;
      if (m == 0xfe) {
        std::uint32_t a = r.u32(), b = r.u32();
        std::snprintf(arg, sizeof arg, ":%x,%x", a, b); text += arg;
        out.linkTargets.push_back(a); out.linkTargets.push_back(b);
      } else if (m == 0xfc || m == 0x0c) {
        std::uint32_t a = r.u32();
        std::snprintf(arg, sizeof arg, ":%x", a); text += arg;
        out.linkTargets.push_back(a);
      }
    } else if (outer == 0x2b) {
      float f1 = r.f32(), f2 = r.f32();
      std::snprintf(arg, sizeof arg, " %.3g %.3g", f1, f2); text += arg;
    }
    const std::uint8_t inner = r.u8();
    std::snprintf(arg, sizeof arg, " i%u", inner); text += arg;
    if (inner == 6 || inner == 0xa) {
      std::uint32_t a = r.u32();
      std::string s = r.str();
      std::snprintf(arg, sizeof arg, " %x \"%s\"", a, s.c_str());
      text += arg;
    } else if (inner == 2 || inner == 4 || inner == 7 || inner == 5) {
      std::string s = r.str();
      std::snprintf(arg, sizeof arg, " \"%s\"", s.c_str()); text += arg;
      if (inner == 5) {
        std::uint32_t a = r.u32();
        std::snprintf(arg, sizeof arg, " %x", a); text += arg;
      }
    }
  }
  if (out.opcode == 0xf9) {                  // [lstr iff mode==1] + link
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t mode = r2.u8();
    if (mode == 1 && r2.ok) {
      std::string s = r.str();
      std::snprintf(arg, sizeof arg, " \"%s\"", s.c_str()); text += arg;
    }
    std::uint8_t m = r.u8();
    std::snprintf(arg, sizeof arg, " link%02x", m); text += arg;
    if (m == 0xfe) {
      std::uint32_t a = r.u32(), b = r.u32();
      std::snprintf(arg, sizeof arg, ":%x,%x", a, b); text += arg;
      out.linkTargets.push_back(a); out.linkTargets.push_back(b);
    } else if (m == 0xfc || m == 0x0c) {
      std::uint32_t a = r.u32();
      std::snprintf(arg, sizeof arg, ":%x", a); text += arg;
      out.linkTargets.push_back(a);
    }
  }
  if (out.opcode == 0x2a) {                  // [2nd lstr iff 1st empty] + link
    // refetch s1: empty -> consume the fallback string
    Reader r2{image, imageBase, codeOff + 1};
    if (r2.str().empty() && r2.ok) {
      std::string s2 = r.str();
      std::snprintf(arg, sizeof arg, " \"%s\"", s2.c_str()); text += arg;
    }
    std::uint8_t m = r.u8();
    std::snprintf(arg, sizeof arg, " link%02x", m); text += arg;
    if (m == 0xfe) {
      std::uint32_t a = r.u32(), b = r.u32();
      std::snprintf(arg, sizeof arg, ":%x,%x", a, b); text += arg;
      out.linkTargets.push_back(a); out.linkTargets.push_back(b);
    } else if (m == 0xfc || m == 0x0c) {
      std::uint32_t a = r.u32();
      std::snprintf(arg, sizeof arg, ":%x", a); text += arg;
      out.linkTargets.push_back(a);
    }
  }
  if (out.opcode == 0x02) {                  // pathBind: fff iff mode==0
    Reader r2{image, imageBase, codeOff + 11};
    if (r2.ok && r2.u8() == 0) {
      for (int k = 0; k < 3; ++k) {
        std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
      }
    }
  }
  if (out.opcode == 0x39) {                  // targets per mode byte
    Reader r2{image, imageBase, codeOff + 4};
    const std::uint8_t mode = r2.ok ? r2.u8() : 0;
    if (mode == 0xfe || mode == 0xfc || mode == 0x0c || mode == 0xfd) {
      std::uint32_t t1 = r.u32();
      std::snprintf(arg, sizeof arg, " ->%x", t1); text += arg;
      out.linkTargets.push_back(t1);
      if (mode == 0xfe) {
        std::uint32_t t2 = r.u32();
        std::snprintf(arg, sizeof arg, ",%x", t2); text += arg;
        out.linkTargets.push_back(t2);
      }
    }
  }
  if (out.opcode == 0xcf) {                  // link | 0xff u8
    Reader r2{image, imageBase, codeOff + 9};
    const std::uint8_t esc = r2.ok ? r2.u8() : 0;
    if (esc == 0xff) {
      std::snprintf(arg, sizeof arg, " ff %u", r.u8()); text += arg;
    } else {
      std::snprintf(arg, sizeof arg, " link%02x", esc); text += arg;
      if (esc == 0xfe) {
        std::uint32_t a = r.u32(), b = r.u32();
        std::snprintf(arg, sizeof arg, ":%x,%x", a, b); text += arg;
        out.linkTargets.push_back(a); out.linkTargets.push_back(b);
      } else if (esc == 0xfc || esc == 0x0c) {
        std::uint32_t a = r.u32();
        std::snprintf(arg, sizeof arg, ":%x", a); text += arg;
        out.linkTargets.push_back(a);
      }
    }
  }
  if (out.opcode == 0x3d) {                  // {mode,[u8|lstr],lstr,u32}
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t smode = r2.ok ? r2.u8() : 0;
    if (smode == 0) {
      std::snprintf(arg, sizeof arg, " r%u", r.u8()); text += arg;
    } else {
      std::string s = r.str();
      text += " \"" + s + "\"";
    }
    { std::string s = r.str(); text += " \"" + s + "\""; }
    std::uint32_t pc = r.u32();
    std::snprintf(arg, sizeof arg, " ->%x", pc); text += arg;
    out.linkTargets.push_back(pc);
  }
  if (out.opcode == 0x5f) {                  // wcall n×{u8,u32}
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t n = r2.ok ? r2.u8() : 0;
    for (std::uint8_t k = 0; k < n; ++k) {
      std::uint8_t w = r.u8();
      std::uint32_t t = r.u32();
      std::snprintf(arg, sizeof arg, " w%u->%x", w, t); text += arg;
      out.linkTargets.push_back(t);
      if (!r.ok) break;
    }
  }
  if (out.opcode == 0x82) {                  // {chance, ref|fff}
    Reader r2{image, imageBase, codeOff + 2};
    const std::uint8_t ref = r2.ok ? r2.u8() : 0;
    std::snprintf(arg, sizeof arg, " %u", ref); text += arg;
    if (ref == 0xff) {
      for (int k = 0; k < 3; ++k) {
        std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
      }
    }
  }
  if (out.opcode == 0x26) {                  // {u8 sel, f32, [f32], link}
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t sel = r2.ok ? r2.u8() : 0;
    std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
    if (sel == 7 || sel == 8) {
      std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
    }
    std::uint8_t m = r.u8();
    std::snprintf(arg, sizeof arg, " link%02x", m); text += arg;
    if (m == 0xfe) {
      std::uint32_t a = r.u32(), b = r.u32();
      std::snprintf(arg, sizeof arg, ":%x,%x", a, b); text += arg;
      out.linkTargets.push_back(a); out.linkTargets.push_back(b);
    } else if (m == 0xfc || m == 0x0c) {
      std::uint32_t a = r.u32();
      std::snprintf(arg, sizeof arg, ":%x", a); text += arg;
      out.linkTargets.push_back(a);
    }
  }
  if (out.opcode == 0x1f || out.opcode == 0x20) { // {u8 n, n×lstr}
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t n = r2.ok ? r2.u8() : 0;
    for (std::uint8_t k = 0; k < n; ++k) {
      std::string s = r.str();
      text += " \"" + s + "\"";
      if (!r.ok) break;
    }
  }
  if (out.opcode == 0x81) {                  // {u8 sel, [u8 n, n×lstr]}
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t sel = r2.ok ? r2.u8() : 0;
    if (sel <= 2) {
      const std::uint8_t n = r.u8();
      for (std::uint8_t k = 0; k < n; ++k) {
        std::string s = r.str();
        text += " \"" + s + "\"";
        if (!r.ok) break;
      }
    }
  }
  if (out.opcode == 0xac) {                  // {u8 sel, ref|fff, f32}
    Reader r2{image, imageBase, codeOff + 1};
    const std::uint8_t sel = r2.ok ? r2.u8() : 0;
    if (sel == 3) {
      std::snprintf(arg, sizeof arg, " r%u", r.u8()); text += arg;
    } else {
      for (int k = 0; k < 3; ++k) {
        std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
      }
    }
    std::snprintf(arg, sizeof arg, " %.3g", r.f32()); text += arg;
  }
  out.length = static_cast<int>(r.pc - codeOff);
  out.text = text;
  return out;
}

std::vector<TraversalScriptInsn> traversalScriptDisasm(
    std::span<const std::byte> image, std::uint32_t imageBase,
    std::uint32_t codeOff, int maxInsn) {
  std::vector<TraversalScriptInsn> out;
  std::uint32_t p = codeOff;
  for (int i = 0; i < maxInsn; ++i) {
    TraversalScriptInsn in = traversalScriptDecode(image, imageBase, p);
    out.push_back(in);
    if (in.opcode == 0xff || in.length <= 0) break;
    p += in.length;
  }
  return out;
}

// ---------------------------------------------------------------------------
// FUN_004546ac — the surface-contact handler seam (OBSERVED): build a
// synthetic context (memset ~0x32e), bound object = the surface owner,
// +0x21d = event byte, +0x108 = script image base + handlerOff, then
// one synchronous FUN_004388d8 run. Stateless per event — nothing
// persists back to the arena's +0x118 block.
//
// `user` carries the TraversalRuntime (set as SurfaceObjectState::
// scriptUser at load). The surface owner is the runtime's current
// arena — the contact sweep binds cs.surface = &cur->surface.
// ---------------------------------------------------------------------------
void traversalScriptSurfaceHandler(SurfaceObjectState& ctx,
                                   std::uint32_t handlerOff,
                                   std::int32_t eventCode,
                                   std::uint8_t& result,
                                   const SurfaceFxState& fx,
                                   void* user) {
  (void)ctx;
  (void)fx;
  TraversalRuntime* rt = static_cast<TraversalRuntime*>(user);
  if (!rt || !rt->cur || handlerOff == 0) return;

  TraversalScriptState synth;        // the 0x54c6d0 synthetic ctx
  synth.pcImageOff = handlerOff;     // image-relative code offset
  synth.eventByte = static_cast<std::uint8_t>(eventCode & 0xff);
  synth.active = true;
  synth.running = 1;

  TraversalScriptEnv env;
  env.image = std::span<const std::byte>(rt->level.cmiBytes.data(),
                                         rt->level.cmiBytes.size());
  env.imageBase = 4;
  env.playerPos = rt->cs.pos;
  env.rt = rt;
  env.currentArena = rt->cur;
  env.selfArena = rt->cur;           // bound object = surface owner
  env.stateOverride = &synth;
  env.dt = 1.0f / 30.0f;
  env.slideChannel = rt->slideChannel;
  env.slideMode = (rt->slideChannel != 0);
  env.hasContactNormal = (rt->lastContactPoly != nullptr);
  env.diagLog = &rt->scriptDiag;

  TraversalScriptResult sr = traversalScriptRun(env);
  rt->scriptInsnTotal += sr.instructions;
  if (env.slideClear) { rt->slideChannel = 0; rt->eventPriority = 0; }
  rt->slideChannel = env.slideChannel;
  if (sr.error) result |= 0x0;      // diagnostics already logged
}

} // namespace mdk
