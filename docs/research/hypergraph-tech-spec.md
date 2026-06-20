# Hypergraph Tech Spec (v1)

**Status: v1 / all decisions resolved.** This document owns the seam between the
*formalism* and the *code*. All four bridging decisions (D-HG1–D-HG4) are now
RESOLVED — each section states the resolution and the shipped implementation,
not a deferred option. Promoted from STUB after M3.3 landed the solver seam
(M3.3a encode, M3.3b invoke/parse/degrade, M3.3c repair passes + acceptance).

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
§5 (M3). **Path:** `docs/research/hypergraph-tech-spec.md`

**Implementation:** `src/engine/grouping/mtkahypar.zig` (encode + subprocess
seam + repair passes), `src/engine/grouping/greedy.zig` (the baseline arm +
shared cost source), `src/engine/grouping/load.zig` (DB bridge + degradation),
`src/engine/closure/weight.zig` (`cost` — the dedup-by-symbol union cost both
arms share). Decisions of record: plan-634 decision 519 (D-HG1), decision 520
(D-HG4, which also ratified D-HG2/D-HG3's repair strategy).

---

## 0. Promotion record

This file was a STUB until its two preconditions held:

1. **M2 landed and produced real closures.** The M2.3 reference-edge walk
   (`src/engine/closure/walk.zig`) on real role-partitioned closures resolved
   D-HG1: write-vs-read on a SHARED unit is the *dominant* cross-task sharing
   shape, not a rarity, so role asymmetry is encoded explicitly (decision 519).
2. **The solver seam was chosen against the actual library.** D-HG4 (decision
   520) selected Mt-KaHyPar via an OPTIONAL SUBPROCESS — not vendored, not
   compiled by `build.zig`. The zero-external-library property is preserved; the
   binary is a warn-only RUN_DEP (like `gh`/`rg`).

The greedy fallback (M3.1) deliberately sidestepped D-HG2/3/4 (see §6) so M3
could produce a measured grouped arm before this spec was completed. With the
solver seam and its repair passes shipped, all four decisions are resolved and
this is a v1 tech spec.

---

## 1. The framing constraint (carried from v0.1 §3)

The partitioner is **not** the contribution. Hypergraph partitioning is
solved; mature solvers exist. The novelty is the reframing, the closure
computation, and the empirical result. Therefore every decision below is an
*integration* decision — how to feed an off-the-shelf solver correctly — not
an algorithm-design decision. If a decision starts to look like "invent a
better partitioner," it is out of scope and miscast.

---

## 2. D-HG1 — Closure-to-hypergraph encoding  *(RESOLVED — decision 519)*

**The question.** The formalism says "one hyperedge per context unit." The
implementation must decide the concrete encoding.

**Resolution (decision 519, from the M2.3 walk on real closures):**

- **Vertices are tasks.** Vertex `i` (1-based, ascending `task.id`) has weight
  `Σ w(u)` over the task's distinct units — the dedup-by-symbol sum that mirrors
  `weight.cost`. This is the quantity D-HG3's `k = ceil(Σ weight / B)` block
  count derives from.
- **Hyperedge weights are `w(u)`** (v0.1 §2): for each distinct symbol `u`, the
  **connectivity hyperedge** pins every task whose effective closure holds `u`,
  weighted `w(u)`. Co-locating those tasks zeroes the λ−1 connectivity term for
  `u` — exactly the replication cost spec §2 minimizes.
- **Role survival: encoded explicitly, NOT flattened.** The gate measured the
  M2 corpus and found write-vs-read on a shared unit is the *dominant* sharing
  shape (the high-overlap shared/utility modules the objective most wants to
  co-locate are exactly the ones most likely to receive edits while others only
  call them). Per the spec §2 resolution gate ("if common, role survival is
  load-bearing"), the encoding emits, for each symbol modified by ≥2 tasks, a
  second **write-conflict penalty hyperedge** over the modify-pins only, weight
  `penalty_mult · w(u)` with `penalty_mult` LOW by default (data-driven tuning
  from the corpus; flattening is the empirical fallback). A lone writer is not a
  conflict (no penalty edge); a 1-pin edge contributes nothing to λ−1 and is
  dropped.
- **Member-granular references (M2.2 caveat).** References resolve to the
  specific defining symbol (e.g. `lib.Store.put`, not `lib.Store`) so the role
  signal is not corrupted by false container-level overlap. Transitive units
  stay excluded from the effective closure (v0.1 §1.3).

**Shipped:** `mtkahypar.encode` / `encodeTo` emit the weighted hMETIS file
(`fmt=11`, deterministic byte-stable ordering). The greedy arm
(`greedy.overlapScore`) encodes the SAME asymmetry as a scalar: a write-write
shared symbol earns ZERO overlap credit.

---

## 3. D-HG2 — Dependency ordering vs. partitioning  *(RESOLVED — partition-then-order + cycle-repair)*

**The question.** Mt-KaHyPar partitions an *undirected* hypergraph; it has no
concept of "task A must precede task B." The task DAG's ordering constraint
must be enforced *around* a partitioner that cannot express it.

**Resolution (decision 520; ratified the cheaper of the two strategies):**
**partition-then-order with a post-hoc cycle-repair pass.** The solver
partitions precedence-blind, then a repair pass makes the result schedulable.
Inter-slice cycles are rare for a connectivity-minimizing partition (a cut
that straddles a tight dependency cycle is also a poor connectivity cut), so
the post-hoc repair is the right cost — `constrain-up-front` (fixed-vertex
tricks, chain pre-contraction) was not needed.

**Governing rule (v0.1 §2 constraint 2):** where ordering and
replication-minimization conflict, **ordering wins** — it is correctness;
replication is cost. The repair therefore sacrifices partition quality to
guarantee schedulability.

**Shipped (`mtkahypar.repairPartition` → `mergeCyclicGroups`):**

- Build the inter-slice DAG from the `blocked_by` edges, SAME direction contract
  M3.1/M3.2 use: an `entity_links(from_id, to_id, 'blocks')` row maps to
  `Dep{ blocked = from_id, blocker = to_id }`; the slice-DAG edge is
  `blocker_slice → blocked_slice`.
- If the slice-DAG has a cycle, collapse every strongly-connected component (via
  Tarjan's SCC) of two-or-more slices into ONE slice. The condensation of a
  graph by its SCCs is always a DAG, so the merged result is schedulable by
  construction.
- The cycle-merge runs FIRST; the D-HG3 union-repair (§4) runs after, so a slice
  that the merge pushed over budget is split back under budget. The split cuts
  along a topological order, which can only reintroduce backward dependencies —
  never a cycle — so the result is BOTH schedulable and budget-compliant.

A precedence-blind solver rarely trips this in practice, but the repair makes
the guarantee unconditional and is independently asserted (Kahn topo-check) in
the acceptance tests.

---

## 4. D-HG3 — Window budget → balance constraint  *(RESOLVED — native ε-proxy + post-hoc union-repair)*

**The question.** Mt-KaHyPar balances *block sizes* against an imbalance
parameter ε (each block's vertex-weight sum within ε of average). Our constraint
is different in kind: each slice's **unioned** closure ≤ window budget B.

**Why they're not the same quantity.** Block size *sums* vertex weights.
Unioned closure *deduplicates* shared units — two tasks in a slice that share
a unit pay for it once. So a slice can be "large" in summed weight but "small"
in unioned-closure weight precisely when its tasks overlap heavily — exactly the
case the objective tries to produce. The native balance metric therefore
*mis-measures* our real constraint, and the M2 corpus overlap density is high
enough that the mismatch is real, not negligible.

**Resolution (decision 520):** the **native ε-balance proxy + a post-hoc
union-repair pass**, NOT a custom solver metric (which would couple us to a
solver-internal API the subprocess seam deliberately avoids) and NOT the
conservative sum-upper-bound (which wastes window headroom). The proxy gives the
solver a sane starting balance (`default_epsilon = 0.03`); the repair restores
exact budget by construction.

**Shipped (`mtkahypar.repairPartition` → `splitOverBudget`):**

- After parsing the partition, compute each block's TRUE union cost via the SAME
  dedup-by-symbol accumulation `weight.cost` / `buildSlice` use (`unionCostOf`).
- For any block whose true union > B, SPLIT it: order the block's members in a
  dependency-respecting topological order, then cut the sequence into
  consecutive chunks, each grown until adding the next member would exceed B.
  Exact budget by construction — mirroring greedy's guarantee — and ordering is
  respected (consecutive chunks of a topo order never form a cycle, so D-HG2's
  schedulability is preserved). A lone task whose own closure already exceeds B
  is emitted as a singleton (cannot be split smaller — same as greedy).

This reuses greedy's exact `weight.cost` dedup, so the solver arm's per-slice
cost is the SAME quantity greedy reports.

---

## 5. D-HG4 — Partitioner integration seam  *(RESOLVED — decision 520, Mt-KaHyPar via optional subprocess)*

**The question.** The concrete binding to the solver, written against the actual
library.

**Resolution (decision 520, the M3.3 design gate):**

- **Which solver: Mt-KaHyPar.** Shared-memory parallel, actively developed,
  km1 (connectivity) objective.
- **Not vendored, not compiled by `build.zig`.** Mt-KaHyPar is C++14/CMake/TBB/
  Boost — not a `build.zig`-compilable amalgam like sqlite/lua/tree-sitter.
  Full-source vendoring was rejected as intractable; an in-process C++/TBB
  binding was rejected because it breaks the zero-dep + cross-compile property.
  `build.zig` is UNTOUCHED; the zero-external-library property holds.
- **Optional RUN_DEP (warn-only).** Like `gh`/`rg`: when `mtkahypar` is absent,
  `groups recommend` degrades to the M3.1 greedy arm and reports
  `optimal_available:false`. No committed binary; the operator builds from
  source and the README documents it. When the optimal arm DID run but greedy's
  total cost was strictly lower, `selected_greedy:true` is set in the JSON output
  (the greedy result is shipped as the safe cost floor; the solver's partition is
  freed). `selected_greedy` is always false when `optimal_available` is false.
- **Invocation: subprocess.** Matches the `planar`-shells-out idiom and keeps
  the engine's C/C++ surface bounded to the vendored SQLite. `solverAvailable`
  probes `mtkahypar --help`; `invoke` writes the hMETIS encoding to an isolated
  temp dir, runs the solver with `-o km1 -m direct`, scans for the
  version-suffixed partition file, and parses it.
- **Input format: weighted hMETIS** (`fmt=11`; D-HG1 vertex + hyperedge
  weights). **Output parse:** one 0-based block id per vertex in
  ascending-task-id order → grouped into `greedy.Slice`s, then run through the
  D-HG2/D-HG3 repair passes (§3, §4).

**Shipped:** `src/engine/grouping/mtkahypar.zig` (the whole seam) behind
`groups recommend --solver=mtkahypar`; swappable with the greedy arm because
both emit `greedy.Slice` in the identical shape.

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

## 7. Resolution checklist (all resolved)

- [x] **D-HG1** role survival — RESOLVED (decision 519): explicit per-role
  hyperedges + a LOW-default write-conflict penalty edge over modify pins.
  Member-granular references; transitive excluded. (§2)
- [x] **D-HG2** ordering strategy — RESOLVED: partition-then-order +
  post-hoc cycle-repair (Tarjan SCC collapse), ordering wins. (§3)
- [x] **D-HG3** budget mapping — RESOLVED: native ε-balance proxy + post-hoc
  union-repair (topo-ordered split, exact budget by construction, reuses
  `weight.cost` dedup). (§4)
- [x] **D-HG4** solver seam — RESOLVED (decision 520): Mt-KaHyPar via optional
  subprocess; not vendored; `build.zig` untouched; degrades to greedy. (§5)

All four are resolved and shipped (M3.3a/b/c). This file is a v1 tech spec.

## 8. Acceptance: "cost ≤ greedy", tested without the binary

The objective's acceptance criterion (v0.1 §3) is that the optimal arm's total
replication cost is **≤ greedy's on the same input**. This is verified WITHOUT
`mtkahypar` in CI via a **golden recorded-partition fixture**:

- A fixture of tasks + effective closures (+ a DAG where ordering matters) + a
  budget B, and a hand-authored block assignment as if the solver had returned
  it (a partition string, one 0-based block id per vertex in ascending-task-id
  order).
- The fixture is fed through `parsePartition` → `repairPartition` (the full
  D-HG3 union-repair + D-HG2 cycle-repair pipeline) → per-slice `weight.cost`.
- Three assertions: (a) every resulting slice's true union cost ≤ B
  (budget-compliant by construction); (b) the slice-DAG is schedulable
  (independent Kahn topo-check); (c) the solver-arm total cost ≤ the greedy
  arm's cost on the SAME input (`greedy.group` over the identical fixture). One
  fixture is engineered so the recorded optimum STRICTLY beats greedy (it
  co-locates two write-write-sharing tasks greedy refuses to merge, paying the
  heavy shared symbol once instead of replicating it).
- Two further fixtures exercise the repairs directly: an over-budget recorded
  block that the union-repair must split, and a recorded partition that induces
  an inter-slice cycle that the cycle-repair must collapse to a schedulable
  result.

A separate `invoke` test does a live solver round-trip, gated `skip-if-absent`
(`return error.SkipZigTest` when `solverAvailable` is false), so the binding is
exercised wherever `mtkahypar` is installed without making CI depend on it.
