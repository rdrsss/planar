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

## Study 2a — Does the derived closure recover what declarations miss? (head-state — **superseded by Study 2b**)

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

### Caveat flagged at time of writing (now resolved — see Study 2b)

The analysis above (call it **Study 2a**) used closures computed 2026-06-16→20
against the corpus repo at **post-implementation state**, and was flagged as a
look-ahead-risk **upper bound**. Study 2b below recomputed the closures
honestly and the bound turned out to be the entire effect.

---

## Study 2b — Look-ahead eliminated: the payoff reverses (same day)

### Method

Each task's closure was recomputed (`planar closure compute`, same extractor
`m2-closure-0.1`) with the corpus repo checked out at that plan's
**pre-feature base SHA** (659 @ `fcb167a`, 668 @ `274b6f6`, 678 @ `32061f7`) —
the state a real forward prediction would be made from. Head-state closures
archived first ([`data/closures-headstate-m2-0.1.csv`](data/closures-headstate-m2-0.1.csv));
base-state in [`data/closures-basestate-m2-0.1.csv`](data/closures-basestate-m2-0.1.csv);
per-task comparison in [`data/derived-vs-declared-basestate.csv`](data/derived-vs-declared-basestate.csv).

### Results: the head-state gain was entirely look-ahead

| predictor | macro recall (n=9) |
|---|---:|
| declared | **0.852** |
| derived-effective, **head-state** (Study 2a) | 0.880 |
| derived-effective, **base-state** (honest) | **0.574** |

- Of the 8 files declarations missed, the base-state closure catches **0**
  (head-state caught 6 — all six "recoveries" were edges the implementations
  themselves had created).
- The under-declared showcase tasks collapse back: 4206 head 0.75 → base
  **0.25** (= declared); 4242 head 1.00 → base **0.50** (below declared 0.58).
- Decomposition (per task): the pre-state symbol walk contributes **zero**
  actual files beyond the declared seeds on *every one of the 9 tasks*.
  `(seeds ∪ base-walk)` recall = 0.852 = declared exactly.
- Part of the raw drop below declared (0.574 < 0.852) is an extractor gap,
  not analysis: it silently **drops seed files that don't exist** at the
  analyzed state (task 4241: 5 seeds, 1 existing at base → 4 vanish; task
  4278's single new-file seed → empty closure). Fixable (closure ⊇ seeds),
  but even fixed, derived = declared: **the walk adds nothing**.

### Interpretation — a genuine negative result, and the mechanism

**The incidental footprint of a change consists overwhelmingly of files the
change itself will newly couple to.** The migration that will be referenced,
the registration point that will gain a line, the sibling verb that will call
the new function — none of these edges exist at prediction time, so forward
static reachability from the seeds cannot see them, *definitionally*. They are
invisible to the author (Study 1: recall 0.56 on multi-file tasks) **and** to
pre-state static analysis (Study 2b: +0 files, n=9). The head-state result is
the cautionary tale: compute the "prediction" after the fact and it looks like
static analysis solves under-declaration (0.88, 6/8 recovered) — a **look-ahead
trap** that this pair of studies now demonstrates and quantifies precisely.

What *could* see future edges: signals that already encode convention and
history rather than current reachability — **co-change mining** (files that
historically change together), **convention detectors** (migrations
directories, registration barrels, CI workflows, build manifests), or
model-predicted footprints. That is now the evidence-backed redefinition of
the extractor's future work.

### Knock-on: the confirmatory grouped arm used look-ahead-informed input

The campaign's grouped-arm slices were computed from the head-state closures.
Re-running `groups recommend` with base-state closures changes the composition
on **2 of 3 plans**: 668 merges to a single 3-task slice (honest closures are
smaller → fits the 128K budget that head-state closures overflowed); 678
splits (the new-file task's empty base closure isolates it); 659 unchanged.
Directionally mixed — 668 would have co-located *more* (likely favoring
grouped further), 678 less. Disclosed as an input-sensitivity caveat on RQ2:
the token win held across both compositions that actually ran (all-in-one
*and* split slices each beat eligibility), but grouped-arm results are
conditional on the closure input, which was oracle-tinged.

### Caveats

1. **n = 9** (6 with original author declarations, 3 blind-agent-declared).
2. Ground truth for the three Plan-388 tasks via merge-first-parent fallback.
3. File-level projection of a symbol-level object; the closure remains a
   *context* predictor — Study 2b judges it only in the role Study 2a claimed
   for it (recovering unpredicted footprint), which it does not fill.
4. One extractor version (`m2-closure-0.1`), one language. A stronger walker
   (type-directed, build-graph-aware) could in principle do better; the
   *future-edge* argument suggests the ceiling is structural, but that is an
   argument, not yet a measurement across extractors.

---

## What this changes for the paper

1. **RQ1 has a real evidence base:** precision ~0.96 everywhere; recall 1.0 on
   trivial tasks but **0.56 on the multi-file tasks that matter** — 35
   external + 6 pilot tasks, construct-valid, two repos, two author
   populations, convergent numbers.
2. **The headline reframes from "derived closure fixes under-declaration" to a
   sharper, more defensible pair:** (a) under-declaration is real and
   concentrated in multi-file work; (b) it is **not fixable by pre-state
   static reachability** — the missing footprint is *future-edge* coupling
   (+0 files from the walk, n=9), and the apparent fix under post-hoc
   computation is a quantified **look-ahead trap** (0.88 vs 0.57). The
   negative result and the trap demonstration are contributions; the
   co-change/convention direction is the evidence-backed future work.
3. **The grouping win (RQ2/RQ3) stands on its own** — its mechanism is context
   co-location, not footprint prediction — with the new input-sensitivity
   disclosure (slice compositions change under honest closures on 2/3 plans;
   the direction of bias is mixed and the win held across both observed
   compositions).
4. **Remaining evidence work:** (a) extend Study 1 to Planar's own history
   (self-hosted secondary corpus); (b) prototype the co-change predictor and
   score it against the same 8 declared-miss files — the table is already
   built; (c) optional live re-runs (timing with the fixed watchdog, gate
   recording) only if a venue demands them.
