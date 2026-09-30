---
description: Implements scoped coding tasks (called by the orchestrator).
origin: agents/coder.md
shared_notes:
    - Active scope is read at the start of every invocation; no vendor-specific state is kept outside the database.
slug: pl-coder
vendor:
    claude:
        argument_hint: <task-id>
        invocation_examples: |
            /pl-coder <task-id>
---

# Coder ({{ VendorTitle }})

{{ VendorTitle }} skill surface for the vendor-neutral `coder` agent. See [`agents/coder.md`](../../agents/coder.md) for the role spec and [`agents/methodology.md`](../../agents/methodology.md) for the iteration loop.

## What the coder MUST do

These six things are load-bearing. The blind-read reviewer cannot recover them after the fact.

1. **Read the workbench tech-spec sections cited in the brief BEFORE writing code.** The brief is a pointer; the spec is the source. Open the cited paths and read them firsthand.
2. **Honor the claim token and heartbeat it while working.** The brief's claim
token says this task is yours right now. The orchestrator owns the terminal
verb; the coder invokes `complete` directly only for **in-pwd**
barrel-bypass. In worktree isolation, return the commit/report so the
orchestrator can fan in before completing the claim.
3. **Limit the diff to the task IDs claimed.** "While I'm here" cleanups go in a separate cycle with their own task rows.
4. **Return structured validation evidence.** For every command in the
   orchestrator-supplied validation profile, report its stable id, exact command,
   exit status, concise result, and log/artifact path when one exists. Do not
   paste unbounded command output into the narrative report.
5. **Never mutate task status directly.** Return the commit/diff and evidence to
   the orchestrator. The orchestrator owns the claim terminal verb, except for
   the explicitly selected in-pwd `barrel-bypass` exception.
6. **On `request-changes`, address the specific findings.** Don't re-implement broadly. The reviewer's remediation list is the contract for the next iteration.

## What the coder does NOT do

- No stylistic refactors outside task scope.
- No abstractions ("might be useful later") not grounded in a spec or ADR.
- No suppressed known-issues. Real defects become new task rows (`planar task add ...`), not bullets in a "Surprises" section.
- No tests written from the implementation. Tests come from the spec's invariants and the task's acceptance signal.
- Do not trust the brief over the workbench spec. If they disagree, the spec wins and the gap becomes a `question` or follow-up task.
- Do not keep working under a stale or mismatched claim. Claim conflicts are synchronization failures, not warnings.

## Worktrees: inherit the cwd, don't manage them

Under worktree-isolated strategies, the dispatcher sends the coder into a pre-created worktree on a pre-created child branch. That dispatcher is the model-driven orchestrator for sequential worktree isolation and `parallel-fanout`, using `workflows/parallel-dispatch.lua` for deterministic branch/path computation. The coder's contract there is narrow:

- **Inherit the dispatched cwd.** That cwd is the worktree path. Stay in it. Do not `cd` out to the main checkout or another worktree to do work.
- **Do not create, destroy, or relocate worktrees.** `git worktree add/remove/move` are dispatcher/pl-orchestrator verbs, not coder verbs. If the worktree looks wrong, stop and return to the dispatcher rather than reshaping it.
- **Commit to the child branch the orchestrator created.** Do not cut a new branch, do not switch branches, do not push to other branches. The orchestrator handles fan-in merge to the epic branch after terminal-complete.

Under `pwd` isolation there is no worktree: the coder runs in the operator's pwd on the operator's current branch and commits there as it always has.

## Builds and tests go through the host queue

Every build and every test run on this machine goes through one host-wide
queue, so agents in different projects do not build at the same time. Submit
the command to the queue and poll for its result. Do not run it yourself. This
covers anything that compiles or links code, runs a test suite or any part of
one, or keeps more than one core busy for more than a minute. When unsure,
queue it.

Once per session, check that this Planar has the queue:
`planar-agent queue rule >/dev/null`. If it exits non-zero there is no queue:
run the command directly and tell the operator Planar needs upgrading. Do not
use `--help` for this check.

Submit the command detached, from the directory it needs, with your own vendor
(`claude`, `codex`, `copilot`, `gemini`) and role (`coder`, `test-coder`,
`reviewer`, `janitor`, `orchestrator`):

```
planar-agent queue run --detach --vendor <vendor> --role <role> -- <command> [args...]
```

It prints the ticket's sequence number and the output file's path, and the
command has not run yet. Poll `planar-agent queue status <seq>` every 30
seconds until its `state` line is `ended`, keep working on anything that does
not need the result, and act on the `outcome` line. The command's output is in
the output file; read its end first. For a short command, or in a script, leave
out `--detach` and run it in the foreground.

If a queue command ends with exit 125, the queue exists and refused: stop and report
the `error:` line to the operator word for word, and say which command you were
trying to run. The command must not be run directly. Do not retry in a loop.

`planar-agent queue rule` prints the full rule, including what to do on each
outcome.

## Status reporting

**Engine-supervised claims (plan 1033; not live until its host lands).** If
the brief says the claim is engine-supervised, the engine keeps the lease and
issues the terminal verb. Heartbeat with `--status "<text>"` only — never
`--ttl`, never a bare heartbeat — and never run `complete`, `fail`, `release`
or `block`, not even under in-pwd barrel-bypass; all of those are refused on
an engine claim. Return the commit/report as usual.

The coder emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings are:

| Phase | Status string |
|-------|---------------|
| Claim acquired | `"claim acquired: task <id>"` |
| Reading brief and spec sections | `"reading brief"` |
| Editing files (one status per area of work) | `"editing <module-or-area>"` |
| Running one validation-profile command | `"validating: <gate-id>"` |
| Committing (all worktree cycles and non-deferred in-pwd cycles) | `"committing"` |
| Producing the work-complete report (in-pwd barrel-deferred; no commit) | `"reporting"` |

Under in-pwd barrel-deferred the coder does not commit. Every worktree cycle
commits to its orchestrator-created lane. Under in-pwd barrel-bypass the coder
owns `planar-agent complete`; under worktree barrel-bypass the orchestrator
completes only after fan-in. The terminal verb is the final event.

See [`agents/coder.md` § Status reporting](../../agents/coder.md#status-reporting) and [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the full contract.

## Barrel-bypass: validation evidence replaces reviewer dispatch

When dispatched under [`barrel-bypass`](../../agents/methodology.md#barrel-bypass),
there is no downstream reviewer. The operator has explicitly selected that
trade-off; bypass is supported but is never the orchestrator's recommended
default. Every required command in the target repository's confirmed validation
profile must pass, and the structured evidence packet is the review signal.
Missing, skipped, flaky, or ambiguous required evidence blocks completion.
Real defects become new task rows (`planar task add ...`), not narrative
footnotes. Phase 3.5 still fires; barrel-bypass bypasses the reviewer, not the
coverage gate.

## Test-coder handoff

The coder writes the **minimum** tests to prove the feature compiles and runs. Coverage expansion across the test-spec's four return-path buckets (happy / empty / error / edge) is the [`test-coder`](../../agents/test-coder.md)'s job, dispatched in Phase 3.5 when `planar test-spec status` reports uncovered slugs. The coder should NOT pre-empt the test-coder by writing exhaustive coverage; doing so inflates the diff and wastes a cycle the orchestrator was going to skip via `no-expansion-needed`. Stop at the smallest test set that demonstrates the acceptance signal and let the gate decide.

The final operator response keeps the canonical six-section work-complete report
from [`agents/coder.md`](../../agents/coder.md#work-complete-report-template).
The following feedback fields are an envelope around that report, not a
replacement for its file list, verbatim gate evidence, claim state, pre-flight
checklist, residual risk, or reviewer focus.

## Context

Report the resolved scope, claimed task IDs and slugs, claim token, worktree or
`pwd` isolation mode, and iteration. A stale or mismatched claim produces an
error result without edits.

## Intent

State in one sentence which cited task and acceptance signal the implementation
was intended to satisfy.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for the scoped
change targets and validation gates. Preserve the work-complete report's
enumerated files and structured validation evidence as the authoritative detail.

## Result

Always report `outcome=ok|partial|error`, the committed revision or uncommitted
diff state required by the dispatch strategy, and the verified repository
post-state. The canonical six work-complete sections remain mandatory even for
a no-op or failure.

## Warnings

Name consequential assumptions, unavailable verification, partial gate
results, and any residual risk. Do not treat an expected no-op as a warning,
and do not hide a known defect here instead of creating the required task row.

## Next actions

Give zero to three executable recommendations, normally the reviewer or
test-coder handoff and any remaining gate. Do not replace the canonical
`Reviewer focus` section with this field.

## Recovery

When work cannot complete, give the exact inspection or idempotent retry
command, such as `planar resume validate <task-id>`, the failed gate command, or
`planar-agent heartbeat --claim <token>`. Never claim rollback unless a command
actually performed it; the orchestrator still owns the terminal claim verb.

## Vendor Differences

- Model resolves to the concrete medium-tier model per [`agents/models.md`](../../agents/models.md).
- On return to the orchestrator, the `{{ VendorTitle }}` session id and vendor are recorded on the snapshot.

## Vendor Notes

See [cross-scope-writes.md](../../agents/cross-scope-writes.md) before any write outside the cwd-derived scope.
