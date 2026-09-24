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

Planar resolves every write verb to a single scope before it touches the
database. Resolution is a pure function of `(--scope flag, cwd, database)` —
there is no session state, no stack, and nothing to forget about.

**This page was rewritten on 2026-09-11 (tasks 6676 and 6746) against the
shipped binary.** Every claim below was measured. Earlier editions described a
four-step algorithm with an active-scope stack, `planar scope push` / `swap`
examples, and a `--no-scope-check` escape hatch — a model plan 153 M5 removed
and a flag that never existed on this binary.

## What it does

- Pins every write verb (`plan create`, `task add`, `artifact add`,
  `decision add`, `question add`, `scenario add`, …) to exactly one scope
  before any row is created[^scope_product_spec].
- Reads **two** inputs, not three: an explicit `--scope` flag and the
  cwd-derived scope. There is no stack to consult.
- Surfaces the cwd-derived resolution through `planar scope show`.

## The algorithm

`resolve_for_write`, in order; the first step that yields a scope wins:

1. **Explicit `--scope`.** Threaded through **verbatim** — not validated
   against the database. It is the operator's stated intent. An unresolvable
   slug surfaces later, from whichever verb tries to use it, as `SlugNotFound`
   (exit 1).
2. **Meta-workspace arm.** Standing exactly on a registered meta-workspace
   root, where the cwd names the org and the root repo equally well, the write
   **refuses** (exit 5) rather than pick. Inside a member repo of a meta
   workspace, the write lands on that concrete repo.
3. **Cwd derivation, most-specific-wins.** The same candidate set, specificity
   ranking and longest-root tie-breaker the read path uses. A member repo
   outranks an org containing it: a write from inside `~/work/repo-a/` lands at
   `repo:repo-a`, not `assoc:work`.
4. **Otherwise `global`, exit 0.**

Step 3 shares its ranking with `resolve_read_scope_set` as of task 6746
(decision 1100). Before that the write path mapped the cwd's project to its
single association and could never yield a `repo:` scope, so `scope show` and
the write path disagreed from the same directory. They now agree, and a test
pins that agreement.

### Step 4 is a footgun, documented because it is true

A write from an unregistered directory does **not** refuse. It succeeds at
global scope. The **read** path refuses the same cwd:

```
$ cd /tmp/nowhere && planar plan list
error: cwd is not inside any registered Planar scope; cd into a registered
       scope or pass --scope global
(exit 1)

$ cd /tmp/nowhere && planar plan create "probe"
scope:    global
(exit 0)
```

Reads are the strict side, not writes. Whether step 4 should refuse is an open
question (decision 1100 deliberately did not settle it). Until it is settled,
run `planar scope show` before writing from an unfamiliar cwd.

### One registered-project exception

A project registered with **no association** does not resolve to
`repo:<slug>`; it yields no scope, with the reason `project_unassociated`.
`plan create` turns that into an exit-5 refusal naming the remedy:

```
error: plan create: project has no association; run `planar assoc create
project:proj --kind project` then `planar assoc add project:proj <repo-path>`,
or pass `--scope global` explicitly
```

`task add` and `scenario add` deliberately do **not** refuse and file under
global. That split is pinned by tests and is not an oversight.

## No escape hatch

There is no flag that downgrades a scope refusal. **`--no-scope-check` does
not exist on this binary** — `planar schema` declares it on no command, and
passing it fails at parse time with exit 2:

```
error: <cmd>: The following argument was not expected: --no-scope-check
```

(An engine-layer `guard_write` bypass parameter of the same shape exists and is
unit-tested, but no `cmd/` handler ever calls it with `true`, so no verb can
reach it from the CLI.)

The remedies for a refusal are `--scope <slug>` or `cd` into the entity's
owning repo. Note that `--scope` selects the write scope only on the verbs
where it means that: on `plan update`, `artifact update` and `annotate update`
it is a **patch field** that reassigns the entity's stored scope. See
[`cli-reference.md § --scope is four different flags`](../cli-reference.md#--scope-is-four-different-flags).

## The removed active-scope stack

Plan 153 M5 dropped the `active_scope` table
(`migrations/00009_drop_active_scope.up.sql`) and the verbs that manipulated
it. Measured behaviour of what operators may still type:

| Typed | Result |
|-------|--------|
| `planar scope use` | retired-verb notice, **exit 2** |
| `planar scope pop` | retired-verb notice, **exit 2** |
| `planar scope clear` | retired-verb notice, **exit 2** |
| `planar scope use <slug>` | retired-verb notice, **exit 2** — the stub declares an optional positional and `allow_extras`, so the notice is NOT preempted |
| `planar scope use --bogus` <!-- cli-lint-ignore: the flag's ABSENCE is the point --> | retired-verb notice, **exit 2** — same reason: the handler runs before CLI11 can reject the flag (task 6446) |
| `planar scope push` / `planar scope swap` | parse error, exit 2 — these never existed |

The notice reads:

```
error: `planar scope pop` was removed in plan 153 M5; the active scope stack
is gone. Pass --scope <slug> to individual verbs, or cd into a registered
scope. Run `planar scope show` to inspect the cwd-derived scope.
```

## CLI

```bash
# Inspect the cwd-derived resolution. Run this before writing from an
# unfamiliar cwd -- step 4 above does not refuse.
planar scope show

# Write under the cwd-derived scope.
planar task add "Wire the resolver"

# Override explicitly. The flag wins over cwd derivation.
planar task add "Wire the resolver" --scope repo:planar

# See which scopes a cwd could resolve to.
planar scope suggest
```

## Implementation notes

The resolver is `planar::engine::identity::resolve_for_write` in
`src/engine/identity/scope.cpp`, called from each binary's `cmd` layer
through `resolve_write_scope`. `derive_write_scope_ranked` performs step 3 and
shares its ranking with `resolve_read_scope_set`, which backs `scope show`.
Doc comments on those functions are the authoritative specification.

Earlier editions pointed at `src/engine/identity/scope.zig`. The Zig tree was
deleted at the M10 cutover; that path no longer exists.

## Related

- [Concepts: scope and association](../concepts.md#scope)
- [Concepts: write resolution](../concepts.md#write-resolution)
- [Concepts: cross-scope guard](../concepts.md#cross-scope-guard)
- [CLI reference: cross-scope guard](../cli-reference.md#cross-scope-guard)

[^scope_product_spec]:
[^scope_tech_spec]:
[^scope_roadmap]:
