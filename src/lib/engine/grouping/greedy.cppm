/// @file greedy.cppm
/// @brief `planar.engine.grouping.greedy` — the pure greedy overlap-merge
/// partitioner behind `planar groups recommend` (plan 996, task 6095).
///
/// Behavior-preserving port (D2) of zig/src/engine/grouping/greedy.zig. No
/// database, no filesystem, no process spawn, no global state — the caller
/// loads tasks/closures/dependencies (see `planar.engine.grouping.load`) and
/// hands them in as plain spans.
///
/// It forms **slices** (groups of tasks sharing one context window) by
/// repeatedly merging the pair with the highest closure OVERLAP, refusing any
/// merge whose unioned closure would exceed the window budget or would make
/// the slice-DAG unschedulable.
///
/// ## D-HG1 — role asymmetry, and the thing that surprises people
///
/// A symbol BOTH slices `modify` is a write-write conflict and earns ZERO
/// overlap credit: co-locating two writers of one symbol concentrates merge
/// risk instead of saving resident context. A symbol shared as
/// reference/reference or modify/reference earns its full weight.
///
/// The consequence is counter-intuitive and is oracle-confirmed, not inferred:
/// two tasks sharing a HEAVY symbol can stay apart while two tasks sharing a
/// LIGHT one merge. In the captured fixture, T1 and T3 both modify `ww.sym`
/// (weight 50) and were NOT merged, while T1 and T2 share `shared.sym`
/// (weight 10, reference/reference) and WERE:
///
///   $Z groups recommend 1 --json
///     -> "slices":[{"task_ids":[1,2],...,"cost":63},
///                  {"task_ids":[3],...,"cost":53}]
///
/// ## Zero-overlap merges still happen — but only along a dependency
///
/// A pair scoring zero is skipped UNLESS a dependency edge connects them, in
/// which case co-location is taken on scheduling grounds. Oracle-confirmed
/// with three mutually disjoint tasks where only one pair carries an edge:
///
///   $Z groups recommend 3 --json
///     -> "slices":[{"task_ids":[10,11],"union_symbols":["d1.only","d2.only"],
///                   "cost":11},
///                  {"task_ids":[12],"union_symbols":["d3.only"],"cost":7}]
///
/// ## HAZARD 4 — the tie-break, established by constructing an actual tie
///
/// Ties break on the smaller `tie_min` (the smallest member id across the
/// pair), then the smaller `tie_max`. Pinned with a fixture where two merges
/// score identically and the budget permits only ONE, so the choice is
/// observable rather than washed out by both eventually happening:
///
///   tasks 30 {x:10}, 31 {x:10, p:90}, 32 {x:10, q:90}, budget 100
///   -> merge(30,31) and merge(30,32) both score 10, both fit;
///      merge(31,32) costs 190 and does not.
///   $Z groups recommend 5 --budget 100 --json
///     -> "slices":[{"task_ids":[30,31],...},{"task_ids":[32],...}]
///
/// The reversed tie-break would have produced `[30,32]` and `[31]`.
///
/// ## Budget is a constraint on MERGING, not on a lone task
///
/// A task whose own closure already exceeds the budget stays a singleton and
/// is reported at its real cost — it is never dropped and never truncated.
/// Oracle-confirmed at `--budget 12` (and at `--budget 0`, which behaves
/// identically): task 1 comes back alone at cost 61.

module;

export module planar.engine.grouping.greedy;

import std;

namespace planar::engine::grouping::greedy {

/// @brief The role a unit plays in a task's closure.
///
/// Only the EFFECTIVE roles appear here: `transitive` closure rows are
/// excluded upstream by the loader and never reach this module.
export enum class role {
  modify,   ///< The task writes this symbol.
  reference ///< The task only reads it.
};

/// @brief One unit of a task's effective closure.
///
/// Identity is the `qualified` symbol name: two units with the same name in
/// different tasks denote the SAME context unit — the thing co-location lets a
/// slice pay for once.
export struct unit {
  std::string   qualified;                ///< Qualified symbol name, e.g. `widget.Foo.bar`.
  role          role_  = role::reference; ///< The owning task's role on it.
  std::uint32_t weight = 0;               ///< Token weight, counted once per distinct symbol.
};

/// @brief One task to be grouped: its id and its effective closure.
export struct task {
  std::int64_t      id = 0; ///< The `tasks.id` the caller pulled from the plan.
  std::vector<unit> units;  ///< Effective-closure units (modify ∪ reference).
};

/// @brief A dependency edge: `blocker` must complete before `blocked`.
///
/// NOTE the orientation. `entity_links` stores `task -[depends-on]-> task`
/// meaning from_id depends on to_id, which is the OPPOSITE of this struct's
/// field order. The loader performs the flip (`blocked = from_id`,
/// `blocker = to_id`); getting it backwards would make the cycle check reason
/// about a mirrored DAG and emit an unschedulable grouping.
export struct dep {
  std::int64_t blocked = 0; ///< Cannot start until `blocker` is done.
  std::int64_t blocker = 0; ///< Must complete first.
};

/// @brief A formed slice.
export struct slice {
  std::vector<std::int64_t> task_ids;      ///< Members, sorted ascending.
  std::vector<std::string>  union_symbols; ///< Distinct unioned symbols, sorted.
  std::uint32_t             cost = 0;      ///< `Σ w(u)` over the union, each unit once.
};

/// @brief The grouping result: the formed slices, ordered by smallest member id.
export struct grouping {
  std::vector<slice> slices; ///< The formed slices, ordered by smallest member id.

  /// @brief Replication-inclusive total across all slices.
  /// @return `Σ` of each slice's unioned-closure cost.
  [[nodiscard]] auto total_cost() const -> std::uint64_t;
};

/// @brief Greedily group `tasks` into slices under `budget`.
///
/// Algorithm: seed one slice per task; then repeatedly merge the best eligible
/// pair (highest overlap, budget-fitting, cycle-free) until none remains.
/// Fully deterministic — with fixed inputs the slices, their unioned closures,
/// and their costs are identical on every run.
/// @param tasks The candidate tasks with their effective closures.
/// @param deps The dependency edges, already oriented as this module expects.
/// @param budget The per-slice window budget in tokens.
/// @return The formed slices, ordered by smallest member id.
export auto group(std::span<const task> tasks, std::span<const dep> deps, std::uint32_t budget) -> grouping;

} // namespace planar::engine::grouping::greedy
