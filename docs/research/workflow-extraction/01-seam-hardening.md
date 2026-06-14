# W0 — Seam hardening (enabling primitives)

**Status:** spec · child of `00-anchor.md` · BUILD (Zig, no harness)
**Blocks:** `04` (ext propagate), `05` (introspect), `09` (recovery/doctor)
**Kind:** thin primitive verbs added to the binaries — *not* a workflow itself.

This child closes the three seam gaps from anchor §5. Each is a small, atomic
verb that lets a harness compose a process it currently cannot decompose. None
of these add business logic to the binary; they *expose* an existing capability
at a finer grain, or expose a table that is already written but unreadable.

---

## 1. P1 — Decompose the propagate tree-walk

`handlers/ext/propagate.zig` (~1094 LOC) is monolithic: it walks the plan
descendants, renders every template, POSTs every counterpart, and records every
link in one pass. A harness cannot drive it one entity at a time, cannot resume
mid-tree after a failure, and cannot interleave its own gating. Split the
*atomic unit* out:

- `planar ext propagate-one <system> --from <kind:id> [--strategy <s>] [--json]`
  — render one entity's template, POST it, record the `external_links` row, in
  one transaction. Idempotent: skips if a link already exists. This is the
  existing per-entity body of `propagate.zig`, surfaced as a verb. (Largely
  overlaps existing `ext create`; reconcile the two rather than duplicate —
  `propagate-one` is `create` plus strategy-cache awareness.)
- `planar plan descendants <plan-id> --json` — emit the anchor's full subtree
  (child plans + tasks + scenarios) in dependency-topological order, the order
  the tree-walk must follow. Read-only.

`ext propagate` (the monolith) stays as a convenience verb but is reimplemented
as the trivial loop `descendants | for each: propagate-one`, proving the
decomposition is faithful.

*Touches:* `src/cmd/planar/handlers/ext/propagate_one.zig` (new),
`src/cmd/planar/handlers/plan/descendants.zig` (new),
`src/cmd/planar/handlers/ext/propagate.zig` (reimplement loop over primitives),
`src/cmd/planar/handlers/ext/cmd.zig` / `plan/cmd.zig` (register).
*Accept:* integration test — `descendants --json` topo order matches the
monolith's walk order; `propagate-one` twice on the same entity creates exactly
one link; the reimplemented `propagate` produces byte-identical link rows to the
pre-split behavior on a fixture plan.

---

## 2. P2 — Make `sync_events` queryable

The `sync_events` table records every propagation/sync outcome, but there is no
`--json` read verb over it — only the coarse `planar report --json` rollup. A
harness mining sync signals (stuck propagations, strategy flips, missing
counterparts) is forced to either hold a SQLite handle (violates the no-handle
invariant) or accept counts-only aggregates.

- `planar-watch sync-events [--plan <id>] [--system <slug>] [--entity <k:id>]
  [--outcome <ok|conflict|error>] [--since <ts>] [--limit <n>] [--json]` —
  per-row read over `sync_events`, sibling to the existing
  `planar-watch actions` / `planar-watch claims`. Read-only (planar-watch opens
  the DB `mode=ro`).

*Touches:* `src/cmd/planar-watch/handlers/sync_events.zig` (new),
`src/cmd/planar-watch/handlers/cmd.zig` (register).
*Accept:* integration test seeds sync_events via a propagate run, reads them back
filtered by `--plan` and `--outcome`, asserts row shape + filter correctness.

---

## 3. P3 — Scope `reconcile` and make it previewable per plan

`planar-agent reconcile` sweeps *all* expired claims globally. A recovery
workflow (W8) needs to scope the sweep to one plan and preview it before
applying.

- `planar-agent reconcile [--plan <id>] --dry-run --json` — already supports
  `--dry-run`; add `--plan` scoping and ensure `--dry-run --json` reports the
  exact claim/action/run rows that *would* change without writing.

*Touches:* `src/cmd/planar-agent/handlers/reconcile.zig` (add `--plan` filter +
dry-run report shape).
*Accept:* integration test — `--plan X --dry-run` lists only plan-X stale claims
and writes nothing (verify via `claims --json` unchanged); applying then sweeps
exactly that set.

---

## 4. What this child does NOT do

It adds no orchestration. Every verb here is one transaction or one read. The
*sequencing* that uses them lives in `04`, `05`, `09`. Keeping P1–P3 as pure
primitives is what preserves anchor D2 (the binary keeps the atomic verb; the
workflow keeps the sequence).

---

## 5. Milestones

1. **P1 propagate-one + descendants.** Two verbs; reimplement `propagate` as the
   loop. *Accept:* faithfulness test above. *(Longest pole — the monolith reuse.)*
2. **P2 sync-events read verb.** *Accept:* filtered read test.
3. **P3 reconcile --plan + dry-run report.** *Accept:* scoped-preview test.

**Exit:** `04`, `05`, and `09` are unblocked; no business logic added to any
binary; the parity/CLI-usage gates stay green.
