# Log: post-hoc instrument review — what the "complete" experiment got wrong (2026-07-03)

*Written 2026-07-03. After the campaign was declared complete and the results
written up (`../results.md`, `../closure-measurement-report.md`), a critical
pre-paper review of the docs + raw data found four significant problems — one
of which invalidates a reported finding and one of which reverses a hypothesis
verdict. This entry records them before the corrective analysis lands, so the
record shows what was believed, when, and why it changed.*

**Status of the findings below: identified 2026-07-03; corrective analysis and
harness fix in progress. `results.md` §-references are to the pre-correction
version.**

## Finding A — M-WALL is a structural artifact (invalidates the wall-clock result)

**Evidence.** 39 of 41 cells have wall-clock durations of *exactly* 20, 40, or
60 minutes (±3 s); the two exceptions are retry cells. Meanwhile the agents'
own `duration_ms` in raw transcripts is 390–480 s — under the 600 s watchdog
slot, and *variable*, as real work should be.

**Diagnosis.** Each agent invocation blocks for the **full 600 s watchdog
window** regardless of when the agent finishes (the watchdog sleeper isn't
reaped on success). So a slice = 2 agents = exactly 1200 s, and
M-WALL = 1200 s × (number of serial slice-waves): a deterministic function of
the arm's *structure*, with zero empirical content. The reported
"grouped 27 min < eligibility 34 < strict 60" ordering is real *only* in the
sense that it counts serial agent invocations — the "55% wall-clock recovery"
number is structurally guaranteed, not measured.

**Why our tests missed it.** The M4 watchdog tests used tiny timeouts (2 s) and
asserted *upper* bounds ("returns within ~deadline"), never that a fast agent
returns *fast*. An assertion like "1 s stub with 600 s timeout must return in
< 10 s" would have caught it immediately.

**Consequences.** (1) Withdraw/reframe every wall-clock claim (RQ2's
parallelism leg, H2's 55% figure). (2) Fix the harness (reap the watchdog on
agent exit) — this also makes future campaigns ~8× faster in wall time; the
overnight runs were mostly the harness sleeping. (3) The campaign's "cells take
20–60 min" pacing that we repeatedly "diagnosed" as normal was the bug, in
plain sight, the entire time.

## Finding B — the live RQ1 construct is contaminated in *both* directions (reverses the H1 verdict)

Computing RQ1 **per arm** (never done before this review):

| arm | precision | recall |
|---|---:|---:|
| strict | 0.736 | **0.796** |
| eligibility | 0.675 | 0.749 |
| grouped | 0.679 | **0.394** |

Two artifacts, opposite signs:

1. **Grouped recall (0.39) is deflated by attribution coarseness** — a grouped
   slice's files attribute to *every* task in the slice, inflating each task's
   "actual" set. Known limitation; its *magnitude* wasn't appreciated.
2. **Strict recall (0.80) is inflated by our own bug-4 fix.** The anti-sprawl
   brief instructs agents to "touch the minimum files for THIS task," and the
   task bodies often *name* the declared files ("lift parsing into
   manifest.zig"). We steered agents toward the declared set, then measured how
   well the declared set predicts where agents went. Partial circularity,
   introduced deliberately (and necessarily — without it the arms collapse) but
   never connected to its effect on RQ1.

**The reported pooled figure (P 0.765 / R 0.644) averages these two artifacts**,
and the published verdict — "recall 0.644 < 0.70, H1 fails on recall, §7.2
branch fires" — is **not supported by the live data as analyzed**: the clean
strict-only subset *clears* the threshold (0.796 ≥ 0.70). The honest position:

- The **uncontaminated** RQ1 evidence remains the retrospective pilot
  (P 1.00 / R 0.58 against real *human* implementation history, no agent in
  the loop) — which *does* support systematic under-declaration.
- The **live** RQ1 should be reported as construct-compromised (both artifacts
  disclosed), or re-run with a de-circularized protocol (briefs stripped of
  file names; strict-arm attribution only).
- The §7.2 "M2-as-headline" pivot can still be argued from the pilot, but not
  from the confirmatory pooled number as previously claimed.

## Finding C — H2's parallelism leg was never actually measured

The pre-registration defines H2's Y = 50% threshold on **M-PAR** (tasks
eligibility serializes that grouped co-locates). `parallelism_recovered.sql`
was never written; M-PAR was never computed; the results substituted wall-clock
recovery — which Finding A invalidates. **H2's support currently rests entirely
on the token condition** (M-TOK grouped ≪ eligibility, 3× margin — which is
real and robust). M-PAR *is* computable post-hoc from the recorded
`slice_dispatch` events (eligibility's serialized-vs-cohort tags + grouped's
slice compositions); doing so is part of the corrective pass.

## Finding D — no functional-quality verification of the implementations

The prereg's arm-independent "definition of done" is the objective gate
(`zig build` clean, `zig fmt`, tests). **The harness never recorded gate
outcomes** — the matrix event log contains no `gate` events at all. So "grouped
did the same work for ⅓ the tokens" has an unverified "same work" leg. Weak
proxies point the right way (grouped had the best reviewer-approve rate;
declared-coverage precision is comparable across arms), but this is a real gap
that needs gate recording in any future run.

## Smaller compliance gaps (prereg §4/§5/§7 vs what was published)

- Results reported **means**; the prereg demands **medians + spread** and sign
  tests. Neither was computed.
- **Pseudoreplication unacknowledged:** "n=123 task-observations" are 9 tasks ×
  ~14 cells with the *declared side identical* across all cells. The effective
  n for declaration quality is **9 tasks**.
- **4 of the 8 frozen metric queries were never written** (parallelism_recovered,
  blast_radius, reviewer_iters, coupling_density).
- **RQ4's x-axis (coupling density) was never computed** — qualitative
  low/coupled/mixed labels were used; the prereg defines a numeric shared-unit
  ratio, computable from the (populated, 4,305-row) `closures` table.
- H3 was never stressed: no rework loop exists in the harness, so the
  co-location "tax" could not have been paid in any arm. H3's support is
  weak-by-construction.

## What survives untouched

- **M-TOK** and dollar cost (real telemetry): grouped ≈ ⅓ the tokens of either
  other arm, consistent across plans — the strongest result.
- **Precision** (declared touches are precise): ≥ 0.67 in every arm, 1.00 in
  the pilot.
- The RQ4 token-slope *direction* (grouping advantage shrinks with coupling).
- The provenance work, the pre-registration discipline, the archived raw data,
  and the instrument itself as a contribution.

## Meta-lesson

The campaign log's lesson #5 ("fixes can introduce bias") turned out to be the
review's biggest finding — the anti-sprawl fix that *saved* the arm comparison
*contaminated* the accuracy measurement. In an experiment where the instrument,
the subject, and the measured quantity are all LLM-agent behavior, **every
intervention is a treatment**. The corrective discipline that worked: compute
every metric *per stratum* (per arm, per plan) before pooling — the pooled
number hid two opposing artifacts that the per-arm table exposed in one glance.
And: **suspiciously clean numbers are the strongest bug signal there is.**
Exactly-20-minute cells and an exactly-diagonal RQ1 both survived multiple
"everything looks healthy" checks because they looked *good*, not *wrong*.
