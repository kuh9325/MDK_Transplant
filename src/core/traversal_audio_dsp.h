// Traversal audio DSP — the BUILD_A DirectSound updater math
// (Phase 17C.2). Pure scalar code, no Godot/DS dependency: the Godot
// presenter drives one of these per live voice and applies the pushed
// params to its AudioStreamPlayer/bus.
//
// EVIDENCE (BUILD_A disasm — all instruction-exact):
//
//   FUN_004026f8 — the mixer dispatch. Once per frame it copies the
//   48-byte listener matrix (arg ESI — the 0x540bb0 view snapshot) to
//   0x49ff5c, picks the updater on the GLOBAL mode
//   (0x49ff58 & 0x300: 0x100 -> FUN_0040282c "2D", 0x200 ->
//   FUN_00402b00 "3D"; the sniper-enter seam FUN_00401ffc writes
//   byte1=2, scope teardown FUN_00402014 restores byte1=1), then walks
//   the instance list: inst+0xb bit 0x40 -> reap, 0x80 -> skip,
//   0x20 -> skip (paused). For a live instance with inst+0x9 bit0
//   (mode 0x100 — the one-shot-param latch, set by mode 0x10106):
//   inst+0x24 < 0 -> run the updater only; else run it, THEN strip
//   byte0's updater bits ((b & 0xf0) | 1) and toggle the latch off.
//   Net: a latched instance gets exactly ONE pushed update.
//
//   FUN_00402160 — positional spawn field init: inst+0x28=snd,
//   +0x8=mode, +0x18..20=baked pos (ECX arg), +0xc=owner slot ptr
//   (with *slot=inst), +0x10=live-pos ptr (stack arg), +0x14=matrix
//   ptr (stack arg), +0x30=vol arg, +0x34=rate arg, +0x38=range arg,
//   +0x3c = vol if (mode&1) else 0, +0x24 = -1.0f (the first-update
//   sentinel), +0x40 = 0, +0x44 = trunc(recRate * rate). The DS buffer
//   is created with the SPAWN values — so a mode&1==0 instance starts
//   silent (vol 0 -> -25 dB) until the updater's first push.
//
//   FUN_004022b8 — flat one-shot spawn: mode=1, +0x30/+0x3c =
//   record volume (snd+0x8), +0x34 = 1.0, +0x38 = 0, +0x24 = -1.0,
//   +0x40 = 0, +0x44 = recRate. Updater never runs (mode&0xe == 0).
//
//   Updater pos source — inst+0xa (mode bits 16..23):
//     bit0 (0x10000): baked +0x18 (modes 0x1000e/0x10106/0x10006)
//     bit1 (0x20000): live *inst+0x10 (mode 0x2000e — object voice)
//     bit2 (0x40000): inst+0x14-matrix * +0x18 + *inst+0x10 — not
//                     emitted by any ported callsite (seam)
//     else: (0,0,0)
//
//   FUN_0040282c "2D" (normal traversal):
//     rel = M * pos      (M = the 0x49ff5c listener copy, row-major)
//     dist = |rel|, or 1.0 when rel == 0 (FLDZ/FCOMP -> JNC keeps 1.0)
//     doppler (mode&8, only when prevDist >= 0):
//       shift = 1 + (prevDist - dist) * 30.0 / (frame * 1100.0)
//       clamp [0.25, 3.0]; freq = trunc(recRate * shift * rate)
//       [0x494190=30.0f, 0x494198=1100.0 dbl, 0x4941a0=0.25,
//        0x4941a8=3.0, 0x49b6f0=frame scalar]
//     pan (mode&4): n = rel/dist;
//       pan = trunc((nz*m[2][0] - nx*m[2][1]) * 32767.0)
//       [0x4941b0=32767.0f; m[2][0..1] = listener row2 cols 0..1]
//     vol (mode&2): dist<20 -> baseVol; dist>250 -> 0;
//       else trunc(baseVol * (250-dist) * (1/230)) — all double math
//       [0x4941b8=20.0, 0x4941c0=250.0, 0x4941c8=1/230]
//     DS pushes (SetFrequency/SetPan/SetVolume) fire only when
//     prevDist >= 0 at tick start; prevDist = dist last.
//
//   FUN_00402b00 "3D" (sniper scope):
//     same rel/dist (zero -> 0.0), same doppler.
//     vol (mode&2; NO pan path exists in this updater):
//       relz > 0            -> behind the listener plane -> vol 0
//       else axial = min(1, (400/zoom*0.75) / |relz|)
//            lat  = sqrt(relx^2 + rely^2)
//            lat <= 2        -> scaled lateral = (0,0)   [cone core]
//            lat >  2        -> latx' = relx*(1-2/lat) * (2/zoom)
//                              laty' = rely*(1-2/lat) * (768/(280*zoom))
//                              [0x4941f0=4.0 lat^2 gate,
//                               0x4941f8=2.0 core radius,
//                               0x494200=280.0f, 0x494204=1/384 —
//                               the 384x280 scope-aperture pair]
//            cone  = 1.3 - sqrt(latx'^2+laty'^2) / |relz|  [0x494218]
//            vol   = clamp(baseVol * cone * axial, 0, 32767)
//     The 1.3 edge means the in-cone boost is *1.3 at dead center —
//     vol can exceed baseVol (clamped 32767). OBSERVED quirk, kept.
//
//   Conversions (OBSERVED):
//     FUN_0040202c(flags, vol): vol * pct / 100 — C int math; flags&2
//       picks the music channel (0x54130c) else SFX (0x541308;
//       factory 70 — MDK.CFG "SoundFX" entry 8, domain [0,100]).
//     FUN_0046c27c: DS millibels = trunc(vol * (2500/32767)) - 2500
//       [0x498f50 = 2500/32767 double, bias 0x9c4]. Godot dB = mB/100.
//     FUN_0046c2ec: DS pan = (pan * 10000) >> 15  (SAR — arith shift).
//     FUN_0046c2c4: DS frequency = inst+0x44 verbatim.
//
// The 63-instance pool cap lives in the frontend (pool FUN_00402604's
// first-free/free-list scan fails when the 63 slots are exhausted).

#ifndef MDK_CORE_TRAVERSAL_AUDIO_DSP_H
#define MDK_CORE_TRAVERSAL_AUDIO_DSP_H

#include <cstdint>

namespace mdk {

// The DSP-relevant subset of one instance record (inst offsets noted).
struct TraversalAudioVoice {
  std::uint32_t mode = 0;      // inst+0x8 (byte1 bit0 = param latch,
                               // byte2 = pos source, byte3 = flags)
  float pos[3] = {0.f, 0.f, 0.f}; // inst+0x18 baked pos; callers refresh
                                  // this from the owner each tick for
                                  // the 0x20000 live-pos mode
  float prevDist = -1.0f;      // inst+0x24 — -1.0 spawn sentinel
  float baseVol = 0.f;         // inst+0x30 — spawn volume arg (0..32767)
  float rate = 1.0f;           // inst+0x34 — rate scale
  float range = 0.f;           // inst+0x38 — stored, unread by updaters
  int effVol = 0;              // inst+0x3c — post-falloff volume domain
  int pan = 0;                 // inst+0x40 — +-32767 domain
  int freqHz = 0;              // inst+0x44 — DS SetFrequency target
  int recRateHz = 0;           // snd+0xc — record sample rate
};

// The per-frame listener the mixer copies (0x49ff5c <- arg = the
// 0x540bb0 view matrix) plus the mode/zoom globals the updaters read.
struct TraversalAudioListener {
  float m[3][4] = {};          // listener matrix, row-major MDK space
  float zoom = 1.0f;           // 0x540b58 — the 3D cone's zoom divisor
  float frame = 1.0f;          // 0x49b6f0 — smoothed frame scalar
  bool mode3d = false;         // (0x49ff58 & 0x300) == 0x200
};

// FUN_00402160's field init (+ the FUN_004022b8 flat variant is just
// mode=1 with vol=record volume). `pos` may be null for flat spawns.
void traversalAudioVoiceInit(TraversalAudioVoice& v, std::uint32_t mode,
                             const float pos[3], int vol, float rate,
                             float range, int recRateHz);

// Which instance params this tick would have pushed onto the DS
// buffer — the original's writes are gated on the pre-tick
// prevDist >= 0 (the -1.0 sentinel's first update computes fields but
// pushes nothing; the spawn already set the DS values).
struct TraversalAudioPush {
  bool vol = false;
  bool pan = false;
  bool freq = false;
};

// One mixer tick on one instance: FUN_004026f8's per-node block
// (updater dispatch + the +0x9 latch bookkeeping). `v.pos` must hold
// the effective source position BEFORE the call — for the 0x20000
// live-pos mode the caller refreshes it from the owner, mirroring the
// original's *inst+0x10 read.
TraversalAudioPush traversalAudioVoiceTick(
    TraversalAudioVoice& v, const TraversalAudioListener& l);

// True while the updater still runs on this voice ((mode & 0xe) != 0).
// The latch strip and mode-1 flat spawns leave it false.
bool traversalAudioVoiceActive(const TraversalAudioVoice& v);

// FUN_0040202c — master volume scale, C int math (imul + idiv 100).
int traversalAudioScaledVol(int vol, int pct);
// FUN_0046c27c — DS millibel bias, trunc(vol * 2500/32767) - 2500.
int traversalAudioMilliBel(int scaledVol);
// Godot volume_db for a scaled domain-0..32767 volume.
float traversalAudioVolDb(int scaledVol);
// FUN_0046c2ec — DS pan domain +-10000.
int traversalAudioDsPan(int pan32767);
// Godot AudioEffectPanner domain (-1..1) from the +-32767 pan field.
float traversalAudioPanUnit(int pan32767);

} // namespace mdk

#endif // MDK_CORE_TRAVERSAL_AUDIO_DSP_H
