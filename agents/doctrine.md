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

**Concrete example (INLINE-eligible):** A single-file comment correction where `make fmt-check` returns clean, `make build` succeeds, and no parity surface was touched.

**Concrete example (STRICT-required):** A 2-file change that adds a new exported function — even though file count is low, the presence of a new function triggers STRICT.

When in doubt: choose STRICT. The reviewer cycle costs one round-trip; a missed defect costs more.

---

## Common defects

Full checklist lives in [`agents/methodology.md §Common defects pre-flight checklist`](methodology.md#common-defects-pre-flight-checklist).

Session-attributed defect log (add entries when a new defect class is observed):

| Defect class | First seen | Description |
|---|---|---|
| zig fmt drift after bulk substitution | 2026-05-16 | `sed`/find-replace on Zig files does not re-run `zig fmt`; always run `make fmt-check` after any bulk edit. (Originally surfaced in the Go era against `gofmt`.) |
| Schema enum value guessed from memory | 2026-05-16 | Coder cited an artifact kind that did not exist in the migration SQL. Always read `migrations/` to confirm enum values. |
| Documentation table internal consistency without cross-check | 2026-05-16 | A table in an ADR was internally consistent but contradicted the migration SQL it summarized. Tables must be validated against their source, not just against each other. |

---

## Verify independently

Full doctrine lives in [`agents/reviewer.md §Verify independently`](reviewer.md#verify-independently).

Summary: every factual claim in a coder's work-complete report must be re-verified by the reviewer running the corresponding command directly. Accepting a claim without running the check is rubber-stamping, not reviewing.

---

## Operator feedback contract

Every user-invocable skill returns one compact account of what it understood,
what it did, and what the operator can do next. The shared fields are:

| Field | Contract |
|---|---|
| Context | Resolved scope, target, and mode. |
| Intent | One sentence stating the interpreted request. |
| Actions | Counts for `attempted`, `applied`, `skipped`, and `failed`; identify failed targets when there are any. |
| Result | The verified post-state, including stable entity identifiers, paths, or external URLs. This field is never omitted. |
| Warnings | Partial failures, consequential assumptions, unavailable checks, or degraded signal. |
| Next actions | Zero to three executable recommendations, ordered by usefulness. |
| Recovery | The exact inspect, retry, resume, or undo command when recovery applies. |

Skills use stable CLI JSON internally when it is available, but translate it
into concise operator-facing prose. A skill that exposes JSON returns the same
information as named fields rather than a different result model. Empty
operator-output fields are omitted except `result`; the canonical authored
skill still carries all seven contract sections so its behavior is explicit.

An exit code is evidence that a command ran, not evidence that the requested
state exists. After a mutation, read the post-state when a supported read is
available and report identifiers from that read. If verification is
unavailable, say so in `warnings` and offer the exact inspection command.

Use these outcome semantics consistently:

- **Success:** `outcome=ok`; report the verified post-state and non-zero action
  counts where work was applied.
- **Successful no-op:** `outcome=ok`; report zero applied, explain why nothing
  changed, emit no warning for the expected empty state, and give an
  appropriate next action when one exists.
- **Partial:** `outcome=partial`; retain completed independent targets, list
  every failed target, and provide its idempotent retry or inspection command.
  Do not claim cross-target rollback or atomicity that the underlying CLI does
  not provide.
- **Failure:** `outcome=error`; distinguish attempted from applied work, report
  the last verified state, and provide actionable recovery. Never imply an
  undo occurred unless the underlying operation actually performed one.

The shared contract is a minimum envelope, not a replacement for a stronger
role-specific schema. Reviewer verdicts, coder work-complete reports,
orchestrator decisions, and other canonical outputs keep their required fields
and decision taxonomies; they add or map the shared context, result, warnings,
next-action, and recovery information without flattening those schemas.

`internal_only: true` in unified skill frontmatter is the sole exemption. It is
valid only for a helper that is never an operator entry point and is invoked by
another canonical skill or role that owns the operator-facing result. The
source must identify that caller and justify the exemption. Hidden,
inconvenient, normally orchestrator-dispatched, or manually invocable skills
are still user-invocable and must implement the contract. Internal-only status
waives only the seven authored feedback sections; it does not waive errors,
warnings, or recovery information owed to the calling workflow.

### Cross-scope write cue

Before a Planar-authored workflow invokes a write whose target is outside the
cwd-derived scope reported by `planar scope show --json`, it emits this
standalone narrative line immediately before the command:

```text
[cross-scope write: <normalized-target-label>]
```

Normalize the stored target and CLI argument exactly as follows:

| Stored target | Cue label | Explicit CLI value |
|---|---|---|
| Repo/project row, project slug `planar` | `project:planar` | `--scope repo:planar` |
| Ordinary association, slug `org:acme` | `association:org:acme` | `--scope assoc:org:acme` |
| Legacy project association: `kind=association`, slug `project:planar`, `kind_label=project` | `project:planar` | `--scope assoc:project:planar` |
| Global | `global` | `--scope global` |

The legacy row therefore never produces the invalid/doubled display label
`association:project:planar`. `planar promote`/`demote` retain their own
destination arguments while using the same cue-label normalization. A generic
warning such as `[cross-scope write]` is insufficient. This cue makes intent
visible in the transcript; it does not grant permission, replace an operator
gate, weaken strict scope resolution, or authorize `--no-scope-check`.
**Same-scope writes MUST NOT emit any cross-scope cue.**

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

Aggregated from [`agents/coder.md`](coder.md) and [`agents/reviewer.md`](reviewer.md) per the reviewer skip-condition rules:

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
