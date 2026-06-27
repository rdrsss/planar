# Closure-measurement — raw run data

Durable, version-controlled snapshot of the confirmatory run's raw measurements,
so the results in [`../results.md`](../results.md) are reproducible from source
and safe from the shared local DB (`~/.planar/planar.db`) being reset, migrated,
or clobbered by a concurrent session.

## Files

| file | contents |
|------|----------|
| `closure-run-2026-06.sql` | portable SQL dump of `runs` + `run_events` + `run_touches` for the 41 analysed cells (`m-6*` run_uids). Reloadable into any SQLite DB. |
| `closure-run-2026-06-cells.csv` | per-cell summary (run_uid, plan, arm, status, timestamps, actual/declared touch counts, token-sample count). |
| `closure-run-2026-06-touches.csv` | every declared + actual touch row (run_uid, arm, plan, task, kind, path). |

## Dataset

- 41 cells: 3 plans (659/668/678) × 3 arms (strict/eligibility/grouped),
  **N=5** except 659-all + 678-eligibility at **N=4** (API-limit casualties,
  excluded as failed invocations — see `../results.md` §1).
- 808 `run_touches` (declared + actual), 471 `run_events`
  (token_sample / reviewer_decision / slice_dispatch / slice_fanin / conflict).
- Spend $143.90; coder/reviewer `claude-sonnet-4-5`.
- Run parameters frozen as Planar decisions 548/549/550 (corpus, N, ceiling,
  declaration provenance).

## Reproduce the analysis

```sh
sqlite3 /tmp/closure.db < closure-run-2026-06.sql
# RQ1 (per-run): set :run to a run_uid and run the frozen metric:
sqlite3 /tmp/closure.db -cmd ".param set :run 'm-659-strict-r1'" < ../../../metrics/rq1_touch_accuracy.sql
```

Reload verified faithful: 41 runs / 808 touches / 471 events; RQ1 macro
precision/recall recomputes to 0.765 / 0.644, matching `../results.md`.

## Provenance / caveats

This is real LLM-agent output (not synthetic). Recall reflects genuine agent
behaviour incl. scope variance; precision <1 on coupled plans = agents touched a
subset of declared files. Path-level harvest (decision D1); grouped per-task
attribution is coarse (a slice's files attribute to every task in the slice).
See `../results.md` §4 and `../preregistration.md` §8 for the full caveat list.
