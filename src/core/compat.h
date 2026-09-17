// Compatibility-surface constants informed by Phase 2B evidence.
//
// ORIGINAL ENGINE OBSERVATION (docs/reverse-engineering/EXECUTABLE_MAP.md):
//   - Win95 build calls DirectDraw SetDisplayMode 640x480x8bpp
//     (FUN_0047aa74) — OBSERVED.
//   - Software working framebuffer DAT_00541650 is ~600x360 8bpp indexed;
//     the present path copies 360 rows into the DirectDraw surface —
//     OBSERVED.
//   - 8bpp indexed implies a 256-entry palette — CORROBORATED by the 8bpp
//     mode; original palette-update mechanics remain UNKNOWN.
//   - Original placement of the 600x360 image inside the 640x480 surface is
//     UNKNOWN (a candidate Phase 2C oracle observation).
//
// NATIVE PORT PROJECT DECISIONS:
//   - The 600x360 working image is centered inside the 640x480 presentation
//     canvas until oracle evidence establishes the original offset.
//   - The presentation canvas keeps the original 4:3 shape; the window may
//     be any size and the canvas is letterboxed/pillarboxed into it.

#ifndef MDK_CORE_COMPAT_H
#define MDK_CORE_COMPAT_H

namespace mdk::compat {

inline constexpr int kWorkWidth = 600;   // OBSERVED (~600x360 back buffer)
inline constexpr int kWorkHeight = 360;  // OBSERVED (360-row present copy)
inline constexpr int kPresentWidth = 640;   // OBSERVED (SetDisplayMode 640)
inline constexpr int kPresentHeight = 480;  // OBSERVED (SetDisplayMode 480)
inline constexpr int kPaletteEntries = 256; // CORROBORATED (8bpp indexed)

} // namespace mdk::compat

#endif // MDK_CORE_COMPAT_H
