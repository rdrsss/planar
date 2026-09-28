/// @file ext_strategy.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.ext_strategy`.

module planar.cmd.planar_ext.handlers.ext_strategy;

import std;
import planar.db;
import planar.engine.extsync;
import planar.engine.external;

namespace planar::cmd::ext::handlers {

namespace propagate    = engine::extsync::propagate;
namespace parent_issue = engine::external::parent_issue;

/// @brief Select the ADR-0006 propagation strategy for a feature. See this
/// function's declaration in ext_strategy.cppm for the full contract
/// (oracle mapping, bucket table, and why `"github-projects-v2"` is still a
/// reported bucket name after decision 1001 cut its execution arm).
///
/// Not a one-choice chooser even post-1001: this is the composition point
/// that lets `propagate.cpp` tell "proceed with `github-parent-issue`" apart
/// from "this feature wants a strategy we deliberately do not implement"
/// and refuse the latter clearly. Collapsing the `>=2`-repo bucket into
/// `github-parent-issue` here (rather than reporting it and letting the
/// caller refuse) would silently propagate to the wrong repo composition.
/// @param conn An open, migrated database connection.
/// @param anchor_plan_id The feature's anchor plan.
/// @param system_kind The registered system's kind text.
/// @return The selected strategy, or the failure.
auto select_strategy(db::connection& conn, std::int64_t anchor_plan_id, std::string_view system_kind)
    -> std::expected<propagate::strategy, strategy_select_error> {
  // jira never touches the database -- the oracle returns before building
  // the count query, and this reproduces that (no query issued on this
  // path at all, not merely one whose result is discarded).
  if (system_kind == "jira") {
    auto selected = propagate::strategy_for_system("jira");
    return *selected; // strategy_for_system("jira") always has a value.
  }

  if (system_kind != "github-issues") {
    return std::unexpected(strategy_select_error::unsupported_system_kind);
  }

  auto count = parent_issue::distinct_repo_count_in_feature(conn, anchor_plan_id);
  if (!count) {
    // A query failure MUST NOT be mapped to the zero-repo bucket -- that
    // would silently propagate as if the feature touches nothing. See this
    // module's header and ext_strategy.t.cpp's query-failure case.
    return std::unexpected(strategy_select_error::query_failed);
  }

  auto selected = propagate::strategy_for_repo_count("github-issues", *count);
  return *selected; // strategy_for_repo_count("github-issues", ...) always has a value.
}

} // namespace planar::cmd::ext::handlers
