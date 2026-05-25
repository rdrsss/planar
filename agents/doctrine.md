---
name: doctrine
description: Consolidated cross-cutting principles for Planar agents. Methodology is the procedural flow; doctrine is the accumulated judgment about when and how to apply it.
---

# Doctrine

This document consolidates cross-cutting principles derived from live sessions. It is vendor-neutral and applies to all agent roles. Role-specific procedure lives in the individual role specs; session flow lives in [`agents/methodology.md`](methodology.md).

---

## Dispatch shape selection

Full rules live in [`agents/methodology.md §Dispatch mode selection`](methodology.md#dispatch-mode-selection).

Summary:
- **INLINE** (skip reviewer): ≤3 files, mechanical edits only, automated validation fully green.
- **STRICT** (full coder + reviewer cycle): any logic change, any spec/ADR/migration change, more than 3 files, or the prior cycle on the same area had a reviewer failure.

**Concrete example (INLINE-eligible):** A single-file comment correction where `gofmt -l .` returns empty, `go build ./...` succeeds, and no parity surface was touched.

**Concrete example (STRICT-required):** A 2-file change that adds a new exported function — even though file count is low, the presence of a new function triggers STRICT.

When in doubt: choose STRICT. The reviewer cycle costs one round-trip; a missed defect costs more.

---

## Common defects

Full checklist lives in [`agents/methodology.md §Common defects pre-flight checklist`](methodology.md#common-defects-pre-flight-checklist).

Session-attributed defect log (add entries when a new defect class is observed):

| Defect class | First seen | Description |
|---|---|---|
| gofmt drift after bulk substitution | 2026-05-16 | `sed`/find-replace on Go files does not re-run gofmt; always run `gofmt -l .` after any bulk edit. |
| Schema enum value guessed from memory | 2026-05-16 | Coder cited an artifact kind that did not exist in the migration SQL. Always read `src/migrations/` to confirm enum values. |
| Documentation table internal consistency without cross-check | 2026-05-16 | A table in an ADR was internally consistent but contradicted the migration SQL it summarized. Tables must be validated against their source, not just against each other. |

---

## Verify independently

Full doctrine lives in [`agents/reviewer.md §Verify independently`](reviewer.md#verify-independently).

Summary: every factual claim in a coder's work-complete report must be re-verified by the reviewer running the corresponding command directly. Accepting a claim without running the check is rubber-stamping, not reviewing.

---

## Work-complete report template

Full template lives in [`agents/coder.md §Work-complete report template`](coder.md#work-complete-report-template).

Required sections (all five must be present; write "N/A" only if the section genuinely does not apply):

1. Files changed
2. Validation run
3. Pre-flight checklist
4. Residual risk
5. Reviewer focus

A reviewer receiving a report missing any section MUST return `request-changes` before reading the work.

---

## When to skip agents

Aggregated from [`agents/coder.md`](coder.md) and [`agents/reviewer.md`](reviewer.md) per the A2 skip-condition rules (see tech-spec plan 48 §A2):

| Agent | Skip when |
|---|---|
| `coder` | Orchestrator determines the change qualifies as INLINE per the Dispatch mode selection rule. Common cases: comment-only edits, import reordering, file deletions with no logic impact, configuration value changes confirmed by automated validation. |
| `reviewer` | ALL inline conditions are met (≤3 files, mechanical edits only, automated validation fully green). Orchestrator makes this call before dispatching the coder. If the coder's work-complete report reveals unexpected complexity, the orchestrator may add a reviewer pass retroactively. |

---

## Retrospective loop

When a cycle surfaces a novel defect not already listed in the Common defects table above:

1. The session's cleanup cycle adds a row to the table with the defect class, the session date, and a one-line description.
2. If the defect implies a new checklist item, it is also added to [`agents/methodology.md §Common defects pre-flight checklist`](methodology.md#common-defects-pre-flight-checklist).
3. The addition is made in the same cleanup cycle, not deferred to a follow-on plan.

This is how doctrine grows: each session either reconfirms existing guidance or extends it.
