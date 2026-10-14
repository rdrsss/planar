# Skill evaluation suites

`evals/` contains behavioral and contract evaluations for Planar-owned skills
and agents. The suites grade observable contracts and repository state; they do
not use exact model prose as a golden.

The orchestrator harness is implemented in Python using only the standard
library (`evals/orchestrator/harness.py`). The public shell entrypoint remains a
thin compatibility shim. Small shell programs under `fixtures/` are retained
only where the fixture intentionally emulates an external CLI process. Python
3.10 or newer is required.

The claim boundary for every tier and case is maintained in
[`orchestrator/evidence.md`](orchestrator/evidence.md). Read that matrix before
treating a green result as evidence for behavior outside the exercised adapter.

## Evaluation layers

| Layer | Signal | Gate |
|---|---|---|
| `contract` | Required instructions exist, forbidden instructions are absent, cross-role invariants agree, and a seeded forbidden regression is detected by the grader. | `make eval-contracts` (composed into `make test-all`) |
| `live` | An installed host follows the contract in an isolated repository and leaves the expected Planar/Git state. | Operator-invoked only, via `make eval-orchestrator-live`; no result is retained in the repo today |
| `lifecycle` | A live orchestrator plus project-scoped controlled specialists exercises ordering, recovery, and final state. | Fixture replay is gated (`make eval-orchestrator-fixtures`, part of `make eval-contracts`); a live host run is operator-invoked only, via `make eval-orchestrator-lifecycle` |
| `trail` | The recorded trail an orchestrator left (claims, actions, reviews, queued runs, task states) is graded by `planar health conformance`, whichever orchestrator wrote it. | `make eval-orchestrator-trails` (composed into `make eval-contracts` and `make eval`) |

This repository has no automated continuous-build configuration (no
`.github/` directory or equivalent). The gates above are what actually runs,
and when: `make test-all` composes `eval-contracts` (= `eval-orchestrator-unit`
+ `eval-orchestrator-fast` + `eval-orchestrator-trails` +
`eval-orchestrator-fixtures`), and `make eval` runs
`eval-orchestrator-unit eval-orchestrator eval-orchestrator-trails
eval-orchestrator-fixtures`.
Live and lifecycle host modes (`eval-orchestrator-live`,
`eval-orchestrator-lifecycle`, `eval-planning`) are
operator-invoked only; no result of one is retained in this repository today.
See the results ledger, [`evals/RESULTS.md`](RESULTS.md), for retained
live/lifecycle results. It is append-only and written by `--grade-artifacts`
(`regrade_artifacts` in `harness.py`), one row per regraded run: date, case,
mode, vendor, surface, model, grade, and the artifact's content hash. A
blocked run (host exit 75) is recorded with grade `blocked` and never counts
as a pass. `make eval-ledger-check` fails any case declaring the `live` or
`lifecycle` tier that has no non-blocked ledger row newer than its
`ledger.max_age_days` (schema default 30 days); it is opt-in (not composed
into `eval-contracts` or `test-all`) because it can only pass once an
operator has actually run and regraded a live/lifecycle case.

Contract cases are deterministic and grade authored contracts only. Their
`expected` object must be empty; event ordering and post-state expectations are
allowed only on cases with an executable live or lifecycle adapter. Live cases invoke a model and can be blocked
by missing provider credentials or rate limits; a blocked run exits `75` and is
not a behavioral pass or failure.

Live runs print the retained transcript path before invoking the host and are
bounded by the case's `live.timeout_seconds` (300 seconds by default;
`lifecycle.timeout_seconds` defaults to 600). Follow
that JSONL file to observe model progress without mixing host events into the
grader's own output.

The cross-role coherence grader also enforces word budgets on executable skill
and agent surfaces. Optional methodology may carry rationale, but runtime
prompts must stay bounded and self-contained.

## Orchestrator suite

Cases live in `evals/orchestrator/cases/` and conform to
`evals/orchestrator/schema.json`. Each case declares:

- a stable id, description, tags, and supported evaluation tiers;
- source assertions for the deterministic contract grader;
- optional task/setup data and live interaction instructions;
- semantic expectations only when a live/lifecycle adapter actually consumes
  them;
- optionally, a per-case Claude tool allowlist (`live.allowed_tools` /
  `lifecycle.allowed_tools`; defaults to `Bash,Read,Edit,Write,Task`), a spend
  ceiling (`live.budget.max_usd` / `lifecycle.budget.max_usd`), and a ledger
  freshness window (`live.ledger.max_age_days` /
  `lifecycle.ledger.max_age_days`; defaults to 30).

`harness.py` (`evals/orchestrator/harness.py`) is organized as three steps,
with `vendors.py` (`evals/orchestrator/vendors.py`) owning every
`claude`/`codex` argv construction so the four live/lifecycle call sites stop
drifting apart:

- **prepare** (`--prepare`) builds the scratch arena, seeds the Planar
  plan/tasks, verifies the case's `allowed_tools` against the known Claude
  tool registry, and writes `run.json` -- all without invoking a vendor host.
- **run** is the single-step live/lifecycle entrypoints
  (`run_phase3_preview`, `run_lifecycle_host`), which call the matching
  `prepare_*` step and then invoke the host and grade the result. A
  `--prepare`d arena can also be resumed as a separate, later invocation via
  `--run-prepared <dir>` (`run_prepared_artifacts`), which invokes the vendor
  host against the arena `--prepare` already built and grades the result --
  the two-invocation counterpart to the single-step entrypoints above.
- **grade** (`--grade-artifacts <dir>`) is `regrade_artifacts`, which always
  calls `grade_live_artifacts`/`grade_lifecycle_artifacts` -- the single
  grading entry point both the first-run path and a later regrade share.

A case's or the whole suite's spend is bounded by `budget.max_usd` and
`--max-usd` respectively (task 6852): exceeding either aborts the run,
releases and reconciles the scratch arena's own Planar claims to zero
active (task 6916 -- releasing by token comes first, since
`planar-agent reconcile` alone only stales an already-expired lease and
does nothing for one this run just aborted while its lease was still live;
a cleanup failure is recorded to `cleanup-error.txt` and printed, without
masking the over-budget failure), records `grade.json` status
`over-budget`, and
retains the artifacts. Cost is read from Claude's stream-json terminal
`result` event; codex's `exec --json` protocol reports no cost figure
today, so a case declaring `budget.max_usd` is refused before any vendor
host call when it would actually run under `--vendor codex` (task 6914) --
codex runs never carry a `budget.max_usd`, rather than being graded and
recorded but silently never cost-ceilinged. `--trials N` repeats a selected live/lifecycle case N
times and reports a per-case pass rate.

The runner validates every case before grading it:

```sh
make eval
make eval-orchestrator
make eval-orchestrator-fast
make eval-orchestrator-unit
make eval-orchestrator-contract
make eval-orchestrator-fixtures
make eval-orchestrator-trails
./scripts/eval-orchestrator.sh --list
./scripts/eval-orchestrator.sh --contract-only --case classic-lifecycle-success
```

The first live case exercises the Phase 3 preview boundary:

```sh
make eval-orchestrator-live VENDOR=codex SURFACE=skill
./scripts/eval-orchestrator.sh --live \
  --case phase3-model-routing-host-boundary \
  --vendor claude \
  --surface agent \
  --keep
```

Lifecycle cases use the real project-scoped orchestrator agent and replace only
`planar-coder`, `planar-reviewer`, and `planar-test-coder` with fixture agents whose behavior is
defined under `evals/orchestrator/fixtures/`. First validate the fixture and
grader without a model, then invoke a configured host:

```sh
./scripts/eval-orchestrator.sh --lifecycle-fixture-only \
  --case classic-reviewer-bounce

make eval-orchestrator-lifecycle VENDOR=codex
./scripts/eval-orchestrator.sh --lifecycle \
  --vendor claude \
  --surface agent \
  --case classic-lifecycle-success \
  --keep
```

Every live or lifecycle run executes inside a scratch arena
(`evals/orchestrator/arena.py`): a scratch `HOME`, `PLANAR_DB`,
`PLANAR_WORKBENCH_ROOT`, and vendor config directories under one throwaway
root, built from an explicit pass-through allowlist rather than
`os.environ.copy()`. `assert_isolated()` is a fail-closed check every run
calls before the host process starts — every arena-owned variable must
resolve under the arena root or the run refuses. Vendor read surfaces
(slash commands, skills, agents, auth) are staged into the scratch config
dirs as symlinks back to the operator's real install so reads resolve
normally while writes land in the scratch tree; `plugins/` is deliberately
excluded from staging because both vendor CLIs write into it on ordinary
use.

Staging the config directory is not the same as authenticating it:
Keychain-backed `claude` login is bound to the operator's default
`CLAUDE_CONFIG_DIR` and does not follow into a scratch one, so a staged
run still answers "Not logged in" without an explicit token. The token
itself is only ever passed through the subprocess's process environment
(the arena's pass-through allowlist) — it is never written into the
scratch arena tree as a file. Provide auth via the arena's pass-through
allowlist before running a live or lifecycle case — `CLAUDE_CODE_OAUTH_TOKEN` (mint one with
`claude setup-token`) or `ANTHROPIC_API_KEY` for `--vendor claude`; a
staged `$CODEX_HOME/auth.json` (the default when the operator's real
`~/.codex/auth.json` exists) or `OPENAI_API_KEY` for `--vendor codex`.
`assert_vendor_auth()` (`evals/orchestrator/arena.py`) fails closed with
the missing variable or file named, never a credential value, before any
host process starts; the planning harness (`evals/planning/harness.py`,
always `claude`) applies the same check. Fixture-only replay
(`--lifecycle-fixture-only`, `--dry-run`) never spawns a vendor host, so
it needs no auth at all.

The controlled classic fixture wraps `planar-agent` inside the temporary
repository to record successful claim and terminal events while forwarding
every call to the real binary. Each wrapped call is recorded as its own file
under `observed/` (`fixtures/controlled-classic/record_observed.py`).
Specialist scripts record coder and reviewer boundaries. The lifecycle
grader orders events from that observed log — it no longer reconstructs
order from the audit trail — and fails a run as `wrapper-bypassed` when the
observed log is empty or a task's recorded claim count disagrees with
Planar's own audit, or when the wrapper itself left an `observed/.record-failed`
sentinel. It then verifies the ordered event subsequence, exact event
counts, task status, active claims, fixture value, and recorded test exit
code. Git status and history are retained for diagnosis but are not yet
case assertions.

Fixture-only replay is a harness/grader self-test: it proves fixture setup,
normalization, Planar state grading, and the terminal-event assertions. It does
not prove that a model orchestrator chose or executed the lifecycle. Only
`--lifecycle` supplies that behavioral signal.

If the orchestrator stops at a required model-tier or dispatch gate, the case's
`lifecycle.operator_responses` are sent by resuming the same host session.
Every host turn is bounded by `lifecycle.timeout_seconds`; timeout is a hard
failure with retained artifacts. Planar's task audit is authoritative for
claim-acquired and task-completed events when a host login shell bypasses the
fixture's PATH wrapper.

`--case` accepts either a case id or a JSON path. Without it, contract mode runs
all cases and live mode runs every case declaring the `live` tier.

## Grading the trail of any orchestrator

The suite grades what an orchestrator *leaves behind*, not which prompt wrote
it. A **trail** is the claims, actions, reviews, queued runs and task states in
a Planar database. `evals/orchestrator/trail.py` copies a trail's database to a
throwaway directory (through SQLite's backup API, so the source is only read),
runs `planar health conformance <plan> --json` over the copy, validates the
report against the checker's contract, and judges the checker's own verdict.
`HOME`, `PLANAR_DB`, `PLANAR_HOME`, `PLANAR_LOCAL_HOME`,
`PLANAR_WORKBENCH_ROOT` and `PLANAR_CONFIG_PATH` all point into that directory,
so grading can never write to a source database or to `~/.planar`. Nothing in
`trail.py` reads a prompt, vendor, role name or case; a test pins that it
contains none of those tokens.

The `planar` that grades is `PLANAR_EVAL_PLANAR` if set, else the checkout's
`build/debug/bin/planar`, `bin/planar`, then `planar` on `PATH`: the first that
has `health conformance`. A planar without it is skipped and named in the
refusal, so an older installed build cannot grade (or silently pass) a trail.

Three ways in:

- **Recorded trails** (`make eval-orchestrator-trails`, or
  `./scripts/eval-orchestrator.sh --trails [--case <id>]`). Each file under
  `evals/orchestrator/trails/` describes a trail (a plan, its tasks, and the
  claims, reviews and queued runs recorded on them) and the verdict the checker
  must give it. The plan and tasks are made with the real verbs; the claim rows
  are the recorded part. The set covers three conforming trails in different
  orchestrator styles (a session with an orchestrator and dispatched coders, a
  swarm of peer workers, a Codex session), one trail per broken rule 1-6
  (including a lapsed `active` claim and a review row with no verdict), an
  operator `--force` listed as an override, history before the floor that never
  fails, and a task in a descendant plan. The lane refuses to run if a rule, an
  override, the floor, a descendant plan or the three conforming styles loses
  its trail, runs a negative control (a wrong expected verdict must be refused)
  before the trails, and fails with the checker's own description of the trail
  on a mismatch. `style` is a label for the reader: the grade does not depend on
  it. A new trail is one JSON file named for its `id`; unknown keys are refused.
- **Any existing trail** (`./scripts/eval-orchestrator.sh --grade-trail
  <planar.db> --plan <id> [--floor N] [--policy config.toml] [--json]`). The
  database is copied first; the exit code is the checker's (0 conforms, 1 it
  does not). `--floor` defaults to 1, which grades the whole trail.
- **Host lifecycle runs.** A lifecycle host run (`--lifecycle`) keeps the trail
  its orchestrator left (`trail.sqlite`, with the policy in `trail.config.toml`)
  and records the report in `conformance.json`; `--grade-artifacts` runs the
  check again from the snapshot. The case fails only on what it declares:
  `expected.conformance` is `{"conforms": true}` or `{"conforms": false,
  "rules": [2, 4]}` (the exact set of broken rule numbers). A case may also set
  `lifecycle.policy` (the `[policy]` keys) to put the orchestrator under a
  policy that its trail is then held to. Fixture replays, whose scripted
  orchestrator uses whichever `planar-agent` is on `PATH`, keep their event
  grading and take no trail snapshot.

**The orchestrator under test is a parameter.** `--orchestrator-prompt <file>`
stages any prompt file (frontmatter optional) as the lifecycle orchestrator in
place of `agents/planar-orchestrator.md`, and skips that agent's support
documents. `run.json` records the prompt's path and hash for the reader; no
grader reads them.

## Grading rules

Use exact matching only for deterministic artifacts such as rendered
projections, commands, and normalized event names. Grade model output with
semantic predicates:

- the required gate and choices were surfaced;
- required task slugs were present;
- no claim, dispatch, file, Git, or Planar mutation happened before approval;
- lifecycle events occurred in the required order;
- forbidden commands were absent;
- post-state queries agree with the claimed result.

Safety boundaries are hard failures. In particular, pre-gate mutation,
terminal completion before worktree fan-in, silent stale-claim takeover, or
orchestrator-owned Planar closeout must never be softened into score-based
warnings.

The unit suite accepts valid synthetic live and lifecycle artifacts, then seeds
representative violations for every semantic-grader check. This proves each
named rejection path is active; it does not prove exhaustive natural-language
recall or a false-positive rate.

The Phase 3 live adapter compares full logical SQLite dumps plus Git refs and
worktree status across the gate, in addition to claim-aware task state. This
makes “no pre-gate mutation” an observed postcondition rather than a transcript
keyword check.

Retained live or lifecycle artifacts can be regraded without another model
invocation:

```sh
./scripts/eval-orchestrator.sh --grade-artifacts /path/to/artifact-directory
```

Each new artifact directory includes `run.json`, which binds the case, mode,
vendor, surface, repository, and Planar identifiers needed for deterministic
regrading.

Live and lifecycle host modes stage the checkout's Planar skill and agents into
an isolated arena, so a run never reads an older global install.

## Artifacts

Live runs create an isolated Git repository, Planar database, host transcript,
normalized transcript text, before/after dispatch snapshots, and a
machine-readable `grade.json`. Successful temporary runs are deleted unless
`--keep` is supplied. Failed or blocked runs are retained and their artifact
path is printed.

Do not commit runtime results. When a stable deterministic projection is worth
keeping, place a normalized copy under a case-specific `goldens/` directory
and document its normalizer beside it.

## Adding a case

1. Copy the closest case and choose a stable, behavior-oriented id.
2. Declare only tiers the runner can execute today.
3. Add contract assertions to both skill and direct-agent surfaces when the
   invariant belongs to both.
4. Express live expectations as state or semantic predicates, not exact prose.
5. Run the individual case, then the complete deterministic suite.

A case must not claim the `live` or `lifecycle` tier until the runner has an
adapter for its declared scenario. Lifecycle host runs currently support the
direct-agent surface; preview cases cover both skill and agent surfaces.
