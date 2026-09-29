// Central mode-dispatch scaffold.
//
// ORIGINAL ENGINE OBSERVATION (Phase 2B, EXECUTABLE_MAP.md):
//   - One main loop (MDK95 FUN_0040103c) dispatches on a primary-mode
//     global (DAT_00541492, observed values 0,2,3,5,6,7,8) with a
//     sub-mode global (DAT_00541493, switch cases 1..11) selecting modal
//     overlay dialogs. A quit flag (DAT_0054148e) ends the loop.
//   - Semantic meaning of most original mode numbers is only partially
//     evidenced (0 frontend, 3 traversal, 8 cinematic are STRONG; others
//     remain candidate labels). We therefore do NOT name original modes.
//
// NATIVE PORT PROJECT DECISIONS:
//   - PrimaryModeId/SubModeId stay neutral integers; original numeric
//     values are recorded in mode::observed::* only as evidence, not
//     implemented.
//   - Native-only modes use negative IDs so they can never collide with
//     an original mode number.
//   - The dispatcher owns current primary/sub mode + quit flag and routes
//     one registered per-frame handler per primary mode.

#ifndef MDK_CORE_MODE_DISPATCH_H
#define MDK_CORE_MODE_DISPATCH_H

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace mdk {

using PrimaryModeId = std::int32_t;
using SubModeId = std::int32_t;

namespace mode {

// OBSERVED original primary-mode values (DAT_00541492 writers). Recorded
// for provenance only — do not implement semantics from these yet.
namespace observed {
inline constexpr PrimaryModeId frontend = 0;   // STRONG evidence
// Phase 18A (OBSERVED): the dispatch-tail `CMP DL,AL` at 0x40153e
// compares against EDX=1 (the loop's sentinel, set at 0x4010a1) —
// primary mode 1 runs FUN_00418e04, the static-noise transition
// frame (intro -> frontend dissolve etc.). Mode 4 and any other
// unlisted value fall through to FUN_0041dc90 (the frontend menu).
inline constexpr PrimaryModeId noiseTransition = 1; // STRONG evidence
inline constexpr PrimaryModeId transition = 2; // STRONG evidence
inline constexpr PrimaryModeId traversal = 3;  // STRONG evidence
inline constexpr PrimaryModeId stats = 5;      // STRONG evidence
inline constexpr PrimaryModeId levelLoad = 6;  // STRONG evidence
inline constexpr PrimaryModeId traverseContinue = 7; // STRONG evidence
inline constexpr PrimaryModeId cinematic = 8;  // STRONG evidence
} // namespace observed

// Native shell modes (NATIVE PORT values; negative = never original).
inline constexpr PrimaryModeId nativeBoot = -1;
inline constexpr PrimaryModeId nativeShell = -2;

inline constexpr SubModeId subModeNone = 0;

} // namespace mode

// Per-frame context handed to mode handlers.
struct FrameContext {
  std::uint64_t frameIndex = 0;
  double dtSeconds = 0.0;
  double elapsedSeconds = 0.0;
};

class ModeDispatcher {
public:
  using Handler = std::function<void(const FrameContext&)>;

  ModeDispatcher();

  PrimaryModeId primary() const { return primary_; }
  SubModeId sub() const { return sub_; }
  bool quitRequested() const { return quit_; }

  void setPrimary(PrimaryModeId id);
  void setSub(SubModeId id);
  void requestQuit();

  // Register the per-frame handler for a primary mode.
  void on(PrimaryModeId id, Handler handler);

  // Invoke the current primary mode's handler, if any.
  void dispatch(const FrameContext& ctx);

private:
  PrimaryModeId primary_;
  SubModeId sub_;
  bool quit_ = false;
  std::unordered_map<PrimaryModeId, Handler> handlers_;
};

} // namespace mdk

#endif // MDK_CORE_MODE_DISPATCH_H
