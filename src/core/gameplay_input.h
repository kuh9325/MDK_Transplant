// Phase 5A — gameplay input consumption.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md for the full evidence chain):
//
//   Per frame the original input pump FUN_004187e0 runs:
//     FUN_0046b688  keyboard poll  -> level/latch bitmaps (internal
//                                   0..127 key-code domain, four dwords)
//     FUN_0046bc18  mouse poll     -> dx/dy/dz deltas + 4-button nibble
//     FUN_0046b9b4  joystick poll  -> 16 button bits + 6 analog axes
//     FUN_00419370  action flags   -> 0x54b650..0x54b6c8 from the
//                                   29-dword binding block 0x5413fe
//   Then in traversal (FUN_00436100 -> FUN_00463608) the consumer
//   FUN_00406f14 folds the action flags, the mouse deltas through the
//   W-set axis map/scales, the mouse-button action masks, and the
//   joystick tables into the control block 0x4ce6e0..0x4ce7ac.
//
//   Phase 5A reproduces exactly that boundary: raw device state +
//   configured bindings -> the semantic per-frame control state. It
//   does NOT move the player, drive the camera, or touch the world —
//   the downstream consumers of the 0x4ce block are later-phase work.
//
// NATIVE PORT DECISIONS:
//   - Platform-neutral types only; no SDL, no frontend objects.
//   - Joystick raw fields/bindings are modeled because the original
//     consumer reads them, but nothing upstream feeds them yet
//     (JoyOn=false reproduces the canonical no-device path).
//   - `moveBoostGate`/`debugMoveBoost` are non-binding runtime gates
//     the original function reads (0x540c80 / 0x5414e4). Their
//     writers' game-state meaning is UNKNOWN (Phase 5B concern);
//     they are modeled as environment inputs, not bindings.
//
#ifndef MDK_CORE_GAMEPLAY_INPUT_H
#define MDK_CORE_GAMEPLAY_INPUT_H

#include "core/keyboard_menu.h"   // kKeyboardDefaults, kKeyboardGlobalCount

#include <array>
#include <cstdint>
#include <string>

namespace mdk {

// The original's internal key-code domain — four 32-bit words cover
// codes 0..127 (Phase 4K). Bitmap index: word = code >> 5,
// bit = 1 << (code & 31).
inline constexpr int kGameplayKeyBitmapWords = 4;
inline constexpr int kGameplayKeyCount = 128;

// The 10 hidden direct-weapon hotkeys — global indices 14..23 of the
// 29-dword block (factory internal codes 2..11 = '1'..'0').
inline constexpr int kGameplayWeaponHotkeyCount = 10;
inline constexpr int kGameplayWeaponSlotBase = 14;

// Mouse action-mask bit semantics (JOY_BA..JOY_BP, OBSERVED from the
// original's own string records — the consumer decodes the same 16
// bits for joystick-button and mouse-button masks alike).
enum GameplayButtonBit : std::uint32_t {
  kBtnFire       = 0x0001,  // JOY_BA
  kBtnSniper     = 0x0002,  // JOY_BB  (level -> synthetic edge)
  kBtnJump       = 0x0004,  // JOY_BC
  kBtnLookUp     = 0x0008,  // JOY_BD
  kBtnLookDown   = 0x0010,  // JOY_BE
  kBtnItemUse    = 0x0020,  // JOY_BF
  kBtnItemNext   = 0x0040,  // JOY_BG
  kBtnItemPrev   = 0x0080,  // JOY_BH
  kBtnZoomIn     = 0x0100,  // JOY_BI
  kBtnZoomOut    = 0x0200,  // JOY_BJ
  kBtnSideStep   = 0x0400,  // JOY_BK  (the strafe MODIFIER)
  kBtnStrafeLeft = 0x0800,  // JOY_BL
  kBtnStrafeRight= 0x1000,  // JOY_BM
  kBtnMoveFwd    = 0x2000,  // JOY_BN
  kBtnMoveBack   = 0x4000,  // JOY_BO
  kBtnTurbo      = 0x8000,  // JOY_BP
};

// Axis-map letter semantics (JOY_AA..JOY_AH + JOY_A0, OBSERVED).
// The same letters serve the 3-axis mouse map and the 6-axis
// joystick map. Letters '0' and anything outside A..H are inert.
enum class GameplayAxisLetter : char {
  Off = '0',
  Turn = 'A',       NegTurn = 'D',
  Move = 'B',       NegMove = 'E',
  SideStep = 'C',   NegSideStep = 'F',
  SniperZoom = 'G', NegSniperZoom = 'H',
};

// Raw per-frame device state in the ORIGINAL device domains — the
// inputs FUN_00406f14 reads (plus the bitmaps FUN_00419370 consumes).
struct RawGameplayInput {
  // Internal key-domain bitmaps (FUN_0046b688 products):
  //   keyLevel — pure level bitmap (press sets, release clears)
  //   keyEdge  — per-poll new-press bitmap (latch & ~prev)
  std::array<std::uint32_t, kGameplayKeyBitmapWords> keyLevel{};
  std::array<std::uint32_t, kGameplayKeyBitmapWords> keyEdge{};
  // DIMOUSESTATE deltas + packed button nibble (FUN_0046bc18 ->
  // 0x54b644..0x54b650 accumulators).
  std::int32_t mouseDx = 0;
  std::int32_t mouseDy = 0;
  std::int32_t mouseDz = 0;
  std::uint32_t mouseButtons = 0;  // bit i = physical button i held
  // Joystick raw state (FUN_0046b9b4 -> 0x54b534/0x54b538): 16 button
  // bits and six already-normalized analog axes. Unwired upstream —
  // zeros reproduce the no-device path.
  std::uint32_t joyButtons = 0;
  std::array<float, 6> joyAxes{};
};

// Configured bindings + device tables — the post-config values the
// consumer reads. Keyboard: the full 29-dword block (19 settings
// slots + 10 hidden weapon hotkeys). Mouse: the W set — the D set
// has no BUILD_A consumer (documented dead for this build). Joystick:
// the W/D button-mask tables and the axis map the consumer reads.
struct GameplayInputBindings {
  std::array<int, kKeyboardGlobalCount> keys = kKeyboardDefaults;
  std::string mouseAxesMap = "ABG";  // up to 3 letters used, NUL ends
  std::array<std::uint32_t, 4> mouseButtMask{1, 4, 2, 0};
  std::array<float, 3> mouseScale{16.0f, 16.0f, 50.0f};
  bool mouseOn = true;                       // 0x541472
  std::uint32_t mouseYReversedBits = 0;      // 0x541476 raw bits
  bool joyOn = false;                        // 0x541310
  std::uint32_t joyType = 0;                 // 0x541338
  std::string joyAxesMap;                    // up to 6 letters
  std::array<std::uint32_t, 16> joyButtMask{};
};

// Non-binding runtime gates the original consumer reads each frame.
// These are game/timing state, not configuration.
struct GameplayInputEnvironment {
  // 0x540c80 — nonzero suppresses the x4/3 move-rate lift. Phase 5B
  // resolved the writer: FUN_00466740 sets it while airborne with
  // the jump key held (the jump-sustain/floaty-gravity gate), and it
  // doubles as the horizontal move-rate lift gate. Still modeled as
  // an environment input until the vertical consumer is native.
  std::uint32_t moveBoostGate = 0;
  // 0x5414e4 — debug flag (written by the debug-command dispatcher);
  // together with Tab level it overrides the move rates to 20/3 x.
  std::uint32_t debugMoveBoost = 0;
  // 0x49b6f0 — the smoothed frame-units factor (EMA ~= 1.0 at the
  // nominal 30 Hz; FrontendTimingState::smoothed). OBSERVED as the
  // FDIV divisor in the FUN_00406f14 mouse-rate path.
  float smoothedDelta = 1.0f;
  // 0x49b6e8 — the integer frame step (1..4; FrontendTimingState::
  // frameStep). The zoom accumulator decays by this count per frame.
  std::int32_t frameStep = 1;
};

// Consumer-internal state that persists across frames — the original
// globals, carried here so the function stays deterministic.
struct GameplayInputState {
  std::int32_t zoomAccumulator = 0;   // 0x4ce760 — clamped [-8,8],
                                      // decays by frameStep toward 0
  std::uint32_t setTurboLatch = 0;    // 0x540d38 — STURB edge toggles
  std::uint32_t sniperButtonLatch = 0;// 0x499f50 — previous button
                                      // snipe-request (synthetic edge)
};

// The semantic per-frame control state — the 0x4ce6e0..0x4ce7ac block
// plus the axis/modifier state the consumer computes. Field comments
// give the original output address. No world state is touched.
struct GameplayInputFrame {
  // Digital action flags (0x4ce768..0x4ce7ac). Mouse/joystick button
  // masks OR into all except weaponSelect (masks have no weapon bits).
  std::uint32_t jump = 0;         // 0x4ce768 — kbd level | mask bit2
  std::uint32_t sniperPulse = 0;  // 0x4ce76c — kbd EDGE | mask bit1
                                  // through a synthetic press edge
  std::uint32_t fire = 0;         // 0x4ce770 — kbd level | mask bit0
  std::uint32_t itemUse = 0;      // 0x4ce774 — kbd edge | mask bit5
  std::uint32_t itemNext = 0;     // 0x4ce778 — kbd edge | mask bit6
  std::uint32_t itemPrev = 0;     // 0x4ce77c — kbd edge | mask bit7
  std::uint32_t lookUp = 0;       // 0x4ce780 — kbd level | mask bit3
  std::uint32_t lookDown = 0;     // 0x4ce784 — kbd level | mask bit4
  std::array<std::uint32_t, kGameplayWeaponHotkeyCount>
      weaponSelect{};             // 0x4ce788..0x4ce7ac — kbd edges
  std::int32_t zoomAccumulator = 0;   // 0x4ce760 — post-frame value
  std::uint32_t mouseTurnActive = 0;  // 0x4ce764 — mouse lateral flag

  // Modifier/latch state the consumer computes.
  bool sideStepHeld = false;   // iVar11 — kbd SIDE level | mask bit10
  bool turboLatched = false;   // 0x540d38 snapshot

  // Normalized digital/analog axes after the kbd/mask/joy merge
  // (intermediate values the original multiplies into rates).
  float turnAxis = 0;    // local_34 — -1 left / +1 right / joy analog
  float moveAxis = 0;    // local_38 — -1 fwd / +1 back / joy analog
  float moveDigital = 0; // local_28 — mask/kbd digital move
  float strafeAxis = 0;  // local_2c — -1 left / +1 right / joy analog
  float yawAxis = 0;     // fVar8 — larger |.| of strafe/turn

  // Rate products — the 0x4ce6e0..0x4ce73c block. `turbo` selects the
  // original's turbo constants; the mouse path overrides with
  // dt-normalized clamped-±4 values. Constants are OBSERVED doubles.
  float yawNorm = 0, yawFast = 0;           // 6e0 6e4
  float moveNorm = 0, moveFast = 0;         // 6e8 6ec
  float zoomVel = 0, zoomVelFast = 0;       // 6f0 6f4
  float strafeNorm = 0, strafeFast = 0;     // 6f8 6fc
  float turnNorm = 0, turnFast = 0;         // 700 704
  float moveVel = 0, moveVelBoosted = 0;    // 708 70c
  float yawNeg45 = 0;                       // 710
  float moveSpeed = 0;                      // 714 — asymmetric fwd/back
  float turnNorm75 = 0, turnFast75 = 0;     // 718 71c
  float moveHalfSlow = 0, moveHalfFast = 0; // 720 724
  float yaw4 = 0;                           // 728
  float move5pct = 0;                       // 72c
  float yawThird = 0, yaw10 = 0;            // 730 734
  float moveThird = 0, move10 = 0;          // 738 73c

  // Pass-through device state for the direct raw-delta consumers
  // (FUN_00464624 free-look applies mouseYReversed to dy;
  // FUN_004691c4 sniper reticle does not). Phase 5B reads these —
  // they are input state, not computed controls.
  std::int32_t mouseDx = 0, mouseDy = 0, mouseDz = 0;
  std::uint32_t mouseButtons = 0;
  bool mouseOn = true;
  bool mouseYReversed = false;
};

// The per-frame consumption — FUN_00419370's flag production plus
// FUN_00406f14's control merge, in original order. `state` carries
// the persistent accumulators/latches (zoomAccumulator, setTurboLatch,
// sniperButtonLatch) and is updated in place.
GameplayInputFrame consumeGameplayInput(
    const RawGameplayInput& raw,
    const GameplayInputBindings& bind,
    const GameplayInputEnvironment& env,
    GameplayInputState& state);

// Settings -> live bindings snapshot. The keyboard block comes from
// keyboardGlobalsFromSettings (factory block + the 19 persisted
// slots; the hidden hotkeys always hold their factory values). The
// mouse W set maps straight from the settings fields. Joystick
// fields stay at their no-device defaults — nothing persists or
// feeds them in BUILD_A's keyboard+mouse route.
struct FrontendSettings;
GameplayInputBindings gameplayBindingsFromSettings(
    const FrontendSettings& s);

// The original level/edge queries (FUN_004192cc / FUN_00419320) —
// right-modifier folding included: querying code 0x2a (LSHIFT) also
// sees 0x36 (RSHIFT); 0x1d (LCTRL) sees 0x61 (RCTRL); 0x38 (LALT)
// sees 0x65 (RALT). Returns the raw masked value (nonzero = active),
// matching the original's `(mask & word)` result, not a normalized 1.
std::uint32_t gameplayKeyLevel(const RawGameplayInput& raw, int code);
std::uint32_t gameplayKeyEdge(const RawGameplayInput& raw, int code);

} // namespace mdk

#endif // MDK_CORE_GAMEPLAY_INPUT_H
