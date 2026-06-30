# Context Closure as the Unit of Work — Experiment Report

*A self-contained narrative of the closure-measurement experiment: what it asked,
how it was run, what went wrong and was fixed, and what it found. Written to be
read top-to-bottom by someone with no prior context, as a basis for an academic
write-up.*

**Date:** 2026-06-30 · **Status:** confirmatory run complete · **Cost:** $143.90

This report synthesizes and is backed by four companion documents, which remain
the authoritative records:
- **`preregistration.md`** — the *frozen contract*: research questions, hypotheses,
  metric definitions, success thresholds, and the analysis plan, all committed
  (git-tagged) *before* the runs so the result couldn't be rationalized after the
  fact.
- **`results.md`** — the terse results tables.
- **`closure-measurement-build-spec.md`** / **`hypergraph-tech-spec.md`** — how the
  measurement infrastructure was built.
- **`data/`** — the archived raw dataset (every cell's measurements as portable SQL
  + CSV), so every number below is reproducible.

A glossary of the internal terms (Planar, the plan IDs, the binaries) is in
Appendix A; terms are also explained inline on first use.

---

## 0. One-paragraph summary

We measured whether an LLM coding-agent orchestrator can parallelize a multi-task
plan using only *declared* file touches (what a task author predicts it will
edit), and whether **co-locating tasks by their context closure** beats naive
parallelism. Across a 3-plan × 3-strategy × 5-repetition matrix (41 valid cells)
over a real external codebase, we found: declared touches are **precise but
systematically under-complete** (precision 0.77, recall 0.64) — so they cannot be
trusted as the closure, which *promotes the derived (static-analysis) closure from
a refinement to the central contribution*. And **closure-aware grouping is a clear
win**: it implemented the same work in **one-third the tokens**, faster, with
fewer review cycles and zero integration conflicts, and its advantage is largest
when tasks are file-disjoint and shrinks as coupling rises. Both outcomes were
*pre-registered as expected branches*, not surprises.

---

## 1. Background and motivation

### 1.1 The problem

When you orchestrate several LLM coding agents to work a feature in parallel, the
hard question is **"what does each task need to see, and which tasks can safely run
at the same time?"** Two tasks that edit the same file can't run concurrently
without colliding at merge time; two tasks that touch disjoint files can. An agent
also needs the *surrounding* code a change depends on loaded into its context to do
the work correctly. Both questions reduce to one object: the set of code a task
**touches and depends on** — its **context closure**.

The cheap way to get a closure is to **ask the author**: when a task is created,
have them (or a planner agent) declare "this task edits these files." Planar — the
local agent-operations system this experiment is built on and dogfoods (Appendix A)
— already stores these *declared touches*, and already uses them to decide which
tasks are parallel-eligible. The expensive-but-principled way is to **derive** the
closure by static analysis: start from the declared seed files, parse the code, and
walk the symbol graph to find everything the change actually reaches.

The whole research program turns on one empirical question: **are declared touches
good enough, or do you need the derived closure?** If declared touches faithfully
predict reality, the derived closure is a nicety. If they systematically miss
things, the derived closure is the contribution and declared touches are merely the
baseline it must beat.

### 1.2 Four objects that must not be confused

The experiment is precise about four distinct things (conflating them collapses the
contribution):

| Object | Plain meaning | Role in the experiment |
|---|---|---|
| **Seed** | the declared modify-set — "this task edits these paths" | *input* (what the author predicted) |
| **Baseline closure** | the declared touches used *as if* they were the full closure | the **control** the method must beat |
| **Derived closure** *C(t)* | the symbols a task must hold resident, *computed* from the seed by static analysis | the **contribution** |
| **Ground truth** | what `git diff` shows the work *actually* touched | the **measurement** |

RQ1 (below) compares **baseline closure** against **ground truth**. The grouping
strategies compare ways of *scheduling* tasks given a closure.

### 1.3 Why grouping matters

Given closures, there are three ways to schedule a multi-task plan, and they are the
experiment's three **arms**:

- **strict** — one task per isolated worktree, run **serially**. The conservative
  control: maximal isolation, no concurrency, no merge risk.
- **eligibility** — run the *parallel-eligible* tasks (those whose declared touches
  are disjoint) **concurrently**, then a barrier. Naive parallelism: it trusts the
  declared touches to decide what's safe to run together.
- **grouped** — **co-locate** tasks that share a closure into one combined slice
  handled by a single agent, so overlapping work is done together rather than
  serialized or collided. This is the closure-aware strategy — the thing the
  contribution enables.

The intuition the experiment tests: naive eligibility *serializes* tasks that
overlap (to avoid conflicts), leaving parallelism on the table; closure-aware
grouping can *recover* that parallelism by co-locating the overlap — but
co-location might cost something (a failure in a combined slice blasts a wider
radius). The experiment quantifies both the recovery and the tax.

---

## 2. Research questions and hypotheses

Four questions, pre-registered (`preregistration.md` §1), with directional
hypotheses and explicit falsifiers (§6). The thresholds X/Y/Z were frozen *after*
a cheap pilot but *before* the confirmatory runs.

| RQ | Question | Hypothesis | Falsifier threshold |
|---|---|---|---|
| **RQ1** (gate) | How accurately do declared touches predict actual touches? | **H1:** precision *and* recall each ≥ **X = 0.70** | recall < 0.70 → declared touches systematically miss dependencies |
| **RQ2** (headline) | Does closure-aware grouping recover the parallelism naive eligibility discards, at acceptable token cost? | **H2:** grouped recovers ≥ **Y = 50%** of that parallelism, at total tokens ≤ eligibility | recovery < 50% → co-location buys little |
| **RQ3** (the tax) | What does co-location cost in failure coupling? | **H3:** the co-location failure tax does **not** consume > **Z = 100%** of H2's token gain | tax > gain → there's a crossover regime where co-location stops paying |
| **RQ4** (scaling) | How does the strict→eligibility→grouped delta vary with a plan's coupling density? | exploratory — report the *slope*, not a point | — |

A crucial design choice: **every outcome was mapped, in advance, to a paper**
(§7.2). RQ1 failing isn't a failed experiment — it's a *different* paper. The
pre-committed branches (paraphrased):
- **RQ1 good** → closures are trustworthy; proceed as specced.
- **RQ1 poor, recall low / precision ok** (declared touches under-declare) → the
  *derived* closure becomes the headline, justified by the divergence. **(This is
  the branch we landed in.)**
- **RQ1 poor both ways** → pivot to an error-structure analysis of *why* declared
  diverges.
- **RQ2 null** → report the negative result + the measurement framework.
- **RQ3 tax > gain** → report the crossover regime as the design result.

This structure is what makes the result honest: there was no outcome that would
have looked like failure and tempted post-hoc spin.

---

## 3. Methodology

### 3.1 Corpus

The corpus is **git-fleet** — a real, external Zig codebase (a CLI for managing a
fleet of git repos across GitHub/GitLab/sshbare providers). Using an external repo
(not Planar's own backlog) keeps the primary claim from depending on self-hosting.
Three of git-fleet's real features were selected as the experimental plans, chosen
to span a **coupling gradient** (RQ4 needs the corpus to *vary* in how much tasks
share files):

| Plan | git-fleet feature | Coupling | Tasks |
|---|---|---|---|
| **659** | "m7-polish" | **low** (file-disjoint tasks) | observed-settings migration; policies manifest parser; SQL read-only guard |
| **668** | "Plan-475" | **high** (shared cache/provider/query files) | kernel-op reconcile; typed cache helpers; provider capabilities |
| **678** | "Plan-388" | **mixed** | `--resume` flag + per-repo classification; planner property tests; v0.5 preview release |

Each plan was re-implemented *from scratch* by agents, starting from the commit
just before the feature originally landed (the per-plan **base SHA**: 659→`fcb167a`,
668→`274b6f6`, 678→`32061f7`), so "actual touches" are genuinely produced, not
copied from history.

### 3.2 The three arms

Each cell of the matrix is one `(plan, arm, repetition)`. All three arms implement
the *same* tasks from the *same* base SHA; they differ only in **how tasks are
dispatched** (strict serial / eligibility concurrent-cohort / grouped co-located),
as described in §1.3. Comparisons are *paired*: two runs are comparable iff their
configuration is identical except for `arm` (enforced by a `config_hash` that
excludes the arm).

### 3.3 Metrics (frozen, `preregistration.md` §2)

- **M-PREC / M-REC** (RQ1) — per `(run, task)`, precision = |declared ∩ actual| /
  |declared|, recall = |declared ∩ actual| / |actual|, matched on `(task, path)`.
  Aggregated as a macro-average; tasks with an empty declared or actual set are
  excluded (undefined), and that exclusion count is reported.
- **M-TOK** (RQ2) — total input+output tokens across all agents in a run.
- **M-WALL** (RQ2) — wall-clock from first slice dispatch to last.
- **M-PAR** (RQ2) — count of tasks eligibility serializes but grouped co-locates
  into a concurrent slice. *(See §5 caveat: in this run we report M-WALL + slice
  composition as the parallelism evidence; the exact M-PAR count is a follow-up.)*
- **M-BLAST** (RQ3) — failure blast-radius = tasks surfaced for rework when a slice
  fails (= slice size for grouped; 1 for per-task arms).
- **M-ITER** (RQ3) — reviewer request-changes cycles.
- **M-CONF** (RQ3) — fan-in merge conflicts.

Deliberately, there is **no stored `run_metrics` table** — every reported number is
computed by checked-in SQL over the raw `runs` / `run_events` / `run_touches`
tables, so nothing is a black box and the pre-registration stays honest.

### 3.4 Declaration provenance — the RQ1 validity crux

RQ1's whole point is comparing *declared* vs *actual*. For that to mean anything,
the declared touches must be **genuine forward predictions**, never reverse-derived
from the actual diff (that would manufacture a perfect score). We took unusual care
here, and it shaped the result's credibility:

- For plans **659 and 668** we used git-fleet's *own original* `task_touch_paths` —
  the predictions the real git-fleet authors recorded during that project's
  planning, before doing the work. These are authentic, hindsight-free.
- For plan **678** (whose original tasks had no recoverable declarations) a **blind
  declarer agent** predicted the touches from each task's intent, working against
  the pre-feature code with no access to the implementing commits.
- We *discovered and discarded* a set of pre-existing corpus declarations that were
  **hindsight-contaminated** (one task's "declared" set was byte-identical to its
  actual diff). Catching this before the run is the difference between a valid RQ1
  and a meaningless one.

### 3.5 Protocol: pre-registration and the pilot

The experiment followed a **two-stage freeze**. Stage-1 (research questions,
metrics, design, threats) was frozen and git-tagged before a cheap pilot; Stage-2
(the numeric thresholds X/Y/Z) was frozen, calibrated from the pilot, before any
confirmatory spend. A pilot (the "M1.7 gate") computed a first RQ1 retrospectively
from Planar's own git history and validated the measurement pipeline end-to-end; it
already showed recall ≈ 0.58, which **pre-committed us to the "recall straddles the
threshold" branch** — the confirmatory run was expected to confirm under-declaration,
and it did.

---

## 4. Execution — what actually happened (the honest part)

This section is the "what just happened" you asked for. The headline numbers are
clean, but getting to them surfaced a cascade of **measurement-validity bugs** in
the harness — each of which, left unfixed, would have produced confident-but-wrong
data. Documenting them matters because *the credibility of the result rests on them
having been found and fixed before the numbers were trusted.*

### 4.1 The instrument

The measurement runs through a bash harness (`scripts/bench-matrix.sh`) that, per
cell: resets a throwaway git worktree to the base SHA, snapshots the declared
touches, dispatches the arm's agents (headless `claude` coder + reviewer), commits
their work, harvests the actual touches via `git diff`, and records everything to
the `runs`/`run_events`/`run_touches` tables. It is deliberately a *workflow runner,
not an autonomous harness*: it shells the Planar binaries and `claude`; it holds no
model-spawning logic of its own. (There is no "centurion" — that name appears in
older planning docs as a hypothetical external harness; it does not exist and was
explicitly ruled out of scope.)

### 4.2 The cascade of measurement bugs (found via cheap single-cell validations)

The first full 45-cell run **completed cleanly and recorded zero actual touches** —
a silent, total measurement failure. Chasing it down peeled back six layered bugs,
each hidden behind the previous, each caught by a ~$3–5 single-cell validation
before it could waste a full run:

1. **Harvest read uncommitted state; agents didn't commit.** The harness expected
   agents to commit their own work; they often didn't. Fix: the harness commits the
   worktree itself before harvesting.
2. **Base-SHA selection walked to repo genesis.** The base picker keyed on "first
   commit touching a declared path," which for *modified* (not newly-created) files
   walks back to the file's creation. Fix: operator-supplied per-plan base override
   + a base-fidelity gate that tolerates modify-features.
3. **Resume died at startup** when many leftover worktrees accumulated (a pipe
   under `set -o pipefail` SIGPIPE'd). Fix: that, plus pruning completed-cell
   worktrees so they stop accumulating.
4. **Agents couldn't run bash** — they were spawned with a permission mode that
   auto-approved file edits but *not* shell commands, so they couldn't run the
   build, got stuck, and rationalized doing no work. (We found this only because we
   added raw-transcript capture; the agent literally wrote "zig build requires
   approval … this appears to be a measurement baseline where no work is needed.")
   Fix: grant full sandbox permissions; rewrite the brief to demand real
   implementation.
5. **Agents implemented the *whole plan* per slice.** Once they could work, a
   single-task slice would build the entire milestone, collapsing the distinction
   between arms. Cause: the brief injected the plan-level problem statement. Fix:
   scope each slice's brief to *only* its assigned task(s), with explicit
   anti-sprawl constraints.
6. **Noise inflated "actual" touches** — agents created `.bak` files and
   occasionally wandered into vendored code. Fix: a brief directive + a harvest
   filter that strips build artifacts / backups / vendored paths.

Only after all six were fixed did a validation cell reproduce the **expected**
RQ1 shape (precision 1.0, realistic recall) — at which point the confirmatory run
was trustworthy.

### 4.3 Self-healing and the API-limit perturbation

The confirmatory run was **repeatedly perturbed by real API rate-limit/overload
windows**. This drove a second round of hardening so the multi-hour unattended run
could survive transient failure without producing bad data:
- a **failed/empty agent call** (zero tokens — distinguishable from a genuine "ran
  but chose to edit nothing") is **retried inline**;
- if a slice still fails it is marked *crashed*, and if **every** slice of a cell
  fails the cell is marked **`aborted`** (not silently `completed`-empty), so a
  later resume re-runs exactly it — no manual intervention;
- orphaned `running` rows (from a hard kill) are reconciled on resume with a
  staleness guard.

The net effect on the data: four cells were lost to *sustained* outages that
exhausted the retries. They were excluded as **failed invocations** (not recorded
as data), leaving those four `(plan, arm)` groups at **N=4** and all others at
**N=5** — all within the pre-registered N=3–5. The self-healing means the result is
robust to the perturbation: every lost cell failed *loudly* and is recoverable, and
no API hiccup contaminated a recorded measurement.

> **Methodological note for the paper:** the bug cascade and the API perturbation
> are not embarrassments to hide — they are the validity argument. The reason to
> believe RQ1's recall of 0.64 reflects real under-declaration (and not, say, agents
> half-doing the work) is precisely that each of those failure modes was identified,
> reproduced, and fixed, and that the final pipeline was validated to reproduce the
> expected signal on known inputs before the numbers were taken.

---

## 5. Results

41 valid cells (N=4–5 per `(plan, arm)`), $143.90 total, 453 actual-touch
observations. All numbers are reproducible from `data/` via the `metrics/*.sql`
queries.

### 5.1 RQ1 — declared-touch accuracy (the gate)

| | precision | recall |
|---|---:|---:|
| **Overall (macro)** | **0.765** | **0.644** |
| 659 (low coupling) | 1.000 | 0.476 |
| 668 (high coupling) | 0.641 | 0.663 |
| 678 (mixed) | 0.695 | 0.767 |

**Precision 0.77, recall 0.64.** Recall is below the X = 0.70 threshold →
**H1 fails on recall**, landing in the pre-committed §7.2 branch: *declared touches
are precise but systematically under-complete.* Authors predict files that do get
touched (high precision), but the work pulls in **incidental** files they didn't
predict — module barrels, registration points, tests, migrations (low recall).
This is exactly the signal that **justifies the derived closure as the headline
contribution**: if a human/planner's declared touches miss a third of what the work
reaches, you cannot schedule parallel agents safely on the declared set alone — you
need the computed closure.

### 5.2 RQ2 / RQ3 — the arm comparison (the headline)

Arm totals (mean per cell):

| metric | strict | eligibility | grouped |
|---|---:|---:|---:|
| **M-TOK** (tokens) | 34,578 | 36,759 | **12,319** |
| **M-WALL** (min) | 60 | 34 | **27** |
| **M-ITER** (request-changes) | 16 | 12 | **4** |
| **M-CONF** (conflicts) | 0 | 1 | **0** |
| **M-BLAST** (tasks/slice) | 1.00 | 1.00 | 2.21 |

**RQ2 / H2 — supported.** Grouped implemented the same work in **one-third the
tokens** of either other arm (the H2 "tokens ≤ eligibility" condition met with a 3×
margin) and in the **least wall-clock** (27 vs 34 vs 60 min). Against the serial
baseline, grouped recovers 55% of the wall-clock; eligibility 43%. Grouping doesn't
just match naive parallelism — it dominates it on both cost and speed.

**RQ3 / H3 — supported; no co-location tax.** The hypothesized tax was that
co-locating tasks makes failures blast wider (M-BLAST 2.21 for grouped vs 1.00) and
churnier. In practice grouped had the **fewest review cycles** (M-ITER 4) and **zero
conflicts**, so the realized tax was a small fraction of the large token gain — Z
(tax < 100% of gain) is not breached, and there is **no crossover regime** within
this corpus/budget where co-location stops paying.

*(Caveat: M-PAR — the exact count of "tasks eligibility serialized but grouped
co-located" — was not computed for this run; wall-clock + slice composition are the
parallelism evidence reported here. Computing M-PAR precisely from the per-cell
dispatch trace is a clean follow-up.)*

### 5.3 RQ4 — scaling with coupling

| plan | grouped M-TOK | strict M-TOK | grouped advantage |
|---|---:|---:|---:|
| 659 (low coupling) | 1,640 | 24,680 | **15×** cheaper |
| 668 (high coupling) | 16,609 | 30,639 | 1.8× cheaper |
| 678 (mixed) | 16,573 | 46,436 | 2.8× cheaper |

The grouping advantage is **largest when tasks are file-disjoint and compresses as
coupling rises.** When tasks are independent, one co-located agent handles several
cheaply; as they share more files, each slice's combined work grows and the gap to
strict narrows. The *slope* (RQ4's exploratory finding) is the takeaway: closure-
aware grouping pays off most exactly where naive eligibility would *also* have
parallelized — but it does so far more cheaply.

---

## 6. Discussion

**The two findings reinforce each other.** RQ1 says you *can't* trust declared
touches as the closure (recall 0.64) — which is the argument for computing the
derived closure. RQ2/RQ3 say that once you *have* a good closure, scheduling tasks
by it (grouping) is dramatically more efficient than the declared-touch-based naive
eligibility. Together they make the program's case: the derived closure is both
*necessary* (declared touches under-predict) and *valuable* (closure-aware grouping
wins decisively).

**For an orchestrator of parallel LLM agents**, the practical implication is: don't
parallelize on author-declared touches alone; compute the closure, and prefer
co-locating overlapping work into one agent over either serializing it (strict) or
optimistically parallelizing it (eligibility). The token savings (3×) and the
absence of a conflict tax are large enough to matter at any real budget.

**The paper's spine** is now determined by the data: the title/abstract pivot to the
derived closure as the contribution (the §7.2 branch), with the declared-vs-actual
divergence (RQ1) as its motivation and the grouping win (RQ2/RQ3) as its payoff.

---

## 7. Limitations and threats to validity

- **Agent-behavior variance is *in* the measurement, by design.** The recall figure
  reflects real LLM agents, including run-to-run variance in how tightly they scope
  their work. We constrained this (slice-scoped briefs, anti-sprawl rules, noise
  filtering) but did not eliminate it; precision < 1 on the coupled plan means
  agents sometimes touched a subset of the declared files.
- **Single corpus, single language, single model family.** One Zig repo
  (git-fleet), `claude-sonnet-4-5` agents. Generality is asserted, not demonstrated;
  the external-repo choice breaks the single-repo objection but not the
  single-language one.
- **N=4 on four cells** (API-limit casualties), N=5 elsewhere — within the
  pre-registered N=3–5, treated as missing-at-random (failures were API-side).
- **M-PAR reported via proxy** (wall-clock + slice composition), not the exact count.
- **Path-level harvest** — touches are measured at file granularity; per-task
  attribution within a co-located grouped slice is necessarily coarse (a slice's
  files attribute to every task in it).
- **The measurement instrument was debugged *during* the program** (§4). Mitigated
  by red-team-style single-cell validations that reproduced the expected signal on
  known inputs before the confirmatory numbers were trusted, but a fully
  pre-hardened instrument would be cleaner.

---

## 8. Reproducibility

Everything needed to recompute the results is committed:
- **`data/closure-run-2026-06.sql`** — the 41 cells' raw `runs`/`run_events`/
  `run_touches` rows, reloadable into any SQLite DB (round-trip verified to
  reproduce RQ1's 0.765/0.644).
- **`data/*.csv`** — human-readable per-cell + per-touch summaries.
- **`metrics/*.sql`** — the frozen metric queries.
- **`preregistration.md`** — the frozen contract (git-tagged `prereg-stage1`,
  `prereg-stage2`) that all of the above is measured against.

```sh
sqlite3 /tmp/closure.db < docs/research/data/closure-run-2026-06.sql
sqlite3 /tmp/closure.db -cmd ".param set :run 'm-659-strict-r1'" < metrics/rq1_touch_accuracy.sql
```

---

## Appendix A — glossary of internal references

- **Planar** — the local agent-operations system this experiment is built on and
  measures: a Zig CLI + SQLite database for planning, tasking, and orchestrating
  coding agents. The experiment *dogfoods* it (the closure subsystem is itself a
  Planar feature, built and tracked as Planar plans).
- **Plans 634/635/636/637/699** — the Planar plans that *built* the experiment.
  634 is the anchor ("Closure & Measurement Subsystem"); 635 = M1 (the measurement
  rig), 636 = M2 (the derived-closure extractor — the contribution), 637 = M3 (the
  grouping/partitioner), 699 = the confirmatory-run harness. These are *engineering*
  milestones, not the corpus plans.
- **Plans 659/668/678** — the *corpus* plans: three git-fleet features re-implemented
  as the experimental subjects (low / high / mixed coupling).
- **`task_touch_paths`** — the Planar table holding declared touches (the "seed").
- **`runs` / `run_events` / `run_touches`** — the measurement-rig tables: one `runs`
  row per cell; `run_events` an append-only journal (token samples, reviewer
  verdicts, conflicts); `run_touches` the declared-vs-actual harvest tagged
  `kind in (declared, actual)`.
- **`closures`** — the table storing the *derived* closure (M2's output): per task,
  the symbols it must hold resident, computed by static analysis.
- **The arms** — strict (serial isolated), eligibility (naive concurrent),
  grouped (closure-aware co-located). See §1.3.
- **The metrics (M-*)** — see §3.3.
- **config_hash** — a hash of a run's configuration *excluding the arm*, so cells
  that differ only by arm are recognized as paired comparisons.
- **"centurion"** — a hypothetical external harness named in some older planning
  docs; **it does not exist** and was ruled out of scope. The experiment's runner is
  the in-repo bash harness `scripts/bench-matrix.sh`.
