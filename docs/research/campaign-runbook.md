# Confirmatory-Run Campaign Runbook

**How to run the closure-measurement confirmatory matrix by hand.** This is the
operator-facing companion to [`preregistration.md`](preregistration.md) (the
frozen experimental contract) and [`pilot-notes.md`](pilot-notes.md) (the M1.7
go/no-go). The driver is `scripts/bench-matrix.sh`; it is a bash harness that
spawns the coder/reviewer agents, brackets them with the deterministic
`workflows/bench_run_ritual.lua` A/C phases, and records everything to the bench
tables. It holds no DB handle of its own — it shells `planar` / `planar-execute`
/ `git` / `claude`.

> **D5 boundary.** The harness is the LLM caller; Planar is not a harness. There
> is no "centurion." Running the matrix is running this script.

---

## 0. What the campaign is

A 3-arm × 3-plan × N=5 matrix (45 cells) over the external `git-fleet` corpus,
measuring:

- **RQ1** declared-vs-actual touch accuracy (arm-independent),
- **RQ2** whether grouping recovers the parallelism the strict closure serializes,
- **RQ3** the co-location tax,
- **RQ4** how the delta moves with coupling density (the 3 plans span a
  low → coupled → mixed gradient).

Arms: `strict` (one isolated worktree per task, serial), `eligibility`
(parallel-eligible cohort concurrent + barrier), `grouped` (closure slices
co-located). Each arm runs the same tasks from the same per-plan base SHA; the
fan-in conflict check (M-CONF) records integration conflicts per arm.

## 1. Committed parameters (frozen — see prereg §9 + plan 637 decisions 548–550)

| Parameter | Value |
|-----------|-------|
| Corpus repo | `git-fleet-corpus` (project 43) at `~/projects/github/rdrsss/git-fleet-corpus` |
| Plans | **659** m7-polish (low coupling), **668** Plan-475 (coupled), **678** Plan-388 (mixed) |
| Per-plan base SHA | **659=`fcb167a`**, **668=`274b6f6`**, **678=`32061f7`** (parent of each plan's earliest task commit) |
| Reps (N) | **5** per cell → 45 cells total |
| Spend ceiling | **$300 USD** (hard stop; harness halts cleanly on breach) |
| Coder / reviewer model | `claude-sonnet-4-5` (held constant; nuisance var per §4) |
| Declarations | gold git-fleet originals (659/668) + blind declarer (678); see prereg §8 |

The bases are **operator-pinned** because the corpus features *modify*
long-lived files; the default `pick_base_sha` would walk the base back to repo
genesis. With `--bases`, the file-existence base-fidelity gate is softened to
"operator-asserted" (logged, not fatal). See prereg §8 "base fidelity for
modify-features."

## 2. Prerequisites

- `planar`, `planar-agent`, `planar-execute` on `PATH` (a current build that
  opens the bench-schema DB).
- `claude` CLI on `PATH`, authenticated (`claude -p` headless must work).
- `git`, `jq`, `sqlite3`, `perl` on `PATH` (`perl` is used by the per-agent
  watchdog for process-group control — see §6/§7 self-healing; macOS ships it).
- The corpus repo present at `~/projects/github/rdrsss/git-fleet-corpus`.
- For the `grouped` arm with the real partitioner: the `mtkahypar` shim on
  `PATH` and `BENCH_SOLVER=mtkahypar` (default solver is `greedy`, which needs
  nothing extra).
- Run from a checkout that contains the merged harness **and** its siblings
  (`workflows/bench_run_ritual.lua`, `metrics/*.sql`) — i.e. a `master`
  checkout, not an extracted copy.

## 3. The DB-override recipe (important)

The harness defaults to an **isolated** `experiment.db` under a fresh temp
`BENCH_HOME`. The corpus plans, their declarations, and all prior bench history
live in the **real** `~/.planar/planar.db`, so the campaign points the harness
there via the override knobs:

```sh
PLANAR_DB_OVERRIDE=~/.planar/planar.db
PLANAR_CONFIG_PATH_OVERRIDE=~/.planar/config.toml
```

Without these the harness sees an empty DB (no plans) and resolves every base to
the corpus HEAD.

## 4. Pre-flight: always dry-run first

`--dry-run` builds the full schedule, resolves each plan's base, prints the
deterministic run_uids + paired config_hash + spend estimate, and **mutates
nothing / spawns nothing**:

```sh
cd <master-checkout>
BENCH_CORPUS_REPO=~/projects/github/rdrsss/git-fleet-corpus \
BENCH_CORPUS_REPO_NAME=git-fleet-corpus \
PLANAR_DB_OVERRIDE=~/.planar/planar.db \
PLANAR_CONFIG_PATH_OVERRIDE=~/.planar/config.toml \
scripts/bench-matrix.sh --matrix --plans 659,668,678 --reps 5 --ceiling 300 \
  --bases 659:fcb167a,668:274b6f6,678:32061f7 --dry-run
```

Confirm the "resolved base_sha per plan" block shows `fcb167a` / `274b6f6` /
`32061f7` each marked `(operator-supplied override)`, 45 cells, and "projected
total within ceiling."

## 5. Launch the full matrix (real spend)

Drop `--dry-run`. Pin `BENCH_HOME` to a persistent dir so worktrees, transcripts,
and per-cell JSONL logs survive and resume is clean:

```sh
cd <master-checkout>
BENCH_HOME=~/.planar/bench-campaign \
BENCH_CORPUS_REPO=~/projects/github/rdrsss/git-fleet-corpus \
BENCH_CORPUS_REPO_NAME=git-fleet-corpus \
PLANAR_DB_OVERRIDE=~/.planar/planar.db \
PLANAR_CONFIG_PATH_OVERRIDE=~/.planar/config.toml \
scripts/bench-matrix.sh --matrix --plans 659,668,678 --reps 5 --ceiling 300 \
  --bases 659:fcb167a,668:274b6f6,678:32061f7 \
  2>&1 | tee ~/.planar/bench-campaign/run-$(date +%Y%m%d-%H%M).log
```

The walk is interleaved (arm order shuffled per rep,plan — §4), each cell's
config_hash is arm-paired, and run_uids are deterministic (`m-<plan>-<arm>-r<rep>`).

### Single-cell smoke (optional, before committing to 45)

```sh
... same env ...
scripts/bench-matrix.sh --plan 659 --arm strict --rep 1 --base fcb167a
```

## 6. What happens during a run

- **Per cell:** phase A (`bench start` + immutable declared-touch snapshot) →
  phase B (arm-shaped coder/reviewer agent dispatch) → phase C (`bench harvest`
  of the committed range + `bench finish`). Conflicts at fan-in are recorded as
  `conflict` events.
- **Telemetry:** every agent's `claude` usage + `total_cost_usd` is recorded as
  a `token_sample` event and added to the spend ledger.
- **Per-agent timeout (M4):** `BENCH_AGENT_TIMEOUT` (default 600s) — a hung agent
  is killed (whole process group) and the cell is marked crashed, not fatal; the
  walk continues.
- **Retry cap (M4):** `BENCH_CELL_RETRY_CAP` (default 2) — a cell that crashes
  deterministically is marked permanently failed and skipped, not retried
  forever.
- **Self-healing on API failure:** a `claude` call that returns empty / errors
  (e.g. a rate-limit/overload hiccup — distinguished from a genuine
  token-bearing no-op) is retried inline up to `BENCH_AGENT_RETRIES` (default 3,
  `BENCH_AGENT_RETRY_BACKOFF`s between). If a slice still fails it is marked
  *crashed*; if **every** slice of a cell crashes (sustained outage) the cell is
  finished `--status aborted` (not silently `completed`-empty) so a later resume
  re-runs it. A real "agent ran, made no edits" no-op stays `completed` (valid
  0-touch data) — the signal is the failed-call/crash, not 0 touches.
- **Noise filter:** build artifacts / backups / vendored paths (`*.bak`,
  `vendor/`, `zig-cache/`, `*.o`, …; configurable via `BENCH_HARVEST_EXCLUDE`)
  are stripped from the harvested `actual` touches before the zero-touch guard.
- **Ceiling (§9):** before each cell the harness checks cumulative spend; on
  breach it stops cleanly (dispatches nothing new, completed cells intact).
- **Interrupt (M4):** Ctrl-C / SIGTERM kills the in-flight agent group and prunes
  the in-flight cell's worktrees; the cell's row is left `running` for resume.

## 7. Resume & recovery

- **Resume:** re-run the *identical* command (same `BENCH_HOME` + same
  `PLANAR_DB_OVERRIDE`). `completed` cells `SKIP`; `aborted` (all-slices-crashed)
  and `running` (orphaned) cells abort + retry up to `BENCH_CELL_RETRY_CAP`, then
  `PERM_FAIL`. Resume is additive — it never reuses a partial uid. **No manual
  `delete` is needed** — an outage-emptied cell self-marks `aborted` and the next
  resume re-runs exactly it.
- **Orphaned `running` rows (hard kill / power loss):** reconciled automatically
  on resume. A staleness guard (`BENCH_STALE_RUNNING_SEC`, default 1200) treats a
  row older than the threshold as a definite orphan; a *recent* running row logs
  a loud WARNING (possible live concurrent run on the shared DB) but still
  proceeds (the harness is single-launcher by design). Each reconciliation writes
  a structured `orphan_reconcile` record to the cell-log JSONL.
- **Inspect:** `sqlite3 ~/.planar/planar.db "select run_uid,arm,status from runs where status in ('running','aborted');"`
- **Watch the retry budget:** every failed resume attempt during a flapping
  outage consumes a `CELL_RETRY_CAP` slot → eventual `PERM_FAIL`. Don't re-run
  repeatedly while limits flap; wait for a stable window (or raise the cap).
- **Debug a cell's output:** `BENCH_KEEP_WORKTREES=1` keeps the per-cell slice
  worktrees instead of pruning them after harvest.

## 8. Reading results

Bench tables live in `~/.planar/planar.db`: `runs`, `run_events`
(`token_sample` / `wall_sample` / `conflict` / `gate` / `reads`), `run_touches`.
The checked-in metrics SQL (`metrics/`) computes each RQ:

```sh
DB=~/.planar/planar.db
sqlite3 "$DB" ".read metrics/rq1_touch_accuracy.sql"   # RQ1 precision/recall per task
sqlite3 "$DB" ".read metrics/tokens_per_plan.sql"      # RQ2 token cost per arm
sqlite3 "$DB" ".read metrics/wallclock_per_plan.sql"   # RQ2/RQ3 wall-clock per arm
sqlite3 "$DB" ".read metrics/conflicts.sql"            # M-CONF fan-in conflicts per arm
```

Report medians + spread across the N=5 reps; never a single run (§4). Thresholds
to falsify against are frozen in prereg §6 (X=0.70, Y=50%, Z).

## 9. Knobs reference

| Env var | Default | Meaning |
|---------|---------|---------|
| `BENCH_CORPUS_REPO` | script's parent dir | path to the corpus repo |
| `BENCH_CORPUS_REPO_NAME` | `planar` | corpus repo slug (set to `git-fleet-corpus`) |
| `PLANAR_DB_OVERRIDE` | isolated temp db | DB the harness reads/writes |
| `PLANAR_CONFIG_PATH_OVERRIDE` | isolated temp config | config the agents inherit |
| `BENCH_HOME` | fresh `mktemp` dir | root for worktrees / transcripts / logs |
| `BENCH_CORPUS_PLANS` / `--plans` | — | plan ids to sweep |
| `BENCH_N_REPS` / `--reps` | 3 | reps per cell |
| `BENCH_CEILING` / `--ceiling` | 50.00 | spend hard stop (USD) |
| `BENCH_BASES` / `--bases` | — | per-plan base map `plan:sha,...` |
| `--base` | — | single-cell base override |
| `BENCH_CODER_MODEL` / `BENCH_REVIEWER_MODEL` | `claude-sonnet-4-5` | agent model tier |
| `BENCH_SOLVER` | `greedy` | grouped-arm partitioner (`greedy` \| `mtkahypar`) |
| `BENCH_AGENT_TIMEOUT` | 600 | per-agent wall-clock kill (s); 0 disables |
| `BENCH_AGENT_RETRIES` | 3 | inline retries of a failed/empty agent call (API hiccup); 0 disables |
| `BENCH_AGENT_RETRY_BACKOFF` | 5 | seconds between inline agent retries |
| `BENCH_CELL_RETRY_CAP` | 2 | max crash-retries per cell (across resumes) before `PERM_FAIL` |
| `BENCH_STALE_RUNNING_SEC` | 1200 | age past which a `running` row is treated as a definite orphan (no WARNING); below it, reconcile-with-warning |
| `BENCH_HARVEST_EXCLUDE` | `*.bak,*.orig,vendor/*,zig-cache/*,.zig-cache/*,zig-out/*,*.o,*.a` | GLOB paths stripped from `actual` touches; empty disables |
| `BENCH_KEEP_WORKTREES` | unset | when set, keep per-cell slice worktrees after harvest (debug) |
| `BENCH_LOG_ROOT` | `$BENCH_HOME/cell-logs` | per-cell JSONL structured logs |
| `BENCH_ITER_CAP` | 5 | frozen-config identity element; reviewer verdict is *measured* (M-ITER), not an enforced retry loop |
