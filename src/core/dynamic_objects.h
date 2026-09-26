// Phase 5E — dynamic collision objects: runtime model records,
// DTI-record spawn, arena +0x68 list attachment, transform/AABB
// rebuild and ride displacement.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md §"Phase 5E" for the full
// evidence chain):
//
//   Geometry source records (CMI table-1 data region and MTO
//   region-A array-B targets share ONE layout, CODE-CORROBORATED by
//   the two call sites):
//
//     record base:   u32 flag             — passed to FUN_00428400 as
//                                          a separate register arg
//                                          (EDX = *(base)); NOT part
//                                          of the parsed stream.
//     stream at +4:  u32 nameCount        — read UNCONDITIONALLY
//                    nameCount x 16B {char[12] name, u32 tag}
//                                          (10 bytes copied per name)
//                    if flag != 0:
//                      u32 elemCount
//                      elemCount x element:
//                        char[12] name     -> runtime elem +0x00
//                        byte[12] field2   -> runtime elem +0x20
//                        u32 vertCount     -> runtime elem +0x0c
//                        f32 verts[vc]     -> runtime elem +0x14, and
//                          FUN_00459d54 min/max -> elem +0x2c localAabb
//                        u32 triCount      -> runtime elem +0x10
//                        byte tris[tc*0x24]-> runtime elem +0x18
//                        byte[0x18] trailer — skipped ONLY on this path
//                    else (flag == 0):
//                      ONE anonymous element (elemCount forced to 1;
//                      no name/field2 copies, no trailer):
//                        u32 vertCount, verts, u32 triCount, tris
//                    byte[0x18] gap        — always skipped
//                    u32 refPointCount     — <= 8 (error path above)
//                    f32 refPoints[rc][3]  -> record +0x24..0x84
//
//     Tail (always): record +0xb = 0xff body-element index, then for
//     each element named "XG1_BODY" +0xb = index; for each element
//     named "XG1_HEAD" record +0xc |= 1<<index.
//
//   Model resolution (FUN_004286c8 / FUN_00403498):
//     CMI table[1] {name, u32 value}; value != 0 -> geometry record at
//     image+value (file offset 4+value). value == 0 -> unresolved
//     (+0xa = 1), later resolved from the level .MTO: for each overlay
//     block, region-A array-B records {name[8], u32 off} are matched
//     by name; the geometry record base is tA + off.
//     FUN_00403720 then DEEP-COPIES the model per spawned object.
//
//   Spawn (FUN_00456808), per 0x24-byte DTI arena sub-record:
//     type 2 "HotGen": fields[0] = enemyIdx<<16 | spawnId (the index
//       half is OR-ed in at load by matching the record's name against
//       the CMI enemy table), fields[2..4] = pos floats. Dedup on
//       (enemyIdx, spawnId, exact pos) within the arena list. Object:
//       +0x04 = enemyIdx, +0x146 = spawnId, +0x10..0x18 = pos,
//       +0x180..0x188 = prevPos, +0x60 = arena, init, +0x11c = 7.
//       Script key "%s$%s_%d" (arena$model_spawnId).
//     type 4 "HotPick": fields[0] = modelIdx (overwrites at load),
//       fields[2..4] = pos. Dedup on (modelIdx, pos). +0x08 = 1,
//       +0x148 dword |= 0x2008a0 (bytes: +0x148=0xa0, +0x149=0x08,
//       +0x14a=0x20 — mover bit). Script key "%s$%s". If the model
//       name is "SW_DUMMY", elements named "SW_DUMMY" get their bit
//       set in +0x2c8 (element-disable mask).
//
//   Arena +0x68 list (FUN_0045cffc / FUN_0045cf90 / FUN_004574d0):
//     singly-linked through object +0x00; push-front on spawn and on
//     portal transfer; +0x60 = current arena, +0x2bc = pending arena.
//     Allocation is pool-based: FUN_0045cffc pops the global freelist
//     (0x540ed0) before falling back to the inactive-arena scavenge,
//     and FUN_0045cf90 memsets the record and pushes it back. The
//     per-arena corpse sweep (FUN_0045cf18, called after each arena's
//     FUN_004572ac pass in the frame loop) unlinks+frees every
//     +0x06==0 record — teardown (FUN_0045828c) only marks a corpse;
//     this sweep is what removes it from the list.
//
//   Init (FUN_004566f0): health +0x08 = 10, scale +0x58 = 1.0,
//     identity matrix, then FUN_0045612c transform/AABB rebuild.
//
//   Transform rebuild (FUN_0045612c, collision-relevant core):
//     object AABB +0x198 seeded degenerately: min = {old minZ x3},
//     max = {old maxZ x3} (OBSERVED quirk — components reset from the
//     previous z bounds, then grown by element unions).
//     +0x148 & 0x40: xform[3x3] = +0x302..0x322 * scale;
//                    origin = {x, y, z + zBias(+0x5c)}.
//     else:          xform = FUN_0046b2f8(pitch +0x54, bank +0x13c,
//                    yaw +0x4c, scale +0x58); origin = pos (NO zBias).
//     per element NOT in +0x2c8: FUN_00459e40 — transform the 8 local
//     AABB corners by the matrix, min/max -> elem +0x44 world AABB,
//     union into object +0x198.
//     (+0x148 & 1 | & 0x80 gate a steering/easing pre-pass that only
//      derives the angle fields fed to the matrix — behavior state,
//      not collision state; objects with +0x148 bit7 set, e.g. type-4
//      0x2008a0, skip it entirely. Documented seam — see docs.)
//
//   FUN_0046b2f8 (verified): angles are DEGREES (FUN_00437f98 =
//     angle * pi/180 -> {sin,cos}); row-major 3x3, scale baked in:
//       row0: c2c3   -s1s2c3-c1s3   -c1s2c3+s1s3
//       row1: c2s3   -s1s2s3+c1c3   -c1s2s3-s1c3
//       row2: s2      s1c2           c1c2
//     where (s1,c1)=pitch +0x54, (s2,c2)=bank +0x13c, (s3,c3)=yaw +0x4c.
//     Matches the native CollisionObject contract:
//       world = M.local + origin; local = M^T.d / scale^2.
//
//   Ride displacement (FUN_004572ac tail, per updated object):
//     if object == DAT_00540dc0 (cs.rideObj): player pos +=
//     pos - prevPos (+0x10..0x18 - +0x180..0x188) and player yaw +=
//     yaw - prevYaw (+0x4c - +0x50); THEN prevPos = pos, prevYaw =
//     yaw. Runs before the floor probe -> same-frame following.
//
// NATIVE PORT DECISIONS:
//   - RuntimeModel owns the per-object deep-copied geometry; the
//     CollisionElement views point into its vectors (rebound after
//     parse/copy — mirrors FUN_00403720's fresh arrays).
//   - DynamicArena owns objects via std::list (stable addresses —
//     CollisionObject::next chain links them exactly like +0x68).
//   - The script VM, surface effects, steering pre-pass and render-
//     only fields (parent matrix +0x7c, screen bounds, ref points)
//     stay outside this layer; their collision-visible outputs are
//     represented as fields the caller/script may write.
//   - flags148 is u16 covering bytes +0x148/+0x149 (the original tests
//     it as a word); flags149/flags14a mirror individual bytes — keep
//     them coherent when writing dword-style flag values (helpers do).
//
#ifndef MDK_CORE_DYNAMIC_OBJECTS_H
#define MDK_CORE_DYNAMIC_OBJECTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/collision_query.h"
#include "core/cmi_directory.h"
#include "core/dti_structure.h"
#include "core/player_surface.h"
#include "core/mto_directory.h"

namespace mdk {

// ---------------------------------------------------------------------------
// Runtime model — the collision-used fields of the original's
// deep-copied model record (object +0x0c; source: FUN_00428400 parse,
// FUN_00403720 copy).
// ---------------------------------------------------------------------------

struct RuntimeModel {
  struct NameRec {                    // file name-table record (16B)
    std::array<char, 12> name{};      //   char[12] (10 copied runtime)
    std::uint32_t tag = 0;            //   u32 tag
  };

  std::uint32_t flag = 0;             // the record's flag u32
  std::vector<NameRec> names;         // name table (all records)

  // Per-element owned storage (parallel to `elems`; rebound by
  // rebind() after parse/copy so the views always point here).
  std::vector<std::array<char, 12>> elemNames;
  std::vector<std::array<std::byte, 12>> elemField2;  // raw +0x20 field
  std::vector<std::vector<float>> elemVerts;          // +0x14 backing
  std::vector<std::vector<std::uint8_t>> elemTris;    // +0x18 backing

  // Collision views — elems[i].verts/tris alias elemVerts[i]/
  // elemTris[i]; .aabb = world (+0x44), .localAabb = local (+0x2c).
  std::vector<CollisionElement> elems;

  float refPoints[8][3]{};            // record +0x24..0x84
  std::uint32_t refPointCount = 0;

  int bodyElemIndex = -1;             // record +0xb (0xff default)
  std::uint32_t headElemMask = 0;     // record +0xc

  // Re-point every view at the owned vectors. Required after the
  // vectors are (re)built or the model is copied — mirrors the fresh
  // allocations FUN_00403720 performs.
  void rebind();

  // +0x1c/+0x20 record view for the collision query.
  CollisionElementSet elementSet() const {
    return {static_cast<std::int32_t>(elems.size()), elems.data()};
  }

  // Printable element name (NUL-trimmed).
  std::string elemName(std::size_t i) const;
  // Printable model name = first name-table entry (the enemy-table
  // name when the record carries one).
  std::string modelName() const;
};

// FUN_00428400 — parse a geometry record (record base at `rec`,
// flag u32 included; `end` bounds every counted walk).
// Returns nullopt on any inconsistent bound (native hardening — the
// original trusts the stream).
std::optional<RuntimeModel> parseGeometryRecord(
    const std::uint8_t* rec, const std::uint8_t* end);

// FUN_00403720 — deep copy: fresh element array + fresh vertex/
// triangle storage, views rebound to the copy.
RuntimeModel deepCopyModel(const RuntimeModel& src);

// ---------------------------------------------------------------------------
// Enemy table — FUN_004286c8's table-1 product (0x88-stride records,
// cap 0x50). Entries keep name + image-relative geometry offset;
// value == 0 marks the MTO-deferred form (+0xa = 1 in the original).
// ---------------------------------------------------------------------------

struct EnemyTable {
  struct Entry {
    std::string name;
    std::uint32_t dataImageOff = 0;   // CMI table-1 value (img-rel)
    bool unresolved = false;          // value == 0 -> MTO array-B
  };
  std::vector<Entry> entries;

  // First index whose name matches (the original compares C strings);
  // -1 when absent.
  int indexOf(const std::string& name) const;
};

// Build the enemy table from a parsed CMI directory's table[1].
EnemyTable buildEnemyTable(const CmiDirectory& cmi);

// Locate the geometry record bytes for enemy-table index `idx`:
//   - resolved entries:   CMI file at 4 + dataImageOff, bounded by
//                         the directory's dataRegionEnd;
//   - unresolved entries: the level .MTO — every block's region-A
//                         array-B is searched by name; the record
//                         base is regionAOffset + fieldAt0x08.
// Returns the {flag u32 + data} record span (bounds-checked) or
// nullopt.
std::optional<std::span<const std::uint8_t>> enemyModelData(
    const EnemyTable& enemies, int idx,
    std::span<const std::byte> cmiFile, const CmiDirectory& cmiDir,
    const MtoDirectory* mtoDir, std::span<const std::byte> mtoFile);

// ---------------------------------------------------------------------------
// Dynamic object + arena attachment
// ---------------------------------------------------------------------------

struct DynamicArena;
struct TraversalArena;

// Runtime object — CollisionObject is the +0x68 list node consumed by
// the query; the rest mirrors the update-side record fields the
// collision loop reads or writes.
struct DynamicObject {
  CollisionObject col{};              // +0x00 next, gates, masks, AABB,
                                      // xform/origin/scale, baseZ
  RuntimeModel model{};               // owned deep copy (+0x0c target)
  CollisionElementSet elemSet{};      // +0x0c record {count, elems}
  // +0x6c..+0x45e — the object's surface-effect block: the FUN_0045d174
  // sweep's surface dispatch (FUN_0040b5d0) writes handler state and
  // surface records here (the same block tr_alcmd surface ops fill).
  SurfaceObjectState surface;
  ~DynamicObject();

  // +0x10/+0x14/+0x18 world position. pos[2] IS +0x18 == col.baseZ —
  // keep them equal (setPosition does; the original has one field).
  float pos[3] = {0, 0, 0};
  float prevPos[3] = {0, 0, 0};       // +0x180..0x188
  // +0x18c/+0x190/+0x194 — per-frame velocity: the FUN_004572ac tail
  // writes (pos - prevPos) * (1.0/DAT_0049b6f0) post-anim, before the
  // prevPos latch. DAT_0049b6f0 = 1.0, so this is the raw frame
  // displacement. OBSERVED 0x45737b.
  float field18c[3] = {0, 0, 0};
  // +0x28/+0x2c/+0x30 — object velocity. FUN_0045bac0 integrates
  // (vel + +0x294 impulse) * dt and applies +0x44 drag each frame
  // (OBSERVED, Phase 11B). The FUN_00432f84 charged-kill displaces
  // the corpse by writing {20*cos,20*sin}(yaw) here — a velocity fling,
  // not a display offset (the corpse then sweeps under physics).
  float field28 = 0.0f;               // +0x28 — vel.x
  float field2c = 0.0f;               // +0x2c — vel.y
  float field30 = 0.0f;               // +0x30 — vel.z
  float yawDeg = 0.0f;                // +0x4c
  float prevYawDeg = 0.0f;            // +0x50
  float pitchDeg = 0.0f;              // +0x54 (matrix arg 1)
  float bankDeg = 0.0f;               // +0x13c (matrix arg 2)
  float zBias = 0.0f;                 // +0x5c (raw-matrix tz fold)
  float rawMatrix[9] = {};            // +0x302..0x322 (+0x148&0x40 src)
  std::uint16_t enemyIndex = 0;       // +0x04
  std::uint16_t spawnId = 0;          // +0x146
  int health = 0;                     // +0x08 — nonzero gates col.model
  int behaviorByte = 0;               // +0x11c (=7 for type-2)

  // Script-spawn metadata (tr_alcmd 0x95/0x56/0xa1/0xce/0xe6 family,
  // FUN_00454894). The original binds a class-table index + name
  // record + script offset; this port preserves them as strings/off
  // while the class's native behavior stays a documented seam.
  std::string scriptClass;            // class name (XCORDOOR/XTUR/…)
  std::string scriptName;             // object/arena name (s2)
  std::uint32_t scriptOff = 0;        // CMI image offset of its script
  int scriptVariant = 0;              // which spawn opcode created it

  DynamicArena* arena = nullptr;        // +0x60
  DynamicArena* pendingArena = nullptr; // +0x2bc
  // +0x2b8 — bound object used as the controlalien broadcast mode-9
  // target (FUN_00438094 dispatches to it directly).
  DynamicObject* field2b8 = nullptr;

  // Arena-connector (door) state — FUN_00457738, gated by
  // col.flags14a & 0x10 (the tr_alcmd 0x95 spawn writes the dword
  // +0x148 = 0x1108000, so +0x14a = 0x10). +0x302 aliases the
  // rawMatrix region only when +0x148 & 0x40 is clear — for a
  // connector it is the destination arena record pointer instead.
  TraversalArena* connDest = nullptr;   // +0x302 (connector only)
  std::uint8_t connState = 0;           // +0x312 — 8 closed / 2 opening
                                      //    / 1 open / 4 closing; high
                                      //    nibble bits 0x40/0x10 are
                                      //    sub-flags (mask +0x313 bit0
                                      //    variant, collision toggle)
  std::uint8_t connStateHi = 0;         // +0x313 — sub-flag byte (bit0
                                      //    gates the closed-state LOCK
                                      //    mask choice)
  float connRadius = 0.0f;              // +0x30e — proximity radius
  const void* animRecNear = nullptr;   // +0x306 — open anim record
                                      // (connector union member)
  const void* animRecFar = nullptr;    // +0x30a — close anim record
                                      // (connector union member)

  // Generic object animation state — FUN_004555bc driver +
  // FUN_00455890 applier (OBSERVED, Phase 11A/G5). These fields are
  // NOT connector-specific: any object may bind an anim record via
  // script ops 0x03 (one-shot) / 0x3b (loop) — the connector path was
  // simply the first consumer. The record view itself is the image
  // span pointed at by animRec (see object_animation.h).
  const void* animRec = nullptr;       // +0x114 — active anim record
  float animAcc = 0.0f;                // +0xdc — frame accumulator;
                                       // op 0x03/0x3b arm it at -1.0
  float animRate = 0.0f;               // +0xe0 — rate mult (30.0f)
  // +0xe4 = applied frame index (i16; the original reads it as the
  // hi16 of the +0xe2 dword — the low half is never independently
  // written). +0x118 = target/done word (hi16 of the +0x116 dword —
  // low half likewise unused). The connector transition writes both
  // to 0xffff (-1: run-to-end); the player latches +0x118 = 0xff00 on
  // completion. "done" == (animLatch == -256) i.e. 0xff00.
  std::int16_t animFrame = 0;          // +0xe4
  std::int16_t animLatch = 0;          // +0x118
  // FUN_004555bc anim-done test: +0x114 null OR +0x118 == 0xff00.
  bool animDone() const {
    return animRec == nullptr ||
           static_cast<std::uint16_t>(animLatch) == 0xff00u;
  }
  // +0x140/+0x144 — animation sound marker (OBSERVED, FUN_004555bc):
  // op 0x18 {u8 mark, str name} stores the bytecode string pointer at
  // +0x140 and mark-1 at +0x144. When the +0xdc accumulator crosses
  // the mark the original emits the named sound once and clears
  // +0x140. Audio is a documented seam; the port keeps the name text
  // and the consume-on-cross state so timing/flow stay faithful.
  std::string animSoundName;           // +0x140 ("" = none)
  std::int16_t animSoundMark = 0;      // +0x144 — trigger frame-1
  // +0x294/+0x298/+0x29c — animation root-motion impulse accumulator
  // (OBSERVED, FUN_00455890): each applied frame's rootKey (a model-
  // space displacement) is rotated by the object 3x3 and accumulated
  // here scaled by 1/DT (x30). FUN_0045bac0's integration consumes
  // the impulse — suppressed while +0x14b bit7 is set.
  float animImpulse[3] = {0, 0, 0};
  std::uint32_t connMaskLock = 0;       // +0x326 — "LOCK"-named elems
  std::uint32_t connMaskHC = 0;         // +0x32a — "HC*"-named elems
  // 0x97 sound-name slots (+0x316/+0x31a/+0x31e/+0x322). The original
  // stores bytecode string pointers, clearing a slot whose operand is
  // the "NONE" sentinel (0x497c54); this port keeps the text ("" = no
  // sound). Sound playback itself is a documented seam.
  std::string connSound316;
  std::string connSound31a;
  std::string connSound31e;
  std::string connSound322;

  // Object-init script (FUN_004566f0's table-2 "%s$%s" lookup) targets.
  // These are generic object fields the tr_alcmd init family writes;
  // initObjectCollision applies the FUN_004566f0 default block first.
  float field38 = 0.0f;              // +0x38 — 50.0 default (0x32 target)
  float field3c = 0.0f;              // +0x3c — 10.0 default (0x33)
  float field40 = 0.0f;              // +0x40 — 15.0 default (0x34)
  float field44 = 0.0f;              // +0x44 — 64.0 default
  float field48 = 0.0f;              // +0x48 — 32.0 default
  std::uint32_t healthMirror2a2 = 0; // +0x2a2 — 0x10's low-16 +0x8 mirror
  std::uint8_t flag21f = 0;          // +0x21f — set when +0x8 >= 0xfde8
  // Phase 5N — punch/homing fields. +0x21e is the "was punched" mark
  // (FUN_00432f84 writes 0xff on a hit). +0x302/+0x306 alias the
  // connector/rawMatrix region for punchable/homing objects: the
  // original stores a homing name-prefix char* at +0x302 and a digit
  // offset at +0x306, consumed by the FUN_0045f634 element predicate
  // (homing weapons 1/3 + the punch element scan).
  std::uint8_t field21e = 0;         // +0x21e — punch-received mark;
                                     // Phase 10A: last-hit element+1
  std::string homingPrefix;          // +0x302 — element-name prefix
                                     // (init opcode 0xc6 writes it)
  int homingDigitOfs = 0;            // +0x306 — digit check offset
                                     // (init opcode 0xc6 byte operand)
  std::uint8_t field11a = 0;         // +0x11a — 0x0b target
  std::uint8_t field11b = 0;         // +0x11b — 0x49 target
  const void* field110 = nullptr;    // +0x110 — 0x4c image-ref target;
                                     // the death boundary hands it to
                                     // the +0x108/+0x230 script PCs
  float field104 = 0.0f;             // +0x104 — 0xc7 target (consumer UNKNOWN)
  float fieldE8 = 0.0f;              // +0xe8 — 1.0 default, 0x5a target
  float field2c0 = 0.0f;             // +0x2c0 — 1.0 default
  float field2c4 = 0.0f;             // +0x2c4 — 1000.0 default; the
                                     // FUN_00460d44 whole-object damage
                                     // gate (aux <= +0x2c4 applies)

  // --- Phase 10A — projectile impact/damage marks (FUN_0045f9b8 /
  // FUN_00460d44 write set). +0x210..+0x218 is the last-hit point,
  // +0x21c the last-hit element+1, +0x21d the damage source byte
  // (shot type / excl mask / punch state), +0x220 the hit tri,
  // +0x224/+0x228 the incoming yaw/pitch. ---
  float field210[3] = {0, 0, 0};     // +0x210 — last-hit position
  std::uint8_t field21c = 0;         // +0x21c — last-hit element+1
  std::uint8_t field21d = 0;         // +0x21d — damage-source byte
  int field220 = 0;                  // +0x220 — last-hit tri index
  float field224 = 0.0f;             // +0x224 — incoming yaw
  float field228 = 0.0f;             // +0x228 — incoming pitch

  // +0x30e — per-element int16 damage pool (the FUN_00460d44 element
  // pass decrements it, clamped at 0; a 0 crossing latches the dead
  // element into +0x21c/+0x220). NOT whole-object health (+0x08) —
  // the two are independent: element death only marks the element.
  // Provenance (OBSERVED): object-init opcode 0xc6 fills all eight
  // slots with the low 16 bits of its dword operand; it is the only
  // writer found. The +0x30e region aliases connRadius (0x99) and the
  // 0x97 sound-name pointers on connectors — elements and the
  // connector union never coexist (OBSERVED: the only real 0xc6 user,
  // LEVEL3 HMO_1$XH1_DOOR, is not a connector).
  std::vector<std::int16_t> elemHp;
  // +0x31e — the second inline int16 element pool (parallel to
  // +0x30e). The punch's element-survived path copies elemThresh[e]
  // into the +0x118 pseudo-object's +0x2a2 when it is <= 900 (the
  // event-latch arm). Same 0xc6 provenance as elemHp.
  std::vector<std::int16_t> elemThresh;
  // +0x30a — the 0xc6 opcode's second dword operand (role UNKNOWN;
  // no consumer found). Aliases animRecFar on connectors.
  std::uint32_t field30a = 0;
  // +0x150 — per-object impact-sound name pointer (OBSERVED): the
  // FUN_00437444 survived-hit callsites pass it as the EBX arg; a
  // null/empty name falls back to the random RICO1/2/3 pick. Stored
  // as an int32 marker — the name data itself is not ported.
  std::int32_t field150 = 0;
  // +0x154 — second name/table pointer in the same CMI-offset family
  // (the save fixups resolve +0x150/+0x154/+0x15c identically);
  // consumer UNKNOWN. Stored as an int32 marker like +0x150.
  std::int32_t field154 = 0;

  // Death-boundary script handoff (FUN_00458140): when +0x110 is set
  // the object keeps a deferred script — field11e cleared, health
  // zeroed, wait cleared, +0x148 |= 0x20 (the dead flag the update
  // scans skip), and +0x110 is copied to BOTH script PCs before the
  // gate clears. +0x108/+0x230/+0x22c are consumed by the per-object
  // tr_alcmd VM tick (traversalObjectScriptTick — FUN_004388d8's
  // object-bound form).
  const void* field108 = nullptr;    // +0x108 — object script PC+gate
  // +0x10c — last remotely-issued call PC (FUN_00438094's mode-7
  // dedup reads it; FUN_004382e0's remote call/gosub writes it).
  const void* field10c = nullptr;
  const void* field230 = nullptr;    // +0x230 — resume/wait PC
  float field22c = 0.0f;             // +0x22c — script wait seconds
                                     // (cleared on death)
  std::uint8_t field11e = 0;         // +0x11e — subtype; cleared on
                                     // death, set to the op's id by
                                     // op 0x4e (OBSERVED 0x44879e)

  // --- Phase 11A — persistent object-script ctx (+0x234..+0x26c
  // inside the object; the +0x108/+0x22c/+0x230 PCs and the +0x21d/
  // +0x21e event+running marks are the fields above / field21d /
  // field21e). Stack PCs are image pointers like +0x108 itself.
  // OBSERVED from the FUN_004388d8 opcode handlers. ---
  float scriptLocals[16] = {};       // +0x234 — ctx f32 locals
                                     // (var mode 2)
  std::uint32_t scriptFlagsLocal = 0;// +0x244 — ctx flag dword
                                     // (flag group 2 / 'else')
  int scriptCallDepth = 0;           // +0x248 — event-call depth
                                     // (cap 4; overflow reports
                                     // "Gosub underflow/overflow")
  const void* scriptRetPc[4] = {};   // +0x24c — return PCs
  const void* scriptSavedPc[4] = {}; // +0x25c — saved +0x108 per level
  std::uint16_t scriptMark[5] = {};  // +0x26c — per-level marks; the
                                     // call tail clears
                                     // mark[depth+1] (post-increment)
                                     // so index 4 (+0x274) is reachable
  std::uint32_t scriptFlagsChild = 0;// +0x312 dword — flag group 5;
                                     // aliases connState's byte on
                                     // connectors (the ctx and
                                     // connector layouts overlap)
  // +0x312 — FUN_004585c4 mover child pointer (the SW_CHUTE spawned
  // by the hop branch). The original holds the child DynamicObject*
  // in the same dword the script ctx reads as flag group 5 and the
  // connector reads as its phase byte — three aliased views of one
  // field. The port keeps them separate; a mover's scripts do not
  // reach flag group 5 in BUILD_A, and connector/mover flag bits are
  // disjoint (+0x14a 0x10 vs 0x20).
  DynamicObject* moverChild = nullptr;

  // --- Phase 11A — path binding (op 0x02, handler 0x438e7c) ---
  // +0xec is the bound path record (image ptr); nonzero gates the
  // FUN_00456d28 follower and selects op 0x66's else-linkage.
  const void* fieldEC = nullptr;     // +0xec — path record
  float fieldF0 = 0.0f;              // +0xf0 — path frame
  float fieldF4[3] = {0, 0, 0};      // +0xf4..0xfc — lateral offset
  std::int16_t fieldE6 = -1;         // +0xe6 — path sync cursor
                                     // (0xffff reset by op 0x02;
                                     // >=0 = waypoint target the
                                     // follower advances +0xf0 toward;
                                     // halts when +0xe6 ==
                                     // FRNDINT(+0xf0))
  float field100 = 0.0f;             // +0x100 — face-travel yaw bias
                                     // (FUN_00456d28: bearing + this,
                                     // single ±360 wrap)
  // +0x302/+0x306/+0x30a/+0x30e — generic scratch dword block aliased
  // per owner: path speed lanes (+0x14b&0x10 — +0x302 approach
  // setpoint, +0x306 far / +0x30a mid / +0x30e near speed), orbit
  // (+0x14a&0x40 — +0x302 phase, +0x306 radius, +0x30a rate,
  // +0x30e target yaw), command runner (+0x14b&0x40 — +0x302 target
  // pos ptr, +0x306 fuse timer) and the FUN_0045897c command bodies
  // (+0x30a command id, +0x30e tick countdown, +0x302/+0x306 i32
  // counters). These alias the connector union (connDest/
  // animRecNear/animRecFar/connRadius), homingPrefix/homingDigitOfs,
  // elemHp — real objects never mix owners (OBSERVED). Stored as raw
  // dword bits like the original; f32 views go through bit_cast.
  std::uint32_t field302 = 0;        // +0x302 bits (f32 setpoint /
                                     // orbit phase; i32 cmd counters)
  std::uint32_t field306 = 0;        // +0x306 bits (f32 lane far /
                                     // orbit radius / runner fuse;
                                     // i32 dropper count)
  const float* field302ptr = nullptr;// +0x302 — pointer view (command
                                     // runner target; FUN_0046aa30
                                     // writes the player pos here).
                                     // The original aliases the same
                                     // dword — a 64-bit pointer can't
                                     // share u32 storage, so the port
                                     // keeps a parallel field; owners
                                     // always arm before reading.
  std::int32_t field30e = 0;         // +0x30e (i32 view — command
                                     // tick countdown; orbit/lane
                                     // f32 uses bit_cast)
  // +0x138 — subtype leader/attach object pointer AND the tick-hold
  // gate: FUN_004533d4 only decrements +0x11c when +0x138 == 0, and
  // subtypes 1/0x1e dereference it as the leader object (OBSERVED).
  DynamicObject* field138 = nullptr;
  // +0x1c..+0x24 — orbit/converge anchor: FUN_00457ab8's swing center
  // and FUN_004599e8's lerp-away origin (op 0xe2 writes it).
  float field1c[3] = {0, 0, 0};
  // +0x34 — per-object speed (subtypes 0x3d timed-shot, 0x58 speed
  // ramp, the 0x2b/0x4e/0xc5 steering tail).
  float field34 = 0.0f;
  // +0x11f — speed-ramp hold flag (subtype 0x58: nonzero pins the
  // target to +0x38 instead of 0).
  std::uint8_t field11f = 0;
  // +0x12c/+0x130/+0x134 — follow/attach offsets (subtype 1 leader
  // follow: side/forward/vertical; subtype 0x4e step target).
  float field12c[3] = {0, 0, 0};
  // (chain-member index is +0x146 = spawnId — the hi16 of the +0x144
  // dword; subtype 0x1e scans same-arena objects by leader+index)
  // +0x1b0..+0x1cb — world-space refpoints: the transform rebuild
  // (FUN_0045612c) transforms the model's up-to-8 local refpoints
  // here; subtype 0x4a (refpoint-attach) reads them.
  float worldRef[8][3] = {};
  // +0x276/+0x277 — refpoint indices bound by op 0x4a (self / target).
  std::uint8_t field276 = 0;
  std::uint8_t field277 = 0;
  // +0x278 — the op-0x4a bound object (subtype 0x4a attach target).
  DynamicObject* field278 = nullptr;
  // +0x158 — spawned FX/child object written by the command bodies
  // (FUN_00402160 seam child). +0x15c — its model-name token.
  DynamicObject* field158 = nullptr;
  std::string field15c;
  // +0x2b0 — floor-contact handle from the vertical sweep
  // (FUN_0045bac0 stores FUN_0045d174's hit poly; released via
  // FUN_00412ef0 conveyor query next frame).
  const CollisionPoly* field2b0 = nullptr;
  // +0x2b4 — vertical sweep contact node (bounce reflects
  // +0x28..+0x30 about its +0x00 plane normal when +0x14b&0x20).
  const CollisionNode* field2b4 = nullptr;
  // +0x27c..+0x290 — post-move clamp box (FUN_0045d174 clamps pos
  // into it when valid: min <= max per axis).
  float clampBox[6] = {0, 0, 0, 0, 0, 0};
  // +0x2d0/+0x2d1 — orbit state flags (FUN_00457ab8 sets both to 1
  // each frame). +0x2d2..+0x2e6 — orbit's saved {pos, center} copy.
  // Op 0xf2 (0x451987) extends the block: flag!=0 writes 12 f32
  // through +0x2d2..+0x301 (field2d2 + field2ea) and sets
  // +0x2d0=1/+0x2d1=0xff; flag==0 clears only +0x2d1.
  std::uint8_t field2d0 = 0;
  std::uint8_t field2d1 = 0;
  float field2d2[6] = {0, 0, 0, 0, 0, 0};
  float field2ea[6] = {0, 0, 0, 0, 0, 0};  // +0x2ea..+0x301 (op 0xf2 tail)

  // --- op 0x4e (handler 0x448741): subtype set — three dwords to
  // +0x120..+0x128, +0x11e = 0x4e, path unbound (+0xec = 0), and the
  // +0x2a0/+0x2a1/+0x2a8/+0x2ac mover block cleared (OBSERVED). ---
  float field120[3] = {0, 0, 0};       // +0x120..0x128 — anchor
                                       // position (op 0x4e writes the
                                       // three target floats here too,
                                       // OBSERVED 0x448741)
  std::uint8_t field2a0 = 0;           // +0x2a0
  std::uint8_t field2a1 = 0;           // +0x2a1
  float field2a4 = 0.0f;               // +0x2a4
  float field2a8 = 0.0f;               // +0x2a8
  float field2ac = 0.0f;               // +0x2ac

  // Write +0x10..0x18 and mirror +0x18 into col.baseZ.
  void setPosition(float x, float y, float z);
  // Refresh elemSet + presence gate after (re)binding the model.
  void syncCollisionView();
};

// Arena runtime wrapper — owns a CollisionArena view plus the +0x68
// object list (std::list storage -> stable CollisionObject addresses).
struct DynamicArena {
  CollisionArena col{};
  std::string name;                    // s2 record name (script keys)
  TraversalArena* owner = nullptr;     // back-pointer to the owning
                                       // TraversalArena (the +0x60
                                       // home-arena view objects hold)
  std::list<std::unique_ptr<DynamicObject>> storage;

  // FUN_0045cffc — allocate + push-front onto +0x68 (col.objects),
  // named=1, +0x60 = this. The original pops the global freelist
  // (0x540ed0) first; the port mirrors that — a recycled record is
  // already memset-clean (the free path wipes it), so it is
  // indistinguishable from a fresh allocation. Storage is owned by
  // the arena; the returned reference stays valid until detach.
  DynamicObject& allocFront();

  // List-tail variant — same freelist/alloc semantics as allocFront
  // but appends to +0x68. The full-save loader uses it: the original
  // fills the list head->tail in ALIE stream order (FUN_00427218's
  // bulk allocation), so a per-record allocFront would reverse the
  // list relative to the original and change the +0x68 walk order
  // the frame pass depends on.
  DynamicObject& allocBack();

  // FUN_0045cf90 — unlink from +0x68, wipe the record (the
  // original's memset), and push it onto the global freelist. The
  // memory stays allocated: stale DynamicObject* held by other
  // objects stay dereferenceable exactly like the original's pool.
  void detach(DynamicObject& obj);

  // FUN_0045cf18's +0x68 sweep — unlink and free every unnamed
  // record (a corpse left by FUN_0045828c teardown). Runs after each
  // arena's FUN_004572ac object pass in the frame loop, before the
  // script pass, so script-side dedup sees live objects only and a
  // same-frame respawn can reuse the freed record. The original's
  // cur-only deferred-free countdown drain (0x540ea8) has no port
  // equivalent — nothing produces deferred-free records yet.
  void reapUnnamed();

  // FUN_004574d0 — unlink from this arena's list and push-front onto
  // `dst`'s (+0x60 updated; storage spliced).
  void transfer(DynamicObject& obj, DynamicArena& dst);
};

// Row-major 3x3 (scale baked) + translation — the matrix contract
// shared with the collision query (world = M.local + origin).
void transformPoint(const float m[9], const float org[3],
                    const float in[3], float out[3]);

// FUN_0046b2f8 — degrees -> scaled row-major 3x3 + origin (see the
// header comment for the verified formula).
void buildObjectMatrix(float pitchDeg, float bankDeg, float yawDeg,
                       float scale, const float pos[3],
                       float outXform[9], float outOrigin[3]);

// FUN_0045612c collision core — matrix path select (+0x148&0x40 raw
// vs Euler), per-element world AABB (FUN_00459e40) and object AABB
// union with the OBSERVED degenerate z-seed. Elements masked by
// col.elemMaskB are skipped (their world AABBs stay stale — original
// behavior).
void rebuildObjectTransform(DynamicObject& obj);

// FUN_004566f0 default block — health 10, behavior-float defaults,
// scale 1.0, identity matrix, origin. Does NOT rebuild: FUN_004566f0
// runs the table-2 init script between the defaults and the rebuild,
// so callers with a script call initObjectDefaults -> script ->
// rebuildObjectTransform. (Script lookup/VM live in traversal_script.)
void initObjectDefaults(DynamicObject& obj);
// FUN_004566f0 with no init script — defaults then sync + rebuild.
void initObjectCollision(DynamicObject& obj);

// FUN_00456808 — spawn the arena record's type-2/type-4 objects onto
// `arena`'s +0x68 list. `modelFor` must return the SOURCE model for a
// CMI enemy-table index (or nullptr) — each spawn deep-copies it.
// `spawned` (optional) receives the created objects in record order.
// `scriptFor` (optional) resolves the persistent object script — the
// CMI table-0 "%s$%s_%d" record — to an image pointer stored in
// +0x108 (nullptr when no record matches; OBSERVED FUN_00456808).
// Returns the number of objects spawned.
using DynamicModelSource =
    const RuntimeModel* (*)(int modelIndex, void* ctx);
using DynamicObjectScriptSource =
    const void* (*)(const char* arenaName, const char* modelName,
                    std::uint16_t spawnId, void* ctx);
int spawnArenaObjects(DynamicArena& arena, const DtiArenaRecord& rec,
                      DynamicModelSource modelFor, void* ctx,
                      std::vector<DynamicObject*>* spawned = nullptr,
                      DynamicObjectScriptSource scriptFor = nullptr,
                      void* scriptCtx = nullptr);

// FUN_00433d40's load-time record rewrite: match each type-2 record's
// name against the enemy table -> fields[0] |= idx<<16; each type-4
// -> fields[0] = idx. Operates on the parsed DTI record in place.
// Returns records that failed to resolve (name not in the table).
std::vector<std::size_t> resolveArenaRecordNames(
    DtiArenaRecord& rec, const EnemyTable& enemies);

// FUN_004572ac tail — ride displacement for ONE updated object: when
// obj is the player's carrier, pos += pos - prevPos and
// *playerYawDeg += yawDeg - prevYawDeg (playerYawDeg may be null).
// Then latch prevPos/prevYaw (the original does both at the end of
// the object's update — keep that order: displace BEFORE latching).
void applyRideDisplacement(const DynamicObject& obj, CollisionState& cs,
                           float* playerYawDeg);
void latchObjectPrevState(DynamicObject& obj);

// Convenience frame step for a moved object: rebuild transform
// (mover path — same-frame visibility), apply ride displacement,
// latch previous state. Mirrors the FUN_004572ac ordering for the
// `+0x14a&0x20` mover objects that reach FUN_0045612c inside the
// update pass.
void updateMoverCollision(DynamicObject& obj, CollisionState& cs,
                          float* playerYawDeg);

} // namespace mdk

#endif // MDK_CORE_DYNAMIC_OBJECTS_H
