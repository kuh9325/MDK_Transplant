// player_pickup.h — the FUN_004696d8 traversal pickup collector
// (Phase P0-B). The per-frame player-vs-pickup contact scan that
// grants ammo/health/inventory and arms the carry-away despawn.
//
// OBSERVED (MDK95.EXE BUILD_A, Ghidra decompile + raw disasm):
//   FUN_004696d8 @ 0x4696d8 — the collector body.
//   Call site @0x46382c inside FUN_00463608 — unconditional, every
//   frame, even while dying (the 0x541510/0x541554 death tail above
//   it does not skip the call).
//   FUN_0046a790 @ 0x46a790 — the 12-id pickup grant switch.
//   FUN_0046a500 @ 0x46a500 — the inventory-item grant (the same
//   body is inlined in FUN_004696d8 for the SW_DUMMY*-table hits).
//   FUN_0045cc2c @ 0x45cc2c — swept-segment AABB test.
//   FUN_0045828c @ 0x45828c — child teardown (objectTeardownNow).
//   FUN_004599e8 @ 0x4599e8 — the carry/fly-away the despawn marks
//   route to (cmdBodyCarry, gated +0x149&0x10 -> +0x14a&4).
#pragma once

namespace mdk {

struct TraversalRuntime;

// FUN_004696d8 — scan the current arena's +0x68 object list once;
// the first touched mover matching the pickup/item name tables is
// granted + despawn-armed and the function returns (one collect per
// frame). A touched mover naming neither table ALSO returns — the
// whole frame's scan ends (OBSERVED quirk). A full inventory on the
// item path is the only outcome that lets the scan continue.
void traversalPickupCollect(TraversalRuntime& rt);

// The freefall->traversal carry grant — applies a 0x49bba0 key/seal
// table row (kFfEvGrantKey `a`) to the shared 0x54155c inventory
// block via the FUN_0046a500 insert, so a mid-fall SW_GATT/SW_KEY/
// SW_SEAL pickup lands in the traversal inventory. The row is the
// 0-based table index -> item id index+1. No presentation (the
// freefall pickup's teletype post already ran).
void traversalInventoryCarryItem(TraversalRuntime& rt, int itemIndex);

// ---------------------------------------------------------------------------
// P0-C — item use (FUN_00465228 tail + FUN_0046a190; see the cpp for
// the full evidence map).
// ---------------------------------------------------------------------------

// The 0x4ce774 item-use edge (0x465675): cmdObj60 live -> FUN_00459d28
// clears the spinning INTER/WMIB's anim latch (remote detonate); else
// the 0x4658ef inventory gate — grounded+still posts event 0x325/8
// (the K_SPWEP anim-timed throw), airborne 0x2bd/2be/2bf spawns now.
void traversalItemUseEdge(TraversalRuntime& rt);

// FUN_00461954's locoState-0x325 (K_SPWEP) frame-8 trigger — the
// anim-timed item throw (0x462e67 -> FUN_0046a190).
void traversalItemUseAnimTrigger(TraversalRuntime& rt);

} // namespace mdk
