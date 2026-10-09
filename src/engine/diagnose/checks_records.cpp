/// @file checks_records.cpp
/// @brief The handoff and sync-conflict family of the diagnose catalog (see diagnose.cppm).
///
/// `handoff-stale` (plan 1132, task 7378): a `pending` or `validated` handoff whose age at the
/// evaluation instant is strictly beyond `core::stale_handoff_threshold_hours`, the cutoff
/// `planar health` and `planar report` apply, shared through `planar.core.thresholds` and not
/// restated here. The age is the whole milliseconds from the handoff's `created_at` to the
/// evaluation instant, so a handoff exactly at the cutoff is not stale. The check describes the
/// state at the evaluation instant and ignores the window start. The plan scope reaches a handoff
/// through its snapshot's task; a handoff whose snapshot has no task is in scope only for a run
/// without a plan. The evidence time is the handoff's `created_at`.

module;

module planar.engine.diagnose;

import std;
import planar.core.thresholds;
import planar.db;
import planar.incident_model;

namespace planar::engine::diagnose::detail {

namespace {

namespace im = planar::incident_model;

auto handoff_stale(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  constexpr std::int64_t k_ms_per_hour = 3'600'000;
  auto sql = std::format("select h.id, coalesce(t.id, 0), h.created_at"
                         " from handoffs h"
                         " left join context_snapshots s on s.id = h.from_snapshot_id"
                         " left join tasks t on t.id = s.task_id"
                         " where h.status in ('pending', 'validated')"
                         "   and cast(round((julianday(strftime('%Y-%m-%dT%H:%M:%fZ', ?1))"
                         "                   - julianday(h.created_at)) * 86400000) as integer) > {}"
                         "   and {}"
                         " order by h.id",
                         core::stale_handoff_threshold_hours * k_ms_per_hour, plan_filter_sql(ctx.scope, "t.plan_id"));
  return query_findings(ctx, sql, ctx.evaluated_at, {}, [](const db::statement& row) {
    im::finding f;
    f.severity = im::diagnostic_severity::warning;
    f.primary  = im::entity_ref{.kind = "handoff", .id = row.column_int64(0)};
    f.evidence = {f.primary};
    if (row.column_int64(1) != 0) {
      f.evidence.push_back(im::entity_ref{.kind = "task", .id = row.column_int64(1)});
    }
    f.evidence_times = {row.column_text(2)};
    return f;
  });
}

} // namespace

auto records_family() -> family {
  family f;
  f.checks.push_back(check_def{.id       = "handoff-stale",
                               .kind     = im::check_kind::state,
                               .severity = im::diagnostic_severity::warning,
                               .category = "handoff_stale",
                               .recovery = "planar handoff validate <id>",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = handoff_stale});
  return f;
}

} // namespace planar::engine::diagnose::detail
