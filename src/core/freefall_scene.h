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
//                 entries: a +0x10c sprite-marker entry, trail, launch
//                 glow, BANG explosion (kind 3), the main model
//                 (kind 2), and the chute attachment (kind 2) — kind-2
//                 entries render a RuntimeModel through the shared
//                 projector under the object's +0xac basis
//                 (FUN_0046b2f8: pitch/roll/yaw/scale + pos). This scene
//                 produces exactly the kind-2 product; the sprite/FX
//                 kinds stay documented seams. (The +0x10c marker is a
//                 dead branch in mode 2 — spawned objects never carry
//                 it; see FREEFALL_BACKDROP.md §6.)
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
// existing seams): the kind-1 +0x10c sprite-marker path (never
// populated on spawned freefall objects — dead branch in mode 2),
// missile launch glow/trail (kind 5/4), the kind-3 BANG frame-block
// overlay, the ZOOM%04d intro sprite sequence, and every sound.

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
// (kFfModelRadar — slot 2 produces the code-built wedge model rather
// than a stream geometry record — or an unlisted pickup name).
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
    // spawn-time FUN_0042eb3c scan selects — the minimum-x and
    // maximum-x vertices of the model's +0x20 ELEMENT TABLE entry 0
    // (OBSERVED: the scan walks model+0x20 -> element[0]'s vertex
    // list; for MISSILE those are verts 11/12 — the tail's widest
    // points). Each feed (FUN_0042ecc4) writes the basis-transformed
    // world-space anchors + centroid into slot +0x1c, advances +0x1c
    // mod 32, and — once count == cap — advances +0x20 (the read
    // cursor) the same way: a proper circular buffer, oldest at
    // `read`. The draw walk (FUN_0042ee74) iterates `count` slots
    // from `read` upward, mod-32 indexed.
    static constexpr int kTrailCap = 32;      // FUN_0042eadc cap arg
    static constexpr int kTrailPts = 4;       // vec3s per slot
    struct Trail {
      float pts[kTrailCap][kTrailPts][3]{};
      int count = 0;          // +0x10 slots fed
      int cursor = 0;         // +0x1c ring write cursor
      int read = 0;           // +0x20 ring read cursor (oldest)
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
  // FUN_0046d780 span blitter before the object walk. `backdropPixels`
  // aliases the pristine mtiBytes copy; `backdropWork` is the mutable
  // per-frame copy the POD seam wedge writes into (the original
  // splices into the loaded image in place — 0x4edc24). The rendered
  // 600x360 indexed output lands in `backdropFrame` each step.
  std::span<const std::uint8_t> backdropPixels;
  int backdropW = 0;
  int backdropH = 0;
  bool backdropOk = false;
  std::vector<std::uint8_t> backdropWork;   // 1024x1024 owned copy
  std::vector<std::uint8_t> backdropFrame;  // 600x360 indexed

  // POD%d — the 64x1024 seam strip the wedge copy splices into the
  // LEVEL center columns (OBSERVED: bytes 32-count..31 of each row ->
  // LEVEL cols 512-count..511).
  std::span<const std::uint8_t> podPixels;
  int podW = 0;
  int podH = 0;

  // ZOOM%04d — the 16 span tables FUN_0046d780 walks, one selected
  // per frame (0x4edbf0 counter & 15 = temporal dither). Parsed rows:
  // 180 entries of {a=600,b=0,c=0} spans; each shaded span carries
  // per-pixel LUT-row bytes. (Record layout: per row [countA,
  // shadeBytes..., countB, countC, shadeBytes...] — OBSERVED in
  // FUN_0046d780's three-phase per-row read.)
  struct BackdropSpanRow {
    std::vector<std::uint8_t> sa;   // phase-A shade bytes (count a)
    std::vector<std::uint8_t> sc;   // phase-C shade bytes (count c)
    std::uint32_t a = 0, b = 0, c = 0;
  };
  std::array<std::vector<BackdropSpanRow>, 16> zoomRows;
  // Per-table parse validity — the 0x4edbf0 counter hits every table
  // every 16 calls, so a single rejected record must not ride through
  // as an empty (black) cycle. `zoomCount` counts the OK tables.
  std::array<bool, 16> zoomOk{};
  int zoomCount = 0;

  // L%d_C000%d — the 8 pod-column chunk sprites drawn through
  // FUN_00403a40 -> 0x46d680 (transparent scaled blit, center pos).
  struct BackdropSprite {
    int w = 0, h = 0;
    std::span<const std::uint8_t> px;
  };
  std::array<BackdropSprite, 8> chunks;
  int chunkCount = 0;

  // FLARE4 — the kind-5 launch-glow sprite (FUN_004109d8 case 5,
  // 0x46d6d1 LUT-remap blit at screen scale 0x80 -> 32x32 px dst).
  BackdropSprite flare4;
  // PICK — the kind-1 marker sprite drawn for +0x10c objects.
  BackdropSprite pick;

  // SPACE/MOON/EARTH — the FUN_0040ff78 intro-scene resources
  // (0x4edb28/0x4edb24/0x4edb20). SPACE is a full 600x360 indexed
  // frame blitted opaque (FUN_0040fcb0); MOON/EARTH composite
  // transparent (index-0 skip) through the same FUN_00403a40 ->
  // 0x46d680 scaled-sprite emitter the chunk sprites use, with the
  // authored center/scale easing OBSERVED at 0x40fe18/0x40fec0.
  BackdropSprite space, moon, earth;
  // SPACEPAL (0x4edb2c) — the intro-scene palette. The init tail
  // forces entry 0 to black (0x40f0f3). During the intro the whole
  // presentation maps through this palette — fade-in dims it toward
  // black (FUN_0040fb08), the 60..90 hold re-uploads it once
  // (0x413b40), fade-out dims it toward WHITE (FUN_0040fba0:
  // out = pal*f + (1-f)*255 — OBSERVED).
  std::array<std::uint8_t, 768> spacePal{};
  bool spacePalOk = false;
  // SC_STAT/SC_BSTAT/SNIP_TXT — the mode-2 HUD records resolved
  // through the shared 21-slot table (0x49a828 -> slots 2/3/7 of
  // 0x49a7d4's names, FUN_00418688). FUN_00417e20 draws SC_STAT at
  // (500,287), the SC_BSTAT-stamped mission-timer wedge, and the
  // 0x541554 health digits centered at (542,312) through 8px
  // SNIP_TXT column slices (80x12 strip = 10 cells of 8x12). The
  // wedge envelope collapses in mode 2 (0x5414a0/a4/a8 all held at
  // 1000.0f — OBSERVED at 0x4014c5/0x401571), so the SC_STAT art
  // draws unstamped.
  BackdropSprite scStat, scBstat, snipTxt;

  // The FUN_0040ff78 per-frame product while rt.introCountdown > 0:
  // SPACE + MOON + EARTH + the ZOOM-table LUT-remap band (0x46d8e2,
  // shade rows 4+(0x4edbf4>>8) rel LUT base — i.e. absolute rows
  // 4..16) composited into one 600x360 indexed frame, valid when
  // `introOk`. The object walk then composites on top, identical to
  // the descent path.
  std::vector<std::uint8_t> introFrame;   // 600x360 indexed
  bool introOk = false;

  // The generated LUT — 6 banks x 64 rows x 256 palette indices,
  // built from the FALLP palette + the 0x49b57c keyframe ramp
  // (FUN_00406d84: LUT[c] = nearestPal(pal[c]*(256-L)/256 + key*L/256),
  // bank strengths {90,85,80,60,40,15}/256). `keyColors[row]` is the
  // 64-entry ramp color every bank's row converges toward — the
  // bridge ships it so Godot can approximate dst=LUT[dst] as an
  // alpha blend toward keyColors[row].
  std::vector<std::uint8_t> lut;               // 384*256
  std::array<std::array<std::uint8_t, 3>, 64> keyColors{};
  bool lutOk = false;

  // OBSERVED: the LUT is regenerated on every palette bind
  // (0x40f75c = bind + rebuild). Mode-2 init binds SPACEPAL first
  // (0x40f518-0x40f522), so the intro remap reads a SPACEPAL-domain
  // table; at countdown<=0 (0x4102cc) FALLP rebinds and `lut` above
  // takes over. `introLut` is the identical build against spacePal.
  std::vector<std::uint8_t> introLut;          // 384*256, SPACEPAL
  bool introLutOk = false;

  // Backdrop scroll state (all FUN_00412530 locals / driver globals):
  //   scrollPos    0x4edc00 — += 1/30 per frame
  //   chunkScroll  0x4edb5c — += 0x49b6f0*dt per frame, wraps mod 8
  //   zoomCounter  0x4edbf0 — += 1 per frame, &15 selects the table
  //   scrollRow    0x4edc30 — the previous-frame seam row
  float backdropScrollPos = 0.0f;
  float backdropChunkScroll = 0.0f;
  int backdropZoomCounter = 0;
  int backdropScrollRow = -1;   // -1 = first frame

  // Backdrop diagnostics for the bridge/tests (written each
  // freefallSceneBackdropStep call).
  struct BackdropDiag {
    float p = 0.0f;        // camZ*0.0001893939 perspective step
    float uStart = 0.0f;   // 16.16 texel u at row 0 px 0
    float vStart = 0.0f;
    int scrollRow = 0;     // new seam row this frame
    int zoomTable = 0;
    int chunkFrame = -1;   // -1 = not drawn
    float chunkX = 0.0f, chunkY = 0.0f;   // center, screen px
    float chunkW = 0.0f, chunkH = 0.0f;
  } backdropDiag;

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

// The trail section pen per FUN_0042ee74's draw walk (OBSERVED):
// sections are indexed s = 1..count-1 in ring order (section s pairs
// walked slots s-1 and s). Sections with s >= count-8 — the NEWEST
// eight — get pen count-s-1066 (the 0x26+secIdx accumulator at
// 0x42f096), i.e. -1058..-1065 -> LUT rows 29-36; all older sections
// get -1054 (0xfffffbe2 -> LUT row 25). `count` is the live ring
// count.
int freefallTrailSectionPen(int count, int section);

// One ZOOM%04d record -> parsed span rows (OBSERVED resource parse;
// FUN_0046d780's three-phase row read). Record payload:
//   {u32 size-4 prefix, then 180 rows of
//    {u32 cntA, u8 shadesA[cntA*4], u32 cntB, u32 cntC,
//     u8 shadesC[cntC*4]}}
// The word AFTER the 4-byte prefix is row 0's cntA — there is no
// second padding word (a +8 read desyncs the whole walk; the bug
// that produced the all-black packaged backdrop). Validity: every
// row's (cntA+cntB+cntC) covers EXACTLY 150 quads / 600 px, exactly
// 180 rows parse, and the record is consumed to its last byte.
// `rec` is the full record INCLUDING the prefix. Returns false on
// any malformed/truncated/over-long layout and clears `rows`.
bool freefallZoomParseTable(
    std::span<const std::uint8_t> rec,
    std::vector<FreefallScene::BackdropSpanRow>* rows);

// FUN_00412530 — one backdrop frame. Integrates the scroll state
// (0x4edc00 += 1/30 per rendered frame ~= seconds of descent —
// dt-scaled here; 0x4edb5c chunk cycle +0.5/frame ~= 15/s;
// 0x4edbf0 zoom dither +1/frame, per call), splices the POD seam
// wedge into the LEVEL working copy, span-samples the 600x360
// window through the ZOOM table + generated LUT, then draws the
// L%d_C000%d pod-column chunk. Output: backdropFrame (600x360
// indexed px); diagnostics in s.backdropDiag.
// Cam args are the ORIGINAL camera position components (0x540b28/2c/30:
// the mode-2 camera copy — 0.85*player.x, 0.85*player.y, player.z+10
// per FUN_004123f4) in game units. dtSec is the presented frame's
// delta (the original's per-call constants are its 30 fps cadence).
void freefallSceneBackdropStep(FreefallScene& s,
                               float camX, float camY, float camZ,
                               float dtSec = 1.0f / 30.0f);

// Resource readiness for the backdrop path — every required record
// resolved, not "the frame buffer exists". A rejected ZOOM table is
// a hard miss (the original binds all 16 unconditionally), never a
// silently-black backdrop.
struct FreefallBackdropStatus {
  bool palette = false;
  bool level = false;      // LEVEL%d decoded
  bool pod = false;        // POD%d decoded
  bool lut = false;
  int chunks = 0;          // L%d_C000%d decoded count (of 8)
  int zoom = 0;            // ZOOM%04d parsed count (of 16)
  int zoomMask = 0;        // per-table bitfield (bit z = ZOOM%04d ok)
  bool flare4 = false;
  bool pick = false;
  bool ready = false;      // level && pod && lut && zoom == 16
};
FreefallBackdropStatus freefallSceneBackdropStatus(
    const FreefallScene& s);

// FUN_0040ff78 — the mode-2 intro scene. Runs each frame while
// rt.introCountdown > 0 INSTEAD of freefallSceneBackdropStep (the
// original's FUN_004103d8 early-returns after the intro tick — the
// whole descent draw block, backdrop included, is skipped). Output:
// introFrame (600x360 indexed against SPACEPAL), validity in
// s.introOk.
void freefallSceneIntroStep(FreefallScene& s,
                            const FreefallRuntime& rt);

// FUN_00412970 + FUN_0040c860's negative-pen dispatch — the trail's
// indexed compositor. Walks each bound twin's trail ring OLDEST->
// NEWEST (from the +0x20 read cursor), applies the OBSERVED head ramp
// {1.0,1.25,1.2,1.1,1.05,1.0} + linear decay taper to each slot's
// two world-space anchors, projects the tapered edge points through
// the mode-2 folded view (x' = scaleX*(px-cx), y' = scaleY*(cy-py),
// z' = cz-pz; sx = (x'+z')/z'*W*0.4999 + 0.05, sy likewise at
// H*0.5011 — the same divisors the snapshot camera is built from),
// and fills each section quad with the pen's OBSERVED operation:
//   dst = lut[row*256 + dst]   (row = -1029 - pen for pens < -1028:
//   -1054 -> 25, -1058..-1065 -> 29..36)
// — the indexed destination remap, NOT an alpha overlay. Runs after
// freefallSceneBackdropStep so trails composite over the backdrop +
// chunk pass in original draw order. z' <= 0.05 sections clip like
// the projector's near bound.
void freefallSceneTrailComposite(FreefallScene& s,
                                 float camX, float camY, float camZ);

// §4C — the serial veil-ordering target. The original's veil ops
// (trail sections, kind-5 flare, the radar wedge's negative-pen
// tris) all perform dst = lut[row][dst] IN DRAW ORDER on the shared
// indexed framebuffer, so a px under K veils receives the serial
// chain L_k[...L_1[p]] — not K independent single remaps. The Godot
// screen_texture path snapshots only the opaque pass, so a veil
// shader can never see an earlier veil's output there. This mask
// retains, per px, the ordered list of {row, z'} records the veil
// ops would apply; the frontend shaders replay the chain with a
// depth gate against the winning opaque surface (painter semantics:
// an element behind a body was overwritten by it, so a buried
// element must not remap).
struct VeilMask {
  static constexpr int kW = 600;
  static constexpr int kH = 360;
  static constexpr int kK = 8;    // per-px record cap
  struct Rec {
    std::uint8_t row = 0;
    float z = 0.0f;               // z' = camZ - pz (view depth)
  };
  // recs[px*kK + k]; counts[px] = valid records (0..kK); overflow =
  // elements dropped past the cap (nearest records win).
  std::vector<Rec> recs;
  std::vector<std::uint8_t> counts;
  int ops = 0;
  int elems = 0;
  int overflow = 0;
  void clear() {
    recs.assign(std::size_t(kW) * kH * kK, Rec{});
    counts.assign(std::size_t(kW) * kH, 0);
    ops = elems = overflow = 0;
  }
};

// Emit the ordered veil records for this frame — trail sections,
// kind-5 flare quads, radar-wedge tris — sorted by the draw-entry
// key (mdk z ascending = far->near painter order; flare keyed
// obj.z + 10.0 OBSERVED, wedge z'~=0 sorts last) and rasterized
// through the same folded-view projection the compositor uses.
void freefallSceneVeilMask(FreefallScene& s, const FreefallRuntime& rt,
                           float camX, float camY, float camZ,
                           VeilMask& out);

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
