/// @file checks_dispatch.cpp
/// @brief The dispatch family of the diagnose catalog (see diagnose.cppm).
///
/// One check over `agent_actions` and `agent_work_claims` (plan 1132; tech spec 689 § Check
/// catalog):
///
///  - `action-unended` (state, warning): an action with no `ended_at` whose claim is terminal or
///    past its lease, started inside the window or on a claim that ended inside it (decision 1384; a
///    claim ends at its release, else its lease expiry). `planar-agent action end` closes the row on
///    a claim in any state; `reconcile` closes only a stale claim's `claim_check` marker.
///
/// The family's two dispatch checks, `dispatch-no-role-action` and `dispatch-confirmed-late`, read
/// the routing dispatch preview and snapshot tables. Migration 00043 dropped those tables, and the
/// checks were retired with them.
///
/// A claim is live while it is `active` and its lease runs to the evaluation instant or later, so a
/// lease that expires exactly now is live. Every timestamp is compared as an instant (`strftime`).
/// Evidence times are row timestamps, never the evaluation instant.

module;

module planar.engine.diagnose;

import std;
import planar.db;
import planar.incident_model;

namespace planar::engine::diagnose::detail {

namespace {

namespace im = planar::incident_model;

/// The plan a claim belongs to, as `checks_claims.cpp` resolves it.
constexpr std::string_view k_claim_plan = "case c.entity_kind"
                                          " when 'task' then (select t.plan_id from tasks t where t.id = c.entity_id)"
                                          " when 'plan' then c.entity_id"
                                          " else (select s.plan_id from plan_steps s where s.id = c.entity_id) end";

/// The stored timestamp format, for normalising a caller-supplied instant.
constexpr std::string_view k_format = "'%Y-%m-%dT%H:%M:%fZ'";

/// The end of a claim's lifetime: its release, else its lease expiry.
constexpr std::string_view k_ends = "coalesce(c.released_at, c.lease_expires_at)";

/// True when claim `c` is no longer live at the instant `now` (an SQL expression).
auto not_live(std::string_view now) -> std::string {
  return std::format("(c.status != 'active' or c.lease_expires_at < {})", now);
}

auto action_unended(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // `?1` is the window start and `?2` its end, which is the evaluation instant. The window bounds the action's start or
  // the end of its claim (decision 1384), so a long-dead backlog is not re-reported on every run.
  auto now = std::format("strftime({}, ?2)", k_format);
  auto sql = std::format("select a.id, c.id, c.entity_kind, c.entity_id, a.started_at"
                         " from agent_actions a join agent_work_claims c on c.id = a.claim_id"
                         " where a.ended_at is null and {0} and {1}"
                         "   and ((strftime({2}, a.started_at) >= ?1 and strftime({2}, a.started_at) <= ?2)"
                         "        or ({3} >= ?1 and {3} <= ?2))"
                         " order by a.id",
                         not_live(now), plan_filter_sql(ctx.scope, k_claim_plan), k_format,
                         std::format("strftime({}, {})", k_format, k_ends));
  return query_findings(ctx, sql, ctx.window.from, ctx.window.to, [](const db::statement& row) {
    im::finding f;
    f.severity       = im::diagnostic_severity::warning;
    f.primary        = im::entity_ref{.kind = "action", .id = row.column_int64(0)};
    f.evidence       = {f.primary, im::entity_ref{.kind = "claim", .id = row.column_int64(1)},
                        im::entity_ref{.kind = row.column_text(2), .id = row.column_int64(3)}};
    f.evidence_times = {row.column_text(4)};
    return f;
  });
}

} // namespace

auto dispatch_family() -> family {
  family f;
  f.checks.push_back(check_def{.id       = "action-unended",
                               .kind     = im::check_kind::state,
                               .severity = im::diagnostic_severity::warning,
                               .category = "claim_action_unended",
                               .recovery = "planar-agent action end --action <id> --outcome aborted",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = action_unended});
  return f;
}

} // namespace planar::engine::diagnose::detail
