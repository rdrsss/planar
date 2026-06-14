# Planar — Design Spec v0.1

**Status:** active design · supersedes prior platform-scoped framing
**Scope discipline:** this document defines the *reduced* Planar. Anything not in service of the one-line mandate below is out of scope by default and must justify its inclusion.

---

## 0. Mandate

> **Given a task graph annotated with context-closure metadata, produce window-sized execution slices that minimize context replication while respecting dependency ordering.**

That sentence is the whole project. Planar orchestrates; it does not execute. Execution belongs to host harnesses (Claude Code, Codex, Hermes, Pi, Opencode) or, for measurement, to a minimal instrumented loop owned by Planar solely as a benchmark instrument — never as a product surface.

### What this explicitly is not

- **Not a workflow execution engine.** General-purpose execution (formerly the Lua workflow layer) is removed from the main repo and archived as an independent toy that may *consume* Planar as a downstream client. Execution is commodity; every harness ships it; building it again differentiates nothing.
- **Not a standalone orchestrator product.** The defensible artifact is the *mechanism* and its *proof*, not a platform competing for adoption against distribution-rich incumbents. The codebase is a vessel; if the idea wins it is reimplemented in the host's language.
- **Not (yet) a host-agnostic library.** A universal interface everyone imports is the seductive v1 and the wrong one. Prove the mechanism inside one host first; let the seam fall out of having done it once for real.

---

## 1. The central term: context closure

The vocabulary is borrowed where it is load-bearing. In Nix, a derivation's *closure* is the transitive set of everything required to build it. In lambda calculus, a *closure* is a function together with the captured environment its free variables bind. **Tasks are the functions.**

> **Definition.** A task's **context closure** $C(t)$ is the captured environment the task must hold resident in-window to execute correctly — the minimal set of context units that must be present for the executing model to perform the work without fabricating the parts it cannot see.

Three distinctions carry the precision. Getting them right is most of the definitional work.

### 1.1 Granularity — what is a *context unit*?

A context unit is the atom at which closures are computed and overlap is detected. Candidate resolutions:

| Granularity | Overlap precision | Computability |
|---|---|---|
| File | Coarse — co-located files may share nothing | Trivial |
| **Symbol** (function / type / decl) | High | Tractable via tree-sitter / LSP |
| Semantic chunk | Highest | Expensive, fuzzy |

**v1 decision: symbol-level.** Two tasks that touch the same file may share no symbols; file granularity would report false overlap and the grouping would optimize noise. Symbol resolution is the sweet spot — fine enough for honest overlap, coarse enough to compute deterministically.

### 1.2 Role — not all closure members are equal

Each task partitions its closure by *why* a unit is needed. This trichotomy is what prevents the closure from ballooning into the whole repository; it is how a human scopes "what do I need open to do this ticket."

- **Modify** — units the task edits. Must be fully resident and writable.
- **Reference** — units the task calls or depends on but does not change. Frequently only the *interface* (signature, type) need be in-window, not the full body.
- **Transitive-beneath** — units below the referenced ones. Usually **not** required: the model need not see the implementation three hops down to correctly call the function in front of it.

The closure is `modify ∪ (interfaces of reference)`, with transitive-beneath excluded unless promoted. This role partition is the lever that keeps closures small.

### 1.3 Maximal vs. effective closure

The full transitive closure is the codebase — useless. What the objective consumes is the **effective closure**: the minimal sufficient context set for correct execution. This set is under-determined, which *is* the static-vs-inferred research question (§4), not a preliminary to it. The effective closure is the hard object; everything downstream assumes it can be computed.

---

## 2. Formalization

Let context units be $u$, each with a token weight $w(u)$. For a task $t$, its effective closure $C(t)$ is a set of units.

Model each unit as a **hyperedge** over the tasks that require it:

$$e(u) = \{\, t : u \in C(t) \,\}$$

Partition the task set into slices $S_1, \dots, S_k$. A unit must be loaded into **every slice containing any task that needs it**, so it is loaded

$$\bigl\lvert \{\, S_i : S_i \cap e(u) \neq \emptyset \,\} \bigr\rvert$$

times. **Total context cost** of a partition:

$$\mathrm{Cost} \;=\; \sum_{u} w(u) \cdot \bigl\lvert \{\, S_i : S_i \cap e(u) \neq \emptyset \,\} \bigr\rvert$$

**Reference points:**

- **Ideal floor** — each unit loaded exactly once: $\sum_u w(u)$.
- **Naive serialization** — each unit loaded once *per task* that needs it; every overlap paid for repeatedly. This is the baseline the mechanism must beat.

**Objective:** minimize $\mathrm{Cost}$ subject to

1. **Window budget** — the unioned closure of each slice fits the context budget $B$: $\;w\!\left(\bigcup_{t \in S_i} C(t)\right) \le B$ for all $i$.
2. **Dependency ordering** — slice ordering respects the task DAG; no task is scheduled before its prerequisites.

This is **balanced hypergraph partitioning with a replication-cost objective**: minimize the weight of hyperedges cut across slices, with the window as the balance constraint.

---

## 3. Novelty framing (read before writing the paper)

Hypergraph partitioning is decades-old machinery; mature solvers exist (hMETIS, KaHyPar). **Do not position the contribution as a new grouping algorithm** — that claim is indefensible and reviewers will dismantle it. The novelty is threefold, and none of it is the partitioner:

1. **The reframing.** Casting LLM work decomposition as context-replication-minimizing hypergraph partition, with the balance constraint set to the context window rather than to human-sprint sizing. This is the conceptual move — the redefinition of the "vertical slice" for swarm executors where context is the binding resource.
2. **Closure computation.** The method for deriving effective closures (§1.3, §4). This is the genuinely unsolved piece and the locus of defensibility.
3. **The empirical result.** Measured reduction in total context loaded versus a named baseline.

Strategic consequence: lean on an off-the-shelf partitioner so that engineering time concentrates on (2), where both the difficulty and the moat reside.

---

## 4. The v1 closure-computation decision

**v1 uses static / deterministic closures, not model-inferred closures.** Closures are derived by static dependency analysis over the task graph and repository (symbol-level, via tree-sitter or LSP).

Rationale:

- **Reproducibility** — a benchmark requires determinism; inferred closures are non-deterministic and contaminate the measurement.
- **Clean baseline** — the static variant becomes the control against which an inferred variant can later be measured.
- **Isolation** — inference now would entangle two unproven things (does grouping help? does inference compute closures well?) into one result.

Model-inferred closures are the **second** experiment — "does inference beat static dependency analysis at overlap detection?" — and the paper is stronger precisely because the deterministic version was held as control. Deferred, not abandoned.

---

## 5. Roadmap

Re-orient against this. The mandate (§0) is the filter: if a task does not serve it, it is out.

1. **Excise & rescope.** Remove the Lua execution layer from the repo; archive the toy as an independent project (may consume Planar as a client). Make it a clean cut, not a gradual deprecation — a half-removed engine keeps attracting maintenance. Rewrite Planar's one-line definition to §0.
2. **Pin the formalization.** Commit §1–§2 as the spec/method section: context unit, role partition, the hypergraph objective, the metric. Cheap — prose and notation. Do it before code so the code has a target. *(This document is the seed.)*
3. **Closure computation — the long pole. Attack first after the spec.** Build the static extractor: parse a real task graph over a real repo; emit $C(t)$ per task at symbol granularity with `modify` / `reference` / `transitive-beneath` roles. Everything downstream is blocked here; if closures are wrong, the grouping optimizes noise. **Budget the most risk to this step.**
4. **Grouping.** Feed closures into an off-the-shelf hypergraph partitioner (KaHyPar) with the replication objective and the window budget as balance constraint. A greedy overlap-merge is a fine first pass to sanity-check before reaching for the solver.
5. **Measurement rig.** The minimal *instrumented* executor — run a slice against a model, count tokens loaded and reused, emit the delta. No Lua, no workflow engine; just enough to produce the number. This is the instrument; it must survive the §1 excision, not be swept out with it.
6. **Baseline & result.** Dependency-topological ordering as the control. Run grouped vs. control on the corpus (§6). Produce the number.
7. *(Deferred.)* Host-agnostic seam + writeup. Downstream of having a result.

**Critical path:** 2 → 3 → 4 → 5 → 6. **Step 3 is where the project lives or dies.** First real keystroke after the excision: the closure extractor on one concrete repo.

---

## 6. Corpus requirement (the step people skip and then stall on)

The result needs a **real task graph over a real codebase** with enough closure overlap to measure. Do **not** synthesize one — a corpus with hand-tuned overlap is the first thing a reviewer distrusts. Decompose an actual feature in one of your own repositories; dogfood as user #1 (e.g. `git-fleet` or `arbustum`). Authenticity of the corpus is part of the claim's credibility, not a logistical detail.

---

## 7. Integration posture

"Leverage existing execution in Claude / Codex / harnesses" resolves to an **output-contract decision**:

- **Emit a plan** the host ingests and runs — the *adoption* path; requires a host-agnostic interchange format.
- **Run it yourself** against a model API via the instrumented loop — the *proof* path; gives you the token counters.

**v1 takes the proof path** (you need the counters). The adoption path is downstream of a result; do not build adapters for adopters who do not yet exist. When integration does begin, prove it inside **one** host — Hermes or oh-my-pi, since both already expose a decomposition / swarm surface to graft onto — and let the seam generalize from that single concrete instance.

---

## 8. Open questions

- **OQ-1 (granularity edge cases).** How are cross-symbol constructs (macros, generics, generated code) assigned to units? Do they inflate closures pathologically?
- **OQ-2 (interface extraction).** For the `reference` role, what precisely constitutes the "interface" of a unit per language? Signature only, or signature + type graph?
- **OQ-3 (weighting).** Is $w(u)$ raw token count, or adjusted for position/role? Does a `modify` unit cost more than a `reference` interface of equal token length in practice?
- **OQ-4 (budget headroom).** $B$ is not the raw window — it must reserve room for instructions, tool schemas, scratch. What fraction?
- **OQ-5 (dependency vs. overlap tension).** When dependency ordering and overlap minimization conflict, ordering wins (correctness), but how often does this degrade the achievable Cost, and by how much?

---

*Naming convention: Latin, consistent with the broader ecosystem. Planar remains the orchestration substrate; execution is elsewhere by design.*
