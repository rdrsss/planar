# Log: the confirmatory-run campaign (2026-06-22 → 2026-06-28)

*Written 2026-07-03, from the session record. This is the unpolished account of
getting the closure-measurement confirmatory matrix from "harness built" to
"valid data" — including the $165 run that measured nothing. The companion
[`2026-07-03-instrument-review.md`](2026-07-03-instrument-review.md) covers the
problems we only found* after *declaring victory.*

## Cast

- **The experiment:** 3 corpus plans (659/668/678, a coupling gradient over the
  external `git-fleet` repo) × 3 arms (strict / eligibility / grouped) × N=5
  reps. Frozen protocol in `../preregistration.md`.
- **The instrument:** `scripts/bench-matrix.sh` — a bash harness that, per cell,
  resets a worktree to a pre-feature base SHA, snapshots declared touches,
  dispatches headless `claude` coder+reviewer agents per the arm's shape,
  commits + harvests actual touches, and records to the `runs`/`run_events`/
  `run_touches` tables.

## Episode 1 — the $165 run that recorded nothing (06-22 → 06-24)

The harness was built as M0–M3 + an M4 "unattended hardening" pass, each
reviewer-gated, all tests green. We launched the full 45-cell matrix. It ran
~30 hours, completed all 45 cells with exit 0, spent **$165.71** — and recorded
**375 declared touches and 0 actual touches**. The headline measurement (RQ1
declared-vs-actual) was completely empty, and *nothing had failed loudly*.

That is the single most important lesson of the campaign: **a measurement
pipeline can be green at every gate and still measure nothing.** Every unit
test passed. Every cell "completed." The reviewers approved. The data was void.

## Episode 2 — the six-layer bug cascade (06-24, ~$20 in validation cells)

Root-causing the void peeled back six bugs, each *hidden behind* the previous
one — none of them findable until its predecessor was fixed. Discovery
methodology that worked: **single-cell live validations (~$3–5 each) between
every fix**, checking the actual DB rows (not exit codes) after each.

1. **Agents don't commit; harvest reads committed state.** The coder brief said
   "commit your work"; headless agents mostly didn't. Some left 3–8 uncommitted
   edits, some left none; harvest (deliberately committed-only, to agree with
   the conflict detector) saw nothing. *Fix: the harness commits the agent's
   worktree itself (`commit_agent_work`). Lesson: never make measurement depend
   on agent compliance.*
2. **Zero-touch cells looked like success.** A cell with no actual touches
   completed silently. *Fix: a loud zero-touch guard. Lesson: instrument the
   absence of data, not just its presence.*
3. **The permission-mode trap.** With `--permission-mode acceptEdits`, agents
   could edit files but **couldn't run bash** — no `zig build`, no `git`. The
   raw transcript (which we only started saving mid-cascade — earlier the
   harness kept only a token-count projection) showed an agent reasoning:
   *"zig build requires approval … this appears to be a measurement baseline
   where no implementation work is needed."* Crippled agents rationalize
   idleness, and the brief's "a no-change task is a valid outcome" escape hatch
   licensed it. *Fix: `--dangerously-skip-permissions` in the throwaway sandbox
   + a brief that says zero edits = unimplemented. Lesson: keep raw transcripts;
   the projection made this undiagnosable for a full campaign.*
4. **Plan-scoped briefs collapsed the arms.** Once agents could work, a
   single-task strict slice implemented the *entire plan* (13 files) — because
   the brief included the whole plan's problem statement. Strict/eligibility/
   grouped all degenerate to "build everything" and the arm comparison is
   meaningless. *Fix: slice-scoped briefs (only the assigned tasks' title+body)
   + explicit anti-sprawl constraints. Lesson — and see the 07-03 review: this
   fix traded one bias for another.*
5. **Base-SHA selection walked to repo genesis.** "Parent of the first commit
   touching a declared path" is wrong for *modified* (vs created) files — for
   `main.zig` that's the initial commit. A live run would have "re-implemented"
   features against an empty skeleton. Caught by the `--dry-run` pre-flight.
   *Fix: operator-pinned per-plan base SHAs + a base-fidelity gate that
   tolerates modify-features.*
6. **Harvest noise.** Agents produced `.bak` backups and occasionally wandered
   into `vendor/`; these counted as "actual touches" and deflated recall. *Fix:
   brief directive + a harvest filter for build artifacts/backups/vendored
   paths.*

Also in this episode: we discovered the corpus's pre-existing declared touches
were **hindsight-contaminated** (one task's declared set was byte-identical to
its actual diff — a giveaway that someone had derived declarations from the
implementation). We wiped them and rebuilt provenance: git-fleet's *original*
author predictions for plans 659/668, a blind declarer agent (no access to the
implementing commits) for 678. Without that catch, RQ1 would have reported a
fraudulent ~1.0/1.0.

## Episode 3 — the API-limit war and the self-healing arc (06-25 → 06-28)

The re-launched campaign was repeatedly perturbed by real API rate-limit /
overload windows. The failure signature: a `claude -p` call returns **empty,
zero tokens** — which the harness initially recorded as a valid "agent chose to
change nothing" no-op. Distinguishing *failed invocation* (no `.usage` in the
response) from *genuine token-bearing no-op* became the load-bearing predicate.

Hardening built mid-campaign, each piece validated before continuing:
- **Inline retry** of failed/empty agent calls (`BENCH_AGENT_RETRIES`, backoff);
- **Slice-crash propagation:** exhausted retries → slice crashed, not clean-empty;
- **Cell-crash propagation:** *all* slices crashed → cell finished `aborted`
  (resume re-runs it) instead of `completed`-empty (resume would skip it) — the
  gap that had silently cost cells;
- **Orphan reconciliation:** `running` rows from hard kills get aborted+retried
  on resume, with a staleness guard and structured audit records;
- Plus process hygiene found the hard way: SIGPIPE at startup once 200 stale
  worktrees accumulated (pipe into early-exiting `awk` under `pipefail`);
  cohort agents surviving SIGTERM as orphans; completed-cell worktrees never
  pruned.

Outcome: **41 of 45 cells with valid data, $143.90**; four cells lost to
*sustained* outages that outlasted the retries (excluded as failed invocations,
leaving four (plan,arm) groups at N=4 — within the pre-registered N=3–5). A
later top-up attempt during a "clear" window failed again — the probe passed
but limits were flapping — which itself validated the self-healing: the cells
self-marked `aborted`, no manual DB surgery, retry budget explicitly tracked.

## What the campaign taught us (the transferable list)

1. **Validate the full measurement path on one live cell before any full run.**
   The $165 void was preventable with one $3 cell + one DB query.
2. **Silent zero is the worst failure mode.** Guard the absence.
3. **Never depend on agent compliance for measurement steps.**
4. **Keep raw transcripts.** Projections destroy diagnosability.
5. **Fixes can introduce bias** — the anti-sprawl fix (bug 4) later turned out
   to contaminate RQ1 itself (see the 07-03 review). Every intervention on
   agent behavior is also an intervention on the measured quantity.
6. **Real-world perturbation (API limits) is a feature of the experiment**, not
   an interruption of it — but only if the instrument distinguishes "the world
   failed" from "the measurement is zero."
7. **Cheap validation cells between fixes** turned a debugging swamp into a
   linear process; total diagnostic spend was ~15% of the wasted run.
