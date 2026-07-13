---
slug: pl-documenter
description: "Propose documentation actions from repo drift, gate every row, route approved prose to doc-author, and apply approved manifest-only actions through planar-doc."
source: agents/documenter.md
model_tier: large
vendor:
  claude:
    argument_hint: "[--json] [--since <git-ref>]"
    invocation_examples: |
      /pl-documenter
      /pl-documenter --json
shared_notes:
  - "The documenter specialist is strictly read-only: it proposes rows and never writes prose, manifest state, or coordination state."
  - "Only operator-approved prose rows are dispatched to doc-author; approved nodoc rows bypass prose authoring."
  - "The skill caller owns all planar-doc mutations and verifies their post-state."
---

# Documenter ({{.VendorTitle}})

{{.VendorTitle}} standalone workflow for the vendor-neutral `documenter`
specialist. See [`agents/documenter.md`](../../agents/documenter.md) for the
proposal policy and read-only capability boundary. This skill adds the caller
workflow around that specialist: inspect drift, present a row gate, dispatch
approved prose to `doc-author`, and apply approved doc-state operations.

The distinction is load-bearing. The documenter proposes; it never writes.
`doc-author` writes only approved prose under `docs/`. The skill caller alone
runs approved `planar-doc` mutations. For a full maintenance pass with the same
boundaries, see [`pl-doc-maintain`](pl-doc-maintain.md).

## Context

Resolve and report:

- the absolute repository root and output mode (`text` or `json`);
- `.planar-manifest` and its prior root;
- the optional `--since <git-ref>` evidence boundary;
- the initial `planar-doc diff --json` row count; and
- whether a dispatcher can invoke fresh `documenter` and `doc-author`
  specialists.

Run from the target repository. Do not open SQLite, invent planning context, or
use a repository-local substitute for the manifest. If the manifest is missing
or unreadable, stop before mutation and recommend an operator-reviewed
`planar-doc build --json`; do not silently accept the current tree as baseline.

## Intent

Interpret the invocation as: inspect the current documentation drift, obtain
operator dispositions for every proposed row, and apply only the explicitly
approved prose or doc-state work while preserving each role's capability
boundary.

Do not invoke mid-cycle. The evidence should be the completed work-cycle tree
and the prior manifest; an in-progress tree produces unstable proposals.

## Workflow

### 1. Read and verify drift

Run:

```text
planar-doc diff --json
planar-doc verify --json
```

Parse JSON rather than human output. `diff` is the authoritative row set. Use
`git log` and targeted `git show` or working-tree reads only as supporting
evidence; `--since` narrows those Git reads and does not replace the manifest
diff.

If the diff is empty and verification confirms `drift=false` with matching
live and stored roots, return a clean no-op. Dispatch neither specialist and do
not rebuild. If the two reads disagree, return an error with both observations
and the exact retry commands.

### 2. Dispatch the read-only proposer

For a non-empty diff, pass a fresh `documenter` specialist this envelope:

```json
{
  "manifest_path": ".planar-manifest",
  "diff_records": [],
  "covered_docs": {},
  "cycle_summary": []
}
```

Preserve `diff_records` exactly from `planar-doc diff --json`. Populate
`covered_docs` from the existing manifest. Include only caller-supplied facts in
`cycle_summary`; never fabricate task or plan identifiers.

The specialist may read the repository and classify rows as `extend-cover`,
`create-doc`, `nodoc`, or `defer`. It must not edit any file, invoke any
`planar-doc` mutation, call `planar` or `planar-agent`, or write coordination
state. Compare the working-tree paths before and after dispatch. Any specialist
write stops the workflow with `outcome=error`.

Normalize proposals into invocation-local `row_id` values. Each gate row must
show the original signal and source path, proposed action, target doc when
applicable, whether prose is required, the exact proposed `planar-doc`
operation, and the reason. Missing or ambiguous fields force `defer`; do not
infer write authority.

### 3. Gate every row

Present the complete normalized worklist before the first mutation. Require an
explicit disposition for each row ID:

| Disposition | Valid proposal | Authorized effect |
|---|---|---|
| `approve-prose` | `create-doc`, or `extend-cover` requiring a prose refresh | Send exactly this row to doc-author. Run no manifest verb until its prose succeeds. |
| `approve-cover` | `extend-cover` requiring no prose and naming a valid doc and source | Caller runs the exact displayed `planar-doc cover` add/remove operation. |
| `approve-nodoc` | `nodoc` | Caller runs the exact displayed `planar-doc nodoc` operation. Never dispatch doc-author. |
| `approve-reseat` | `hand-edit` or already-accurate covered source requiring no prose or coverage change | Run no row-level command; approval permits the final build to absorb it. |
| `reject` | any | No write; row remains unresolved. |
| `defer` | any | No write; preserve the proposal and recovery path. |

Silence, general encouragement, or approval of one row does not approve the
others. Approval is bound to the displayed row fields. A changed action,
target, source, removal mode, prose requirement, or authoring instruction
requires a new gate. A prose deletion, a target outside `docs/`, or an action
outside this allowlist is always deferred.

Do not run `planar-doc build` while any initial row is rejected, deferred,
ambiguous, or failed. Build reseats the whole tree and would absorb unapproved
state.

### 4. Route approved prose through doc-author

Collect only `approve-prose` rows and dispatch one fresh `doc-author` specialist
using the contract in [`agents/doc-author.md`](../../agents/doc-author.md):

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
      "operator_instruction": "Explain the approved public workflow."
    }
  ]
}
```

Every field must come from the gated row and operator response. Empty prose
sets skip dispatch. Never include `nodoc`, manifest-only coverage, reseat,
rejected, deferred, or unapproved rows.

Inspect the filesystem diff before any doc-state mutation. Every path
attributable to doc-author must be an approved `target_path` below `docs/`, and
every changed or reported row must belong to the approved set. Unexpected paths
or an absent row stop the workflow. Preserve valid approved prose as an
inspectable partial result; do not claim rollback.

On partial authoring failure, stop before `cover`, `nodoc`, or `build`. Recovery
uses a new envelope containing only the still-approved failed rows. A changed
target or instruction requires renewed approval.

### 5. Apply caller-owned doc state

After every approved prose row succeeds, the caller—not documenter or
doc-author—runs the exact gated operations:

```text
planar-doc cover <target-doc> <source-path>
planar-doc cover <target-doc> <source-path> --remove
planar-doc nodoc <source-path>
planar-doc nodoc <source-path> --remove
```

Use only the displayed approved form. A successful `create-doc` normally needs
its approved `cover` operation after authoring. An `approve-reseat` row runs no
row-level command.

An approved `nodoc` row bypasses prose authoring entirely: do not dispatch
doc-author for it, run its approved `planar-doc nodoc` operation directly, and
verify the observable manifest state before advancing. Apply the same post-state
check after every `cover` operation. Retain per-row command evidence because
independent operations are not one transaction.

### 6. Close and verify

Only after every initial row is approved and successfully applied or approved
for reseating, run:

```text
planar-doc lint --json
planar-doc build --json
planar-doc verify --json
planar-doc diff --json
```

Lint must pass before build. Retain the build root. Verification must return
exit 0, `drift=false`, and matching live, stored, and build roots. The final
diff must contain zero rows. A successful command exit without these post-state
checks is not success.

On lint, build, verify, or final-diff failure, preserve the approved prose and
manifest diff, report the last verified row state, and give exact inspect and
retry commands. Never fabricate rollback across independent writes.

## Status reporting

Publish short statuses through the dispatcher's status channel. If the caller
has a claim token, use
`planar-agent heartbeat --claim <token> --status "<text>"`; otherwise do not
create coordination state just to report status.

| Phase | Status string |
|---|---|
| Read manifest drift | `"reading documentation drift"` |
| Classify known rows | `"classifying documentation <current>/<total>"` |
| Wait for the row gate | `"awaiting:operator-confirmation"` |
| Dispatch approved prose | `"dispatching doc-author"` |
| Apply approved doc state | `"applying doc state <current>/<total>"` |
| Run closing gates | `"verifying documentation state"` |

Only the genuine operator wait uses `awaiting:`. Use bounded counters when the
total is known, keep every status below 256 bytes, and return the final result
without a redundant terminal heartbeat. Status failure is a warning and never
masks the workflow result. The documenter specialist's statuses end with its
proposal; caller-owned mutation phases must not be attributed to that read-only
role.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` over the initial diff
rows. Count a row as applied only when its approved prose/doc-state/reseat path
passes the final gates. Rejected and deferred rows are skipped. Invalid,
dispatch-failed, command-failed, lint-failed, or verification-failed rows are
failed. Report documenter and doc-author dispatch counts separately so a clean
diff or nodoc-only run proves which specialists did not run.

## Result

Always return `outcome=ok|partial|error` plus the repository root, output mode,
prior and new manifest roots when available, initial and final drift counts,
row IDs and dispositions, doc-author changed paths, executed `cover` and
`nodoc` operations, and lint/build/verify/final-diff outcomes.

The clean no-op is `outcome=ok` with zero row counts, zero dispatches, no
warning, and the existing verified root. A nodoc-only successful run has zero
doc-author dispatches and records the direct caller-owned `nodoc` operations.
Rejected or deferred rows produce an inspectable no-write or partial result,
not permission to build.

In `--json` mode, mirror Context, Intent, Actions, Result, Warnings, Next
actions, and Recovery with stable keys and row arrays. Text mode omits empty
sections except Result.

## Warnings

Report assumptions, unavailable evidence, status or dispatch failures,
unexpected paths, unresolved rows, partial application, and degraded
verification. An authoritative clean diff is not a warning. Never hide an
unresolved row behind a successful exit code.

## Next actions

Give zero to three executable recommendations, normally selected from:

```text
planar-doc diff --json
planar-doc lint --json
planar-doc verify --json
```

When operator judgment is required, name the exact row IDs and disposition
needed before suggesting a mutation. Do not suggest build until every initial
row is approved and ready to be absorbed.

## Recovery

For read or parse failure, retry `planar-doc diff --json` from the reported
repository root. For prose failure, inspect the working-tree diff and retry
doc-author with only still-approved failed rows. For a row-level failure, retry
the exact gated `planar-doc cover` or `planar-doc nodoc` command and recheck its
post-state. For a closing-gate failure, retry the failed command, followed by
`planar-doc verify --json` and `planar-doc diff --json`.

Never recommend a blanket reset or say completed operations were rolled back
unless a supported undo command actually ran. Any changed approval returns to
the row gate.

## Source and render rules

This file under `skills/src/` is the only authored skill source. Do not edit
generated Claude, Codex, or Copilot projections. Maintainers render and verify
against an out-of-tree staging directory:

```text
planar skills render --out <staging-dir> pl-documenter
planar skills render --check --out <staging-dir> pl-documenter
```

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```text
{{.InvocationBlock -}}
```
{{- end}}
