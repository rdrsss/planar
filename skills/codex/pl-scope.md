---
name: pl-scope
description: Inspect the cwd-derived scope and learn how to override it per verb.
source: docs/cli-reference.md#domain-scope
---

# Planar Scope (Codex)

Inspects the scope Planar derives from the current working directory and shows how to override it on a per-verb basis. There is no scope stack to manage — plan 153 M5 removed it.

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

`planar scope use`, `planar scope pop`, and `planar scope clear` were removed in plan 153 M5 alongside the `active_scope` table. The active stack had become a footgun: it persisted across sessions and silently re-routed writes to whatever was last pushed, even from unrelated cwds. The post-M5 model is "your cwd is your scope; `--scope` is the explicit override." The deleted verbs now exit 1 with a redirect message pointing at `scope show`.

## When To Invoke

At the start of a session, before running a write verb from an unfamiliar cwd, or when a verb refuses with `AmbiguousScopeError` and you need to see the candidate scopes for the cwd.

## Vendor Notes

- Installed into `~/.codex/skills/pl-scope` from `~/.planar/codex-skills/pl-scope`.
- Resolved scope is read from the cwd at the start of every invocation; no vendor-specific state is kept outside the database.
