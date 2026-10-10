/// @file checks_records.cpp
/// @brief The handoff and sync-conflict family of the diagnose catalog (see diagnose.cppm).
///
/// `sync-conflict-unresolved` (plan 1132, task 7381): an external-scope `sync_events` row with outcome
/// `conflict` and a link, with no later event for the same link (a higher row id) that ends the
/// conflict. An event ends it when its outcome is `ok`, `noop` or `success` (a clean sync, and the event
/// `planar-ext sync resolve` writes) or `resolved-fs`/`resolved-db`; `error` and the other outcomes end
/// nothing. Of several conflicts on one link only the latest is reported, since resolve only accepts a
/// link's latest event. Workbench-scope conflicts and events without a link are not reported: a workbench
/// sync writes a new conflict row on every run, `sync resolve` refuses an event with no link, and a
/// deleted link nulls `link_id`, so such a row would stay a finding for good. The workbench case is a
/// separate follow-up. The incident is the link's: the fingerprint is `sync-conflict-unresolved|
/// external_link:<id>|global` and does not name the event, because every `sync pull` on a still-conflicting
/// link writes a new conflict event. The primary entity stays the latest event (the `sync resolve`
/// target) and its `at` is the evidence time, so each new conflict is a new occurrence of the same
/// incident. A state check: it reads every matching row regardless of the window start. The plan scope
/// reaches a link through its task or plan; a link to anything else belongs to no plan and appears only
/// in a run without `--plan`. When the last attempt after the conflict was an `error`, the hint says to
/// pull again first.
///
/// `handoff-stale` (plan 1132, task 7378; decision 1384): a `pending` or `validated` handoff whose age at the
/// evaluation instant is strictly beyond `core::stale_handoff_threshold_hours`, the cutoff
/// `planar health` and `planar report` apply, shared through `planar.core.thresholds` and not
/// restated here. The age is the whole milliseconds from the handoff's `created_at` to the
/// evaluation instant, so a handoff exactly at the cutoff is not stale. The check describes the
/// state at the evaluation instant and ignores the window start. The plan scope reaches a handoff
/// through its snapshot's task; a handoff whose snapshot has no task is in scope only for a run
/// without a plan. A handoff whose task is `done` or `cancelled`, or was claimed (in any status) after
/// the handoff's `created_at`, is not stale: the work moved on without consuming it (decision 1384).
/// The evidence time is the handoff's `created_at`.

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
                         "   and (t.id is null or (t.status not in ('done', 'cancelled')"
                         "        and not exists (select 1 from agent_work_claims c where c.entity_kind = 'task'"
                         "                        and c.entity_id = t.id and c.claimed_at > h.created_at)))"
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
      std::format("select e.id, e.link_id, e.at,"
                  "       exists (select 1 from sync_events r where r.link_id = e.link_id and r.id > e.id"
                  "               and r.outcome = 'error')"
                  " from sync_events e"
                  " left join external_links l on l.id = e.link_id"
                  " where e.outcome = 'conflict' and e.scope = 'external' and e.link_id is not null"
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
    f.evidence = {f.primary, im::entity_ref{.kind = "external_link", .id = row.column_int64(1)}};
    f.group    = im::grouping{.key_parts = {im::entity_ref_text(f.evidence[1])}, .scope = "global"};
    if (row.column_int64(3) != 0) {
      f.recovery =
          std::format("planar-ext sync pull {} first (the last attempt after the conflict errored), then planar-ext sync "
                      "resolve <sync_event id> --keep local|remote (see its --help for the evidence flags)",
                      row.column_int64(1));
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
