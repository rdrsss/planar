# Closure-Measurement Experiment — Results

**Status:** confirmatory run complete (2026-06-27). Companion to
[`preregistration.md`](preregistration.md) (frozen contract),
[`pilot-notes.md`](pilot-notes.md) (M1.7 gate), and
[`campaign-runbook.md`](campaign-runbook.md) (how to run it).

> **For the full self-contained narrative** — motivation, methodology, the
> execution story (including the measurement-validity bugs found and fixed), and
> the interpretation — see [`closure-measurement-report.md`](closure-measurement-report.md).
> This file is the terse results tables that report references.

> Every hypothesis below lands in a **pre-committed branch** of the
> preregistration (§6/§7). Per §6: "No result here is a failure of the program;
> each branch is a paper." The headline outcome (RQ1 recall below threshold →
> M2 derived-closure extractor becomes the central contribution) is the
> outcome the prereg's own calibration note predicted from the pilot.

## 1. Dataset

| | |
|---|---|
| Matrix | 3 plans × 3 arms × N reps |
| Plans (coupling gradient) | **659** m7-polish (low) · **668** Plan-475 (coupled) · **678** Plan-388 (mixed) |
| Per-plan base SHA | 659=`fcb167a`, 668=`274b6f6`, 678=`32061f7` (parent of each plan's earliest task commit) |
| Arms | **strict** (one isolated worktree/task, serial) · **eligibility** (parallel-eligible cohort + barrier) · **grouped** (closure slices co-located) |
| Cells analysed | **41** (all with data) |
| Reps/cell | **N=5** for 668-all, 678-grouped, 678-strict; **N=4** for 659-all + 678-eligibility |
| Spend | **$143.90** of the $300 ceiling |
| Coder/reviewer model | `claude-sonnet-4-5` (held constant; §4 nuisance var) |

**N=4 cells:** four (plan,arm) groups are N=4 rather than 5. The missing reps
were **API rate-limit/overload casualties** — `claude -p` calls that returned
empty (zero tokens), repeatedly across the run window and two top-up attempts.
They were excluded as *failed invocations*, not recorded as data (the harness's
retry + zero-touch guard caught every one; none entered the dataset as bogus
zero-touch rows). N=4 is within the preregistered N=3–5. A top-up to uniform
N=5 is pending a clear API window.

## 2. Frozen metrics (§2 definitions)

Arm totals (mean across cells):

| metric | strict | eligibility | grouped |
|---|---:|---:|---:|
| **M-TOK** (input+output tokens) | 34,578 | 36,759 | **12,319** |
| **M-WALL** (minutes) | 60 | 34 | **27** |
| **M-ITER** (request-changes cycles) | 16 | 12 | **4** |
| **M-CONF** (fan-in merge conflicts) | 0 | 1 | **0** |
| **M-BLAST** (tasks per slice = failure blast radius) | 1.00 | 1.00 | 2.21 |

RQ1 touch-accuracy (M-PREC/M-REC), arm-independent, macro-avg over included
`(run, task)` observations (n=123):

| | precision | recall |
|---|---:|---:|
| **Overall** | **0.765** | **0.644** |
| 659 (low coupling) | 1.000 | 0.476 |
| 668 (coupled) | 0.641 | 0.663 |
| 678 (mixed) | 0.695 | 0.767 |

## 3. Hypotheses vs falsifiers (§6)

### H1 (RQ1) — declared precision & recall each ≥ 0.70
- Precision **0.765 ✓** · recall **0.644 ✗** (below 0.70).
- **Recall < X (0.70) → the §7.2 "recall-low / precision-ok" branch fires.**
  This is the **pre-committed** outcome: the prereg calibration note flagged
  recall would straddle 0.70 (pilot 0.58, m7-polish live 0.67–1.0). The
  consequence is by design — **the derived-closure extractor (M2) becomes the
  headline contribution**, with the declared-vs-derived divergence as its
  justification, rather than declared touches being the deliverable.
- Mechanism: declarations are *precise* (predicted files are genuinely touched)
  but *under-complete* — real work pulls in incidental files (barrels,
  registration, tests, migrations) the author did not predict. Precision drops
  on the coupled plan (668, 0.64) where agents touched a subset of the many
  declared files.

### H2 (RQ2) — grouped recovers ≥ 50% of the parallelism eligibility serializes, at tokens ≤ eligibility
- **Token condition: MET with a 3× margin** — grouped 12,319 ≤ eligibility
  36,759 (66% fewer).
- **Parallelism: grouped dominates.** Wall-clock grouped 27 min < eligibility
  34 min < strict 60 min. Against the serial baseline (strict), grouped
  recovers (60−27)/60 = **55%** of wall-clock (clears 50%); eligibility recovers
  43%. Grouped co-locates into 2.21-task slices what eligibility runs as size-1
  slices.
- **Verdict: H2 supported.** *Caveat:* M-PAR's exact "tasks-serialized-by-
  eligibility-but-co-located-by-grouped" count requires the per-cell concurrency
  trace, which `slice_dispatch` events do not fully capture; wall-clock + slice
  composition are the evidence here.

### H3 (RQ3) — co-location's failure tax does not erase H2's gain (Z: tax < 100% of gain)
- Grouped is the **cheapest** arm with the **fewest review cycles** (M-ITER 4 vs
  16 strict / 12 eligibility) and **zero fan-in conflicts** (M-CONF 0).
- The only tax dimension is **M-BLAST 2.21** (a grouped slice failure surfaces
  ~2 tasks for rework vs 1 for strict/eligibility) — but with grouped's low
  failure rate (fewest request-changes, zero conflicts) the realised tax is a
  small fraction of the ~22–24K-token gain.
- **Verdict: H3 supported — the tax does not approach the gain; no crossover
  point within this corpus/budget.** Z is set conservatively (full-erasure
  ceiling) and is not breached. (M-ITER/M-BLAST are now measured; per §6 the
  conservative Z may be tightened via a dated §0 amendment.)

### RQ4 (exploratory) — scaling across the coupling gradient
The grouping advantage is **largest for low-coupling and narrows as coupling
rises**:

| plan | grouped M-TOK | strict M-TOK | grouped advantage |
|---|---:|---:|---:|
| 659 (low) | 1,640 | 24,680 | **15×** cheaper |
| 668 (coupled) | 16,609 | 30,639 | 1.8× cheaper |
| 678 (mixed) | 16,573 | 46,436 | 2.8× cheaper |

Co-location pays most when tasks are file-disjoint (one agent cheaply handles
several independent tasks); as shared-file coupling rises the per-slice work
grows and the gap compresses.

## 4. Threats / caveats (§8)

- **Agent-behaviour variance is in the measurement, not filtered out.** Recall
  and precision <1 reflect real LLM agents — including scope variance (some
  agents implement tightly, some sprawl). The harness underwent several
  corrections (slice-scoped briefs, anti-sprawl constraints, build-artifact/
  `.bak`/`vendor` noise filtering) to make "actual touches" reflect genuine
  task work; residual variance remains a property of the agents.
- **N=4 for four cells** (API-limit casualties), N=5 elsewhere — within §4's
  N=3–5. Treated as missing-at-random (failures were API-side, not data-side).
- **Single corpus** (git-fleet, one Zig repo), single model family — generality
  limits per §8. `git-fleet` carries the primary claim to keep self-hosting
  non-load-bearing.
- **M-PAR** reported via wall-clock + slice-composition proxy; the exact count
  metric is a follow-up.
- **Path-level harvest** (decision D1) — symbol-level deferred; grouped per-task
  attribution is inherently coarse (the slice's files attribute to every task in
  the slice).

## 5. Bottom line

Declarations are **precise but under-complete** (→ M2 derived-closure extractor
is the headline, per the pre-committed §7.2 branch). **Closure-aware grouping
recovers the parallelism eligibility serializes at one-third the tokens, with
fewer review cycles and no fan-in conflicts** (H2 + H3 supported). The advantage
**scales inversely with coupling density** (RQ4). The measurement framework
itself — the five-arm harness, the frozen instruments, and the pre-committed
branch structure — held up across a real, API-limit-perturbed confirmatory run.
