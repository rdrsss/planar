---
description: Inspect the cwd-derived scope and learn how to override it per verb.
origin: docs/cli-reference.md#domain-scope
shared_notes:
    - Resolved scope is read from the cwd at the start of every invocation; no vendor-specific state is kept outside the database.
slug: pl-scope
vendor:
    claude:
        argument_hint: <show|suggest>
        invocation_examples: |
            /pl-scope show
            /pl-scope show --json
            /pl-scope suggest
---

# Planar Scope ({{.VendorTitle}})

Inspects the scope Planar derives from the current working directory and shows how to override it on a per-verb basis. There is no scope stack to manage.

## What It Does

Reads the cwd, walks up to find the nearest registered Planar scope (project, org, or workspace), and reports what verbs invoked from this cwd will resolve to. Also lists the candidate associations the cwd project is a member of, so callers can pick the right `--scope` override when working across repos.

## CLI Commands

Wraps [`scope`](../../docs/cli-reference.md#domain-scope):

```
planar scope show [--json]
planar scope suggest [--json]
```

`planar scope show` prints the resolved scope (one association line, or a multi-element slice at a workspace root, or `none` when the cwd is outside every registered scope). `--json` emits `{"resolved_scopes": [...], "source": "cwd", "cwd": "<path>"}` for scripting.

`planar scope suggest` lists the associations the cwd project is a member of — useful when the resolver refuses to guess (workspace root, ambiguous membership) and you need to pick a `--scope` override.

## Override pattern

Every write verb accepts `--scope <slug>` to override the cwd-derived value for that invocation. Use it when:

- working from outside the target repo's cwd (e.g. drafting a plan for `service-a` while sitting in `service-b`),
- standing at a workspace root where the resolver refuses because membership is ambiguous,
- scripting or batching across multiple scopes from a neutral cwd.

The cross-scope guard (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)) compares the resolved write scope against the target entity's stored scope and refuses with exit 1 on mismatch.

## What Was Removed

`planar scope use`, `planar scope pop`, and `planar scope clear` were removed alongside the `active_scope` table. The active stack had become a footgun: it persisted across sessions and silently re-routed writes to whatever was last pushed, even from unrelated cwds. The current model is "your cwd is your scope; `--scope` is the explicit override." The deleted verbs now exit 1 with a redirect message pointing at `scope show`.

## When To Invoke

At the start of a session, before running a write verb from an unfamiliar cwd, or when a verb refuses with `AmbiguousScopeError` and you need to see the candidate scopes for the cwd.

## Context

Report the cwd, `show` or `suggest` mode, and JSON or text output mode. Scope is
resolved fresh for this invocation and is never session state.

## Intent

State in one sentence whether the operator wants the resolved scope or
candidate associations for an explicit override.

## Actions

Report `attempted`, `applied=0`, `skipped`, and `failed` counts for resolution
and suggestion checks.

## Result

Always report `outcome=ok|partial|error`, the verified `resolved_scopes`, source
cwd, and candidate override slugs when requested. No matching scope is an
explicit successful empty result, not a silent success.

## Warnings

Name ambiguous membership, an unregistered cwd, or unavailable resolution.
Do not warn for an expected empty suggestion list outside registered projects.

## Next actions

Give zero to three executable recommendations, normally the exact
`--scope <slug>` form for the intended write or `planar scope suggest --json`
when resolution is ambiguous.

## Recovery

For a failed or ambiguous read, provide `planar scope show --json` and `planar
scope suggest --json`. This read-only skill has no undo path and must not
recommend the removed scope-stack verbs.

## Vendor Notes

