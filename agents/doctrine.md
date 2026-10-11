---
description: Consolidated cross-cutting principles for Planar agents. Methodology is the procedural flow; doctrine is the accumulated judgment about when and how to apply it.
kind: doc
slug: doctrine
---

# Doctrine

This document consolidates cross-cutting principles derived from live sessions. It is vendor-neutral and applies to all agent roles. Role-specific procedure lives in the individual role specs; session flow lives in [`agents/methodology.md`](methodology.md).

---

## Dispatch shape selection

Full rules live in [`agents/methodology.md §Dispatch mode selection`](methodology.md#dispatch-mode-selection).

Summary:
- **REVIEWER-ON** is the default for every repository mutation.
- **BYPASS** is supported only when an expert operator explicitly selects
  `barrel-bypass` and every required entry in the confirmed target-repository
  validation profile passes.

File count, documentation labels, and apparently mechanical edits never imply
bypass. The orchestrator does not recommend bypass.

---

## Common defects

Full checklist lives in [`agents/methodology.md §Common defects pre-flight checklist`](methodology.md#common-defects-pre-flight-checklist).

Session-attributed defect log (add entries when a new defect class is observed):

| Defect class | First seen | Description |
|---|---|---|
| formatting drift after bulk substitution | 2026-05-16 | Bulk replacement can bypass the repository's normal formatter; rerun the confirmed formatting/static-analysis profile entry after mechanical edits. |
| Schema value guessed from memory | 2026-05-16 | Coder cited a value absent from the authoritative schema source. Read the target repository's documented schema/migration source; do not assume its layout. |
| Documentation table internal consistency without cross-check | 2026-05-16 | A table was internally consistent but contradicted the authoritative source it summarized. Tables must be checked against their source, not just against each other. |

---

## Verify independently

Full doctrine lives in [`agents/planar-reviewer.md` §What the reviewer does](planar-reviewer.md#what-the-reviewer-does).

Summary: the blind reviewer does not receive the coder's narrative report. It
receives a structured validation packet and independently verifies the diff.
The reviewer reruns validation selectively when evidence is absent,
inconsistent, high-risk, or cheap enough to add material signal; it records
which evidence was reproduced and which was accepted from the packet.

---

## Builds and tests are queued, not locked

Full rule: [`agents/methodology.md §Builds and tests go through the host queue`](methodology.md#builds-and-tests-go-through-the-host-queue).

Summary: agents on one machine share its cores, so builds and test runs are
ordered host-wide through one queue instead of each agent deciding when it is
safe to start. The queue does not lock a build directory or a resource; it
serializes execution, and a dead submitter cannot leave it stuck. Submit the
command, observe its ticket with a finite `queue wait` budget, and treat the
structured reason and recorded outcome as the truth rather than the process
exit code alone. A short observation slice can be resumed on the same ticket
after an explicit decision. A queue that refuses is reported to
the operator, never bypassed by running the command directly: the operator
chose refusal over bypass. Only a Planar with no queue at all is the exception,
and the agent then says Planar needs upgrading.

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
gate, weaken strict scope resolution, or bypass the cross-scope guard — no such CLI flag exists.
**Same-scope writes MUST NOT emit any cross-scope cue.**

---

## Work-complete report template

Full template lives in [`agents/planar-coder.md §Work-complete report template`](planar-coder.md#work-complete-report-template).

Required sections (all six must be present; write "N/A" only if the section genuinely does not apply):

1. Files changed
2. Validation evidence
3. Claim state
4. Pre-flight checklist
5. Residual risk
6. Reviewer focus

The orchestrator checks report completeness before reviewer dispatch. The blind
reviewer does not receive this narrative report; it receives the diff base,
spec citations, confirmed validation profile, and structured evidence packet.

---

## When to skip agents

Aggregated from [`agents/planar-coder.md`](planar-coder.md) and [`agents/planar-reviewer.md`](planar-reviewer.md) per the reviewer skip-condition rules:

| Agent | Skip when |
|---|---|
| `coder` | Only when no repository source-content mutation is required. Every mutation, including a one-line documentation edit, uses a freshly spawned write specialist. |
| `reviewer` | A non-mutation cycle has no independent review signal, or the expert operator explicitly selected `barrel-bypass`. The orchestrator never infers bypass from task shape. |

---

## Retrospective loop

When a cycle surfaces a novel defect not already listed in the Common defects table above:

1. The session's cleanup cycle adds a row to the table with the defect class, the session date, and a one-line description.
2. If the defect implies a new checklist item, it is also added to [`agents/methodology.md §Common defects pre-flight checklist`](methodology.md#common-defects-pre-flight-checklist).
3. The addition is made in the same cleanup cycle, not deferred to a follow-on plan.

This is how doctrine grows: each session either reconfirms existing guidance or extends it.
