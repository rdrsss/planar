/// @file mtkahypar.cppm
/// @brief `planar.engine.grouping.mtkahypar` — the optional Mt-KaHyPar
/// hypergraph-solver seam behind `groups recommend --solver mtkahypar`
/// (plan 1006, task 6460).
///
/// Two build shapes share this ONE interface, selected at configure time by
/// `-DPLANAR_WITH_MTKAHYPAR=ON` (see the top-level `CMakeLists.txt` and this
/// bucket's `CMakeLists.txt`):
///
///   - **OFF (the default).** `available()` returns `false` unconditionally
///     and `try_partition` returns `std::nullopt`. No `mt_kahypar_*` symbol
///     is referenced and nothing extra is linked — this is the SAME shape
///     `groups recommend --solver mtkahypar` already has today (a genuinely
///     unavailable solver degrades to greedy with `optimal_available:false`)
///     and it is deliberately preserved so wiring this seam never forces a
///     default `cmake --build` to compile the 330MB `mtkahypar` library.
///   - **ON.** Links `MtKaHyPar::mtkahypar` and actually calls it. See
///     mtkahypar.cpp for the hypergraph translation.
///
/// This module owns ONLY the translation and invocation. The
/// never-worse-than-greedy comparison lives in `planar.engine.grouping.load`
/// (`recommend_with`), which calls both arms on identical inputs and scores
/// each with `greedy::grouping::total_cost()` — the IDENTICAL cost function,
/// not a re-derived approximation of it.
///
/// ## D-HG1 translation (read this before touching the encoding)
///
/// The greedy cost model (see greedy.cppm) treats a symbol two tasks BOTH
/// `modify` as a write-write conflict worth ZERO overlap credit, while
/// modify/reference or reference/reference is worth full weight. This
/// module generalises that pairwise rule to the hypergraph's necessarily
/// n-ary hyperedges: for a distinct symbol held by two or more tasks, if
/// two or more of those tasks hold it as `modify`, NO connectivity
/// hyperedge is emitted for that symbol at all (the zero-credit case,
/// documented rather than approximated away — see `build_hypergraph`).
/// Otherwise (zero or one writer, any number of readers) one connectivity
/// hyperedge is emitted pinning every holder, weighted `w(u)`, mirroring
/// the "full weight" case. A symbol held by exactly one task contributes no
/// edge either way (a single pin cannot be cut).
module;

export module planar.engine.grouping.mtkahypar;

import std;
import planar.engine.grouping.greedy;

namespace planar::engine::grouping::mtkahypar {

/// @brief Whether this build was configured with `-DPLANAR_WITH_MTKAHYPAR=ON`
/// AND the vendored library is actually linked in.
/// @return `true` iff `try_partition` can meaningfully run the solver.
export auto available() -> bool;

/// @brief Translate `tasks`/`deps` into Mt-KaHyPar's hypergraph input, run
/// the solver, and translate the resulting partition back into a
/// `greedy::grouping` in the SAME shape greedy emits (sorted member ids,
/// deduped union symbols, union cost).
///
/// Deliberately does NOT compare against greedy's own result — that
/// comparison is `load::recommend_with`'s job, over the identical
/// `total_cost()` function, so the "never worse than greedy" contract is
/// proved by the caller, not asserted here.
///
/// @param tasks The candidate tasks with their effective closures (same
/// input greedy consumes).
/// @param deps The dependency edges (unused by the pure hypergraph
/// objective — Mt-KaHyPar has no precedence concept — but accepted for
/// interface symmetry with `greedy::group` and future D-HG2-style
/// precedence repair).
/// @param budget The per-slice window budget, used ONLY to derive the
/// solver's block count (`ceil(deduped_union / budget)`, clamped to at
/// least 1) -- exactly the oracle's `mtkahypar.blockCount` (task 4247's
/// fix). NOTE, matching the oracle's own phasing: this does NOT enforce
/// `budget` as a hard per-block cap on the solver's OWN partition (the
/// oracle's D-HG3 "union-repair / exact-budget pass" is explicitly a LATER
/// milestone, M3.3c, never landed even there). A returned slice can in
/// principle exceed `budget` if the solver merges more aggressively than
/// the block count anticipates. The `total_cost()` comparison in
/// `load::recommend_with` still holds (a caller cannot get a worse TOTAL
/// cost than greedy), but per-slice budget compliance on the solver arm is
/// the same accepted gap the oracle carried at this milestone -- flagged
/// here rather than silently assumed away.
/// @return The solver's partition as a `greedy::grouping`, or
/// `std::nullopt` when the solver is unavailable (`!available()`) or the
/// call itself failed.
export auto try_partition(std::span<const greedy::task> tasks, std::span<const greedy::dep> deps, std::uint32_t budget)
    -> std::optional<greedy::grouping>;

} // namespace planar::engine::grouping::mtkahypar
