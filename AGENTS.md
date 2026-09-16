# AGENTS.md — Rules for all AI/code agents working in this repository

This project is a **preservation-oriented compatibility reimplementation** of the
original 1997 game MDK. It is not a redesign, remaster, or modernization.

Read `docs/reverse-engineering/EVIDENCE_POLICY.md` before making any claim about
the original game's behavior or formats.

## Hard rules

1. **Compatibility over improvement.** Architecture improvements, refactors, and
   modernization must never take precedence over reproducing original behavior.
2. **Do not modernize gameplay** without explicit user approval. No "quality of
   life" changes to mechanics, physics, timing, difficulty, or content.
3. **Do not silently alter game rules.** If observed behavior seems buggy,
   reproduce it and document it; do not "fix" it.
4. **Distinguish evidence levels.** Mark every claim OBSERVED, CORROBORATED,
   DOCUMENTED, HYPOTHESIS, or UNKNOWN per the evidence policy. Never present
   decompiler guesses or format speculation as fact.
5. **No proprietary material in Git.** Never commit original MDK executables,
   assets, archives, disc images, or substantial decompiler output. User-supplied
   original data lives only in ignored local directories (see `.gitignore`).
6. **Evidence-first parsers.** Every file-format parser must start from captured
   evidence (hex dumps, samples, hash manifests) and synthetic/unit tests —
   never from assumed or guessed structures.
7. **Oracle comparison.** Every reconstructed gameplay/rendering subsystem should
   eventually be compared against a legally owned original build where feasible.
8. **Work incrementally.** Do not build large systems from unverified
   reverse-engineering guesses. Prefer small verifiable steps over speculative
   architecture.
9. **Independence.** Reimplementation code must not depend on original
   executable code at runtime, and must not embed original copyrighted data.

## Current phase status

See `docs/reverse-engineering/` for the current state of knowledge. As of the
baseline commit, everything about original formats and behavior is UNKNOWN
unless a document there says otherwise.
