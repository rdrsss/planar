/// @file checks_records.cpp
/// @brief The handoff and sync-conflict family of the diagnose catalog (see diagnose.cppm).
///
/// `sync-conflict-unresolved` (plan 1132, task 7381): a `sync_events` row with outcome `conflict` and
/// no later event for the same link (a higher row id) that ends the conflict. An event ends it when
/// its outcome is `ok`, `noop` or `success` (a clean sync, and the event `planar-ext sync resolve`
/// writes) or `resolved-fs`/`resolved-db` (a settled workbench conflict); `error` and the other
/// outcomes end nothing. Of several conflicts on one link only the latest is reported, since resolve
/// only accepts the link's latest event. A conflict row without a link (a workbench conflict, which the
/// resolver settles by updating the row's outcome in place) has no later event for "the same link", so
/// it is reported for as long as its outcome reads `conflict`. A state check: it reads every matching row
/// regardless of the window start. The evidence is the event and its link, timed by the event's `at`.
/// The plan scope reaches a link through its task or plan; a link to anything else, and a link-less
/// event, belongs to no plan and appears only in a run without `--plan`.
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

auto sync_conflict_unresolved(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto sql =
      std::format("select e.id, coalesce(e.link_id, 0), e.at"
                  " from sync_events e"
                  " left join external_links l on l.id = e.link_id"
                  " where e.outcome = 'conflict'"
                  "   and not exists (select 1 from sync_events n where n.link_id = e.link_id and n.id > e.id"
                  "                   and n.outcome in ('conflict', 'ok', 'noop', 'success', 'resolved-fs', 'resolved-db'))"
                  "   and {}"
                  " order by e.id",
                  plan_filter_sql(ctx.scope, "case l.entity_kind when 'task' then"
                                             " (select t.plan_id from tasks t where t.id = l.entity_id)"
                                             " when 'plan' then l.entity_id end"));
  return query_findings(ctx, sql, {}, {}, [](const db::statement& row) {
    im::finding f;
    f.severity = im::diagnostic_severity::warning;
    f.primary  = im::entity_ref{.kind = "sync_event", .id = row.column_int64(0)};
    f.evidence = {f.primary};
    if (row.column_int64(1) != 0) {
      f.evidence.push_back(im::entity_ref{.kind = "external_link", .id = row.column_int64(1)});
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
  f.checks.push_back(
      check_def{.id       = "sync-conflict-unresolved",
                .kind     = im::check_kind::state,
                .severity = im::diagnostic_severity::warning,
                .category = "sync_conflict",
                .recovery = "planar-ext sync resolve <sync_event id> --keep local|remote (see its --help for the evidence flags)",
                .inputs   = {},
                .built    = true,
                .evaluate = sync_conflict_unresolved});
  return f;
}

} // namespace planar::engine::diagnose::detail
