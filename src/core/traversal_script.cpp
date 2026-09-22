// traversal_script.cpp — Phase 5H tr_alcmd VM. See the header for the
// evidence map. All opcode semantics below are OBSERVED from
// MDK95.EXE handler disassembly; the operand grammars were validated
// against the real BUILD_A corridor scripts (LEVEL3–LEVEL8).

#include "core/traversal_script.h"

#include <cstdio>
#include <cstring>

#include "core/traversal_runtime.h"
#include "core/player_surface.h"
#include "core/dynamic_objects.h"
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
      (void)r.str();                  // name — matched inside activate
      std::uint8_t k0 = r.u8(), k1 = r.u8();
      float rate = r.f32();
      if (!r.ok) { fail("volact"); return res; }
      if (env.selfArena) {
        // OBSERVED call shape: kind/rate/mask feed surfaceVolumeCreate.
        traversalVolumeActivate(*env.selfArena, id, k1, rate,
                                static_cast<std::uint32_t>(k0),
                                /*noFalloff=*/false);
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
    case 0xe6: {              // spawn3 {x,y,z,u32,u32,class,scOff}
      float x = r.f32(), y = r.f32(), z = r.f32();
      std::uint32_t a = r.u32(), b = r.u32();
      std::string cls = r.str();
      std::uint32_t scOff = r.u32();
      if (!r.ok) { fail("spawn3"); return res; }
      traversalScriptSpawn(env, x, y, z, 0.0f, a ^ b, cls, "", scOff,
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
  // mark[depth]=0; return pops the frame and restores +0x108.
  bool doCall(std::uint32_t target) {
    if (obj.scriptCallDepth >= 4) { fail("Gosub overflow"); return false; }
    const int d = obj.scriptCallDepth;
    obj.scriptRetPc[d] = ptrAt(r.pc);
    obj.scriptSavedPc[d] = obj.field108;
    ++obj.scriptCallDepth;
    obj.scriptMark[d] = 0;
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
  // goto checkpoints as it jumps.
  void doGoto(std::uint32_t target) {
    obj.field108 = ptrAt(target);
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

  if (!r.ok) { v.fail("script read out of bounds"); return; }
  const std::uint32_t insnOff = r.pc;
  const std::uint8_t op = r.u8();
  ++res.instructions;
  if (!r.ok) { v.fail("opcode fetch out of bounds"); return; }

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
    if (f1 & 1) obj.col.flags149 |= 0x2; else obj.col.flags149 &= ~0x2;
    // f1&2 -> +0x14b|8 (ghost); +0x14b byte is unmodelled.
    if (f2 & 1) obj.col.flags149 |= 0x4; else obj.col.flags149 &= ~0x4;
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
  case 0x4e: {                            // subtype set (0x448741)
    for (int i = 0; i < 3; ++i) obj.field120[i] = r.u32();
    if (!r.ok) { v.fail("subtype4e"); return; }
    obj.field11e = 0x4e;
    obj.fieldEC = nullptr;                 // unbind path
    obj.field2a0 = 0; obj.field2a1 = 0;
    obj.field2a8 = 0.0f; obj.field2ac = 0.0f;
    // FUN_00451ee8(obj) — subtype helper; a seam.
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
      std::uint16_t v = r.u16();
      obj.health = static_cast<int>(v);
      obj.healthMirror2a2 = v;                       // +0x2a2 = low16(+0x8)
      if (v >= 0xfde8) {                             // +0x21f = 1 sentinel
        obj.flag21f = 1;
      } else if (v == 0) {                           // +0x8==0 -> FUN_004581a4
        obj.health = 0;                              //   remove (seam — the
        obj.syncCollisionView();                     //   list detach is not
      }                                              //   modelled; health=0
      return;                                         //   makes it inert)
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
    case 0x5a: obj.fieldE8 = resolveVar(r, env, ctx); return;  // +0xe8
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
      const void* rec = imageRef(off, 12);
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

    // ----- connector config family -----
    case 0x96: {                              // {u32,u32}->+0x306/+0x30a
      std::uint32_t a = r.u32(), b = r.u32();
      obj.animRecNear = imageRef(a, 12);      // +0x306 (open anim)
      obj.animRecFar  = imageRef(b, 12);      // +0x30a (close anim)
      // FUN_00438898 lazy-resolve (*ptr==0 -> symbol) is a seam — the
      // door's records carry rate=1.0 (nonzero) so no resolve occurs.
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

    default: {
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "Unrecognised object op 0x%02x at +%x",
                    op, insnOff);
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
    if (obj.field22c > 0.0f) { res.waited = true; return res; }
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
  } else {
    o.col.flags14a = flags & 0xffffu;        // spawn-flags byte (+0x14a)
  }

  // FUN_004566f0 — generic object init, common to every spawned object:
  // default block (health/scale/fields/identity) -> bind the model ->
  // the table-2 "%s$%s" init script -> FUN_0045612c transform rebuild
  // (applies the script's +0x58 scale / +0x5c zBias).
  initObjectDefaults(o);
  o.syncCollisionView();                     // bind +0x0c (elemSet+gate)
  {
    // Key "%s$%s" = homeArena$className. *+0xc is the model record's
    // name — use the deep-copied model's name, falling back to the
    // class/name operand when the model is absent (unresolved).
    std::string cn = o.model.modelName();
    if (cn.empty()) cn = cls.empty() ? name : cls;
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
  ++env.seamsSpawned;
}

// ---------------------------------------------------------------------------
// Read-only decode / disassembly — --script-disasm support. Shares the
// proven operand grammars; unknown opcodes report as "???".
// ---------------------------------------------------------------------------
namespace {

// Operand grammars (identical to the executor's fetch order).
const char* opcodeGrammar(std::uint8_t op) {
  switch (op) {
  case 0x01: case 0x09: case 0xff: case 0xfd: return "";
  case 0x40: return "v";
  case 0x44: case 0x45: case 0x61: case 0x0b: case 0xca:
    return (op == 0x44 || op == 0x45) ? "bb" : "b";
  case 0x46: case 0x47: case 0x48: case 0x7b: case 0x0d: return "bbl";
  case 0x60: return "ffffl";
  case 0x67: return "ffffffl";
  case 0x62: case 0xa8: return "bb";
  case 0x41: case 0x63: return "bbw";
  case 0x0c: case 0xfc: return "n";
  case 0x95: return "ffffwssw";
  case 0x56: case 0xa1: return "fffsw";
  case 0xe6: return "fffwwsw";
  case 0xce: return "wfsw";
  case 0x8e: return "bsbbf";
  case 0x99: case 0x05: return "f";
  case 0xc6: return "sbww";        // obj-init elem-set declaration
  case 0xc7: return "v";           // obj-init +0x104 var write
  case 0xe0: return "b";           // + conditional ff when flag != 0
  case 0x0a: return "sbl";
  case 0x03: case 0x3b: return "l";    // anim bind {u32 imgref}
  case 0x18: return "bs";              // anim sound {u8 mark, str}
  case 0x75: case 0x76: return "w";    // flag-mask / +0x118 target
  default: return nullptr;
  }
}
const char* opcodeName(std::uint8_t op) {
  switch (op) {
  case 0x01: return "ckpt";   case 0x09: return "stop";
  case 0xff: return "end";    case 0xfd: return "ret";
  case 0x40: return "wait";   case 0x44: return "bitset";
  case 0x45: return "bitclr"; case 0x46: return "brSet2";
  case 0x47: return "brSet";  case 0x48: return "brClr";
  case 0x60: return "box2d";  case 0x67: return "box3d";
  case 0x61: return "setObj148"; case 0x62: return "surfop";
  case 0x63: return "surfbind";  case 0xa8: return "surfcfg";
  case 0x0c: return "rgoto";  case 0xfc: return "rcall";
  case 0x95: return "spawn";  case 0x56: return "spawn2";
  case 0xe6: return "spawn3"; case 0xa1: return "spawnNamed";
  case 0xce: return "imgobj"; case 0x8e: return "volact";
  case 0xca: return "setG";   case 0x99: return "ctx30e";
  case 0x05: return "gFloat"; case 0x7b: return "ifPartner";
  case 0x0d: return "linkGate"; case 0xe0: return "deflect";
  case 0x0a: return "brObj11a";  case 0x0b: return "setVar11a";
  case 0x41: return "setVar";
  case 0xc6: return "elemset"; case 0xc7: return "setF104";
  case 0x03: return "animBind"; case 0x3b: return "animLoop";
  case 0x18: return "animSnd";  case 0x75: return "clr148";
  case 0x76: return "animTgt";
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
