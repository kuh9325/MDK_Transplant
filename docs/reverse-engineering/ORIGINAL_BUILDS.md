# Original MDK Builds — Strategy

Status: Phase 0 baseline. No original binaries have been analyzed yet.
Everything below about executable internals is UNKNOWN until verified from
user-provided files or authoritative technical documentation.

## Historical platform releases (documented at a high level)

MDK (1997, Shiny Entertainment) is documented to have shipped on multiple
platforms, including at least:

- **DOS x86** — the original PC release
- **Windows x86** — a Windows-targeted build
- **Classic Mac OS / PowerPC** — the contemporary Mac port

Console ports (e.g., PlayStation) are also DOCUMENTED to exist but are out of
scope unless a specific need arises.

> We do NOT claim exact executable structures, linker layouts, or toolchain
> fingerprints for any of these builds. Those must be established per-build
> from actual files (see `docs/research/ORIGINAL_DATA_CHECKLIST.md`).

## Analysis strategy

Each original build serves as an **oracle** — a reference implementation whose
observable behavior constrains our reimplementation:

| Build | Role |
|---|---|
| DOS x86 | Primary **behavioral oracle**: game logic, timing, data formats |
| Windows x86 | Secondary **behavioral + platform oracle**: API usage, portability differences vs DOS |
| Classic Mac OS / PPC | **Cross-platform comparison oracle**: agreement/disagreement with x86 builds is high-value evidence for what is game logic vs platform accident |

Rationale: where DOS, Windows, and Mac builds agree on a data layout or
behavior, that behavior is very likely engine-level rather than platform-level.
Where they disagree, the disagreement itself localizes platform abstraction
boundaries.

## Independence requirement

- Reimplementation code must **not** depend on original executable code at
  runtime — no wrapping, no dynamic loading of original binaries, no embedded
  original code.
- Original executables are used **offline only**, as analysis subjects and
  behavioral oracles.
- Original executables are never modified, patched, or redistributed.

## Confirmed facts vs research questions

Confirmed (this session):
- Multiple platform builds exist at the level documented above.

Research questions (all UNKNOWN):
- Exact compiler/toolchain per build (fingerprinting pending binaries)
- Whether DOS/Windows share a common logic core or differ substantially
- How the Mac port relates architecturally to the PC builds
- File format versions per build and whether they are interchangeable
- Which subsystems were statically vs dynamically linked

See `RESEARCH_QUESTIONS.md` for the full open-question list.
