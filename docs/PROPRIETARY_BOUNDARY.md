# Proprietary Boundary

This repository is an independent, preservation-oriented reimplementation.
MDK remains the property of its rights holders. This document defines the
boundary between our work and original copyrighted material. It is a working
policy, **not legal advice**, and makes no legal guarantees.

## Rules

1. **No original assets in this repository.** No textures, models, levels,
   audio, video, fonts, or any other shipped game data may be committed.
2. **No original binaries in this repository.** No MDK executables, DLLs,
   overlays, drivers, or disc images may be committed.
3. **Original data is local-only.** The user supplies legally owned original
   files on their own machine, placed in ignored drop-zone directories
   (`original/`, `retail/`, `gog/`, `cdrom/`, `game-data/`, `proprietary/`,
   `analysis-private/` — see `.gitignore`).
4. **No circumvention.** We do not bypass DRM or copy protection, patch
   original executables, or modify original binaries.
5. **No redistribution.** We never download, mirror, upload, or redistribute
   copyrighted MDK material.

## What MAY be stored in the repository

Generated metadata and independent work product, where appropriate:

- Filenames, file sizes, SHA-256 hashes, and directory-structure descriptions
  of user-supplied original data (e.g., output of `tools/hash_manifest.py`)
- Structural descriptions of file formats (offsets, field layouts) derived
  from analysis — written as documentation/specifications
- Independently written parsers, tools, tests, and the reimplementation itself
- Evidence notes: observations, experiments, and comparisons against oracles

## What must be handled with care

- **Decompiler output:** do not paste large blocks of decompiled original code
  into production source. Short excerpts may be quoted in analysis documents
  for study purposes where justified; reconstructed logic must be written as
  independent implementation informed by observed behavior.
- **Hash manifests** identify user-owned files without containing them; they
  are safe to commit and are the preferred way to reference original data.

## Distribution

Distribution of the reimplementation (this repository) and distribution of
original game data are **separate matters**. This project assumes the player
supplies their own legally owned copy; nothing here enables acquiring one.
