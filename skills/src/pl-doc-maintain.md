---
description: Maintain published documentation through a gated diff, proposal, prose-authoring, manifest, lint, build, and verification workflow.
origin: docs/features/doc-system.md
shared_notes:
    - The documenter is read-only, doc-author writes only operator-approved prose rows, and the caller alone runs planar-doc mutation verbs.
    - 'A clean diff is a verified no-op: do not dispatch either specialist and do not rebuild the manifest.'
slug: pl-doc-maintain
vendor:
    claude:
        argument_hint: '[--json]'
        invocation_examples: |
            /pl-doc-maintain
            /pl-doc-maintain --json
---

# Planar Documentation Maintenance ({{.VendorTitle}})

Runs the complete published-documentation maintenance loop without weakening
the boundary between proposal, prose authoring, and manifest state. The
workflow may read the repository and `.planar-manifest`; it never opens SQLite
directly. `planar-doc` is the only manifest writer.

## Context

Run from the root of the repository being maintained. Resolve and retain its
absolute path, the `.planar-manifest` path, the requested output mode (concise
text or `--json`), and a snapshot of the working-tree paths already modified
before this workflow starts. Existing changes belong to the operator and must
not be reverted, overwritten, or reported as doc-author output.

If repository guidance such as `AGENTS.md` exists, read it before dispatching a
specialist or applying a row. Published reference prose belongs under `docs/`.
Internal product specs, tech specs, roadmaps, test specs, ADRs, and sessions
remain Planar artifacts or workbench material; they may be evidence but are not
documentation write targets.

## Intent

Interpret the request as: inspect documentation drift, obtain a read-only
proposal, gate every row with the operator, author only approved prose, apply
only approved manifest dispositions, and prove the resulting documentation
state with lint, build, and verify.

## Commands

Use only the shipped doc-state surface:

```text
planar-doc diff --json
planar-doc cover <doc-path> <source-path>
planar-doc cover <doc-path> <source-path> --remove
planar-doc nodoc <source-path>
planar-doc nodoc <source-path> --remove
planar-doc lint --json
planar-doc build --json
planar-doc verify --json
```

`diff --json` is JSON Lines: each non-blank line is one record containing
`signal`, `path`, `doc`, and `detail`. Exit 0 with no records is clean. Exit 1
with valid records means drift was found and is the normal proposal path. An
unparseable row, a contradictory exit/output combination, or another command
failure is `outcome=error`; do not continue to a specialist or mutation.

Never pass a documenter-provided shell fragment directly to a shell. Construct
every mutation from validated repo-relative fields and the allowlisted command
shapes above. Reject absolute paths, `..` traversal, paths outside the
repository, and symlink resolution outside the repository before any write.

## Workflow

### 1. Diff and clean no-op

Run `planar-doc diff --json` and parse every JSONL record.

If there are zero records, do not dispatch documenter or doc-author and do not
run `cover`, `nodoc`, `lint`, or `build`. Run `planar-doc verify --json` as the
post-state read. A clean verify must report `drift=false` and matching `root`
and `manifest_root`; return those existing roots with zero applied changes. If
verification disagrees with the empty diff, return an error with both outputs
and the exact retry commands rather than rebuilding implicitly.

If `.planar-manifest` is missing or unreadable, stop before mutation. Recommend
an operator-reviewed initialization with `planar-doc build --json`; do not
silently create the baseline because doing so would accept the current tree as
documented state.

### 2. Read-only documenter proposal

For a non-empty diff, read the current manifest and construct this envelope:

```json
{
  "manifest_path": ".planar-manifest",
  "diff_records": [],
  "covered_docs": {},
  "cycle_summary": []
}
```

`diff_records` contains the parsed `planar-doc diff --json` rows verbatim.
`covered_docs` is the current manifest entries map needed to compare existing
coverage. For a standalone invocation, `cycle_summary` may be empty; include
only caller-supplied cycle facts and never invent task or plan identifiers.

Dispatch a fresh `documenter` specialist through the host's agent-dispatch
mechanism with that envelope and the canonical
[`agents/documenter.md`](../../agents/documenter.md) instructions. The
documenter may read the repository and return `extend-cover`, `create-doc`,
`nodoc`, or `defer` proposals. It must not write prose, invoke `planar-doc`,
change `.planar-manifest`, or write coordination state. If it changes any
working-tree path, stop with `outcome=error` and surface the unexpected diff.

Normalize the returned proposal into stable invocation-local row IDs while
preserving each original diff record. Each row shown at the gate must include:

```text
row_id, signal, source path, proposed action, target doc (when applicable),
whether prose is required, exact proposed planar-doc operation, and reason
```

Do not infer a missing target, source, action, or prose requirement. Mark an
incomplete or ambiguous proposal `defer` and ask the operator to resolve it.

### 3. Explicit row gate

Present the complete normalized worklist before the first mutation. Require an
explicit disposition for each row ID; the operator may answer with an exact
set of row IDs when applying the same disposition to several rows. Silence,
general encouragement, or approval of one row is not approval of the rest.

| Gate disposition | Allowed proposal | Effect after confirmation |
|---|---|---|
| `approve-prose` | `create-doc`, or `extend-cover` that requires a prose refresh | Include the exact row in the doc-author envelope. No manifest verb yet. |
| `approve-cover` | `extend-cover` with a validated target doc and source path | Caller runs the exact `planar-doc cover` add/remove operation after any required prose succeeds. |
| `approve-nodoc` | `nodoc` | Caller runs `planar-doc nodoc <source-path>` (or the explicitly proposed `--remove` form). Never dispatch doc-author. |
| `approve-reseat` | A hand edit or already-accurate covered source for which the proposal requires no prose or coverage change | Authorizes this row to be absorbed by the later build; performs no row-level command. |
| `reject` | any | No write. The row remains unresolved for this run. |
| `defer` | any | No write. Preserve the proposal and recovery command. |

Approval is bound to the displayed row fields. Any later change to action,
target doc, source path, removal mode, or prose requirement invalidates that
approval and requires the row to be shown again. A proposed deletion of prose,
a write outside `docs/`, or a choice not expressible by the allowlist is
`defer`; this workflow does not broaden doc-author's write authority.

Do not run `build` while a diff row is rejected, deferred, ambiguous, or
failed. A build recomputes the manifest across the tree and would otherwise
absorb state the operator did not approve.

### 4. Dispatch doc-author for approved prose only

Collect only `approve-prose` rows and construct the input required by
[`agents/doc-author.md`](../../agents/doc-author.md):

```json
{
  "repository_root": "/absolute/path/to/repository",
  "approved_rows": [
    {
      "row_id": "doc-1",
      "signal": "new-authoring",
      "action": "create-doc",
      "target_path": "docs/features/example.md",
      "source_paths": ["src/engine/example/"],
      "reason": "The public subsystem has no reference page.",
      "operator_instruction": "The operator's approved authoring direction."
    }
  ]
}
```

Every field must come from the approved row and gate response. Dispatch one
fresh `doc-author` specialist through the host's agent-dispatch mechanism with
this envelope and its canonical agent instructions. An empty prose set skips
the dispatch. `nodoc`, manifest-only coverage, reseat, rejected, deferred, and
unapproved rows must never appear in `approved_rows`.

Before any manifest operation, inspect the returned filesystem diff. Every new
or modified path attributable to doc-author must be an approved `target_path`
under `docs/`; every reported row and changed path must belong to the approved
set. Unexpected paths or an attempted write for an absent row stop the workflow
before `cover`, `nodoc`, or `build`. Preserve valid prose already written and
report a partial result; do not fabricate rollback.

If doc-author reports a partial failure, preserve the successful approved prose
diff and stop before building. Recovery is a new envelope containing only the
still-approved failed rows; approval does not extend to a changed target or
instruction.

### 5. Caller-owned cover and nodoc

After all approved prose rows succeed, the caller—not documenter or
doc-author—runs the exact approved row operations:

- Run `planar-doc cover <target-doc> <source-path>` for each approved new
  coverage edge, including successful `create-doc` rows that require coverage.
- Run `planar-doc cover <target-doc> <source-path> --remove` only when that
  removal form was displayed and approved.
- Run `planar-doc nodoc <source-path>` or its approved `--remove` form for each
  `approve-nodoc` row. These rows bypass prose authoring entirely.
- Run no command for an `approve-reseat` row; its approval applies only to the
  final build.

Validate the observable manifest state after every row-level command before
advancing. Count each approved row once even if it contains multiple source
paths, and retain per-command evidence so a later failure can name completed
and failed rows precisely. Independent commands are not one transaction.

### 6. Lint, build, verify

Proceed only when every initial diff row has an approved, successfully applied
or approved-reseat disposition and no unexpected path exists.

1. Run `planar-doc lint --json`. On failure, leave the approved prose and
   manifest diff inspectable, return `outcome=partial`, and provide the exact
   lint retry. Do not build.
2. Run `planar-doc build --json` and retain its returned `root`. This is the
   sole step that reseats the complete manifest after the row gate.
3. Run `planar-doc verify --json`. Require exit 0, `drift=false`, and equality
   between the live `root`, stored `manifest_root`, and the build root.
4. Run `planar-doc diff --json` once more. Require zero records. A remaining
   row is a verification failure even if the root comparison passed.

If build or verify fails, preserve the working-tree diff, report the last
verified row-level state, and provide exact inspect and retry commands. Never
claim rollback unless a command actually performed it.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts over the initial
diff rows. `applied` means the row's approved prose/coverage/nodoc/reseat path
completed and passed the final gates. Rejected and deferred rows are skipped;
invalid, dispatch-failed, command-failed, lint-failed, or verification-failed
rows are failed. Also report specialist dispatch counts separately so an empty
diff visibly proves that neither agent ran.

For a mixed result, retain row-level status and return `outcome=partial`.
Completed independent writes stay in place unless the operator explicitly
requests an existing supported undo command.

## Result

Always return `outcome=ok|partial|error` plus:

- repository root and output mode;
- prior manifest root and, after a successful build, the new manifest root;
- initial and final drift-row counts;
- approved row IDs and dispositions;
- doc-author `changed_paths` restricted to approved targets;
- executed `cover` and `nodoc` operations;
- lint, build, verify, and final-diff outcomes.

The clean no-op result is `outcome=ok`, all row counts zero, both dispatch
counts zero, no warning, and the existing verified manifest root. A non-empty
proposal with rows rejected or deferred is an inspectable no-write or partial
result, not a clean no-op and not permission to build.

In `--json` mode, mirror the same Context, Intent, Actions, Result, Warnings,
Next actions, and Recovery fields with stable keys and row arrays. In text
mode, omit empty sections except Result.

## Warnings

Report assumptions, unavailable evidence, status/dispatch failures, unexpected
changed paths, unresolved rows, partial application, or degraded verification.
Do not warn merely because an authoritative clean diff contains no work. Never
hide an unresolved row behind a successful command exit.

## Next actions

Give zero to three executable recommendations. Prefer, in order:

```text
planar-doc diff --json
planar-doc lint --json
planar-doc verify --json
```

When operator judgment is still required, name the exact row IDs and required
decision before suggesting a mutation. Do not suggest `build` until every diff
row is approved and ready to be absorbed.

## Recovery

For read or parse failure, retry `planar-doc diff --json` from the reported
repository root. For prose failure, inspect the working-tree diff and retry the
doc-author with an envelope containing only still-approved failed rows. For a
row-level manifest failure, retry the exact validated `cover` or `nodoc`
command for that row. For lint/build/verify failure, preserve the diff and
retry the failed command, followed by `planar-doc verify --json` and
`planar-doc diff --json`.

Never recommend a blanket reset or claim that completed independent operations
were rolled back. If approval must change, return to the row gate and obtain a
new explicit disposition.

## Source and render rules

This file under `skills/src/` is the only authored skill source. Do not create
or edit generated Claude, Codex, or Copilot projections directly. Maintainers
render, then verify, against an out-of-tree directory:

```text
planar skills render --out <staging-dir> pl-doc-maintain
planar skills render --check --out <staging-dir> pl-doc-maintain
```

Installation renders and links the vendor surfaces from this canonical source.

## Vendor Notes

