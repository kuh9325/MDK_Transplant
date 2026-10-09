// player_pickup.cpp — FUN_004696d8 (Phase P0-B) — the traversal
// pickup collector. See player_pickup.h for the evidence map.
//
// OBSERVED semantics (MDK95.EXE BUILD_A decompile + disasm):
//
//   Scan (0x4696f9..0x4697b9): the CURRENT arena's +0x68 object list,
//   head-to-tail, once. An object is eligible when:
//       +0x06 named != 0
//       +0x14a & 0x20            (mover)
//       dword+0x148 & 0x40010 == 0  ->  +0x148&0x10 (dead) clear AND
//                                      +0x14a&0x04 (already
//                                      collected) clear
//   Contact: the object's +0x198 AABB expanded by
//       min += (-1.0, -1.0, -5.0)   (0x498d0c, 0x498d0c, 0x498d10)
//       max += (+1.0, +1.0, +1.0)
//     is tested by FUN_0045cc2c(0x540c08, 0x540bfc, box) — the swept
//     segment prevPos -> pos, per-axis min/max vs the box, inclusive
//     edges (touch counts).
//   Name dispatch: exact FUN_0042fa50 streq compares of the +0x0c
//   record's byte-0 — the table-1 ENTRY NAME written inline by
//   FUN_004286c8 (0x88-stride records; name chars copied to +0,
//   +0x0a = deferred flag, geometry parsed in after). The 9-name
//   item table (0x49bba0 -> ids 1..9 via 0x49bbc4) is tried BEFORE
//   the 12-name pickup table (0x49bad4 -> ids 0..11). No match ->
//   RETURN abandons the whole frame's scan (OBSERVED quirk).
//   Despawn (shared by both paths, 0x469824..0x469872): arm the
//   carry-away —
//       +0x30e = 30                     (carry tick countdown)
//       dword+0x148 |= 0x41000          -> +0x149|=0x10 (routes the
//                                        object into FUN_0045897c's
//                                        +0x14a&4 carry branch next
//                                        update) AND +0x14a|=0x04
//                                        (the carry bit itself;
//                                        also this collector's own
//                                        re-collect exclusion)
//       +0x1c..+0x24 = +0x10..+0x18     (pos -> the converge anchor
//                                        FUN_004599e8 lerps from)
//       +0x312 child torn down via FUN_0045828c when set, then = 0
//   The function RETURNS after one collect — a full inventory on
//   the item path is the only non-collect outcome and scanning
//   continues past that object.

#include "core/player_pickup.h"

#include <string>

#include "core/collision_query.h"
#include "core/dynamic_objects.h"
#include "core/enemy_runtime.h"
#include "core/traversal_audio.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

// 0x49bba0 — the 9-name inventory-item table; index+1 = the item id
// (0x49bbc4 holds the identity map 1..9).
const char* const kItemNames[9] = {
    "SW_DUMMY", "SW_INTER", "SW_TWIST", "SW_THUMP", "SW_HBOMB",
    "SW_GATT",  "SW_KEY",   "SW_SEAL",  "SW_SBONE"};

// 0x49bad4 — the 12-name standard-pickup table; index = the
// FUN_0046a790 switch id.
const char* const kPickupNames[12] = {
    "SW_HOME", "SW_SGREN", "SW_HGREN", "SW_LGREN", "SW_BONES",
    "SW_H25",  "SW_H50",   "SW_H100",  "SW_H150",  "SW_H01",
    "SW_EWJ",  "BONEFLC"};

// 0x49bb04 — ammo amounts for pickup ids 0..4 (ids 5..11 do not read
// the table).
const int kPickupAmmo[5] = {8, 3, 3, 8, 1};

// The FUN_0046a500/FUN_004696d8 per-difficulty item rows
// (0x49bb34 / 0x49bb58 / 0x49bb7c, selected by 0x54147a). One dword
// per item id-1; only id5 (SW_HBOMB) and id6 (SW_GATT) differ from
// 1: {5,400} easy / {3,200} normal / {1,100} hard.
const int kItemAmt[3][9] = {
    {1, 1, 1, 1, 5, 400, 1, 1, 1},
    {1, 1, 1, 1, 3, 200, 1, 1, 1},
    {1, 1, 1, 1, 1, 100, 1, 1, 1}};

// FUN_0045cc2c — swept-segment-vs-box: per-axis endpoint min/max
// against the box, inclusive edges (0x45cc2c, OBSERVED).
bool segVsAabb(const float a[3], const float b[3], const float box[6]) {
  for (int i = 0; i < 3; ++i) {
    const float lo = a[i] < b[i] ? a[i] : b[i];
    const float hi = a[i] < b[i] ? b[i] : a[i];
    if (hi < box[i] || box[3 + i] < lo) return false;
  }
  return true;
}

// The shared despawn arm (0x469824..0x469872 — identical code in all
// three collect branches).
void collectDespawn(TraversalRuntime& rt, DynamicObject& o) {
  o.field30e = 30;                          // +0x30e = 0x1e
  o.col.flags148 |= 0x1000;                 // dword |0x41000:
  o.col.flags149 |= 0x10;                   //   +0x149|=0x10 -> enemy
  o.col.flags14a |= 0x04;                   //   dispatch; +0x14a|=0x04
  // +0x1c..+0x24 = +0x10..+0x18 — the carry lerp anchors at the
  // pickup spot (FUN_004599e8 converge origin).
  o.field1c[0] = o.pos[0];
  o.field1c[1] = o.pos[1];
  o.field1c[2] = o.pos[2];
  if (o.moverChild != nullptr) {            // +0x312 child teardown
    objectTeardownNow(rt, *o.moverChild);
    o.moverChild = nullptr;
  }
}

// FUN_0041cad0(name, 1, 0x40000000) — the status-message post. The
// `name` is the object's +0xc entry name for items / the table name
// for pickups; FUN_00414890 resolves it through MDKFONT.FTI into the
// drawn string. flags=1 (slide-in), rate=2.0 (0x40000000).
void collectNotify(TraversalRuntime& rt, const std::string& name) {
  traversalTeletypePost(rt, name, 1, 2.0f);
}

// FUN_00402388(record, 1) — kRestart play of the named SNI record.
// Record bindings OBSERVED from the FUN_0043394c table-init writes
// (0x54c634=APPLE, 0x54c65c=WMIB, 0x54c660=BONES, 0x54c664=COLLECT).
void collectSfx(TraversalRuntime& rt, const char* rec) {
  traversalAudioEmit(rt, TraversalAudioOp::kRestart, rec);
}

// FUN_0046a790 — the 12-id pickup grant switch. The health cases
// guard with hp<cap before adding/setting; the ammo cases divide
// the table amount by 2 on difficulty 2 when the amount exceeds 1
// (OBSERVED 0x46a7be..0x46a7da) and latch wpnSel1 = id+1.
void pickupGrant(TraversalRuntime& rt, int id) {
  switch (id) {
    case 4:
      collectSfx(rt, "BONES");        // id4's own sfx (0x54c660) —
      [[fallthrough]];                // falls into the shared block
    case 0:
    case 1:
    case 2:
    case 3:
      collectSfx(rt, "COLLECT");      // 0x54c664 — on mode 3 id4
                                      // therefore plays BONES AND
                                      // COLLECT (OBSERVED quirk).
      {
        int amt = kPickupAmmo[id];
        if (rt.difficulty == 2 && amt > 1) amt /= 2;
        // 0x541619 = RAW pickup id (MOV AL,CL — OBSERVED 0x46a7e5):
        // the select lands one slot BELOW the granted pool
        // (ammo[1+id]) — an original off-by-one the port keeps.
        rt.wpnSel1 = id;                        // 0x541619
        rt.ammo[1 + id] += amt;                 // 0x541623[id]
      }
      break;
    case 5:                                   // SW_H25 — hp +10 cap 100
      collectSfx(rt, "APPLE");
      if (rt.fieldHealth < 100) {
        rt.fieldHealth += 10;
        if (rt.fieldHealth > 100) rt.fieldHealth = 100;
      }
      break;
    case 6:                                   // SW_H50 — hp +50 cap 100
      collectSfx(rt, "APPLE");
      if (rt.fieldHealth < 100) {
        rt.fieldHealth += 50;
        if (rt.fieldHealth > 100) rt.fieldHealth = 100;
      }
      break;
    case 7:                                   // SW_H100 — hp = 100
      collectSfx(rt, "APPLE");
      if (rt.fieldHealth < 100) rt.fieldHealth = 100;
      break;
    case 8:                                   // SW_H150 — hp = 150
      collectSfx(rt, "APPLE");
      if (rt.fieldHealth < 150) rt.fieldHealth = 150;
      break;
    case 9:                                   // SW_H01 — hp +1 cap 100
      collectSfx(rt, "APPLE");
      if (rt.fieldHealth < 100) {
        rt.fieldHealth += 1;
        if (rt.fieldHealth > 100) rt.fieldHealth = 100;
      }
      break;
    case 10:                                  // SW_EWJ
      collectSfx(rt, "COLLECT");
      // FUN_0046aa30 — the marker-joint spawn (arena scan + spawn
      // with flags 0x40000806, health 65000). NOT PORTED: no
      // Level-3 SW_EWJ exists to verify the target-selection and
      // spawn terms against — counted seam, sfx/notify still run.
      ++rt.seams.ewjSpawnCalls;
      break;
    case 11:                                  // BONEFLC — sfx only
      collectSfx(rt, "BONES");
      break;
  }
  // FUN_0041cad0 tail: ids 9 and 11 post nothing (OBSERVED). The name
  // is the pickup-table entry ([ecx*4 + 0x49bad4]) — kPickupNames[id].
  if (id != 9 && id != 11) collectNotify(rt, kPickupNames[id]);
}

// ---------------------------------------------------------------------------
// P0-C — item use. FUN_00465228's 0x4ce774 edge tail (0x465675..0x465996)
// plus the two FUN_0046a190 call sites (the edge's airborne path and
// FUN_00461954's K_SPWEP frame-8 trigger).
//
// OBSERVED semantics (MDK95.EXE BUILD_A disasm):
//
//   Edge dispatch (0x465675): 0x4ce774 != 0 ->
//       0x540e60 (cmdObj60) != 0 -> FUN_00459d28: clears the object's
//           +0x118 latch when >= 0. cmdObj60 is written by cmdBody2 —
//           the thrown SW_INTER/WMIB spin loop — so a second itemUse
//           press DETONATES a live WMIB early (remote-detonate).
//       0x540e60 == 0 -> the inventory gate at 0x4658ef:
//           0x541610 (inv count) == 0            -> return
//           slot[0x541614].id == 6             -> return (SW_GATT is
//               feed-ammo only — no thrown form)
//           0x540e54 (cmdFlag54) != 0          -> ids 1/8/9 only
//           |0x540c78|==0 AND 0x540c54&1       -> grounded & still:
//               posts event 0x54cb08=0x325 / 0x54cb00=8 — the
//               dispatcher then enters locoState 0x325 (K_SPWEP) and
//               the ANIM frame-8 crossing calls FUN_0046a190
//               (anim-timed throw, OBSERVED 0x462e5d..0x462e67).
//           else locoState must be one of 0x2bd/0x2be/0x2bf
//               (K_CHUTE/K_JUMP/K_RJMP — airborne only) ->
//               0x541558 = 0x3c, FUN_0046a190 now.
//
//   FUN_0046a190 — the thrown-item spawn:
//       scan 0x49bbc4 (identity 1..9) for slot.id -> 0x49bba0 name ->
//       FUN_00454794 = EnemyTable::indexOf; JG — idx must be > 0.
//       allocFront (FUN_0045cffc) on the current arena; +0x0c = the
//       FUN_00403720 deep-copied model (fallback: table base — the
//       port leaves the model empty, no analogue); +0x04 = idx.
//       pos = player pos, z += 4.0 (0x498d38); +0x1c anchor and
//       +0x180.. prevPos = pos; +0x60 = current arena.
//       FUN_004566f0 init; +0x30e = 150; +0x44 = 0; +0x58 = 0.1f;
//       dword+0x148 |= 0x818a6 (+0x148|=0xa6, +0x149|=0x18 — the
//       enemy-dispatch bit — +0x14a|=0x08); +0x30a = slot.id — the
//       item id doubles as the command id: the thrown object runs
//       cmdBody<id> (DUMMY=1, INTER=2, TWIST=3, THUMP=4, KEY=7,
//       SEAL=8, SBONE=9 — the ported cmd bodies ARE the item
//       behaviors; id5 SW_HBOMB arms cmdDetonate58 instead).
//       +0x4c = 0x540c2c yaw; FUN_00437f98 -> +0x28 = cos*25,
//       +0x2c = sin*25 (0x498d40); +0x30 = 0 when chute else 15.0.
//       id==5: +0x28/+0x2c *= 3.0 (0x498d48).
//       id==1 (DUMMY): +0xe0=0, +0x118=-1, +0xe4=-1, +0xdc=0,
//           +0x114 = 0x54c6a0 (the image-resident decoy record — the
//           port has no analogue; animRec stays nullptr, the same
//           convention cmdBody1's 0x54c6ac re-arm already uses).
//       id==8/9: +0x30e = 750.
//       FUN_0045612c rebuild; 0x541558 = 0x3c; slot.charges -= 1;
//       == 0 -> FUN_0046a3d8 (inventoryRemove) on 0x541614.
// ---------------------------------------------------------------------------

// FUN_0046a190 — spawn the selected inventory item as a thrown
// object in the current arena.
void itemUseSpawn(TraversalRuntime& rt) {
  if (rt.inventoryCount == 0 || rt.cur == nullptr) return;
  if (rt.inventorySel < 0 || rt.inventorySel >= 5) return;
  InventoryRecord& slot = rt.inventory[rt.inventorySel];
  const int id = slot.id;
  const char* name = (id >= 1 && id <= 9) ? kItemNames[id - 1] : nullptr;
  if (name == nullptr) return;
  const int eidx = rt.level.enemies.indexOf(name);  // FUN_00454794
  if (eidx <= 0) return;                            // JG — strictly > 0

  DynamicObject& o = rt.cur->dyn.allocFront();      // FUN_0045cffc
  if (const RuntimeModel* m = traversalModelFor(eidx, &rt.level))
    o.model = deepCopyModel(*m);                    // FUN_00403720
  o.enemyIndex = static_cast<std::uint16_t>(eidx);  // +0x04
  o.setPosition(rt.cs.pos[0], rt.cs.pos[1],
                rt.cs.pos[2] + 4.0f);               // C(0x498d38)
  o.field1c[0] = o.pos[0];                          // +0x1c..+0x24 = pos
  o.field1c[1] = o.pos[1];
  o.field1c[2] = o.pos[2];
  o.prevPos[0] = o.pos[0];                          // +0x180.. = pos
  o.prevPos[1] = o.pos[1];
  o.prevPos[2] = o.pos[2];
  initObjectCollision(o);                           // FUN_004566f0
  o.field30e = 0x96;                                // 150-tick life
  o.field44 = 0.0f;
  o.col.scale = 0.1f;                               // 0x3dcccccd
  o.col.flags148 = static_cast<std::uint16_t>(o.col.flags148 | 0x18a6);
  o.col.flags149 = static_cast<std::uint8_t>(o.col.flags148 >> 8);
  o.col.flags14a |= 0x08;                           // dword |0x818a6
  o.field30a = static_cast<std::uint32_t>(id);      // +0x30a cmd = id
  o.yawDeg = rt.motion.yawDeg;                      // +0x4c = 0x540c2c
  float s = 0.0f, c = 0.0f;
  sincosDeg(rt.motion.yawDeg, &s, &c);              // FUN_00437f98
  o.field28 = c * 25.0f;                            // +0x28 — C(0x498d40)
  o.field2c = s * 25.0f;                            // +0x2c
  o.field30 = (rt.locoState == 0x2bd) ? 0.0f : 15.0f; // 0x41700000
  if (id == 5) {                                    // SW_HBOMB throws
    o.field28 *= 3.0f;                              //   3x — C(0x498d48)
    o.field2c *= 3.0f;
  }
  if (id == 1) {                                  // DUMMY anim arm —
    o.animRate = 0.0f;                            // +0xe0 = 0
    o.animLatch = -1;                             // +0x118 = 0xffff
    o.animFrame = -1;                             // +0xe4 = 0xffff
    o.animAcc = 0.0f;                             // +0xdc = 0
    o.animRec = rt.animSwDumI;                    // +0x114 = 0x54c6a0
                                                //   (SW_DUM_I record)
  }
  if (id == 8 || id == 9) o.field30e = 0x2ee;      // 750 — SEAL/SBONE
  rebuildObjectTransform(o);                      // FUN_0045612c
  rt.invHudTimer = 0x3c;                          // 60
  if (--slot.charges == 0) inventoryRemove(rt, rt.inventorySel);
  ++rt.seams.itemSpawnCalls;
}

} // namespace

// The 0x4ce774 edge consumer — FUN_00465228 tail 0x465675.
void traversalItemUseEdge(TraversalRuntime& rt) {
  ++rt.seams.itemUseCalls;
  if (rt.cmdObj60 != nullptr) {
    // FUN_00459d28 — a live cmd-2 (INTER/WMIB) object: clearing its
    // +0x118 latch makes the next anim-done check detonate it.
    if (rt.cmdObj60->animLatch >= 0) rt.cmdObj60->animLatch = -1;
    return;
  }
  // 0x4658ef — the inventory activation gate.
  if (rt.inventoryCount == 0) return;
  if (rt.inventorySel < 0 || rt.inventorySel >= 5) return;
  const int id = rt.inventory[rt.inventorySel].id;
  if (id == 6) return;                            // SW_GATT unusable
  if (rt.cmdFlag54 != 0 && id != 1 && id != 8 && id != 9) return;
  if (rt.vert.vertVel == 0.0f && (rt.vert.contactFlags & 1) != 0) {
    // Grounded & still — post event 0x325/8 (0x54cb08/0x54cb00): the
    // dispatcher enters locoState 0x325 (K_SPWEP) and the anim
    // frame-8 crossing spawns the item instead (anim-timed throw).
    rt.eventMag = 0x325;
    rt.eventType = 8;
    return;
  }
  if (rt.locoState != 0x2bd && rt.locoState != 0x2be &&
      rt.locoState != 0x2bf)
    return;                                     // airborne states only
  rt.invHudTimer = 0x3c;                        // 0x541558 = 60
  itemUseSpawn(rt);                             // FUN_0046a190
}

// FUN_00461954's K_SPWEP (locoState 0x325) frame-8 trigger —
// 0x462e5d..0x462e67: invHudTimer = 60 is set by the caller, then
// FUN_0046a190 fires the anim-timed throw.
void traversalItemUseAnimTrigger(TraversalRuntime& rt) {
  ++rt.seams.animActionCalls;
  itemUseSpawn(rt);
}

void traversalPickupCollect(TraversalRuntime& rt) {
  if (rt.cur == nullptr) return;
  const int* row = kItemAmt[rt.difficulty == 0 ? 0
                            : rt.difficulty == 1 ? 1
                                                 : 2];
  const bool isDant2 = rt.cur->name == "DANT_2";  // +0x540c48 arena
                                                  // name compare —
                                                  // the id5 charge
                                                  // override (OBSERVED)
  for (auto& up : rt.cur->dyn.storage) {
    DynamicObject& o = *up;
    if (!o.col.named) continue;                    // +0x06
    if ((o.col.flags14a & 0x20) == 0) continue;    // mover bit
    if ((o.col.flags148 & 0x10) != 0 ||
        (o.col.flags14a & 0x04) != 0) {            // dword &0x40010
      continue;
    }
    const float box[6] = {o.col.aabb[0] - 1.0f, o.col.aabb[1] - 1.0f,
                          o.col.aabb[2] - 5.0f, o.col.aabb[3] + 1.0f,
                          o.col.aabb[4] + 1.0f, o.col.aabb[5] + 1.0f};
    if (!segVsAabb(rt.cs.entryPos, rt.cs.pos, box)) continue;
    // *(obj+0xc) — the deep-copied 0x88 table-1/model record; the
    // streq reads its byte-0, the table-1 ENTRY NAME written inline
    // by FUN_004286c8 (e.g. "SW_BONES"), NOT the geometry's own
    // name-table[0] ("BONEHEAD"). entries[enemyIndex].name is the
    // same string. Out-of-range mirrors the +0xc==0 DAT_004edcc0
    // fallback (name "", matches nothing).
    const std::string& name =
        o.enemyIndex < rt.level.enemies.entries.size()
            ? rt.level.enemies.entries[o.enemyIndex].name
            : o.scriptClass;

    // Item-table names first (0x49bb7f..0x46981b).
    int itemId = 0;
    for (int i = 0; i < 9; ++i) {
      if (name == kItemNames[i]) {
        itemId = i + 1;
        break;
      }
    }
    if (itemId == 0) {
      int pk = -1;
      for (int i = 0; i < 12; ++i) {
        if (name == kPickupNames[i]) {
          pk = i;
          break;
        }
      }
      // A mover that contacts but names neither table ends the WHOLE
      // scan for the frame (0x469800..0x469812 — the pickup-name
      // loop's exhaustion exit is `return`, OBSERVED quirk).
      if (pk < 0) return;
      pickupGrant(rt, pk);
      collectDespawn(rt, o);
      ++rt.seams.pickupCollects;
      return;
    }

    // --- item path (the FUN_0046a500 body inlined at 0x46987c+) ---
    if ((itemId == 5 || itemId == 6) && rt.inventoryCount > 0) {
      for (int s = 0; s < rt.inventoryCount; ++s) {
        InventoryRecord& rec = rt.inventory[s];
        if (rec.id != itemId) continue;
        // Stack onto the existing slot (0x469885..0x469872).
        collectSfx(rt, "COLLECT");
        if (itemId == 6) {
          rt.ammo[0] += row[5];               // 0x54161f carry rounds
        } else {
          rec.charges += isDant2 ? 1 : row[4];
        }
        collectNotify(rt, name);              // +0xc — the entry name
        collectDespawn(rt, o);
        ++rt.seams.pickupCollects;
        rt.invHudTimer = 0x3c;                // 0x541558
        if (itemId == 6) return;              // id6 returns before the
        rt.inventorySel = s;                  // sel write (OBSERVED)
        return;                               // 0x541614
      }
    }
    if (rt.inventoryCount < 5) {
      // New slot (0x46998d..0x469b96): id2's sfx is the WMIB record
      // (0x54c65c — the "World's Most Interesting Bomb" jingle, the
      // SW_INTER item; MDKFONT.FTI maps SW_INTER -> "World's Most\n
      // Interesting Bomb", while SW_KEY (id 7) is the nuke -> "World's
      // Smallest\nNuclear Explosion"); all other ids play COLLECT.
      collectSfx(rt, itemId == 2 ? "WMIB" : "COLLECT");
      collectNotify(rt, name);                // +0xc — the entry name
      rt.invHudTimer = 0x3c;
      collectDespawn(rt, o);
      ++rt.seams.pickupCollects;
      const int idx = rt.inventoryCount;
      // id6 selects the new slot only when the inventory was empty
      // (0x469a19 — OBSERVED exception); all other ids always select.
      if (itemId != 6 || rt.inventoryCount == 0) rt.inventorySel = idx;
      rt.inventoryCount += 1;
      InventoryRecord& rec = rt.inventory[idx];
      rec.aux = 0;                            // +0x20
      rec.slotX = idx * 0x30 + 0x20;          // +0x18 HUD target x
      rec.slotY = 0x148;                      // +0x1c bar row y
      // animX/animY start at the object's screen-rect center
      // ((+0x64 + +0x6c)/2, (+0x68 + +0x70)/2) and slide to the slot
      // at rate 2.0 (0x498d08). +0x64..+0x70 is render-projection
      // scratch with no headless-core mirror — the port evaluates
      // the same formulas with the absent input = 0 (the icon flies
      // from the origin; presentation-only difference).
      rec.animX = 0.0f;
      rec.animY = 0.0f;
      rec.animVel = (static_cast<float>(rec.slotX) - rec.animX) * 2.0f;
      rec.animAux = (static_cast<float>(rec.slotY) - rec.animY) * 2.0f;
      rec.id = itemId;
      rec.charges = (itemId == 5 && !isDant2) ? row[4] : 1;
      if (itemId == 6) rt.ammo[0] += row[5];
      return;
    }
    // Full inventory — the object stays; scanning continues
    // (0x469a2c loop-continue, OBSERVED).
  }
}

} // namespace mdk
