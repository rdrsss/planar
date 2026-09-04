/// @file ext_strategy.cppm
/// @brief `planar.cmd.planar_ext.handlers.ext_strategy` — ADR-0006
/// propagation strategy selection (plan 996, task 6421, lifted from the
/// halted `salvage/task-6409-ext-rehome` lane).
///
/// Behavior-preserving port (D2) of `selectStrategy` from
/// `zig/src/engine/extsync/propagate.zig` (409 lines total; this is the
/// bucketing entry point, not the whole file — see below for what else that
/// file names and where each piece already lives).
///
/// ## Lifted, not replayed
///
/// `salvage/task-6409-ext-rehome` (b4dd8eb) built this against `planar`'s
/// `planar.cmd.planar.handlers.ext_strategy` before the extraction (task
/// 6412) moved the `ext`/`sync` family to `planar-ext`. The logic is
/// unchanged; only the module name, namespace, and file location are
/// re-homed to this binary — see this task's report for what else changed
/// between the salvage and here.
///
/// ## Why this lives at LAYER 3 and could not live anywhere else
///
/// `selectStrategy` composes two things that already exist in TWO DIFFERENT
/// layer-2 buckets:
///
///   - `planar::engine::extsync::propagate::strategy_for_repo_count` (pure,
///     `engine_extsync`, task 6335) — the static jira/github kind mapping
///     plus the ADR-0006 repo-count bucket switch.
///   - `planar::engine::external::parent_issue::distinct_repo_count_in_feature`
///     (SQL, `engine_external`, task 6189) — the distinct-touched-repo COUNT
///     that feeds the bucket switch.
///
/// D18 FATALs an `engine_* -> engine_*` edge at configure time (see
/// `cmake/architecture.cmake`), so neither bucket may call the other, and
/// this function may not live in either of them. It belongs to the first
/// layer that may legally name both, which is the cmd layer — the exact
/// same shape `ext_adapter_factory.cppm` already establishes for the
/// `engine_extsync` x `engine_external` composition its own factory needs.
/// No new layering pattern was invented here; the existing one was reused.
///
/// ## What else `propagate.zig` names, and where it already is
///
/// `strategyForSystem` (the DB-free fallback shape) is `strategy_for_system`,
/// already in `planar.engine.extsync.propagate` alongside
/// `strategy_for_repo_count`. `walkTree`/`freeTree` (propagate.zig lines
/// 200-291) are already `planar.engine.planning.descendants::walk_tree` —
/// ported at task 6298 for the unrelated `plan descendants` leaf; `ext
/// propagate`'s CLI bridge (`propagate.cpp` in this directory) calls that
/// directly rather than duplicating it.
/// `countDistinctReposInFeature`/`hasExistingMirrorLink`/
/// `loadExistingMirror` stay out of scope for this file; the first is what
/// `distinct_repo_count_in_feature` already replaces (task 6189), the other
/// two are `ext propagate-one`'s (task 6335, `planar.engine.external.link`).
///
/// ## The `query_failed` no-silent-degradation invariant, restated at THIS
/// ## boundary
///
/// `distinct_repo_count_in_feature` already propagates `query_failed`
/// rather than defaulting to zero touched repos (task 6189, closed after a
/// permissive break probe on the `!repos` guard survived). `select_strategy`
/// must not re-introduce that gap one layer up by mapping a query failure
/// to `github-zero-repo` (or any other bucket) instead of propagating the
/// failure — see `select_strategy`'s own doc comment and
/// `ext_strategy.t.cpp`'s query-failure case, which drives this composed
/// entry point the same way `parent_issue.t.cpp`'s sibling case drives the
/// wrapped one.
module;

export module planar.cmd.planar_ext.handlers.ext_strategy;

import std;
import planar.db;
import planar.engine.extsync;
import planar.engine.external;

namespace planar::cmd::ext::handlers {

/// @brief Why `select_strategy` could not select a strategy.
export enum class strategy_select_error : std::uint8_t {
  unsupported_system_kind, ///< Neither `jira` nor `github-issues`.
  query_failed,            ///< The distinct-repo-count query failed.
};

/// @brief Select the ADR-0006 propagation strategy for a feature.
///
/// Mirrors Go `internal/extsync/selector.go::SelectStrategy` via the oracle
/// `propagate.zig::selectStrategy`:
///
///   - `"jira"` -> `"jira-epic"` regardless of repo count. The database is
///     NOT consulted on this path — matching the oracle, which returns
///     before ever building the count query.
///   - `"github-issues"` -> bucketed by
///     `distinct_repo_count_in_feature(anchor_plan_id)`:
///     - 0 repos  -> `"github-zero-repo"`
///     - 1 repo   -> `"github-parent-issue"`
///     - >=2 repos -> `"github-projects-v2"`
///   - anything else -> `unsupported_system_kind`.
///
/// A count-query failure returns `query_failed` — it MUST NOT be mapped to
/// the zero-repo bucket, which is what a query failure would look like if
/// swallowed. See this module's header.
///
/// **`"github-projects-v2"` is a NAME this function still returns, not a
/// STRATEGY this binary can execute** — decision 1001 cut the ProjectsV2
/// GraphQL arm entirely. The CLI bridge (`propagate.cpp`) is the layer that
/// refuses a `>=2`-repo feature explicitly rather than silently
/// mis-executing it under the parent-issue path; this function still
/// reports the oracle-accurate bucket name so that refusal can name the
/// real reason.
/// @param conn An open, migrated database connection.
/// @param anchor_plan_id The feature's anchor plan.
/// @param system_kind The registered system's kind text.
/// @return The selected strategy, or the failure.
export auto select_strategy(db::connection& conn, std::int64_t anchor_plan_id, std::string_view system_kind)
    -> std::expected<engine::extsync::propagate::strategy, strategy_select_error>;

} // namespace planar::cmd::ext::handlers
