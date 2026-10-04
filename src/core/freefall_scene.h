// freefall_scene.h — Phase 16C: FALL3D freefall presentation scene.
//
// The freefall runtime (freefall_runtime.h) owns mode-2 gameplay;
// this module owns the mode-2 PRESENTATION data the original renderer
// consumes each frame: the per-object deep-copied RuntimeModels that
// the shared object-anim driver mutates, the FALL3D_<course>.MTI
// material bank, and the FALLP_<course> palette.
//
// Original ownership (MDK95.EXE BUILD_A, OBSERVED via disassembly):
//
//   FUN_0040ef28  mode-2 init: loads FALL3D/FALL3D_<course+1>.MTI
//                 (render material bank), binds the FALLP_<course+1>
//                 palette record (0x4edc28), binds the anim records
//                 KURTANIM/KURT_HIT/BONESANM (0x4edae0/4/8), and
//                 resolves the model roster by name.
//   Roster table  OBSERVED .data pair — the 23-entry name block at
//   (0x49a67c)   0x49a67c and the flag bytes at 0x49a664. Byte bit7
//                 marks a named-element geometry record; the low 7
//                 bits are the model-slot index into the 0x88-stride
//                 model table at 0x4edcc0 (FUN_00454794 resolves a
//                 FALLPU pickup name to that slot).
//   FUN_00428400  the shared geometry parser — freefall records are
//                 the same stream MINUS the leading flag u32 (the flag
//                 arrives as a register arg, not stream data; the port
//                 stores it as parseGeometryRecord's head word, so the
//                 record bytes are re-headed with the roster flag).
//   FUN_004109d8  the per-object render walk emits up to six sorted
//                 entries: radar sprite, trail, launch glow, BANG
//                 explosion (kind 3), the main model (kind 2), and the
//                 chute attachment (kind 2) — kind-2 entries render a
//                 RuntimeModel through the shared projector under the
//                 object's +0xac basis (FUN_0046b2f8: pitch/roll/yaw/
//                 scale + pos). This scene produces exactly the kind-2
//                 product; the sprite/FX kinds stay documented seams.
//   FUN_004555bc  the object anim driver — object_animation.cpp's
//                 objectAnimTick. Freefall objects carry the same
//                 fields (+0xdc acc, +0xe0 rate, +0xe4 frame, +0x114
//                 record, +0x118 latch); the runtime advances the
//                 authoritative accumulators, this scene's twins run
//                 the real driver so the vertex mutation is identical.
//
// Presentation model: each live pool object gets a "twin"
// DynamicObject — a deep-copied model plus the mirrored anim fields —
// stepped by objectAnimTick once per freefallStep. Consumers read the
// twin's model.elemVerts (the vertex buffer objectAnimApply mutates
// in place) and col.xform/origin (FUN_0046b2f8 basis). Nothing here
// drives gameplay; the FreefallObject fields stay authoritative.
//
// Deferred seams (presented only as state, matching the runtime's
// existing seams): the kind-1 radar marker sprite, missile launch
// glow/trail (kind 5/4), the kind-3 BANG frame-block overlay, the
// ZOOM%04d intro sprite sequence, and every sound.

#ifndef MDK_CORE_FREEFALL_SCENE_H
#define MDK_CORE_FREEFALL_SCENE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/arena_render.h"
#include "core/bni_directory.h"
#include "core/dynamic_objects.h"
#include "core/freefall_runtime.h"
#include "core/mti_directory.h"

namespace mdk {

class DataRoot;

// One roster entry — name block 0x49a67c[i] + flag byte 0x49a664[i]
// (OBSERVED BUILD_A .data). `flag` bit7 = the named-element record
// form; low 7 bits = the model-slot index.
struct FreefallModelRecord {
  const char* name;    // FALL3D.BNI record name
  std::uint8_t flag;   // 0x49a664 byte
};

extern const std::array<FreefallModelRecord, 23> kFreefallModelTable;

// The object's model-slot index (0x4edcc0-table identity). Resolves
// the fixed tags and the FALLPU pickup names through the roster
// table; returns -1 when the tag carries no geometry record
// (kFfModelRadar — slot 2 exists but the radar's visual is the
// kind-1 marker sprite, a documented seam — or an unlisted pickup
// name).
int freefallObjectModelSlot(const FreefallRuntime& rt,
                            const FreefallObject& o);

// The FALL3D.BNI record name for a FreefallAnimTag (nullptr for
// kFfAnimNone / unknown).
const char* freefallAnimRecordName(int animTag);

// A resolved visual material for one tri's material index / name-table
// entry. Three OBSERVED sources fold into one result:
//   - MTI payload record  -> `pixels` (indexed, width*height*frameCount
//     bytes; frameCount>1 = the EXPLODE-style animated strip — the
//     kind-3 render picks frame*(w*h) byte slices, FUN_004109d8 case 3)
//   - MTI index record    -> `paletteIndex` (+0x0c field — GREY*/,
//     PEN_*, BLACK, NONE names)
//   - PEN_<n> name absent from the bank -> `paletteIndex` = the name's
//     digits (the documented name->index correlation, DATA_FORMATS.md;
//     HYPOTHESIS as a fallback — OBSERVED MTI index records always
//     carry the same digits at +0x0c)
// Negative tri material indices bypass the name table entirely —
// `arenaPenIndex(mat)` = (-mat)&0xff is the flat pen (same class as a
// resolved index record).
struct FreefallMaterial {
  std::string name;
  std::span<const std::uint8_t> pixels{};  // empty = not a texture
  int width = 0;
  int height = 0;
  int frameCount = 1;
  int paletteIndex = -1;                   // >=0 = flat palette pen
  bool valid = false;
};

// The presentation bundle for one loaded course.
struct FreefallScene {
  int course = -1;

  // FALL3D/FALL3D.BNI — the file buffer the model/anim spans alias.
  std::vector<std::byte> bniBytes;
  BniDirectory bni{};

  // FALL3D/FALL3D_<course+1>.MTI — the render material bank
  // (FUN_0040ef28's first load; buffer keeps decoded pixel spans
  // alive).
  std::vector<std::byte> mtiBytes;
  MtiDirectory mti{};

  // FALLP_<course+1> — 768B RGB palette bound at 0x4edc28.
  std::array<std::uint8_t, 768> palette{};
  bool paletteOk = false;

  // FALLPU_<course+1> — the 12-byte {name[8], u32} pickup list the
  // init pops backward (FUN_004118b0). Parsed here so the caller can
  // build FreefallCourseData without re-reading the BNI.
  std::vector<FreefallPickupRec> pickups;

  // Parsed RuntimeModel prototypes keyed by model slot (flag&0x7f),
  // built lazily on first bind. protoFailed caches unresolvable slots
  // (absent/unparsable records) so a bad entry doesn't retry per
  // frame.
  std::unordered_map<int, RuntimeModel> protos;
  std::unordered_set<int> protoFailed;

  // Anim record payload spans into bniBytes, indexed by
  // FreefallAnimTag (0 = none). `limit` is the record's payload end.
  struct AnimSpan {
    const std::uint8_t* rec = nullptr;
    const std::uint8_t* limit = nullptr;
  };
  std::array<AnimSpan, 4> anims{};

  // Per-pool-slot presentation twin. `obj.model` is the deep copy
  // objectAnimTick mutates; `obj.col.xform/origin` is the FUN_0046b2f8
  // basis the render walk consumes. `bound` tracks the active-list
  // membership exactly (an object leaves the list -> unbound).
  struct Twin {
    // +0x60 — the trail record (FUN_0042eaa8 freelist alloc +
    // FUN_0042eadc init). `anchorPts` are the model-space points the
    // spawn-time FUN_0042eb3c scan selects (the extreme-x entries of
    // the model's +0x20 point table — implemented over the model's
    // element vertex pools, the only point-table-shaped data the
    // record format carries; HYPOTHESIS on the table identity).
    // Each feed (FUN_0042ecc4) transforms the anchors by the object
    // basis into the next ring slot; slot pt[anchors] = pt[0] closes
    // the section. The draw walk consumes the ring (FUN_0042ee74).
    static constexpr int kTrailCap = 32;      // FUN_0042eadc cap arg
    static constexpr int kTrailPts = 4;       // vec3s per slot
    struct Trail {
      float pts[kTrailCap][kTrailPts][3]{};
      int count = 0;          // +0x10 slots fed
      int cursor = 0;         // +0x1c ring write cursor
      int anchors = 0;        // +0x18 anchor count (<=3)
      float anchorPts[3][3]{};
    };
    Trail trail;

    DynamicObject obj;
    bool bound = false;
    int modelSlot = -1;
    int type = -1;
    int pickupRec = -1;
    int animTag = -1;
  };
  std::array<Twin, 399> twins;

  // LEVEL%d — the 1024x1024 indexed minecrawler surface the backdrop
  // pass (FUN_00412530) scroll-samples through POD%d strips and the
  // FUN_0046d780 span blitter before the object walk. The pixels
  // span aliases mtiBytes; palette-expanded at upload by the bridge.
  std::span<const std::uint8_t> backdropPixels;
  int backdropW = 0;
  int backdropH = 0;
  bool backdropOk = false;

  // Lazily resolved materials by name-table string ("CB3", "PEN_16",
  // ...); materialMiss caches names that resolve to nothing. The
  // decoded `pixels` spans alias mtiBytes.
  std::unordered_map<std::string, FreefallMaterial> materials;
  std::unordered_set<std::string> materialMiss;
};

enum class FreefallSceneError {
  kOk,
  kReadBni,        // FALL3D/FALL3D.BNI unreadable
  kParseBni,       // BNI directory parse failed
  kReadMti,        // FALL3D/FALL3D_<c+1>.MTI unreadable
  kParseMti,       // MTI directory parse failed
  kPaletteMissing, // FALLP_<c+1> record absent/short (non-fatal-ish:
                   // scene still loads with paletteOk=false)
};
const char* freefallSceneErrorName(FreefallSceneError e);

// FUN_0040ef28 presentation subset: load the course's FALL3D bundle —
// the BNI records, the MTI material bank, the FALLP palette. Missing
// palette records degrade to paletteOk=false rather than failing the
// load (the original dereferences its lookup unconditionally; the
// native port reports the miss).
FreefallSceneError freefallSceneLoad(const DataRoot& root, int course,
                                     FreefallScene* out,
                                     std::string* detail);

// Mirror the runtime's active object list into the twins and run one
// objectAnimTickDt per bound anim — call once per freefallStep, after
// the step, with the SAME dtSec (the original driver's accumulator
// step is rate*+0xe0*dtSec == the runtime's frameUnits model for the
// rate-1.0 freefall records — both equal smoothed*1.0).
// The walk visits listHead..next exactly like FUN_004109d8's entry
// collection: an object contributes a kind-2 entry only while in the
// active list with model != 0.
void freefallSceneStep(FreefallScene& s, const FreefallRuntime& rt,
                       float dtSec);

// The bound twin for a pool index (nullptr when the slot is
// unbound/unmodeled — radar objects, unresolved pickups).
const FreefallScene::Twin* freefallSceneTwin(const FreefallScene& s,
                                             int poolIdx);

// The bound twin's trail ring (nullptr when unbound or no anchors
// were scanned — non-FX objects never run FUN_0042eb3c).
const FreefallScene::Twin::Trail* freefallSceneTrail(
    const FreefallScene& s, int poolIdx);

// The CHUTE attachment prototype — the pickup's second kind-2 entry
// renders this model under the SAME object basis (FUN_004109d8 emits
// it at +0x306 without a separate transform). Binds slot 4 lazily.
const RuntimeModel* freefallSceneChuteModel(FreefallScene& s);

// Resolve one name-table string to its render material. nullptr when
// the name is absent from the MTI bank AND not a decodable pen name
// (materialMiss-cached).
const FreefallMaterial* freefallSceneMaterial(FreefallScene& s,
                                              const std::string& name);

} // namespace mdk

#endif // MDK_CORE_FREEFALL_SCENE_H
