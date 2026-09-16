# Original Data Checklist

Phase 0 finding: **no original MDK files were present** in the project root or
in explicitly provided locations at baseline.

Phase 1 update (2026-09-16): an installed PC tree was supplied at
`original/installed/` (inventoried as BUILD_A — see
`docs/reverse-engineering/ORIGINAL_BUILDS.md`). It covers the executable,
DLL/driver, data-archive, level, video, config, and documentation items
below for the DOS + Windows builds. Still not supplied: Classic Mac build,
CD-ROM image/installer, and a verifiably unmodified retail dump (BUILD_A
executables are NoCD-fixed per its own readme).

This checklist describes what the user may later place into a local drop-zone
directory (e.g. `original/` — git-ignored) from a **legally owned** copy.
Nothing on this list is ever committed; only metadata (hashes, sizes,
structural notes) enters the repository.

## Desired inventory (when available)

For each build the user owns (DOS / Windows / Classic Mac), ideally:

- [ ] Executable file(s) — main binary plus any overlays/extenders
- [ ] DLLs / shared libraries (Windows build)
- [ ] Data archives — packed game data files
- [ ] Level files — if shipped separately from archives
- [ ] Textures/images — if shipped loose
- [ ] Audio — music and SFX files
- [ ] Video/cinematic files — if present
- [ ] Configuration files — shipped defaults and user-generated saves/configs
- [ ] Installer/media structure — CD layout, directory tree, install scripts
- [ ] Documentation — manuals/readmes shipped on disc (text only; useful for
      corroboration)

## What to record once files are provided

1. Run `tools/hash_manifest.py <drop-zone>` → store JSON manifest in repo
   (`analysis-private/` stays local; committed manifests go under
   `docs/reverse-engineering/` only if they contain no asset content — hashes
   and paths are fine).
2. Run `tools/binary_inventory.py <files>` → record format identifications.
3. Note source edition (CD retail, budget re-release, digital store version)
   and any version strings — different editions may differ.

## Handling rules

- Never copy these files into `samples/` or anywhere tracked by Git.
- Never modify them; analysis is read-only.
- If unsure whether a file is proprietary, treat it as proprietary.
