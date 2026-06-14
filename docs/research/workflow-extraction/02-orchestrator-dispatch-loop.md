# W1 — Orchestrator dispatch loop workflow

**Status:** spec · child of `00-anchor.md` · EXTRACT
**Current home:** `agents/orchestrator.md` (LLM agent running the execution loop)
**Determinism:** ~95% — a state machine wrapping two judgment callouts (coder,
reviewer) that are *already* separate sub-agents.
**New seam needed:** none — the claim ritual is complete (verified in the seam
inventory: `pull`/`peek`/`heartbeat`/`complete`/`fail`/`release`/`block`,
`plan next --json`, `claim-associate`, `run`/`context` all exist).

The execution phase is a six-state loop the orchestrator agent currently runs by
interpreting prose. Every operational bug in the operator's memory index —
stranded claims, lapsed heartbeats, split terminal verbs, gate misreports — is a
determinism failure of an LLM hand-running this protocol. Encode the loop; keep
the judgment as callouts.

---

## 1. The loop (deterministic, with trace points)

```
Step 0 — Open run, snapshot declared touches
  planar run start --plan <id> --arm <strict|eligibility|grouped> --base-sha <sha> ...
  ‹trace: run open›  (snapshots task_touch_paths → run_touches kind=declared)

LOOP until plan has no available task OR iteration cap hit:
  Step 1 — Select work (claim-aware)
    planar plan next <plan> --json        # buckets: available/claimed/stale/blocked
    planar-agent peek <plan> --json       # dry-run what pull would take
    ‹trace: kind=select, payload={task, bucket}›

  Step 2 — Dispatch-shape gate (operator confirm, or pre-committed flag)
    propose one of {strict, grouped, single, barrel}; planar plan recommend-strategy --json
    ‹trace: kind=dispatch-shape›

  Step 3 — Claim + dispatch coder  (CALLOUT)
    planar-agent pull <plan> --parent-action <orch-action> --run <id> --stage exec --json
    → fork coder sub-agent (judgment); coder heartbeats, returns; ORCH owns terminal verb
    heartbeat --claim <token> --status awaiting:coder   at TTL/2 cadence
    ‹trace: kind=coder-return, payload={gate-output}›

  Step 3.5 — Optional test-coder  (CALLOUT, gated)
    oracle: planar test-spec status --json → uncovered_task_slugs ∩ this task?
    if uncovered: fork test-coder; ‹trace: kind=test-coder-return›

  Step 4 — Dispatch reviewer  (CALLOUT)
    compose brief: task ids, spec paths, claim token, coder GATE OUTPUT (not narrative)
    → fork reviewer sub-agent (judgment); returns one of approve|request-changes|open-question|abort
    ‹trace: kind=review, payload={decision, iteration}›

  Step 5 — Route decision → terminal verb (THE atomic step)
    approve         → planar-agent complete --claim <token>
    request-changes → loop back to Step 3 (iteration++) UNLESS iteration == cap
    open-question   → planar-agent block --claim <token> --blocker <q>
    abort           → planar-agent fail --claim <token> --reason ...
    iteration == cap (5): reject request-changes; force approve|abort   (cap rule)
    ‹trace: kind=terminal, payload={verb, decision}›
    planar capture note "<dispatch-shape sentinel>"

Step 6 — Close run
  planar run finish <run_uid> --status completed|aborted   ‹trace: run close›
```

The claim ritual — `pull → heartbeat@TTL/2 → exactly one of
complete|fail|release|block` — is encoded **once, here**, as the loop body. The
orchestrator owns the terminal verb (per the four-binary doctrine); coders only
heartbeat and return. This single encoding retires the entire stranded-claim /
split-terminal-verb bug family.

---

## 2. Judgment callouts (the 5% that stays LLM)

| Callout | Sub-agent | Why it stays | What the loop hands it |
|---------|-----------|--------------|------------------------|
| Implement task | `coder` | code authoring | task id, spec context, claim token, worktree |
| Cover scenarios | `test-coder` | adversarial test authoring | test-spec, coder diff, claim token |
| Review diff | `reviewer` | adversarial judgment | task ids, spec paths, claim token, **gate output** |
| Brief composition | (harness templates) | summarization only | pointers, not synthesis |
| Escalation prose | (harness templates) | operator messaging | failing `run_event` |

Dispatch-shape *recommendation* is heuristic (`recommend-strategy`), not LLM —
the loop calls the verb and presents its output behind the confirm gate.

---

## 3. What stays in Zig

`planar plan next` (claim-aware bucketing), `planar-agent pull/peek` (atomic
claim + status flip), `planar plan recommend-strategy` (parallelizability
heuristic), and the terminal verbs (`complete`/`fail`/`release`/`block` — each
flips claim status *and* `tasks.status` in one transaction). The loop never
splits a terminal verb into `task done` + `release`; it calls the atomic verb.

---

## 4. Relationship to centurion / planar-execute

This *is* the loop the excised `planar-execute` ran and that `centurion` runs
downstream. This child does not re-home the harness — it pins the **contract**:
the exact verb sequence, the cap rule, the heartbeat cadence, the trace points.
Whatever harness runs it (centurion today) implements this contract; the contract
lives here and in the integration tests, not in agent prose.

---

## 5. Milestones

1. **Pin the loop contract** as a harness-agnostic step list with the cap rule,
   heartbeat cadence, and trace points; reconcile against `centurion`'s current
   implementation (note any divergence as a decision).
   *Touches:* this spec → workflow source; `agents/orchestrator.md` (shrink the
   execution-phase prose to a pointer at this contract + the confirm gates).
   *Accept:* a dry-run drives select→dispatch→review→terminal for a one-task
   fixture; `run show --json` shows the cycle with iteration counter.
2. **Cap-rule + heartbeat tests.** Red tests: iteration-6 `request-changes`
   rejected; a coder that returns without heartbeating past TTL/2 surfaces a
   lapsed-claim trace, not a silent stall.
   *Accept:* both assertions hold.
3. **Terminal-atomicity test.** Assert no path emits `task done` + `release`
   separately; only the atomic terminal verbs appear in the trace.
   *Accept:* trace contains exactly one `kind=terminal` per cycle.

**Exit:** the orchestrator agent carries only the judgment callouts and confirm
gates; the loop contract is pinned and tested; the claim-ritual bug family is
structurally impossible.

---

## 6. Open questions

- **OQ-1.** Test-coder gate (Step 3.5) — keep inside this loop, or factor into a
  reusable sub-workflow shared with non-orchestrated dispatch? (Gate: after `03`
  proves the sub-workflow composition pattern.)
- **OQ-2.** Should `arm` be set by the orchestrator (operational) or only by the
  benchmark harness? Operational runs may want `arm=op` to keep experiment cells
  clean. (Gate: `00020_runs` landing; coordinate with `03` OQ-1.)
