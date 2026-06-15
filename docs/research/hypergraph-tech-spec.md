# Hypergraph Tech Spec (STUB — v0.1)

**Status: STUB / decisions-parked.** This document marks a known gap so it is
not forgotten. It frames the decisions that bridge the *formalism* to the
*code*; it does **not** answer them yet. Each decision below is DEFERRED with
its resolution gate stated. Do not treat a framed option here as a chosen one.

**What this document owns:** the seam between `planar-spec-v0.1.md` §2 (the
hypergraph objective, as math) and the build spec's **M3** (the grouping
build, as tasks). Specifically: how a closure set becomes an in-memory
hypergraph, how dependency ordering is enforced through a partitioner that
has no concept of a DAG, how the window budget maps onto the partitioner's
balance machinery, and the partitioner integration seam.

**What it does NOT own:** the math (that is v0.1 §2 — do not re-derive it) or
the task list (that is build-spec M3 — do not re-list it). This is only the
decisions that sit *between* them.

**Parents:** `planar-spec-v0.1.md` §2–§3, `closure-measurement-build-spec.md`
§5 (M3). **Suggested path:** `docs/research/hypergraph-tech-spec.md`

---

## 0. When to complete this stub

Two preconditions gate fleshing this into a real tech spec, and writing it
before them means specifying against things that don't exist yet:

1. **M2 has landed and produces real closures.** Three of the four decisions
   below (encoding, role survival, budget mapping) depend on *seeing* the
   closures the partitioner must consume — their size distribution, overlap
   density, and how often `modify`/`reference` roles actually conflict.
   Specifying against imagined closures produces guesses you will relitigate.
2. **The partitioner is vendored.** Decision D-HG4 must be written against the
   *actual* library and version in `vendor/`, not from memory — KaHyPar vs.
   Mt-KaHyPar differ in interface, parallelism, and license, and these change.

Until both hold, this stays a stub. The greedy fallback (M3.1) deliberately
sidesteps D-HG2/3/4 (see §6), so M3 can begin and produce a measured grouped
arm *before* this spec is completed.

---

## 1. The framing constraint (carried from v0.1 §3)

The partitioner is **not** the contribution. Hypergraph partitioning is
solved; mature solvers exist. The novelty is the reframing, the closure
computation, and the empirical result. Therefore every decision below is an
*integration* decision — how to feed an off-the-shelf solver correctly — not
an algorithm-design decision. If a decision starts to look like "invent a
better partitioner," it is out of scope and miscast.

---

## 2. D-HG1 — Closure-to-hypergraph encoding  *(DEFERRED → M2 closures)*

**The question.** The formalism says "one hyperedge per context unit." The
implementation must decide the concrete encoding.

**Open sub-decisions:**
- **Vertex weights.** Tasks are vertices — weighted by what? Token cost of the
  task's own closure? Unweighted (count balance)? Affects how the balance
  constraint behaves (D-HG3).
- **Hyperedge weights.** Per v0.1 §2 this is `w(u)`, the unit's token weight —
  confirm this survives encoding as the hyperedge weight the solver minimizes.
- **Role survival (the one most at risk).** A unit one task *writes* (`modify`)
  and another only *reads* (`reference`) is a different risk than two readers
  of the same unit. The flat hypergraph sees only "these tasks share unit u."
  Does the encoding preserve the role asymmetry, or flatten it? If flattened,
  the closure definition's central role-partition (v0.1 §1.2) is lost at the
  grouping step. Candidate resolutions: separate hyperedges per role; a
  write-conflict penalty layered on the connectivity cost; or accept the
  flattening and document it as a precision ceiling.

**Resolution gate:** inspect real M2 closures — measure how often a shared unit
is write-vs-read across tasks. If rare, flattening is cheap; if common, role
survival is load-bearing and needs explicit encoding.

---

## 3. D-HG2 — Dependency ordering vs. partitioning  *(DEFERRED → M3; this is OQ-5)*

**The question.** KaHyPar partitions an *undirected* hypergraph; it has no
concept of "task A must precede task B." The task DAG's ordering constraint
must be enforced *around* a partitioner that cannot express it.

**The two strategies, with their failure modes:**
- **Partition-then-order.** Partition freely, then topologically order the
  resulting slices. *Risk:* a partition can place a dependency edge *across*
  two slices in a way that induces a cycle between slices (slice 1 needs slice
  2's output and vice versa), which is unschedulable — requiring a repair pass
  that may undo the partition's quality.
- **Constrain-up-front.** Forbid the partitioner from cutting in ways that
  violate ordering. *Risk:* standard hypergraph partitioners don't support
  precedence constraints natively; this needs an encoding trick (e.g.
  fixed-vertex constraints, or pre-contracting dependency chains into single
  vertices before partitioning).

**Governing rule (from v0.1 §2 constraint 2):** where ordering and
replication-minimization conflict, **ordering wins** — it is correctness;
replication is merely cost. So whichever strategy is chosen, it must guarantee
a schedulable slice-DAG, even at the expense of a higher-cost partition.

**Resolution gate:** decide at M3 against real plan DAGs from the corpus —
measure how often naive partition-then-order actually induces inter-slice
cycles. If rare, the cheaper post-hoc repair suffices; if common, pay for
constrain-up-front.

---

## 4. D-HG3 — Window budget → balance constraint  *(DEFERRED → M2 closures)*

**The question.** KaHyPar balances *block sizes* against an imbalance parameter
ε (each block's vertex-weight sum within ε of average). Our constraint is
different in kind: each slice's **unioned** closure ≤ window budget B.

**Why they're not the same quantity.** Block size *sums* vertex weights.
Unioned closure *deduplicates* shared units — two tasks in a slice that share
a unit pay for it once, not twice. So a slice can be "large" in summed task
weight but "small" in unioned-closure weight precisely when its tasks overlap
heavily — which is exactly the case the objective is trying to produce. The
solver's native balance metric therefore *mis-measures* our real constraint.

**Candidate resolutions:**
- A custom balance/objective that measures unioned closure directly (most
  faithful, most work — verify the vendored solver permits custom metrics).
- Partition under native balance as a proxy, then a **post-hoc repair pass**
  that splits any slice whose true unioned closure exceeds B.
- Conservative vertex weighting that upper-bounds union by sum (simplest,
  loosest — wastes window headroom, may under-pack).

**Resolution gate:** depends on M2 overlap density. High overlap → native
balance badly mis-estimates the budget → custom metric or repair pass earns
its cost. Low overlap → sum ≈ union → the proxy is adequate.

---

## 5. D-HG4 — Partitioner integration seam  *(DEFERRED → vendoring)*

**The question.** The concrete binding to the vendored solver. Must be written
against the actual library, not from memory.

**Open sub-decisions:**
- **Which solver.** KaHyPar (single-threaded, C++) vs. Mt-KaHyPar (shared-
  memory parallel, actively developed). Interface, build surface, and
  **license differ between the two** — confirm the license permits bundling
  before committing.
- **Input format.** The exact hMETIS-style hypergraph input the solver demands;
  how vertex/hyperedge weights (D-HG1) are serialized to it.
- **Invocation.** In-process C++ binding vs. subprocess to a solver binary.
  Subprocess keeps the engine's C/C++ surface smaller and matches the
  `planar`-shells-out idiom, at a serialization cost.
- **Output parse.** Mapping the solver's block assignment back onto task IDs
  and into `groups recommend` output.

**Resolution gate:** write at M3.3, against the vendored library. Pull current
version, license, and Zig-build integration details fresh at that point — they
change, and reciting them stale here would be a liability.

---

## 6. The greedy fallback (build-spec M3.1) — what it lets you skip

The greedy overlap-merge pass exists so M3 can produce a measured grouped arm
*without* resolving D-HG2/3/4:

- It sidesteps **D-HG4** entirely (no solver).
- It handles **D-HG3** trivially: merge tasks by descending closure overlap,
  stop a slice when its *actual unioned closure* hits B — the union is computed
  directly, so the budget is exact by construction, no balance-metric mismatch.
- It handles **D-HG2** by refusing any merge that would violate ordering.

What greedy does **not** give you is *optimality* — it is a heuristic with no
quality guarantee, which is exactly why the solver (and this spec) exist: to
show how much further an optimal partition pushes the replication cost below
the greedy baseline. So greedy unblocks measurement; this spec, completed at
M3, is what turns "grouping helps" into "grouping helps, and here is the gap
between heuristic and optimal."

D-HG1 (role survival) is the one decision greedy does **not** let you skip — a
greedy merge still has to decide whether write/read asymmetry affects merge
eligibility. Resolve D-HG1 first, at M2; the rest can wait for the solver.

---

## 7. Stub checklist (resolve in this order)

- [ ] **D-HG1** role survival — at M2, from real closures (also needed by greedy)
- [ ] **D-HG2** ordering strategy — at M3, from real plan DAGs
- [ ] **D-HG3** budget mapping — at M2/M3, from overlap density
- [ ] **D-HG4** solver seam — at M3.3, against the vendored library

When all four are resolved, promote this file from STUB to a v1 tech spec.
