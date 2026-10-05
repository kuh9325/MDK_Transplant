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

// FUN_0041cad0(name, 1, 0x40000000) — the status-message post; a
// counted seam in the port (the same convention as the OOT_L%d
// mission posts).
void collectNotify(TraversalRuntime& rt) { ++rt.seams.hudMsgPosts; }

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
  // FUN_0041cad0 tail: ids 9 and 11 post nothing (OBSERVED).
  if (id != 9 && id != 11) collectNotify(rt);
}

} // namespace

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
        collectNotify(rt);
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
      // (0x54c65c — the World's Most Interesting Bomb jingle, the
      // SW_INTER item); all other ids play COLLECT.
      collectSfx(rt, itemId == 2 ? "WMIB" : "COLLECT");
      collectNotify(rt);
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
