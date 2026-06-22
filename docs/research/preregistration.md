# Pre-Registration & Experiment Protocol (v0.1)

**Status: FREEZE-ONCE.** This document is the immutable experimental contract.
Once Stage-1 freeze is recorded (see below), the research questions, metric
definitions, success criterion, design, and analysis plan **do not change**.
Amendments are append-only and dated; nothing is edited in place. If a change
is substantive, it is a new protocol version, not an edit.

**Why standalone.** The build spec is a *living* document (milestones
complete, tasks change). Pre-registration's protective value depends on being
frozen *before* runs. Co-locating a frozen artifact inside a living one
defeats the freeze. The build spec points *to* this file; it does not contain it.

**Parents:** `planar-spec-v0.1.md` (formal mandate), `closure-measurement-
build-spec.md` (implementation). **Capture mechanics:** `run-record-schema.md`
(the tables) — that doc may evolve; the metric *definitions* below may not.

**Suggested path:** `docs/research/preregistration.md`

---

## 0. Freeze protocol (two stages)

- **Stage-1 freeze — before the pilot (M1.7).** Freezes §§1–5 and §7–8: the
  research questions, metric definitions, success criterion, experimental
  design, analysis plan, and threats. Record by committing this file with a
  dated line below and tagging the commit.
- **Stage-2 freeze — after the pilot, before confirmatory runs (M3.4).**
  Freezes §6: the quantitative falsifier thresholds (X/Y/Z), which are
  calibrated from the pilot's observed spread. The pilot is explicitly *not*
  confirmatory; it exists to set thresholds and validate the pipeline.

```
Stage-1 frozen: 2026-06-15           commit/tag: prereg-stage1
Stage-2 frozen: 2026-06-21           commit/tag: prereg-stage2
Amendments (append-only):
  - 2026-06-15: corpus repo named `git-fleet` (was placeholder codename
    `arbustum`); naming reconciled pre-freeze, no change to definitions/design.
  - 2026-06-21: Stage-2 thresholds set (§6 X=0.70, Y=50%, Z = H3 tax must not
    consume >100% of the H2 token gain). Calibrated from a SMALL sample — the
    M1.7 pilot (N=1) + an in-session pilot expansion (m7-polish N=2: grouped
    28–48% fewer tokens than eligibility) + one coupled-plan run (Plan-475:
    grouped recovers the parallelism eligibility serializes). H3's Z is set
    CONSERVATIVELY because no reviewer-inclusive run has been done yet (M-ITER /
    M-BLAST unmeasured); first confirmatory reps that include reviewers may
    refine Z via an explicit, dated amendment. Thresholds frozen now to unblock
    confirmatory runs per the operator's decision.
  - 2026-06-22: Confirmatory run parameters committed. Spend ceiling = $300
    (§9); corpus = plans 659/668/678 × 3 arms × N=5 = 45 cells. Declaration
    provenance reset: gold git-fleet originals (659/668) + blind declarer (678,
    agent-predicted); hindsight-contaminated pre-existing declarations replaced
    (see §8). Per-plan bases pinned (659=fcb167a, 668=274b6f6, 678=32061f7);
    base fidelity operator-asserted for modify-features; 659/668 milestone-span
    noted exploratory (§8). Harness gains a per-plan base override + softened
    base-existence gate to support modify-feature corpora.
```

Anything discovered after a freeze is **exploratory** (§8), labeled as such,
and is a hypothesis for the next experiment — never a primary claim in this one.

---

## 1. Research questions

- **RQ1 — Gate (descriptive).** How accurately do declared touches predict
  actual touches? Path-level precision/recall of declared closure vs. `git
  diff` ground truth. *No arms.* This is the go/no-go for the whole program.
- **RQ2 — Headline (comparative).** Does closure-aware grouping recover
  parallelism that naive eligibility discards, and at what throughput cost?
  eligibility vs. grouped on parallelism-recovered, wall-clock, total tokens.
- **RQ3 — The tax (comparative).** What does co-location cost in failure
  coupling? failure blast-radius, reviewer iterations, conflict rate.
- **RQ4 — Scaling (exploratory).** How does the strict→eligibility→grouped
  delta vary with the coupling density of the plan?

---

## 2. Frozen metric definitions

Definitions are frozen because ambiguity here is where results get
unconsciously shaped. Every primary metric is computed by a named query in
`metrics/` (§7 maps query→RQ); no metric is stored in a table.

**M-PREC / M-REC (touch prediction, RQ1).** Per `(run, task)`, over
**path-level** sets:
- precision = |declared ∩ actual| / |declared|
- recall = |declared ∩ actual| / |actual|
- `declared` = the `run_touches kind='declared'` snapshot at run start;
  `actual` = `kind='actual'` from fan-in `git diff --name-only`.
- **Edge cases (frozen):** a task with |declared| = 0 has undefined precision
  (empty-touch = "touches-everything" in `strategy.zig`); it is **excluded**
  from precision and its count reported separately as *undeclared*. A task with
  |actual| = 0 (touched nothing) has undefined recall; **excluded**, count
  reported. Aggregate = macro-average across included tasks; the full
  per-task distribution is reported, not only the mean.

**M-WALL (wall-clock per plan).** Seconds from first slice dispatch to last
slice fan-in, per run. Compared as paired per-plan medians across arms.

**M-TOK (total tokens per plan).** Sum of input + output tokens across **all**
agents in the run, **including reviewer agents** (reviewer cost is part of an
arm's cost). Primary figure is raw billed tokens; cache-adjusted tokens are a
**secondary** figure, reported alongside, never substituted.

**M-CONF (merge conflicts at fan-in).** Count of slices that produce a git
merge conflict against the integration branch at fan-in, per run.

**M-PAR (parallelism recovered, RQ2).** Count of tasks that the eligibility arm
serializes (excluded from its parallel set) but the grouped arm places into a
concurrently-executable slice, per plan; also reported as a fraction of total
open tasks. Measured from `recommend-strategy` vs. `groups recommend` output on
the identical plan + base SHA.

**M-BLAST (failure blast-radius, RQ3).** When a slice fails, the number of
tasks surfaced for rework (= slice size for the grouped arm; 1 for per-task
dispatch). Reported as a distribution over failed slices.

**M-ITER (reviewer iterations, RQ3).** Request-changes cycles to reach the
objective gate, per slice. A measured variable — never a success definition.

---

## 3. Success criterion (arm-independent — frozen)

A cycle is **done** iff an identical objective gate passes across all arms:
test suite green **and** `zig build` clean **and** `zig fmt` clean. Reviewer-
agent approval is **measured** (M-ITER, request-changes rate), never the
definition of done. Rationale: the reviewer is an LLM whose judgment may vary
with diff size, and grouped arms produce larger diffs; defining success by
reviewer approval would contaminate the dependent variable and invite the
"grouped arm was just rubber-stamped" objection.

---

## 4. Experimental design (frozen)

- **Factor:** decomposition strategy, one factor, three levels {strict,
  eligibility, grouped}. No other factors. Reviewer cadence and model tier are
  *separate experiments*, not extra arms.
- **Unit:** one run = (plan, arm, repetition).
- **Blocking:** within-plan. Each plan runs under all arms; the plan is its own
  control; comparison is **paired per plan**, not pooled.
- **Nuisance handling:** model tier, reviewer cadence, budgets, brief-template
  version held constant (frozen by `config_hash`); plan identity blocked; model
  drift over calendar time **randomized by interleaving arms** (not running all
  of one arm then the next).
- **Repetition:** N = 3–5 per cell. Report medians + spread; never a single run.

---

## 5. Statistical analysis (frozen)

Small N forbids NHST theater. The analysis is:
- **Per-plan paired deltas** (grouped − eligibility, eligibility − strict) on
  each primary metric.
- **Sign test across plans** (binomial) for direction: e.g. "grouped beat
  eligibility on M-TOK in k of n plans." This is the primary inferential claim
  shape.
- **Effect sizes** (median ratio, e.g. tokens grouped/eligibility) with
  bootstrap CIs where N within a cell permits; reported in preference to
  p-values.
- **No pooling across plans** for primary claims; pooled views are exploratory.

---

## 6. Hypotheses & falsifiers (Stage-2 freeze — thresholds from pilot)

Directional hypotheses with explicit nulls and falsifiers. X/Y/Z are filled
from the pilot's observed spread and frozen at Stage-2, before confirmatory runs.

- **H1 (RQ1).** Declared-touch precision and recall are each ≥ **X = 0.70**.
  *Null:* declared touches are no better than a path-frequency baseline.
  *Falsifier / pivot:* if recall < **X (0.70)** (declared systematically misses
  real dependencies) → triggers the RQ1-poor branch (§7), and the derived-closure
  extractor (M2) becomes the central contribution rather than a refinement.
  *Calibration note:* observed precision 0.89–1.0 (clears X); observed recall
  spans 0.58 (M1.7 pilot, plan 635) – 0.67–1.0 (m7-polish live), so recall
  straddles X — the low end already places the program in the §7.2 "recall low /
  precision ok" branch, i.e. M2 as headline. This is the expected, pre-committed
  outcome, not a surprise.
- **H2 (RQ2).** Grouped recovers ≥ **Y% = 50%** of the parallelism that
  eligibility serializes, at total tokens ≤ eligibility.
  *Null:* grouped = eligibility on M-PAR and M-TOK.
  *Falsifier:* M-PAR recovery < **Y% (50%)** → co-location buys little; report the
  null and pivot the contribution toward the measurement framework + RQ1/RQ3.
  *Calibration note:* the token condition (tokens ≤ eligibility) is already met
  with margin in the in-session data (grouped 28–48% fewer tokens, N=2 m7-polish);
  on the coupled plan (Plan-475) grouped formed 2 concurrently-executable slices
  where eligibility serialized all 3 (overlap), demonstrating the recovery
  mechanism Y bounds.
- **H3 (RQ3).** Co-location's failure tax (M-BLAST × failure rate, M-ITER, M-CONF)
  does not erase H2's throughput gain at budget B — concretely, **Z:** the
  combined co-location overhead (extra reviewer iterations + fan-in conflict
  rework) must not consume **> 100%** of H2's measured token gain.
  *Falsifier:* if the tax exceeds the gain (Z breached) → the finding is the
  **crossover point** (the budget/coupling regime where co-location stops paying),
  itself a design result.
  *Calibration note:* Z is set conservatively (a full-erasure ceiling) because no
  reviewer-inclusive run exists yet — M-ITER and M-BLAST are unmeasured. The first
  confirmatory reps that run reviewers will provide the data to tighten Z via a
  dated append-only amendment (§0).

No result here is a failure of the program; each branch is a paper. What would
be a failure is *spinning* a result post hoc — which the pre-committed branches
in §7 exist to prevent.

---

## 7. Analysis plan: query→RQ mapping and contingency tree

### 7.1 Which artifact answers which RQ

| RQ | Metric(s) | Query (`metrics/`) | Source tables |
|---|---|---|---|
| RQ1 | M-PREC, M-REC | `rq1_touch_accuracy.sql` | `run_touches` (self-join on `kind`) |
| RQ2 | M-PAR, M-WALL, M-TOK | `parallelism_recovered.sql`, `wallclock_per_plan.sql`, `tokens_per_plan.sql` | `runs`, `run_events` |
| RQ3 | M-BLAST, M-ITER, M-CONF | `conflicts.sql`, `blast_radius.sql`, `reviewer_iters.sql` | `runs`, `run_events` |
| RQ4 | (RQ2/RQ3 metrics) × coupling density | `coupling_density.sql` joined to above | `closures` (M2), `run_touches` |

Coupling density (RQ4) is computed per plan as the ratio of shared closure
units to total units — i.e. how much the plan's hyperedges overlap. This is
why the corpus must *vary* in coupling: RQ4's finding is the slope, not a point.

### 7.2 Pre-committed contingency tree (decided before runs)

- **RQ1 good** (H1 holds) → proceed M2/M3 as specced; closures trustworthy.
- **RQ1 poor, recall low / precision ok** (systematic under-declaration) →
  declared touches miss real dependencies; this *motivates* the derived
  extractor. M2 is reframed as the headline, with the divergence as its
  justification. Paper title and abstract pivot accordingly.
- **RQ1 poor, noisy both ways** → paper pivots to *"static touch declaration is
  insufficient for parallel LLM decomposition: an error-structure analysis"* —
  characterize *where* and *why* declared diverges from actual (by role, by
  file type, by task size). Publishable; a different paper than the headline.
- **RQ2 null** → report honestly; contribution = measurement framework + the
  negative result (real-repo closure overlap may be lower than assumed) +
  RQ1/RQ3 findings. A null is informative here, not a failure.
- **RQ3 tax > gain** → report the crossover regime (§6 H3); the design result
  is *when* co-location pays, not *that* it always does.

---

## 8. Threats to validity (frozen; doubles as the paper's TtV section)

- **Construct — circularity.** The eligibility arm's baseline (declared touches)
  is authored by the same methodology under test. *Mitigation:* RQ1 measures
  declaration accuracy *independently* against git ground truth, so the
  baseline's quality is itself a reported quantity, not an assumption.
- **Internal — model drift.** Endpoint behavior changes over the experiment
  window. *Mitigation:* interleave arms (§4); drift becomes noise, not bias.
- **Internal — reviewer variance.** *Mitigation:* arm-independent objective gate
  (§3); reviewer behavior is measured, not load-bearing.
- **External — generality.** Single language (one tree-sitter grammar), few
  repos, single model family. *Mitigation:* ≥1 non-Planar repo (`git-fleet`
  primary) to break the single-repo objection; remaining limits disclosed, not
  hidden.
- **Conclusion — small N + multiplicity.** *Mitigation:* primary metrics
  pre-registered (§2); everything else labeled exploratory (§0); sign tests and
  effect sizes over p-values (§5).
- **Disclosure — self-hosting.** Planar evaluated partly on its own backlog.
  Accepted in systems work; disclosed explicitly; `git-fleet` carries the
  primary claim to keep self-hosting from being load-bearing.
- **Construct — declaration provenance.** Confirmatory declared touches use the
  corpus features' *original* git-fleet `task_touch_paths` (plans 659, 668) —
  genuine predictions authored during git-fleet's own planning, independent of
  the corpus implementation. Plan 678's three tasks had no recoverable original
  declaration and use a *blind declarer-agent* prediction made at the feature's
  base commit with no access to the implementing commits (labeled
  agent-predicted). Pre-existing corpus declarations that were hindsight-derived
  (declared == actual diff) were discarded before any confirmatory run.
- **Internal — base fidelity for modify-features.** The corpus features modify
  long-lived files, so the file-existence base gate does not apply; each plan's
  base is pinned to the parent of its earliest task commit (659=fcb167a,
  668=274b6f6, 678=32061f7) and base fidelity is operator-asserted. Plans 659
  and 668 bundle tasks spanning more than one milestone, so their pinned base
  precedes all of the plan's tasks; later tasks are therefore re-implemented
  from an earlier-than-natural base. The magnitude of RQ2/RQ3 effects on those
  two plans is treated as exploratory; RQ1 (declared-vs-actual) is unaffected.

---

## 9. Stopping rule & budget (frozen)

Runs cost real money: N × 3 arms × M plans × multi-agent cycles. An
experiment-level spend ledger (cumulative by arm) is queryable, and a spend
ceiling is committed at Stage-2. On reaching the ceiling, stop and analyze what
exists rather than extending. The pilot (M1.7) is the cheap go/no-go before any
corpus spend.

**Committed at Stage-2 (2026-06-22):** ceiling = **$300 USD**; confirmatory
matrix = plans 659 (m7-polish), 668 (Plan-475, coupled), 678 (Plan-388, mixed)
× 3 arms × **N=5** reps = 45 cells. On reaching $300 cumulative spend the
harness stops cleanly (no new cell dispatched) and we analyze what exists.
