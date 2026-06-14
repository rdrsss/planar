# W6 — Sync reconcile loop (STUB)

**Status:** STUB / decisions-parked · child of `00-anchor.md` · PARTIAL
**Current home:** `handlers/sync/{pull,push,resolve}.zig` (~390 LOC) + `pl-sync`
**Determinism:** ~55% — per-link adapter dispatch + idempotent skip is
mechanical; conflict resolution has an operator-decision gate.

**What it owns:** the bidirectional sync cycle — `pull` (per-link adapter fetch +
field-change detection + conflict flagging), `push` (per-link render + POST +
`sync_event` record), and `resolve` (per-conflict: fetch both sides → present →
operator picks winner → apply).

**Why a stub.** `sync resolve` is a genuine multi-step conditional workflow with
an explicit operator gate — a strong EXTRACT shape. But:

- **D-W6.1 — per-link atomicity vs loop.** `pull`/`push` are *loops over links*
  where each link's fetch+detect+record should stay one transaction (KEEP-ZIG),
  while the *loop and the conflict-escalation branch* are the workflow. The split
  needs a per-link primitive (`planar sync pull-one --link <id>`?) analogous to
  `01`/P1's `propagate-one`. *Gate:* decide whether to mint `sync pull-one` /
  `sync push-one` — this is the same decomposition pattern as W3, so resolve it
  *consistently with `01`/P1*, not independently.
- **D-W6.2 — conflict-resolution gate.** The winner-selection is an operator
  `confirm`, not LLM judgment — extractable. But it depends on `01`/P2
  (`sync-events --json`) to *show* the conflict context. *Gate:* after `01`/P2.

**Resolution gate (stub → spec):** after `01`/P1+P2 land and the
`propagate-one`/`pull-one` decomposition pattern is settled in W3. Speccing this
before W3 would relitigate the same per-entity-primitive decision.

**Pre-declared touches (for ingest):** `src/cmd/planar/handlers/sync/pull.zig`,
`push.zig`, `resolve.zig` (read-only audit at stub stage).
