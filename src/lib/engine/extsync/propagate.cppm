/// @file propagate.cppm
/// @brief `planar.engine.extsync.propagate` — the PURE half of
/// `zig/src/engine/extsync/propagate.zig` (plan 996, task 6335).
///
/// ## Why this file carries two of that file's seven functions and no more
///
/// `propagate.zig` is 409 lines and was carried in the unported inventory as
/// one indivisible blocker under four leaves. It is not indivisible. Measured
/// by SYMBOL rather than by file, `ext propagate-one` reaches exactly two of
/// its exported functions — `strategyForSystem` and `loadExistingMirror` —
/// and nothing either of them calls leaves the pair. The remaining five
/// (`selectStrategy`, `countDistinctReposInFeature`, `walkTree`, `freeTree`,
/// `hasExistingMirrorLink`) plus six dead declarations belong to `ext
/// propagate`, which stays deferred.
///
/// This is the same correction already recorded three times in
/// `surface.cpp`'s inventory — `audit commits` (task 6272), the `sync` write
/// trio (task 6294), `plan descendants` (task 6298): a LEAF's dependencies
/// inferred from its MODULE's. `plan descendants` took `walkTree` out of this
/// same file on exactly this reasoning.
///
/// ## Why only ONE of the two is here
///
/// The pair splits across buckets because this bucket has NO `db` edge (see
/// CMakeLists.txt), and that invariant is what keeps every test here runnable
/// with no database. `strategy_for_system` consults no database — it is two
/// string comparisons over static data — so it lands here.
/// `loadExistingMirror` is nothing but SQL against `external_links`, so it
/// lands in `planar.engine.external.link` beside the table it reads, as
/// `load_existing_mirror`. Splitting a source file across two modules is the
/// `plan descendants` precedent again, not a new liberty.
///
/// ## `strategy_for_system` is a FALLBACK and callers must know which they want
///
/// It always returns `github-parent-issue` for `github-issues` and never
/// looks at the database. The ADR-0006 bucketing that picks between
/// `github-zero-repo` / `github-parent-issue` / `github-projects-v2` by repo
/// count lives in `selectStrategy`, which is NOT ported. `ext propagate-one`
/// wants this one; `ext propagate` will want the other. The oracle's own doc
/// comment says so and is accurate — unusually for this milestone, where
/// prose has lost to code five times.
///
/// Note also what the returned `kind` does and does not affect in
/// `propagate-one`: it reaches the emitted JSON `"strategy"` field and the
/// anchor's `config_json` cache, and it never selects a template.
/// `template_kind_for_entity` discards it for GitHub outright.
module;

export module planar.engine.extsync.propagate;

import std;

namespace planar::engine::extsync::propagate {

/// @brief The four template kinds a propagation strategy names.
///
/// Every member is a static string view into a string literal with static
/// storage duration, so a `strategy` is freely copyable and outlives any
/// caller frame. The oracle's struct is four `[]const u8` fields pointing at
/// the same literals.
export struct strategy {
  std::string_view kind;             ///< The strategy's own name, e.g. `jira-epic`.
  std::string_view plan_anchor_kind; ///< Template kind for the anchor plan.
  std::string_view plan_child_kind;  ///< Template kind for a child plan.
  std::string_view task_kind;        ///< Template kind for a task.
};

/// @brief Return the default-shaped strategy for a system kind, WITHOUT
/// consulting the database.
///
/// Recognizes exactly two system kinds; anything else is the error. The
/// comparison is exact and case-sensitive, matching the oracle's
/// `std::mem.eql`.
///
/// @param system_kind The registered system's kind text, `jira` or
/// `github-issues`.
/// @return The strategy, or `std::nullopt` for an unrecognized kind — the
/// oracle's `error.UnsupportedSystemKind`.
export auto strategy_for_system(std::string_view system_kind) -> std::optional<strategy>;

/// @brief Apply ADR-0006's GitHub bucket after the caller counted feature repos.
/// @param system_kind Registered external-system kind.
/// @param distinct_repos Distinct repositories touched by the feature.
/// @return The selected strategy, or unset for an unsupported system.
export auto strategy_for_repo_count(std::string_view system_kind, std::size_t distinct_repos) -> std::optional<strategy>;

} // namespace planar::engine::extsync::propagate
