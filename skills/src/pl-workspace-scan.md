---
description: Initialize, inspect, refresh, diagnose, and repair a Planar workspace while preserving the workspace-scan invocation.
origin: docs/cli-reference.md#domain-workspace
shared_notes:
    - Workspace state is resolved and changed only through the Planar CLI; generated routing and guidance files are never edited directly.
slug: pl-workspace-scan
vendor:
    claude:
        argument_hint: '[scan|init|doctor|routing-show|routing-build|regenerate|repair] [--enrich] [--workspace <slug>] [--dry-run]'
        invocation_examples: |
            /pl-workspace-scan                          # compatible default: routing build + regenerate
            /pl-workspace-scan --enrich                 # compatible enriched refresh
            /pl-workspace-scan routing-show --workspace org:work
            /pl-workspace-scan doctor                   # diagnose and repair registered workspaces
            /pl-workspace-scan repair --workspace org:work
---

# Planar Workspace Lifecycle ({{ VendorTitle }})

Use the existing `pl-workspace-scan` entry point for the complete workspace
lifecycle: initialize, diagnose, inspect or rebuild routing, regenerate the
canonical guidance, and repair drift. An invocation with no operation remains
the original scan workflow: routing build followed by regenerate.

## Context

Resolve the requested operation, workspace, and mode before acting. Supported
operations are `scan` (the default), `init`, `doctor`, `routing-show`,
`routing-build`, `regenerate`, and `repair`. `--enrich` applies only to `init`,
`scan`, `routing-build`, and `repair`. `--dry-run` is a skill-level preview: it
performs reads only and does not pretend that the write verbs have a CLI dry-run
flag.

For `routing-show`, `routing-build`, `regenerate`, and `repair`, translate
`--workspace <id|org:slug|slug>` to the optional positional `<workspace>` on
the CLI. If it is omitted, rely on the CLI's documented workspace resolution;
do not guess when multiple org workspaces exist. `init` always operates on the
current directory and accepts its documented `--name`, `--slug`, `--scan`,
`--meta-repo`, `--no-scan`, and `--enrich` flags. `doctor` intentionally checks
all registered org workspaces and takes no workspace target.

Before a scoped write, run `planar scope show --json`. The cwd must resolve
inside the intended workspace, or the operator must supply `--workspace` where
the verb supports it. Stop instead of weakening scope validation.

## Intent

State the interpreted operation in one sentence, including whether it is a
read-only preview, a static refresh, or an enriched refresh. If the request is
ambiguous between initializing a new workspace and refreshing an existing one,
stop before writing and present those two choices.

## Actions

Use only these documented commands:

```text
planar workspace init [--name <text>] [--slug <text>] [--scan <N>] [--meta-repo] [--no-scan] [--enrich] --json
planar workspace doctor --json
planar workspace routing build [<workspace>] [--enrich] --json
planar workspace routing show [<workspace>] --json
planar workspace regenerate [<workspace>] --json
```

Compose operations as follows:

1. `init`: run `workspace init` from the requested workspace root. Treat a
   successful response with a pipeline warning as partial: registration may
   already be committed, so do not claim rollback.
2. `doctor`: run `workspace doctor --json`, report every org's found and
   repaired issues, then run it once more to verify the fleet's post-state.
3. `routing-show`: run only `workspace routing show ... --json`.
4. `routing-build`: run the build, then verify with `routing show --json`.
5. `regenerate`: first verify routing exists with `routing show --json`, run
   regenerate, then retain its returned canonical path, project count, and byte
   count as the post-state evidence.
6. `scan`: run routing build (add `--enrich` when requested), verify with
   routing show, run regenerate, and report both results.
7. `repair`: run doctor, rebuild routing, verify routing show, regenerate, then
   run doctor again. Doctor repairs state directories and eligible sibling-root
   guidance links; build and regenerate repair missing or drifted generated
   content.

For `--dry-run`, run `planar scope show --json` and, for an existing workspace,
`planar workspace routing show [<workspace>] --json`. Report the exact commands
that would run, with `applied=0`; do not call init, doctor, build, or regenerate.

Track `attempted`, `applied`, `skipped`, and `failed` per CLI stage. Stop stages
that depend on a failed prerequisite, but still perform safe read-only
inspection when it can establish the persisted post-state.

## Result

Return `outcome=ok|partial|error` plus the resolved scope, workspace identifier,
operation, and action counts. Include stable post-state from CLI JSON:

- init: org id/slug, project and membership results, and pipeline results;
- doctor/repair: issues found and repaired per org and the verification pass;
- routing operations: routing path, project count, dependency edges, enrichment
  state or misses, and the verified routing-table JSON result;
- regenerate/scan: canonical `agents_path`, project count, and bytes written.

A successful idempotent run is an informative no-op, not a warning. Say what
was already current and provide the most useful next read command.

## Warnings

Report pipeline warnings, unresolved workspaces, malformed workspace config,
enrichment misses, and partial stage failures. `--enrich` delegates enrichment
to the configured CLI lifecycle; this skill does not inspect projects, compute
fingerprints, invoke an implementation-specific cache protocol, or write cache
files itself.

The workspace-root `AGENTS.md` and `CLAUDE.md` are generated links or copy
fallbacks to canonical state for sibling workspaces. Never hand-edit them.
Meta-repo root instruction files are repo-owned and doctor leaves them alone.
Operator routing changes belong in
`~/.planar/workspaces/<org_id>/routing-table-overrides.json`; never edit
generated `routing-table.json` or canonical generated `AGENTS.md` directly.

## Next actions

Give at most three executable recommendations. Prefer:

```text
planar workspace routing show [<workspace>] --json
planar workspace doctor --json
planar assoc tree --json
```

Also point existing `/pl-workspace-scan` users to the lifecycle operation names
without changing the meaning of the no-argument, `--workspace`, or `--enrich`
forms.

## Recovery

Name the failed stage and give its exact idempotent recovery:

- unresolved target: `planar workspace routing show <workspace> --json`;
- incomplete init pipeline or missing state: `planar workspace doctor --json`;
- missing/stale routing: `planar workspace routing build <workspace> --json`;
- enriched refresh: `planar workspace routing build <workspace> --enrich --json`;
- missing/stale canonical guidance: `planar workspace regenerate <workspace> --json`;
- suspected link drift: `planar workspace doctor --json`, then rerun the failed
  scoped stage and its post-state read.

Do not promise rollback across init registration, filesystem repair, routing
build, and regeneration: they are separate CLI operations. Completed stages
remain applied and must be named in a partial result.

## Boundaries

- Use the CLI as the only workspace access layer; never write SQLite directly.
- Do not hand-edit generated workspace guidance, routing tables, manifests, or
  workspace-root links/copies.
- Do not remove a workspace or association; there is no workspace destroy verb
  in this lifecycle.
- Do not invent a target for global `workspace doctor` or a `--dry-run` CLI
  flag that the schema does not expose.
- Keep strict scope behavior and stop on ambiguous resolution.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
