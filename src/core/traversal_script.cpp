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
static inline int varIndex(std::uint8_t idx) { return idx < 4 ? idx : 0; }
// Resolve a var-operand given an already-read mode byte. mode 3 =
// inline f32; otherwise a u8 index selects a slot via FUN_00438654.
float resolveVarMode(std::uint8_t mode, Reader& r,
                     TraversalScriptEnv& env, TraversalScriptState& st) {
  if (mode == 3) return r.f32();          // inline f32 (read-op form)
  const int idx = varIndex(r.u8());
  switch (mode) {
  case 0: return env.gVars[idx];                 // 0x540d88 globals
  case 1:                                        // boundObj+0x48
    return env.selfArena ? env.selfArena->objVars48[idx] : 0.0f;
  case 2:                                        // ctx+0x234 locals
  default: return st.locals[idx];                // >=3: caller +0x234
  }                                              // -> local (bounded)
}
float resolveVar(Reader& r, TraversalScriptEnv& env,
                 TraversalScriptState& st) {
  return resolveVarMode(r.u8(), r, env, st);
}

// Var-operand WRITE resolver — FUN_00438654's pointer form. Returns
// the 4-byte slot the mode/index pair selects (the same arrays
// resolveVar reads). A write op has no inline mode — mode selects
// the array, index the slot.
float* resolveVarRef(std::uint8_t mode, std::uint8_t idx,
                     TraversalScriptEnv& env, TraversalScriptState& st) {
  const int i = varIndex(idx);
  switch (mode) {
  case 0: return &env.gVars[i];
  case 1:
    return env.selfArena ? &env.selfArena->objVars48[i] : nullptr;
  case 2:
  default: return &st.locals[i];
  }
}

// Flag-group resolver. Returns a reference to the flag dword the
// 0x44..0x48 family operates on.
std::uint32_t& resolveFlag(std::uint32_t group, TraversalScriptEnv& env,
                           TraversalScriptState& st) {
  switch (group) {
  case 0: return env.gFlags;                    // 0x540d98
  case 1:                                        // boundObj+0x58
    if (env.selfArena) return env.selfArena->flags58;
    return env.gFlags;                           // bounded fallback
  case 5: return st.flagsChild;                 // ctx+0x312
  case 2:                                        // ctx+0x244
  default: return st.flagsLocal;                // 'else' = caller ctx
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
      float secs = resolveVar(r, env, st);  // +0x230=resume, exit
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
      std::uint32_t& f = resolveFlag(grp, env, st);
      if (op == 0x44) f |= (1u << (bit & 31));
      else f &= ~(1u << (bit & 31));
      break;
    }
    case 0x46: case 0x47: case 0x48: {      // branch if bit set/clr
      std::uint8_t grp = r.u8(), bit = r.u8();
      Linkage L;
      if (!readLinkage(r, L)) { fail("flag branch"); return res; }
      const std::uint32_t f = resolveFlag(grp, env, st);
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
      if (float* slot = resolveVarRef(mode, idx, env, st)) {
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
// Object-init interpreter — the FUN_004566f0 table-2 "%s$%s" script.
// Same VM bytecode + linkage rules as FUN_004388d8, but the bound
// object is a DynamicObject (field ops write its +0xNN fields, not the
// arena ctx's). Runs synchronously to completion at spawn.
//
// OBSERVED opcode set implemented (MDK95.EXE handler disassembly):
//   control:  0x01 ckpt / 0x09 stop / 0xff end / 0xfd ret /
//             0x0c rgoto / 0xfc rcall (random-pick -> first, seam)
//   fields:   0x08 +0x4c yaw(i16,neg+360)  0x0b +0x11a  0x49 +0x11b
//             0x10 +0x8/+0x2a2/+0x21f health(u16)  0x6f +0x146(u16)
//             0x32/0x33/0x34 +0x38/+0x3c/+0x40     0x53 +0x58 scale
//             0x54 +0x5c zBias   0x5a +0xe8        0xc6 +0x104
//             0x75 +0x118 anim-target(u16-1)     0x4c +0x110 imgref
//   flags:    0x23 +0x148|2   0x24 +0x148|1  0x29 +0x149|1/+0x14a|0x80
//             0x3f +0x148^0x10(inv)  0x61 +0x148^0x80(inv,+0x54=0)
//             0x74 +0x148 dword |=
//   masks:    0x1f {count,strings} -> +0x2c8 element-name/"ALL" mask
//   vars:     0x41 {mode,idx,u32} -> *FUN_00438654 slot
//   connect:  0x96 {u32,u32}->+0x306/+0x30a anim recs
//             0x97 {4 strs}->+0x316/31a/31e/322 sound names
//             0x98 +0x312 hi nibble  0x99 +0x30e radius
// Unknown/other opcodes halt with a diagnostic (native safety policy —
// the original would desync on a mis-framed stream).
// ---------------------------------------------------------------------------
TraversalScriptResult traversalObjectInitScript(
    TraversalScriptEnv& env, DynamicObject& obj, std::uint32_t codeOff) {
  TraversalScriptResult res;
  if (codeOff == 0) return res;

  // Var mode 1 resolves the bound object's home arena (+0x48). Rebind
  // selfArena to the object's actual home so resolveVar/resolveVarRef
  // hit the right arena even if env.selfArena differs.
  TraversalScriptEnv oenv = env;
  if (obj.arena && obj.arena->owner) oenv.selfArena = obj.arena->owner;

  TraversalScriptState st;                 // transient ctx (locals+stack)
  st.pcImageOff = codeOff;
  Reader r{oenv.image, oenv.imageBase, codeOff};
  const char* name =
      oenv.selfArena ? oenv.selfArena->name.c_str() : "<obj>";

  auto fail = [&](const char* why) {
    res.error = true;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: %s", name, why);
    res.diag = buf;
    if (oenv.diagLog) oenv.diagLog->push_back(res.diag);
  };
  auto doCall = [&](std::uint32_t target) -> bool {
    if (target == 0) return true;
    if (st.callDepth >= 4) { fail("Gosub overflow"); return false; }
    st.retPc[st.callDepth] = r.pc;
    ++st.callDepth;
    r.pc = target;
    return true;
  };
  auto doGoto = [&](std::uint32_t target) { r.pc = target; };
  // Image-relative ref (cmiBase + off). operand==0 -> the image base
  // (cmiBase+0), matching the original's unconditional add.
  auto imageRef = [&](std::uint32_t off, std::size_t n) -> const void* {
    return r.ptr(off, n);
  };

  for (int i = 0; i < 1000; ++i) {
    if (!r.ok) { fail("init read out of bounds"); return res; }
    const std::uint32_t insnOff = r.pc;
    const std::uint8_t op = r.u8();
    ++res.instructions;
    if (!r.ok) { fail("init opcode fetch out of bounds"); return res; }

    switch (op) {
    case 0xff:                              // end
      res.halted = true; return res;
    case 0x09:                              // stop
      res.stopped = true; return res;
    case 0xfd:                              // standalone return
      if (st.callDepth <= 0) { fail("Gosub underflow"); return res; }
      --st.callDepth; r.pc = st.retPc[st.callDepth];
      break;
    case 0x01:                              // ckpt: transient in init
      break;
    case 0x0c: {                            // rgoto {u8 n, n×u32}
      std::uint8_t n = r.u8();
      if (!r.ok || n == 0) { fail("init rgoto"); return res; }
      std::uint32_t tgt = 0;
      for (std::uint8_t k = 0; k < n; ++k) {
        std::uint32_t o = r.u32();
        if (k == 0) tgt = o;
      }
      if (!r.ok) { fail("init rgoto offs"); return res; }
      doGoto(tgt);
      break;
    }
    case 0xfc: {                            // rcall {u8 n, n×u32}
      std::uint8_t n = r.u8();
      if (!r.ok || n == 0) { fail("init rcall"); return res; }
      std::uint32_t tgt = 0;
      for (std::uint8_t k = 0; k < n; ++k) {
        std::uint32_t o = r.u32();
        if (k == 0) tgt = o;
      }
      if (!r.ok) { fail("init rcall offs"); return res; }
      if (!doCall(tgt)) return res;
      break;
    }

    // ----- object field writes -----
    case 0x08: {                            // +0x4c yaw (MOVSX u16)
      std::int16_t v = static_cast<std::int16_t>(r.u16());
      obj.yawDeg = static_cast<float>(v);
      if (obj.yawDeg < 0.0f) obj.yawDeg += 360.0f;   // normalize to [0,360)
      break;
    }
    case 0x0b: obj.field11a = r.u8(); break;         // +0x11a
    case 0x49: obj.field11b = r.u8(); break;         // +0x11b
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
      break;                                         //   makes it inert)
    }
    case 0x6f: obj.spawnId =                    // +0x146 (u16 of u32)
        static_cast<std::uint16_t>(r.u32() & 0xffff); break;
    case 0x32: obj.field38 = resolveVar(r, oenv, st); break;  // +0x38
    case 0x33: obj.field3c = resolveVar(r, oenv, st); break;  // +0x3c
    case 0x34: obj.field40 = resolveVar(r, oenv, st); break;  // +0x40
    case 0x53: {                              // +0x58 scale
      std::uint8_t mode = r.u8();
      if (mode == 0xff) {                     // ramp form {u8,u32,u32}:
        (void)r.u8(); (void)r.u32(); (void)r.u32();   // per-frame ease of
        // +0x58 toward a target — a runtime behavior; no init script
        // uses it, so consume operands only (HYPOTHESIS: single step).
      } else {
        obj.col.scale = resolveVarMode(mode, r, oenv, st);
      }
      break;
    }
    case 0x54: obj.zBias = resolveVar(r, oenv, st); break;    // +0x5c
    case 0x5a: obj.fieldE8 = resolveVar(r, oenv, st); break;  // +0xe8
    case 0xc6: obj.field104 = resolveVar(r, oenv, st); break; // +0x104
    case 0x4c: {                              // +0x110 image ref (0->null)
      std::uint32_t off = r.u32();
      obj.field110 = (off == 0) ? nullptr : imageRef(off, 4);
      break;
    }
    case 0x75:                                // +0x118 anim target word
      // {u32 slot, low u16 used}: +0x118 = (i16)low16 - 1. Shared
      // anim-status word — for XM3 etc. it pre-loads the frame the
      // anim player (FUN_004555bc) runs toward before latching done.
      obj.connAnimLatch = static_cast<std::int16_t>(
          static_cast<std::int16_t>(r.u32() & 0xffff) - 1);
      break;
    case 0x41: {                              // setVar {mode,idx,u32}
      std::uint8_t mode = r.u8(), idx = r.u8();
      std::uint32_t v = r.u32();
      if (!r.ok) { fail("init setVar"); return res; }
      if (float* slot = resolveVarRef(mode, idx, oenv, st)) {
        float f; std::memcpy(&f, &v, 4); *slot = f;
      }
      break;
    }

    // ----- +0x148/+0x149/+0x14a flag ops (byte-addressed) -----
    case 0x23: {                              // +0x148 bit2
      if (r.u8()) obj.col.flags148 |= 0x4; else obj.col.flags148 &= ~0x4u;
      break;
    }
    case 0x24: {                              // +0x148 bit1
      if (r.u8()) obj.col.flags148 |= 0x2; else obj.col.flags148 &= ~0x2u;
      break;
    }
    case 0x3f: {                              // +0x148 bit4 INVERTED
      if (r.u8()) obj.col.flags148 &= ~0x10u; else obj.col.flags148 |= 0x10u;
      break;
    }
    case 0x61: {                              // +0x148 bit7 INVERTED
      if (r.u8()) { obj.col.flags148 &= ~0x80u; }
      else { obj.col.flags148 |= 0x80u; obj.pitchDeg = 0.0f; }  // +0x54=0
      break;
    }
    case 0x29: {                              // +0x149 bit0 / +0x14a bit7
      std::uint8_t v = r.u8();
      if (v) { obj.col.flags148 |= 0x100u; obj.col.flags149 |= 0x1; }
      else   { obj.col.flags148 &= ~0x100u; obj.col.flags149 &= ~0x1; }
      if (v == 2) obj.col.flags14a |= 0x80; else obj.col.flags14a &= ~0x80;
      // +0x149 bit0 clear while ridden -> FUN_00461878 dismount (seam).
      break;
    }
    case 0x74: {                              // +0x148 dword |= operand
      std::uint32_t v = r.u32();
      obj.col.flags148 = static_cast<std::uint16_t>(
          obj.col.flags148 | (v & 0xffffu));
      obj.col.flags149 = static_cast<std::uint8_t>(obj.col.flags148 >> 8);
      obj.col.flags14a |= static_cast<std::uint8_t>((v >> 16) & 0xff);
      break;                                  // +0x14b byte unmodelled
    }

    // ----- element-name mask -> +0x2c8 -----
    case 0x1f: {                              // {u8 count, count×str}
      std::uint8_t n = r.u8();
      for (std::uint8_t k = 0; k < n; ++k) {
        std::string s = r.str();
        if (!r.ok) break;
        for (int e = 0; e < obj.elemSet.count; ++e) {
          const std::string en = obj.model.elemName(e);
          if (en == s || s == "ALL")            // FUN_0042fa50 match /
            obj.col.elemMaskB |= (1u << (e & 31)); //  "ALL" wildcard
        }
      }
      if (!r.ok) { fail("init elemmask"); return res; }
      break;
    }

    // ----- connector config family -----
    case 0x96: {                              // {u32,u32}->+0x306/+0x30a
      std::uint32_t a = r.u32(), b = r.u32();
      obj.connAnimNear = imageRef(a, 12);      // +0x306 (open anim)
      obj.connAnimFar  = imageRef(b, 12);      // +0x30a (close anim)
      // FUN_00438898 lazy-resolve (*ptr==0 -> symbol) is a seam — the
      // door's records carry rate=1.0 (nonzero) so no resolve occurs.
      break;
    }
    case 0x97: {                              // {4 strs}->+0x316..+0x322
      std::string s[4];
      for (auto& x : s) { x = r.str(); if (x == "NONE") x.clear(); }
      // OBSERVED field order: op0->+0x31a op1->+0x322 op2->+0x316
      // op3->+0x31e (scrambled). "NONE" sentinel clears the slot.
      obj.connSound31a = s[0]; obj.connSound322 = s[1];
      obj.connSound316 = s[2]; obj.connSound31e = s[3];
      break;
    }
    case 0x98: {                              // +0x312 hi nibble
      std::uint32_t v = r.u32();
      obj.connState = static_cast<std::uint8_t>(
          (obj.connState & 0x0f) | (v & 0xf0));
      break;
    }
    case 0x99: obj.connRadius = r.f32(); break; // +0x30e

    default: {
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "Unrecognised objinit op 0x%02x at +%x",
                    op, insnOff);
      fail(buf);
      return res;
    }
    }
  }

  {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "Objinit %s looped %d commands, off %lx", name,
                  res.instructions, static_cast<unsigned long>(r.pc));
    res.diag = buf;
    if (oenv.diagLog) oenv.diagLog->push_back(res.diag);
    res.error = true;
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
    o.connAnimNear = nullptr;                // +0x306
    o.connAnimFar = nullptr;                 // +0x30a
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
  case 0xe0: return "b";           // + conditional ff when flag != 0
  case 0x0a: return "sbl";
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
