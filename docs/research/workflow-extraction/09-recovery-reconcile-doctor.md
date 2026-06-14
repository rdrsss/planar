# W8 — Recovery / reconcile (doctor) workflow (STUB)

**Status:** STUB / decisions-parked · child of `00-anchor.md` · PARTIAL ·
**depends on `01`/P3**
**Current home:** `planar-agent reconcile` (~373 LOC) + `pl-doctor` skill
**Determinism:** ~70% — the sweep is mechanical; the *triage* (which in-flight
task to cancel vs reset vs leave) is an operator-decision gate.

**What it owns:** the degraded-DB recovery procedure — diagnose contributors,
preview the stale-claim/orphaned-action/dead-run sweep, let the operator triage
non-resumable in-flight tasks, apply, verify. Today `reconcile` is one big sweep
verb and `pl-doctor` is the prose around it; the *procedure* around the sweep is
the workflow.

**Why a stub.** The sweep itself is atomic (KEEP-ZIG). The recovery *flow* —
diagnose → preview → operator triage → apply → verify — is a clean EXTRACT, but:

- **D-W8.1 — scoped, previewable sweep.** Needs `01`/P3
  (`reconcile --plan <id> --dry-run --json`) so the workflow can show exactly
  what would change before the operator confirms. *Gate:* `01`/P3 lands.
- **D-W8.2 — triage gate shape.** Cancel-vs-reset-vs-leave for a non-resumable
  in-flight task is an operator `confirm` per task, not LLM judgment. The open
  question is whether the diagnose step can emit a structured triage list
  (task, claim, last-heartbeat, resumable?) the harness renders for the gate.
  *Gate:* after `01`/P3 — likely `planar-watch claims --plan --json` +
  `planar resume validate` already supply the triage signal; confirm.

**Resolution gate (stub → spec):** after `01`/P3 and after `03` (janitor) proves
the diagnose→preview→confirm→apply→verify pattern — this workflow is the same
shape as the janitor's reconcile step, scaled to a full recovery session, so it
should reuse that proven structure rather than invent its own.

**Pre-declared touches (for ingest):** `src/cmd/planar-agent/handlers/reconcile.zig`,
`skills/src/pl-doctor.md` (read-only audit at stub stage).
