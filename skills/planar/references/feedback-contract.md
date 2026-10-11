# Operator feedback contract

Every Planar workflow ends with one compact account of what it understood, what it did,
and what the operator can do next. This file is the single statement of that envelope;
the other references point here instead of repeating it. The canonical source is the
operator feedback contract in `doctrine.md` in the Planar agents directory.

## The seven sections

| Section | Contract |
|---------|----------|
| Context | Resolved scope, target and mode (read-only, preview, apply). |
| Intent | One sentence stating the interpreted request. |
| Actions | Counts for `attempted`, `applied`, `skipped` and `failed`; name every failed target. |
| Result | `outcome=ok`, `outcome=partial` or `outcome=error`, then the verified post-state with stable identifiers, paths or external URLs. Never omitted. |
| Warnings | Partial failures, consequential assumptions, unavailable checks, degraded signal. |
| Next actions | Zero to three executable recommendations, most useful first. |
| Recovery | The exact inspect, retry, resume or undo command, when recovery applies. |

Omit an empty section in operator-facing output, except Result. When the operator asked
for JSON, return the same information as named fields rather than a different model.
Use stable CLI JSON internally and translate it into concise prose.

## Read back before reporting

An exit code shows that a command ran, not that the requested state exists. After a
mutation, read the post-state with the matching `show` or `list` verb and report the
identifiers from that read. When no supported read exists, say so under Warnings and give
the exact inspection command.

For read-only workflows, `applied` counts successful reads and never implies a write. A
preview never counts as applied.

## Outcome semantics

- **Success.** `outcome=ok`; report the verified post-state and the non-zero counts.
- **Successful no-op.** `outcome=ok` with zero applied; say why nothing changed. An
  expected empty state is not a warning. An already-present link, an already-matching
  scope or an unchanged preview is a skip, not a failure.
- **Partial.** `outcome=partial`; keep the completed independent targets, list every
  failed target, and give each one its idempotent retry or inspection command.
- **Failure.** `outcome=error`; separate attempted from applied work, report the last
  verified state, and give actionable recovery.

## Never claim a rollback you did not get

Independent CLI calls are independent writes. Do not claim cross-target rollback or
atomicity that the underlying verb does not provide, and never imply an undo happened
unless the operation itself performed one. A persisted snapshot, posted issue, created
artifact or resolved event stays persisted when a later step fails; report it as done and
give recovery for the rest.

Some verbs are atomic within one boundary, and only that boundary may be described as
rolled back. For example, `planar spec ingest --apply` commits or rolls back per anchor
plan, and each `planar-agent` terminal verb flips the claim and the task status in one
transaction.

## Recovery commands

A recovery line is a command the operator can paste. Prefer an idempotent read first,
then the exact retry with the original arguments, for example:

```sh
planar handoff validate 812
```

Require a fresh operator confirmation before retrying any destructive or gated write
(`planar task cancel`, `planar handoff abandon`, `planar-ext sync resolve`,
`planar spec ingest --apply`). A gated command offered as a next action is offered, not
run.

## Stronger schemas keep their fields

The envelope is a minimum. Role outputs with their own required fields, such as reviewer
verdicts, coder work-complete reports, spec-review packets and orchestrator decisions,
keep those fields and decision taxonomies. They add or map Context, Result, Warnings,
Next actions and Recovery around their own schema instead of being flattened into it.

Back to [../SKILL.md](../SKILL.md).
