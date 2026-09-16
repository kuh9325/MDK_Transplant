# Evidence Policy

All claims in this project about the original MDK's behavior, formats, or
implementation MUST carry an evidence level. Facts, hypotheses, and unknowns
are never blurred.

## Evidence levels

| Level | Meaning |
|---|---|
| **OBSERVED** | Directly demonstrated from binary analysis, runtime observation, or data inspection of a legally owned original build |
| **CORROBORATED** | Independently supported by multiple observations or by agreement across multiple original builds (e.g., DOS + Windows + Mac) |
| **DOCUMENTED** | Stated by a reliable external technical source (specification, contemporaneous technical documentation, verified developer account) |
| **HYPOTHESIS** | Plausible interpretation consistent with available evidence but not confirmed |
| **UNKNOWN** | Insufficient evidence to support any claim |

## Rules

1. **Never promote** a hypothesis to observed fact without evidence.
2. **Decompiler-invented names are not evidence.** Auto-generated variable,
   function, and structure names from Ghidra/Hopper/etc. carry zero semantic
   weight until corroborated by behavior or documentation.
3. **Similarity is not identity.** MDK resembling another engine or game does
   not establish shared implementation.
4. **Runtime behavior beats memory.** Observed original runtime behavior is
   stronger evidence than recollection or aesthetic similarity.
5. **Cross-build agreement is gold.** Agreement between DOS, Windows, and Mac
   builds is the strongest readily available evidence that something is
   engine-level rather than platform-specific.
6. **Oracle comparison.** Every reconstructed subsystem should be compared
   against an original build's observable behavior when feasible.

## Application

- Documents should tag claims inline, e.g. `(UNKNOWN)`, `(HYPOTHESIS: …)`.
- When evidence upgrades a claim, record what established the upgrade
  (file, offset, experiment, build version).
- If evidence later contradicts a claim, downgrade it and note the conflict
  rather than deleting the history.
