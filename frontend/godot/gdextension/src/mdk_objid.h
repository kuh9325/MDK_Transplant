// Phase 9 (G3) — stable opaque object IDs for the frontend.
//
// Godot/GDScript must never see a DynamicObject* — the bridge hands
// out dense uint64 IDs instead. Core stores DynamicObjects in
// std::list<std::unique_ptr>, so an object's address is stable for
// its whole lifetime INCLUDING arena transfers (list splice moves
// ownership, not the object). That address is the map key; the ID is
// an unrelated counter value.
//
// Lifetime rules:
//   * same live address + same identity fingerprint -> same ID
//     (covers frames, moves, and arena transfers);
//   * object freed -> entry pruned on the next pass; the stale ID
//     resolves to nothing forever after;
//   * address reused by a different object (fingerprint mismatch)
//     -> a FRESH ID, so a stale ID can never alias a new object.
// A same-fingerprint address reuse is indistinguishable from a
// re-spawn of the same record and keeps its ID — documented edge.
//
// The map is engine-free scalar code so the native frontend tests
// exercise the same rules the bridge relies on.
#ifndef MDK_FRONTEND_OBJID_H
#define MDK_FRONTEND_OBJID_H

#include <cstdint>
#include <unordered_map>

namespace mdkfront {

class MdkObjectIds {
public:
  // Start a snapshot pass: all entries become unseen.
  void beginPass() {
    for (auto& kv : byKey_) kv.second.seen = false;
  }

  // Report a still-allocated object (call for EVERY live object,
  // whether or not it is emitted this frame).
  void markLive(const void* key) {
    auto it = byKey_.find(key);
    if (it != byKey_.end()) it->second.seen = true;
  }

  // ID for an object being emitted now. `fingerprint` is a digest
  // over stable identity fields (enemy index, spawn id, model name,
  // script class/offset) — NOT the address. Mints on first sight,
  // re-mints when the fingerprint says the address now holds a
  // different object.
  uint64_t idFor(const void* key, uint64_t fingerprint) {
    auto it = byKey_.find(key);
    if (it != byKey_.end() && it->second.fingerprint == fingerprint) {
      it->second.seen = true;
      return it->second.id;
    }
    if (it != byKey_.end()) {
      byId_.erase(it->second.id);
      it->second.id = next_++;
      it->second.fingerprint = fingerprint;
      it->second.seen = true;
      byId_[it->second.id] = key;
      return it->second.id;
    }
    Entry e;
    e.id = next_++;
    e.fingerprint = fingerprint;
    e.seen = true;
    byKey_[key] = e;
    byId_[e.id] = key;
    return e.id;
  }

  // Drop entries for objects no longer allocated.
  void endPass() {
    for (auto it = byKey_.begin(); it != byKey_.end();) {
      if (it->second.seen) {
        ++it;
      } else {
        byId_.erase(it->second.id);
        it = byKey_.erase(it);
      }
    }
  }

  // Reverse lookup for per-object detail calls; nullptr for a stale
  // or unknown ID. Only live entries resolve.
  const void* find(uint64_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
  }

  std::size_t size() const { return byKey_.size(); }

private:
  struct Entry {
    uint64_t id = 0;
    uint64_t fingerprint = 0;
    bool seen = false;
  };
  std::unordered_map<const void*, Entry> byKey_;
  std::unordered_map<uint64_t, const void*> byId_;
  uint64_t next_ = 1;
};

}  // namespace mdkfront

#endif  // MDK_FRONTEND_OBJID_H
