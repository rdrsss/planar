# Closure-Measurement Experiment — Results (v2)

**Status:** confirmatory run complete (2026-06-27); **analysis revised v2
(2026-07-03)** after a pre-paper instrument review. Companion to
[`preregistration.md`](preregistration.md) (frozen contract),
[`pilot-notes.md`](pilot-notes.md) (M1.7 gate),
[`campaign-runbook.md`](campaign-runbook.md) (how to run it), and
[`data/`](data/) (archived raw dataset — every number here is recomputable).

> **For the full self-contained narrative** — motivation, methodology, the
> execution story — see [`closure-measurement-report.md`](closure-measurement-report.md).
> This file is the results of record.

## Revision v2 — what changed and why

A critical review before paper-writing
([`log/2026-07-03-instrument-review.md`](log/2026-07-03-instrument-review.md))
found that the v1 analysis contained one invalid metric and one unsupported
verdict. v2 corrects both, adds the metrics the frozen protocol required but v1
never computed, and applies the prereg's own statistics rules
(medians + spread + sign tests). Specifically:

1. **M-WALL withdrawn.** Wall-clock was a structural artifact: each agent
   invocation blocked the full 600 s watchdog slot regardless of when the agent
   finished, so 39/41 cells measured exactly (serial invocations × 600 s) —
   arm structure, not work. No wall-clock findings are reported. (Harness bug
   fixed separately; task 4529.)
2. **The pooled RQ1 recall (0.644) and the v1 "H1 fails on recall" verdict are
   withdrawn.** Per-arm analysis shows the pooled figure averaged two opposing
   artifacts (grouped attribution coarseness deflating recall; anti-sprawl
   briefing inflating it). See §3 for the corrected treatment.
3. **M-PAR computed** — the frozen H2 metric that v1 substituted with the
   now-withdrawn wall-clock. H2's parallelism leg is now evaluated on the
   frozen metric (and passes).
4. **Coupling density computed** (RQ4's numeric x-axis; v1 used qualitative
   labels).
5. **Medians + IQR + exact sign tests** replace v1's means, per prereg §4/§5.

All campaign numbers below are over the 41 valid cells
(`runs.run_uid glob 'm-6*'` and `status='completed'`); the `metrics/*.sql`
files are the generic frozen definitions (whole-DB), so apply that filter when
reproducing campaign figures.

## 1. Dataset

| | |
|---|---|
| Matrix | 3 plans × 3 arms × N reps |
| Plans (coupling density, `metrics/coupling_density.sql`) | **659** m7-polish (**0.051**) · **668** Plan-475 (**0.445**) · **678** Plan-388 (**0.323**) |
| Per-plan base SHA | 659=`fcb167a`, 668=`274b6f6`, 678=`32061f7` (parent of each plan's earliest task commit) |
| Arms | **strict** (one isolated worktree/task, serial) · **eligibility** (parallel-eligible cohort + barrier) · **grouped** (closure slices co-located) |
| Cells analysed | **41** (all with data) |
| Reps/cell | **N=5** for 668-all, 678-grouped, 678-strict; **N=4** for 659-all + 678-eligibility |
| Spend | **$143.90** of the $300 ceiling |
| Coder/reviewer model | `claude-sonnet-4-5` (held constant; §4 nuisance var) |

**N=4 cells:** the missing reps were **API rate-limit/overload casualties** —
`claude -p` calls that returned empty (zero tokens) across the run window and
two top-up attempts. Excluded as *failed invocations*, never recorded as data
(the retry + zero-touch guard caught every one). Within the preregistered
N=3–5; treated as missing-at-random (failures were API-side).

**Unit-of-analysis note (v2):** the declared side is identical across reps and
arms, so the **effective n for declaration quality is 9 tasks**; the reps
sample *agent behavior*, not declarations. Sign tests below are over
(plan, rep) pairs and support agent-cost claims only.

## 2. Frozen metrics (§2 definitions) — v2 values

Arm-level, medians with IQR across cells:

| metric | strict | eligibility | grouped |
|---|---:|---:|---:|
| **M-TOK** median | 35,434 | 37,266 | **15,567** |
| M-TOK IQR | [26.5K, 43.4K] | [30.7K, 40.9K] | [2.3K, 19.7K] |
| **M-PAR** (recovery of eligibility-serialized tasks) | — | — | **3/4 = 75%** pooled (668: 1/2 = 50%; 678: 2/2 = 100%; 659: N/A — eligibility serialized nothing) |
| **M-ITER** (request-changes; *measured — no rework loop executes*) | 16 | 12 | **4** |
| **M-CONF** (fan-in merge conflicts) | 0 | 1 | **0** |
| **M-BLAST** (*structural* slice size; no failure was ever paid) | 1.00 | 1.00 | 2.21 |
| ~~M-WALL~~ | | | *withdrawn — instrument artifact (Revision v2 #1)* |

**Sign tests** (paired per (plan, rep), exact binomial, two-sided):

| contrast | wins | p |
|---|---|---:|
| grouped vs eligibility (H2 token condition) | grouped cheaper **13/13** | **0.0002** |
| grouped vs strict | grouped cheaper **13/14** | **0.0018** |
| strict vs eligibility (control contrast) | 6/13 | 1.0 |

The control contrast is the key internal check: the two non-grouped arms are
statistically indistinguishable on tokens — the savings are specific to
**grouping**, not to any incidental dispatch difference.

## 3. RQ1 — declared-touch accuracy (v2 treatment)

Per-arm macro over per-task observations (v2 — v1 pooled across arms):

| arm | precision | recall | attribution validity |
|---|---:|---:|---|
| strict | 0.736 (median 1.00) | 0.796 (median 1.00) | clean per-task attribution, **but** recall is inflated by the anti-sprawl brief steering agents toward the declared files (partial circularity — the fix that saved the arm comparison biased this measurement) |
| eligibility | 0.675 | 0.749 | same steering caveat |
| grouped | 0.679 | 0.394 | recall **deflated** by slice-level attribution (a slice's files attribute to every task in it) |

The v1 pooled figure (0.765 / 0.644) averaged these opposing artifacts; the v1
verdict built on it is withdrawn. Note strict-only *clears* X = 0.70 on both
axes — but its recall is upward-biased by our own intervention, so **the live
run neither cleanly confirms nor refutes H1's recall bound.** The per-task
distribution is strongly bimodal (strict-arm recall: 24/42 observations at
exactly 1.0, a cluster at 0.4–0.5): tasks are either predicted essentially
exactly or missed substantially — the mean is not a typical value.

**The construct-valid RQ1 evidence remains the retrospective pilot**
([`pilot-notes.md`](pilot-notes.md)): declared predictions vs *real human
implementation history*, no agent in the loop — **precision 1.00, recall
0.58.** **Scaled up post-hoc** (2026-07-03) to all 37 declared git-fleet tasks
— precision 0.957, recall 0.722 overall but **0.558 on multi-file tasks**.
The derived-closure payoff was then measured and **reversed under look-ahead
control**: computed honestly at pre-feature state, the closure walk adds zero
recall beyond the seeds (the post-hoc 6/8 "recovery" was look-ahead artifact) —
see [`retrospective-studies.md`](retrospective-studies.md) Study 2b.

**H1 verdict (v2):** precision confirmed high everywhere (≥ 0.67 live in every
arm; 1.00 retrospective). The recall bound is **unresolved by the live
confirmatory run** (construct compromised in both directions) and rests on the
pilot's retrospective 0.58 — which supports the pre-committed §7.2 pivot
(derived closure as headline), argued from the pilot with the live data as
corroborating-but-compromised, fully disclosed.

## 4. RQ2 / H2 — parallelism recovery at token cost (v2)

- **Token condition: MET, with inferential support** — grouped median 15.6K vs
  eligibility 37.3K; **13/13 paired wins, p = 0.0002**.
- **Parallelism condition, on the frozen M-PAR: MET** — of the 4 tasks
  eligibility serialized across the corpus, grouped co-located **3 (75%) ≥
  Y = 50%** (668: 50%, 678: 100%, 659: undefined — eligibility serialized
  nothing there; on a fully disjoint plan there is no parallelism to recover,
  itself an RQ4-relevant observation). Slice shapes are structural (identical
  across reps), so M-PAR is deterministic per plan.
- Wall-clock is **not cited** (withdrawn, Revision v2 #1).

**H2 verdict (v2): supported, on the frozen metrics.**

## 5. RQ3 / H3 — the co-location tax (v2)

Grouped shows the lowest hypothetical rework demand (M-ITER 4; request-changes
rate 0.20 vs strict 0.39 / eligibility 0.31) and zero conflicts; its only tax
dimension is the structural blast radius (2.21 tasks/slice).

**H3 verdict (v2): not falsified, but weakly tested.** The harness executes no
rework loop, so no failure tax could actually be *paid* in any arm — M-ITER is
measured-hypothetical and M-BLAST structural. Z (tax < 100% of gain) is
formally satisfied with wide margin, but H3 needs a rework-enforcing harness to
be tested seriously. Disclosed; consistent with the prereg's own calibration
note that Z was set conservatively pending reviewer-inclusive data.

## 6. RQ4 — scaling with coupling density (v2, numeric x-axis)

| plan | coupling density | grouped median M-TOK | strict median M-TOK | grouped advantage |
|---|---:|---:|---:|---:|
| 659 | 0.051 | 1,749 | 24,216 | **13.8×** |
| 678 | 0.323 | 20,544 | 45,280 | 2.2× |
| 668 | 0.445 | 16,861 | 32,878 | 1.9× |

The grouping advantage is **monotone-decreasing in measured coupling density**
(13.8× → 2.2× → 1.9×). Exploratory (three points), but now a real slope on the
protocol's numeric x-axis rather than qualitative labels. Mechanism: when
closures are disjoint, one co-located agent handles several tasks with full
context reuse; as closures overlap, per-slice work grows and the gap to strict
compresses.

## 7. Threats / caveats (v2)

- **Anti-sprawl circularity (RQ1):** the brief fix that made the arm comparison
  possible steers agents toward declared files, inflating live recall. In an
  experiment where instrument, subject, and measured quantity are all
  LLM-agent behavior, every intervention is a treatment. De-circularized RQ1
  protocol (briefs stripped of file names) is future work.
- **Grouped attribution coarseness (RQ1):** deflates that arm's recall; per-task
  attribution inside a co-located slice needs finer harvest granularity.
- **Pseudoreplication:** effective n = 9 tasks for declaration-quality claims.
- **No functional-quality verification:** objective-gate outcomes (build/tests)
  were not recorded per cell. "Same work for fewer tokens" rests on proxies
  (comparable declared-coverage 0.68–0.74 across arms; grouped's *better*
  review rate). Gate recording is required in any future run.
- **N=4 on four cells** (API-limit casualties, missing-at-random).
- **Single corpus / language / model family.** On the disjoint plan (659),
  grouped degenerates to one-agent-whole-plan, so grouping-vs-monolith is only
  distinguished on the coupled plan (668's 2-slice partition).
- **Path-level harvest** (decision D1); symbol-level deferred.
- **Grouped-arm input sensitivity (post-hoc finding):** the campaign's grouped
  slices were computed from closures extracted at post-implementation state;
  with honest pre-state closures the composition changes on 2/3 plans
  (mixed direction; the token win held across both observed compositions).
  See `retrospective-studies.md` Study 2b.

## 8. Bottom line (v2)

Declared touches are **precise everywhere**, but their completeness could not
be certified by this live design — the under-declaration claim (recall 0.58)
rests on the construct-valid retrospective evidence (pilot 0.58; 35-task
study 0.56 on multi-file tasks). The §7.2 pivot **evolved under further
scrutiny**: pre-state static reachability does *not* recover the missing
footprint (retrospective-studies.md Study 2b — the missing files are
future-edge coupling; the post-hoc "fix" was a quantified look-ahead trap), so
the headline is the sharper pair: under-declaration is real **and** structurally
resistant to static closure — pointing to co-change/convention predictors. The robust experimental win is **closure-aware grouping**:
the same corpus at **half to one-third the tokens** (13/13 paired wins,
p = 0.0002, vs a null control contrast between the other two arms), **75% of
serialized parallelism recovered on the frozen M-PAR**, the best review
outcomes, zero conflicts — with the advantage **monotone in measured coupling
density**. The instrument, its failure story
([`log/`](log/)), and the pre-registered branch discipline are themselves
contributions.
