---
title: Scope resolution
doc_kind: feature
template_version: 1
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
references:
  scope_product_spec:
    kind: planar
    entity: artifact:63
  scope_tech_spec:
    kind: planar
    entity: artifact:64
  scope_roadmap:
    kind: planar
    entity: artifact:65
---

# Scope resolution

> **Stale document (task 6140, 2026-09-09).** This page describes the
> active-scope-stack model (`planar scope push`/`pop`, stack-top
> fallback, `--no-scope-check`) that plan 153 M5 removed entirely —
> the `active_scope` table is dropped and those verbs no longer exist.
> The current write-resolution algorithm (`--scope` explicit override
> → cwd derivation → refuse) is documented in
> [`concepts.md § Write resolution`](../concepts.md#write-resolution)
> and [`concepts.md § Cross-scope guard`](../concepts.md#cross-scope-guard).
> This task fixed only the `--no-scope-check` claim below in place; the
> rest of the page's stack-based model is a larger doc-drift finding
> filed separately (see task list for the scope-resolution feature
> doc rewrite).

Planar resolves every write verb to a single, explicit scope before it
touches the database. The resolver runs the same four-step algorithm
for every command, and refuses to guess when the active-scope stack
and the current working directory disagree[^scope_tech_spec].

## What it does

- Pins every write verb (`plan add`, `task add`, `artifact add`,
  `decision add`, `question add`, `scenario add`, …) to exactly one
  scope before any row is created[^scope_product_spec].
- Reads three independent inputs every time: an explicit `--scope`
  flag, the active-scope stack (`planar scope show`), and the
  cwd-derived scope (the association whose member project contains
  the current working directory).
- Refuses to write — exits with a `cperr.User` error and a clear
  diagnostic — when the stack-top scope and the cwd-derived scope
  disagree without `--scope` to break the tie.
- Prints a non-blocking warning when cwd derives a scope that is
  in the stack but not on top, suppressible with `--quiet`.
- Surfaces both the resolved scope and any pending disagreement in
  `planar health` so onboarding catches mismatches before they
  silently misroute work.

## The 4-step algorithm

The resolver applies these steps in order; the first one to produce
a unique scope wins[^scope_tech_spec]:

1. **`--scope` explicit override** — if the flag is present, the
   resolver uses it verbatim. The scope still has to resolve to a
   real `associations.id`; an unknown slug is a hard error. No
   cwd derivation runs in this branch.
2. **Cwd derivation** — Planar walks upward from the current
   working directory, finding the first git remote (`origin`) that
   matches a `projects.git_remote`. The resolver then looks up
   every association that includes that project. Zero hits leaves
   cwd unresolved; one hit yields a cwd-scope.
3. **Stack-top fallback** — if cwd produced nothing, the resolver
   uses the top of the active-scope stack (`planar scope show`).
   This is the common path for sessions started from an unrelated
   directory after `planar scope push`.
4. **Strict-mode tiebreak** — when both cwd and stack are populated
   and they point at different associations, the resolver refuses
   to write. The error message names both scopes and the offending
   verb, plus the two ways to resolve: pass `--scope`, or change
   directories so cwd agrees.

The strict tiebreak is what makes the resolver safe to run with
multiple in-flight agents on the same machine. Pre-hardening,
Planar would silently pick the stack-top scope and misroute writes
into the wrong project — a class of bug the strict resolver makes
impossible to hit by default[^scope_roadmap].

## No `--no-scope-check` escape hatch

The current binary implements no such flag; `planar schema` declares
it on no command, and passing it fails at parse time with exit 2
(`error: <cmd>: The following argument was not expected: --no-scope-check`).
It is not merely undocumented-but-present — `grep -rn
guard_write src/` shows the engine-layer bypass primitive exists and
is unit-tested, but no `cmd/` handler ever calls it with `true`, so no
verb can reach it from the CLI. There is no flag-based way to
downgrade a cross-scope refusal to a warning.

The only present-day remedies for a cross-scope-guard refusal are:

- **Pass `--scope <slug>` explicitly** — the write resolver's step 1
  above; it wins over cwd derivation outright.
- **`cd` into the entity's owning repo** so cwd derivation resolves
  to the matching scope.

A genuine mismatch — operator scope and entity scope disagree and
neither of the above is done — fails outright. See
[`concepts.md § Cross-scope guard`](../concepts.md#cross-scope-guard)
for the exact refusal message format and the guarded/unguarded verb
matrix.

## The cwd-stack-mismatch warning

When cwd derives a scope that sits in the active stack but is *not*
the top, the resolver prints a one-line warning to stderr:

```
warning: cwd scope project:web-app is in the stack but not on top
         (stack top: project:platform). Use --scope to override or
         `planar scope swap` to reorder.
```

The warning is non-blocking — the resolver proceeds with cwd's
scope. It exists to make subtle stack-ordering bugs visible without
breaking the user's flow. `--quiet` suppresses the warning entirely
for batched / scripted contexts where the noise is unwelcome.

## CLI

```sh
# Inspect the current resolution inputs.
planar scope show

# Push an explicit scope; useful when cwd-derivation is ambiguous.
planar scope push assoc:project:platform

# Run a write verb under the resolved scope.
planar task add "Refactor scope resolver" --priority 100

# Break the tie explicitly with --scope (wins even if cwd disagrees).
planar task add "Cross-repo bookkeeping" --scope org:platform

# Inspect resolution health.
planar health
```

## Implementation notes

The resolver lives in `src/engine/identity/scope.zig` and is
called by every write verb's handler under `src/cmd/planar/handlers/`.
The single entry point is `scope.resolveForWrite`; the cwd derivation
primitive is factored as `scope.deriveFromCwd` so the same code path
serves both the resolver and the `planar scope show` read. Doc
comments on those functions are the authoritative specification.

## Related

- [Concepts: scope and association](../concepts.md#scope)
- [Workflows: switching scope mid-session](../workflows.md)

[^scope_product_spec]:
[^scope_tech_spec]:
[^scope_roadmap]:
