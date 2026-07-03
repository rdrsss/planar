# Retrospective Studies: Declaration Accuracy at Scale & the Derived-Closure Payoff

**Date:** 2026-07-03 · **Cost:** $0 (git history + SQL; no agents in the loop) ·
**Data:** [`data/retrospective-rq1-gitfleet.csv`](data/retrospective-rq1-gitfleet.csv),
[`data/derived-vs-declared.csv`](data/derived-vs-declared.csv)

Two studies closing the evidence gaps identified in the pre-paper review
([`log/2026-07-03-instrument-review.md`](log/2026-07-03-instrument-review.md)):
the under-declaration claim rested on a 6-task pilot, and the pivoted paper's
headline — *the derived closure recovers what declarations miss* — had never
been measured at all.

**Status relative to the pre-registration:** these are **post-hoc extensions**
of the pilot's retrospective method (declared predictions vs. real human
implementation history), not pre-registered confirmatory runs. They use the
frozen §2 metric definitions (per-task precision/recall on `(task, path)`,
undefined-denominator exclusions reported) but were designed after the
confirmatory campaign. Labeled exploratory-confirmatory accordingly.

---

## Study 1 — Retrospective RQ1 at scale: 37 tasks, real human history

### Method

All 37 git-fleet tasks carrying original author-declared touches
(`task_touch_paths`, project `git-fleet`) were compared against ground truth
from the repo's real development history (`epic/git-fleet-vision`, 163
commits — the original human implementations, **no agents involved**; the
corpus repo's refs were *not* used, as its bench branches contain campaign
agent commits). A task's *actual* set is the union of files touched by its
non-merge commits (commit subjects carry task ids); where a task id appears
only on fan-in merge commits, the merge's first-parent diff is used. 2 tasks
had no implementing commits (planning-only; excluded from recall per the
frozen rule, count reported), leaving **n = 35**.

### Results

| | precision | recall |
|---|---:|---:|
| macro (n=35) | **0.957** | **0.722** |
| median | 1.00 | 1.00 |
| micro (pooled paths) | 0.906 | 0.563 |

Recall distribution: **19/35 tasks at exactly 1.0**; 5 in [0.7, 1.0); 5 in
[0.5, 0.7); 10 below 0.5. Precision = 1.0 on 31/35 tasks.

**The size-conditioned cut is the real finding.** The distribution is bimodal
because single-file tasks are trivially predicted:

| stratum | n | macro recall |
|---|---:|---:|
| \|actual\| = 1 | 13 | 1.00 (trivial) |
| **\|actual\| ≥ 2** | **22** | **0.558** |

**Conditional on a task actually touching two or more files, author
declarations miss ~44% of the footprint** — converging almost exactly with the
pilot's 0.58 (6 multi-file Planar tasks) from an independent repo and authors.
Precision stays ~0.96 in both strata: what authors declare, they touch; what
they miss is the incidental periphery.

### Interpretation

- The naive H1 read ("macro recall 0.722 ≥ 0.70, H1 passes") is dominated by
  trivial singletons. The parallelization-relevant population is multi-file
  tasks — where disjointness decisions can actually go wrong — and there,
  recall is **0.56**: a task declared as one file that in fact touches six
  silently breaks the eligibility computation.
- This is now the paper's RQ1 evidence base: **35 external-repo tasks + 6
  pilot tasks, construct-valid (human history, no agent loop, no
  brief-steering circularity)**, replacing the compromised live-RQ1 numbers.

---

## Study 2 — Does the derived closure recover what declarations miss?

### Method

For the 9 corpus tasks with computed derived closures (extractor
`m2-closure-0.1`; 4,305 symbol rows), three predictors of the task's actual
file footprint were compared against the same human-history ground truth:

- **declared** — the author/blind-agent declared touches;
- **derived (modify-role)** — the closure's predicted *edit* set, projected to
  files;
- **derived (effective)** — modify ∪ reference: the full context the task must
  hold resident, projected to files.

Note the semantics: the effective closure is a *context* predictor, so its
file-edit "precision" is low **by design** (reference files are meant to be in
context without being edited); the meaningful axes are **recall** (don't miss
a dependency) and **size/token cost**.

### Results (n = 9; per-task table in the CSV)

| predictor | macro recall | macro precision | note |
|---|---:|---:|---|
| declared | 0.852 | 0.978 | 6/9 already fully declared (ties) |
| derived, modify-role | **0.880** | 0.463 | the edit-set comparison |
| derived, effective | **0.880** | 0.152 | context predictor; 8K–229K tokens/task |

The aggregate gain looks small because most of these tasks were already fully
declared. The gain concentrates exactly where Study 1 says the problem lives —
the under-declared multi-file tasks:

| task (gf) | declared recall | derived recall |
|---|---:|---:|
| 4206 (3268, migration + schema-derive) | 0.25 | **0.75** |
| 4242 (3493, provider capabilities, 12 files) | 0.58 | **1.00** |

Of the **8 files that declarations missed** across the corpus, the derived
closure **caught 6**.

### The blind spot — and the one loss

The files missed by **both** predictors are, without exception, **non-Zig
artifacts**: `src/migrations/0005_observed_settings.sql`,
`.github/workflows/ci.yml`, `build.zig.zon`, `RELEASE_NOTES_*.md`,
`fixtures/*.toml`, `scripts/*.sh`. A Zig symbol-graph walker is structurally
blind to them. The one task where the derived closure *loses* to declarations
— 4277 (2981, "cut the v0.5 preview release": recall 0.17 vs 0.83) — is
exactly the task whose footprint is almost entirely non-code release
scaffolding.

This is a precise error-structure result, not a shrug: **declarations fail on
incidental *code* (the symbol-coupled periphery the author didn't trace);
the static closure fails on *non-code* build/CI/migration/fixture artifacts
(edges outside the language graph).** The two failure modes are complementary
— which both (a) justifies the derived closure for the code fraction and
(b) scopes its required extension (artifact-coupling heuristics: migrations
directories, build manifests, CI workflows) as concrete future work rather
than an open-ended limitation.

### Caveats

1. **Look-ahead risk (the important one):** the closures were computed
   2026-06-16→20 against the corpus repo at post-implementation state — the
   symbol graph the extractor walked may contain edges created by the very
   implementations being predicted. The clean version recomputes each task's
   closure at its pre-feature base SHA (static and free; queued as follow-up).
   Until then, Study 2's derived-closure recall is an **upper bound**.
2. **n = 9** (6 with original author declarations, 3 blind-agent-declared —
   including the pathological release task).
3. Ground truth for the three Plan-388 tasks via merge-first-parent fallback
   (their ids appear only on fan-in merges).
4. File-level projection of a symbol-level object; token costs (8K–229K per
   task) are the context-budget axis the grouping objective optimizes,
   reported in the CSV.

---

## What this changes for the paper

1. **RQ1 now has a real evidence base:** precision ~0.96 everywhere; recall
   1.0 on trivial tasks but **0.56 on the multi-file tasks that matter** —
   35 external + 6 pilot tasks, construct-valid, two repos, two author
   populations, convergent numbers.
2. **The headline claim is now measured, with honest structure:** the derived
   closure recovers **6/8 of declared misses** (0.25→0.75, 0.58→1.00 on the
   under-declared tasks), at a quantified context cost, with a
   *characterized* blind spot (non-code artifacts) and one adversarial case
   (release scaffolding) reported rather than hidden.
3. **Remaining evidence work, in priority order:** (a) recompute closures at
   pre-feature bases to eliminate the look-ahead bound; (b) extend Study 1's
   method to Planar's own history as the disclosed self-hosted secondary
   corpus; (c) optional: artifact-coupling extension to the extractor, which
   Study 2's error structure now specifies exactly.
