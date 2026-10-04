/// @file optimal_arm.cppm
/// @brief `planar.engine.grouping.optimal_arm` — the optimal-arm seam behind
/// `groups recommend --solver mtkahypar` (decision 1293, generalizing the
/// seam of decision 1006).
///
/// An `arm` is an optional second partitioner that
/// `load::recommend_with` runs next to greedy on identical inputs. Greedy is
/// the only implementation on master, so the one arm master ships is
/// `none()`, which is never available. Solver arms live on branch
/// `dev/grouping-solvers` and plug into this shape.
///
/// This module owns only the shape. The never-worse-than-greedy comparison
/// lives in `load::recommend_with`, which scores both arms with the identical
/// `greedy::grouping::total_cost()`, so the contract is proved by the caller
/// and not asserted by an arm.
module;

export module planar.engine.grouping.optimal_arm;

import std;
import planar.engine.grouping.greedy;

namespace planar::engine::grouping::optimal {

/// @brief An optional optimal partitioner that competes with greedy.
///
/// Both members must be set. The struct holds no shared state; thread safety
/// is whatever the stored callables provide.
export struct arm {
  /// Whether `try_partition` can meaningfully run on this build.
  std::function<bool()> available;
  /// Partition `tasks` under `deps` and `budget` into the same shape greedy
  /// emits (sorted member ids, deduped union symbols, union cost). Returns
  /// `std::nullopt` when the arm is unavailable or the call failed; the
  /// caller then degrades to greedy.
  std::function<std::optional<greedy::grouping>(std::span<const greedy::task>, std::span<const greedy::dep>, std::uint32_t)>
      try_partition;
};

/// @brief The always-unavailable arm that master ships.
/// @return An arm whose `available()` is `false` and whose `try_partition`
/// returns `std::nullopt`.
export inline auto none() -> arm {
  return arm{
      .available     = [] { return false; },
      .try_partition = [](std::span<const greedy::task>, std::span<const greedy::dep>,
                          std::uint32_t) -> std::optional<greedy::grouping> { return std::nullopt; },
  };
}

} // namespace planar::engine::grouping::optimal
