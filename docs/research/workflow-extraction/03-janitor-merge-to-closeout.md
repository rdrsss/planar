# W2 — Janitor merge-to-closeout workflow

**Status:** spec · child of `00-anchor.md` · EXTRACT · **build first** (anchor §9)
**Current home:** `agents/janitor.md` (LLM agent following a prose checklist)
**Determinism:** ~100% — zero code/judgment authoring; a pure procedure with
hard-won safety rules.
**New seam needed:** none (claim ritual complete; git/gh are encapsulated, not
Planar's domain).

This is the poster child. Today an LLM agent *interprets* a six-step procedure
with seven non-negotiable safety rules; every run risks the model skipping a
rule (the operator's memory index records exactly these failures: stranded
claims, branch-deleted-before-merge, force-closed plans). As a traced workflow
the procedure becomes code: the rules are assertions, the trace is the journal.

---

## 1. The procedure (deterministic, with trace points)

Each step emits `planar run event --kind <k>` so the run journal *is* the audit
trail. `‹trace›` marks an emission point.

```
Step 0 — Open run
  planar run start --plan <id> --arm janitor --base-sha <sha> ...   ‹trace: run open›

Step 1 — Verify delivery evidence
  gh pr view <N> --json state,mergeable,mergeStateStatus
  assert state=OPEN ∧ mergeable=MERGEABLE ∧ mergeStateStatus ∈ {CLEAN, HAS_HOOKS}
  ‹trace: kind=evidence, payload={pr, checks}›   — abort run if assertion fails

Step 2 — Merge
  gh pr merge <N> --merge --delete-branch
  poll gh pr view <N> --json state,mergedAt UNTIL state=MERGED   (Rule 1)
  ‹trace: kind=merged, payload={sha, mergedAt}›

Step 3 — Reconcile Planar state
  planar-agent reconcile --plan <id> --dry-run --json    (from W0/P3)   ‹trace: reconcile-preview›
  planar-agent reconcile --plan <id>
  if holding a claim: planar-agent complete|release --claim <token>     (Rule 6)
  ‹trace: kind=reconciled›

Step 4 — Cleanup (ORDER MATTERS — Rules 2,3,4)
  git worktree remove <path>          # BEFORE branch deletion (Rule 2)
  git branch -D <branch>              # skip if absent
  git fetch --prune origin
  git pull --ff-only origin <target>  # verify current branch first (Rule 3); leave foreign WIP (Rule 4)
  ‹trace: kind=cleanup, payload={worktree, branch}›

Step 5 — DB closeout via the gate (Rule 7)
  planar plan closeout <plan> --dry-run --json
  parse {ready, blocked_by}
  if ready: planar plan closeout <plan>           ‹trace: kind=closed›
  else: surface blocked_by; finish run status=aborted; DO NOT force-close
  ‹trace: kind=blocked, payload={blocked_by}›

Step 6 — Optional reinstall (Rule 5)
  if engine|CLI|migrations changed: ./install.sh
  ‹trace: kind=reinstall›

Step 7 — Close run
  planar run finish <run_uid> --status completed|aborted   ‹trace: run close›
```

The seven safety rules become **preconditions asserted in the workflow**, not
prose the model must remember:

1. Never delete a branch before merge confirms `MERGED`.
2. Remove the worktree before deleting the branch.
3. Verify the current branch before any destructive git op.
4. Leave foreign WIP untouched (`git status` gate before `pull --ff-only`).
5. On schema lockout, rebuild — never roll back.
6. Heartbeat live claims; `reconcile` only lapsed ones.
7. The closeout gate is authority — never force-close a `ready=false` plan.

---

## 2. Judgment callouts

**None.** This is the cleanest case in the whole initiative — no step requires an
LLM. The only non-mechanical moments are *error narration to the operator* on
abort (which the harness can template from the failing `run_event`) and the
operator gate before destructive cleanup, which is a `confirm`, not a judgment.

---

## 3. The seam it composes

| Step | Verbs (all exist) |
|------|-------------------|
| evidence/merge | `gh pr view`, `gh pr merge` (encapsulated; not Planar) |
| reconcile | `planar-agent reconcile --plan --dry-run --json` (W0/P3), `complete`/`release` |
| cleanup | `git worktree`, `git branch`, `git fetch/pull` (encapsulated) |
| closeout | `planar plan closeout --dry-run --json`, `planar plan closeout` |
| trace | `planar run start/event/finish` |

Note the only non-Planar reach is `git`/`gh`. That is correct — version control
is not Planar's domain — but it MUST live inside the workflow, never behind a
Planar verb. A binary that shells to `gh` would be a layering violation.

---

## 4. What stays in Zig

`planar plan closeout`'s **gate evaluation** (task-terminal counts, claim
liveness, finalization-task filtering, status transition) stays atomic in
`handlers/plan/closeout.zig` — the workflow *calls* the dry-run gate and trusts
its `ready` verdict; it never reimplements the gate. Likewise `reconcile`'s
sweep stays one transaction. The workflow owns the *order* and the *rules*, not
the transactions.

---

## 5. Milestones

1. **Lift the procedure verbatim** from `agents/janitor.md` into a harness-agnostic
   step list with the seven rules as assertions; wire `run event` at each point.
   *Touches:* this spec → workflow source (harness repo); `agents/janitor.md`
   (shrink to: dispatch context + the single confirm gate, procedure removed).
   *Accept:* dry-run of the workflow against a fixture merged PR emits the full
   `run_events` sequence; `run show --json` shows steps 1–7 in order.
2. **Rule-assertion tests.** One red test per rule (e.g. branch-delete attempted
   pre-merge → workflow aborts at Step 2, never reaches Step 4).
   *Accept:* each rule violation aborts with a traced `kind=blocked`/abort event.
3. **Closeout-gate integration.** `ready=false` path surfaces `blocked_by` and
   finishes the run `aborted` without a status transition.
   *Accept:* fixture plan with an open task → workflow does not close it.

**Exit:** the janitor agent's `.md` no longer carries the procedure; the
procedure is a traced workflow; every safety rule has a red test. This proves
the end-to-end pattern for the rest of the initiative.

---

## 6. Open questions

- **OQ-1.** Does `run start --arm janitor` belong in the experiment's arm
  namespace, or do operational workflows need a separate run namespace from
  benchmark runs? (Resolution gate: when `00020_runs` lands — see
  `run-record-schema.md` §2 on `arm` being CLI-enforced, not schema-enforced.)
- **OQ-2.** The operator confirm before Step 4 — always, or only when
  `git status` shows uncommitted foreign WIP? (Default: always; revisit after
  first real runs.)
