# Pilot Notes — M1.7 (closure-measurement experiment, plan 634/635)

**Date:** 2026-06-15
**Pre-registration:** Stage-1 frozen 2026-06-15 (tag `prereg-stage1`) — this pilot ran *after* the freeze.
**Status:** **GATE — go/no-go decision pending (human).** RQ1 has a number; the pipeline is validated end-to-end.

> **Update (2026-06-30):** the gate was passed and the confirmatory run is complete — see [`results.md`](results.md). This document is retained as the historical M1.7 pilot record.

## What this pilot is (and is not)

- **Purpose (per build-spec M1.7 + preregistration §9):** the cheap go/no-go before any corpus spend — produce the first RQ1 (touch-prediction accuracy) and prove the measurement pipeline produces clean, joinable records.
- **NOT confirmatory.** N=1 (one plan, one rep). No arm comparison — RQ1 is arm-independent (preregistration §1), so a single retrospective observation is valid for the gate.

## Method (retrospective, zero new LLM spend)

The primary corpus `git-fleet` is a real codebase but its Planar decomposition (declared `task_touch_paths`) is not present in any reachable DB, so it could not supply the *declared* side retrospectively. The pilot therefore used the **secondary, self-hosted corpus**: Planar's own **plan 635 (the M1 measurement rig itself)** — the cleanest available dataset because its declared touches were authored as **genuine predictions during ingest, before the work** (no hindsight bias), and the actual touches are the merged M1 commits.

- **run_uid:** `pilot-p635-rq1` · **base_sha:** `88e8390` · **arm:** retrospective · **corpus:** planar (self-hosted)
- **declared** = `bench start` snapshot of plan 635's `task_touch_paths` (the pre-work predictions).
- **actual** = `bench harvest` of each build task's merged commit vs its parent (`git diff --name-only base..head`):
  - 4078 M1.1 `88e8390..94e74ad` · 4079 M1.2 `94e74ad..3d53e10` · 4080 M1.3 `3d53e10..4874ac7`
  - 4081 M1.4 `4874ac7..56f60d2` · 4082 M1.5 `56f60d2..71d17d5` · 4083 M1.6 `71d17d5..60f48dd`
- **RQ1 computed** with the frozen `metrics/rq1_touch_accuracy.sql` (`:run = 'pilot-p635-rq1'`).

The whole flow dogfooded the instrument: `planar bench start | harvest | finish` + the checked-in metrics SQL.

## RQ1 result (the 6 implemented M1 slices)

| task | declared | actual | hit | precision | recall |
|------|---------:|-------:|----:|----------:|-------:|
| 4078 M1.1 | 2 | 3 | 2 | 1.00 | 0.67 |
| 4079 M1.2 | 2 | 4 | 2 | 1.00 | 0.50 |
| 4080 M1.3 | 7 | 11 | 7 | 1.00 | 0.64 |
| 4081 M1.4 | 2 | 3 | 2 | 1.00 | 0.67 |
| 4082 M1.5 | 1 | 2 | 1 | 1.00 | 0.50 |
| 4083 M1.6 | 5 | 10 | 5 | 1.00 | 0.50 |

- **Macro-avg precision = 1.00** (all six slices).
- **Macro-avg recall ≈ 0.578.**
- Declared totals: 19 paths predicted; actual totals: 33 paths touched; 19 hits.

(The frozen-SQL aggregate over *all snapshotted tasks* reports precision 0.857 / recall 0.578 because task **4084** — the pilot meta-task itself — had its 2 declared paths auto-snapshotted but was never harvested, yielding a spurious precision=0. 4084 is excluded from the headline; see Limitations.)

## Finding

**Declared touches are perfectly precise but systematically under-complete** (precision 1.00, recall 0.58). Every path an author predicted was in fact touched; the misses are entirely *un-predicted* files — the incidental "glue" the work pulled in: module barrels (`src/engine/runs.zig`, `root.zig`), verb/command registration (`main.zig`, `verb_classification.zig`), test files and `all_test.zig` registration, and the `architecture.md` schema-doc update.

This lands in preregistration **§7.2's "RQ1 poor, recall low / precision ok"** branch: *systematic under-declaration → the derived-closure extractor (M2) becomes the central contribution, with the declared-vs-derived divergence as its justification.* The pilot thus actively motivates M2 rather than discouraging it.

## Pipeline validation (M1.7 acceptance)

- ✅ RQ1 precision/recall computed from the frozen query.
- ✅ No orphaned / unjoinable records: every `run_touches` row joins to the run and to a task; per-task join is clean across all 7 snapshotted tasks. Declared snapshot is immutable (M1.5).
- ✅ `bench start/harvest/finish` + metrics SQL ran end-to-end against the live schema-25 DB.

## Limitations / caveats

1. **Self-hosted, not the primary corpus.** Disclosed per preregistration §8. `git-fleet` (primary, external) carries the confirmatory claim once it's decomposed into Planar with declared touches — a prerequisite for the M3.4 corpus runs.
2. **Retrospective, not live arms.** Valid for RQ1 (arm-independent); RQ2/RQ3 require live `strict`/`eligibility`/`grouped` execution.
3. **N=1.** Pilot only; not a confirmatory claim.
4. **Path-level harvest** (decision D1) — symbol-level deferred.
5. **Snapshot-scoping refinement surfaced:** `bench start` snapshots declared touches for *all* plan tasks, including meta-tasks (4084 = the pilot, 4170 = a cleanup with no touches). A measured "run" should scope its declared snapshot to the slices actually executed/harvested, or RQ1 should be filtered to harvested tasks. Filed as a refinement for the harness layer; it did not affect the recall headline (4084 is excluded from recall as no-actual) and only injected a spurious precision=0 into the all-task aggregate.

## Go/no-go (the gate)

RQ1 is computed and the pipeline is validated. The result (precision 1.00, recall 0.58) is the **"recall-low / precision-ok"** outcome, which *strengthens* the case for M2 (the derived closure extractor) as the headline contribution. **Awaiting human decision to proceed to M2** (per build-spec critical path: do not pass the gate autonomously).
