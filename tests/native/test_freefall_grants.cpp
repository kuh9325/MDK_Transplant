// Regression for freefall pickup accounting without proprietary MDK assets.
// The synthetic player/pickup pool uses the real freefallStep collision and
// FUN_0046a9c8 pickup dispatch, then verifies cross-frame grant persistence.
#include <cstdio>
#include <cstring>

#include "core/freefall_runtime.h"

namespace {

int failures = 0;

void check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

bool saw(const std::vector<mdk::FreefallEvent>& events,
         int kind, int id, int amount = -1) {
  for (const auto& e : events) {
    if (e.kind == kind && e.a == id && (amount == -1 || e.b == amount))
      return true;
  }
  return false;
}

// Freefall's live pool keeps the player at listHead, and pickup records
// after the player in the object list. Start the pickup above the player
// and sweep it down across the updated player AABB in one 20 ms tick.
void collect(mdk::FreefallRuntime& ff, int rec, int eventKind) {
  auto& pl = ff.pool[0];
  auto& pk = ff.pool[1];
  pk = mdk::FreefallObject{};
  pk.alive = 1;
  pk.type = 4;
  pk.pickupRec = rec;
  pk.pz = pl.pz + 7.0f;
  pk.vz = -600.0f;
  pk.chute = mdk::kFfModelChute;
  pk.next = -1;
  pl.next = 1;
  ff.freeHead = 2;
  (void)mdk::freefallStep(ff, {}, 1, 1.0f, 0.02f);
  check(saw(ff.events, eventKind, rec == 0 ? 8 :
                                   rec == 1 ? 5 :
                                   rec == 2 ? 6 :
                                   rec == 3 ? 9 :
                                   rec == 4 ? 0 : 5),
        "pickup must dispatch its original grant event");
  check(pl.next == -1, "collected pickup must leave the live list");
}

} // namespace

int main() {
  mdk::FreefallRuntime ff;
  mdk::FreefallCourseData data;
  data.course = 1;
  data.skill = 1;
  for (const char* name :
       {"SW_H150", "SW_H25", "SW_H50", "SW_H01", "SW_HOME", "SW_GATT"}) {
    mdk::FreefallPickupRec row{};
    std::strncpy(row.name, name, sizeof(row.name) - 1);
    data.pickups.push_back(row);
  }
  mdk::freefallInit(ff, data, 42);
  ff.introCountdown = 0;
  ff.phase = mdk::FreefallRuntime::Phase::kPlay;
  ff.timeline = 1.0f;
  ff.listHead = 0;
  ff.freeHead = 2;        // pool[0] = player, pool[1] = synthetic pickup
  ff.pool[0].alive = 1;
  ff.pool[0].type = 0;
  ff.pool[0].next = -1;
  ff.cameraPos[2] = 100.0f;

  ff.health = 140;
  collect(ff, 0, mdk::kFfEvGrantHealth);
  check(ff.health == 150, "SW_H150 must raise health to 150");
  collect(ff, 1, mdk::kFfEvGrantHealth);
  check(ff.health == 150, "SW_H25 must not lower 150 HP to 100");
  collect(ff, 2, mdk::kFfEvGrantHealth);
  check(ff.health == 150, "SW_H50 must not lower 150 HP to 100");
  collect(ff, 3, mdk::kFfEvGrantHealth);
  check(ff.health == 150, "SW_H01 must not lower 150 HP to 100");

  ff.health = 75;
  collect(ff, 1, mdk::kFfEvGrantHealth);
  check(ff.health == 85, "SW_H25 must still heal 10 under 100 HP");
  collect(ff, 2, mdk::kFfEvGrantHealth);
  check(ff.health == 100, "SW_H50 must still cap healing at 100 HP");

  collect(ff, 4, mdk::kFfEvGrantAmmo);
  check(saw(ff.carryGrants, mdk::kFfEvGrantAmmo, 0, 8),
        "ammo grant must reach the fall-long ledger");
  collect(ff, 5, mdk::kFfEvGrantKey);
  check(saw(ff.carryGrants, mdk::kFfEvGrantKey, 5),
        "SW_GATT must reach the fall-long inventory ledger");
  check(ff.carryGrants.size() == 2,
        "health notifications must not pollute the inventory ledger");

  // The next regular frame clears presentation events, not campaign grants.
  (void)mdk::freefallStep(ff, {}, 1, 1.0f, 0.02f);
  check(ff.events.empty(), "presentation events clear after a quiet frame");
  check(ff.carryGrants.size() == 2,
        "items collected in earlier frames must survive to handoff");

  if (failures == 0) {
    std::puts("PASS: persistent freefall ammo/key grants and SW_H150 floor");
  }
  return failures == 0 ? 0 : 1;
}
