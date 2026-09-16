# Tool & Machine Inventory

Recorded: 2026-09-16 (Phase 0 baseline); updated 2026-09-17 (Phase 2A).
Phase 0 method was availability checks only; Phase 2A installed one
runtime tool (DOSBox-X, below).

## Machine

| Item | Value |
|---|---|
| Kernel | Darwin 27.0.0 (xnu-13432.1.9~1/RELEASE_ARM64_T8132) |
| Architecture | arm64 (Apple Silicon, T8132) |
| macOS | 27.0 (build 26A428) |
| Xcode | 27.0 (build 27A266a) |
| clang | Apple clang 21.0.0, target arm64-apple-darwin27.0.0 |
| cmake | 4.4.3 |
| Python | 3.9.6 (system); Homebrew also has python@3.12, python@3.14 |
| git | 2.54.0 (Apple Git-157) |
| file | 5.41 |
| lldb | lldb-2103.0.34.103 |
| Homebrew | present at /opt/homebrew |

## Reverse-engineering / analysis tools

| Tool | Status |
|---|---|
| Ghidra | NOT installed |
| Hopper | NOT installed |
| ImHex | NOT installed |
| Kaitai Struct compiler | NOT installed |
| DOSBox-X | **2026.08.31** SDL2 — Homebrew formula, arm64, `/opt/homebrew/bin/dosbox-x` (Phase 2A; see `RUNTIME_ORACLE.md`) |
| DOSBox / DOSBox-staging | NOT installed |
| QEMU | NOT installed (11.1.1 bottled in Homebrew; planned Win95-lane tool once OS media is supplied) |
| Wine / CrossOver | NOT installed (`wine-stable` cask is x86-only + Gatekeeper-disabled; no Rosetta) |
| Rosetta 2 | NOT installed |
| radare2 | NOT installed |
| Binary Ninja | NOT installed |
| LIEF (Python) | NOT installed |

## Other potentially relevant installed software (via Homebrew)

- SDL3 and sdl2-compat (candidate platform-layer dependency — PROJECT DECISION, not yet chosen)
- ffmpeg, SDL-adjacent media libraries
- Blender (cask), codex, copilot-cli (casks)
- git-lfs

## Notes

- Phase 0 proceeds with stock macOS tools (`file`, `lldb`, `otool`, Python stdlib).
- Any future tool installation is a user decision; this document should be
  updated when the tool set changes.
- `otool`, `lipo`, `nm`, `strings` ship with Xcode CLT and are assumed available
  (verify at point of use).
