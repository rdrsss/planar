---
slug: pl-documenter
description: "Walk the repo-state merkle diff and propose a worklist of doc actions (extend-cover / create-doc / nodoc / defer) for operator gating."
source: agents/documenter.md
model_tier: large
vendor:
  claude:
    argument_hint: "[--json] [--since <git-ref>]"
    invocation_examples: |
      /pl-documenter
      /pl-documenter --json
shared_notes:
  - "Never opens SQLite. Operates entirely on the working tree and `.planar-manifest`."
  - "Never writes prose to disk autonomously. The worklist is the only output."
  - "Never invokes `planar-doc cover` / `nodoc` / `build` itself. The operator gates each row."
---

# Documenter ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `documenter` agent. See [`agents/documenter.md`](../../agents/documenter.md) for the full role spec, decision policy, and capability boundary.

## When to use

- The orchestrator's Phase 6 fires at the end of a work cycle (default-on; opt-out via `--no-docs` on the orchestrator invocation).
- The operator wants a manual sweep after a non-orchestrated change.

Do **not** invoke mid-cycle. The documenter's inputs are the post-cycle repo state and the prior manifest; running it before a cycle completes produces noise.

## What it does

1. Runs `planar-doc diff --json` to read the current drift worklist.
2. For each row, reads the prior doc body (if any) and a slice of the changed source.
3. Classifies each row as one of `extend-cover` / `create-doc` / `nodoc` / `defer` per the [decision policy](../../agents/documenter.md#decision-policy).
4. Emits a worklist (text or JSON) where each row carries the verb the operator would run.

The documenter does NOT write to `.planar-manifest`, run `planar-doc cover` / `nodoc` / `build`, or commit any prose. It produces the worklist and stops.

## Underlying CLI surface

```
planar-doc diff --json
git log --since <manifest.regenerated_at> -- <changed-paths>
git show <ref>:<path>  (targeted reads)
```

No `planar`, `planar-agent`, or `planar-watch` calls — the documenter is a pure planar-doc consumer.

## Worklist shape

See [`agents/documenter.md` § Worklist shape](../../agents/documenter.md#worklist-shape) for the canonical JSON schema. Text output uses one row per line:

```
<signal>  <path>  <action>  — <reason>
```

The trailing `verb` field is printed below each row so the operator can copy it directly.

## Operator gating

The operator reads the worklist and decides per row:

- **extend-cover** → run `planar-doc cover <path> <repo-path>` (or refresh prose, then `planar-doc build`).
- **create-doc** → author the proposed doc body, commit it, then `planar-doc cover <new-path> <repo-path>`.
- **nodoc** → run `planar-doc nodoc <repo-path>`.
- **defer** → escalate; no action this cycle.

After applying the chosen rows, close the loop with `planar-doc build` to reseat the manifest.

## Context

Report the repository root, prior manifest state and root hash when present,
the `--since` boundary when supplied, drift-row count, and text or JSON mode.
An absent manifest is an uninitialized documentation state, not permission to
create or modify one.

## Intent

State in one sentence that the run will inspect documentation drift and return
an operator-gated worklist without changing prose or manifest state.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed`. `attempted` is the
number of drift rows inspected; `applied` is always zero because this skill is
a proposer; `skipped` counts rows that need no proposal; and `failed` counts
rows whose evidence could not be read or classified. Also report proposal
counts for `extend-cover`, `create-doc`, `nodoc`, and `defer`.

## Result

Always report `outcome=ok|partial|error`, both manifest root hashes when
available, and the complete worklist with each row's signal, path, proposed
action, reason, and operator command. A clean diff is `outcome=ok` with zero
applied and an explicit "no documentation drift" result. The worklist is the
verified result; this skill does not mutate a post-state.

## Warnings

Name a missing or unreadable manifest, unavailable source or history evidence,
and every row that could not be classified. Do not warn merely because the
diff is clean. Never imply that a proposed row has been applied.

## Next actions

Offer at most three executable recommendations drawn from the highest-priority
worklist rows. Keep every action operator-gated. Do not route prose authoring
through a new workflow here; standalone documenter remains a proposer.

## Recovery

On a failed or partial inspection, preserve every completed proposal and give
`planar-doc diff --json` as the exact reinspection command, followed by the
idempotent `/pl-documenter` retry with the original arguments. There is no undo
command because this skill performs no writes. If the manifest is absent,
propose `planar-doc build` for the operator to gate; do not run it.

## Vendor Notes

- Installed to `~/.claude/commands/pl-documenter.md`.
- Invoked as `/pl-documenter [args]`.
- Reads the working tree directly; does not require any DB handle.

## Invocation

```
/pl-documenter
/pl-documenter --json
/pl-documenter --since main
```
