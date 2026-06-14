# W5 — Workspace bootstrap pipeline (STUB)

**Status:** STUB / decisions-parked · child of `00-anchor.md` · PARTIAL
**Current home:** `handlers/workspace/{init,doctor,regenerate}.zig` (~920 LOC)
**Determinism:** ~50% — file/git scanning + pipeline phase sequencing is
mechanical; shape-detection and enrichment gating carry judgment.

**What it owns:** the multi-phase bootstrap — repo scan → org registration →
layout ensure → routing build → AGENTS.md regenerate → symlink install — and the
`workspace doctor` recovery loop (per-org diagnose → repair → reconcile).

**Why a stub.** The *phase sequencing* is a clean extraction candidate (scan,
then build, then regenerate, then install — each a primitive the harness orders).
But two parts resist clean externalization until measured:

- **D-W5.1 — registration atomicity.** Org/project registration + membership is
  an atomic multi-table write that must stay in Zig (KEEP-ZIG). The open question
  is whether the harness can cleanly straddle "scan (harness) → register (one Zig
  verb) → pipeline (harness)" without the verb needing to know about the
  surrounding phases. *Gate:* inspect `workspace/init.zig:registerWorkspace` —
  does it already accept a pre-scanned child list, or does it re-scan? If it
  re-scans, the seam isn't clean yet.
- **D-W5.2 — `--enrich` LLM pass.** `workspace scan --enrich` invokes an LLM for
  routing summaries. That is a flagged callout (like `synthesize --enrich`), so
  the deterministic floor extracts cleanly and the callout stays optional. Low
  risk; resolve with D-W5.1.

**Resolution gate (stub → spec):** after `03` (janitor) proves the
phase-sequencing-with-confirm pattern, and after confirming `registerWorkspace`
accepts pre-scanned input. Until then, the pipeline phases are too entangled with
the atomic registration to spec the seam honestly.

**Pre-declared touches (for ingest):** `src/cmd/planar/handlers/workspace/init.zig`,
`doctor.zig`, `regenerate.zig` (read-only audit, no edits at stub stage).
