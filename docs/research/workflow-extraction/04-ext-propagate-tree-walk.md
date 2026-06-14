# W3 — ext propagate tree-walk workflow

**Status:** spec · child of `00-anchor.md` · PARTIAL · **depends on `01`/P1**
**Current home:** `handlers/ext/propagate.zig` (~1094 LOC, the single most
workflow-y handler in the tree) + `agents/ext-sync.md`
**Determinism:** ~70% — tree traversal + idempotent per-entity create is
mechanical; strategy *selection* has a thin judgment/heuristic edge.

`ext propagate` walks an anchor plan's descendants and, per entity, renders a
template, POSTs a counterpart, and records a link — plus restrategize prompting,
verify-counterparts probing, and unlink/recreate conditionals. The traversal and
the conditionals are sequencing; the render+POST+link is the atomic unit. Move
the sequence out; keep the unit in Zig (exposed by `01`/P1 as `propagate-one`).

---

## 1. The procedure (after `01` decomposes the monolith)

```
Step 0 — Open run; resolve plan + system; read cached strategy
  planar plan show <plan> --json ; planar ext list --json
  ‹trace: run open, payload={plan, system, cached_strategy}›

Step 1 — Strategy gate (heuristic, optional confirm)
  if --restrategize: pick fresh strategy (repo-count rule, ADR-0006); confirm; mark counterparts for abandon
  ‹trace: kind=strategy, payload={chosen, source}›

Step 2 — Walk descendants in topo order        (NEW primitive: 01/P1)
  planar plan descendants <plan> --json
  for each entity:
    planar ext propagate-one <system> --from <kind:id> --strategy <s> --json   (atomic: render+POST+link, idempotent skip)
    ‹trace: kind=propagated, payload={entity, link_id|skipped|error}›
    on error: record, continue (do not abort the whole tree)   ← resumability the monolith lacks

Step 3 — Verify counterparts (optional)
  for each linked entity: probe remote exists?
    planar-watch sync-events --plan <plan> --json     (NEW read: 01/P2)
    if missing: --unlink then re-propagate-one, OR report   ‹trace: kind=verify›

Step 4 — Close run; emit summary
  planar run finish ... ; ‹trace: run close, payload={created, skipped, errors}›
```

The resumability in Step 2 is the concrete payoff: the monolith creates all-or-
nothing across a tree; the workflow records each `propagate-one` outcome and can
re-run to fill only the gaps, because `propagate-one` is idempotent.

---

## 2. Judgment / heuristic edge

Strategy selection (parent-issue vs projects-v2 vs tracking-issue) is the
repo-count rule from ADR-0006 — **deterministic**, not LLM. The only genuinely
soft moment is the operator confirm on `--restrategize` (abandoning existing
counterparts is destructive). No LLM callout is required; `agents/ext-sync.md`
can shrink to context + the restrategize confirm.

---

## 3. What stays in Zig

- `ext propagate-one` (`01`/P1) — template render + adapter POST + atomic
  `external_links` write + `sync_events` record, one transaction. The harness
  must never render a template or hold credentials.
- `plan descendants` (`01`/P1) — topo-ordered subtree read.
- The adapter factory and `text/template` rendering — schema-aware, stays.

What leaves Zig: the *traversal*, the *restrategize branch*, the *verify loop*,
and the *unlink/recreate conditional* — i.e. the ~30% of `propagate.zig` that is
decision flow rather than the per-entity transaction.

---

## 4. Milestones

1. **Consume `01`/P1+P2.** Build the walk as
   `descendants | for each propagate-one`, with per-entity error capture and a
   run trace. *Touches:* workflow source; `handlers/ext/propagate.zig` becomes
   the thin built-in that delegates to the same primitives (anchor §W0 P1
   already reimplements it as the loop — this child adds the restrategize/verify
   *flow* on top).
   *Accept:* propagating a fixture plan via the workflow yields the same
   `external_links` rows as the built-in; a forced mid-tree error leaves prior
   links intact and a re-run fills only the missing one.
2. **Restrategize + verify flows.** *Accept:* restrategize abandons old
   counterparts then re-creates; verify detects a remotely-deleted counterpart
   via `sync-events` and reports it.

**Exit:** the tree-walk is a resumable, traced workflow over idempotent
primitives; `propagate.zig`'s decision flow is gone, its per-entity transaction
remains.

---

## 5. Open questions

- **OQ-1.** Reconcile `ext create` and `ext propagate-one` — are they the same
  verb with a strategy-cache flag, or siblings? (Resolution: at `01`/P1 design;
  prefer one verb to avoid the parity surface drifting.)
- **OQ-2.** Topo order for `plan descendants` — strict dependency order, or
  parent-before-child structural order? Propagation needs parent counterparts to
  exist before children link to them. (Gate: `01`/P1 — structural is likely
  sufficient; confirm against the projects-v2 strategy's parent-link needs.)
