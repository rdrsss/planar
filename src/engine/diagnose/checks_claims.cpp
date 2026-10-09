/// @file checks_claims.cpp
/// @brief The claim-liveness family of the diagnose catalog (see diagnose.cppm).
///
/// Five checks over `agent_work_claims`, `tasks`, `plans`, `plan_steps` and `agent_actions`
/// (plan 1132, task 7374; tech spec 689 § Check catalog):
///
///  - `claim-lease-lapsed`: an `active` claim past its lease that heartbeated, on a `doing` task.
///  - `claim-process-died`: an `active` claim past its lease that never heartbeated after
///    `claimed_at`; "active" is what "no terminal verb" means, since every terminal verb ends it.
///  - `claim-superseded-active`: an `active` claim on a terminal entity, or behind a later
///    exclusive claim on the same entity that is no longer `active`.
///  - `task-doing-unclaimed`: a `doing` task with no `active` claim whose lease runs to now or later.
///  - `claim-closed-by-reconcile`: an event for a claim that ended `stale`, inside the window.
///
/// The four state checks describe the world as of the evaluation instant, so they read every
/// matching row regardless of the window start; only the event is bounded by the window. A lease
/// that expires exactly at the evaluation instant is still live. "Heartbeated" means the claim's
/// `last_heartbeat_at` moved past `claimed_at` or a `heartbeat` action row is tied to it. Every
/// evidence time is a row timestamp (a lease expiry, a claim time, a release time, a task's
/// `updated_at`), never the evaluation instant.

module;

module planar.engine.diagnose;

import std;
import planar.db;
import planar.incident_model;

namespace planar::engine::diagnose::detail {

namespace {

namespace im = planar::incident_model;

/// The plan a claim belongs to: its task's plan, its plan, or its step's plan.
constexpr std::string_view k_claim_plan = "case c.entity_kind"
                                          " when 'task' then (select t.plan_id from tasks t where t.id = c.entity_id)"
                                          " when 'plan' then c.entity_id"
                                          " else (select s.plan_id from plan_steps s where s.id = c.entity_id) end";

/// True when the claim's entity can no longer take work.
constexpr std::string_view k_entity_terminal =
    "case c.entity_kind"
    " when 'task' then (select t.status in ('done', 'cancelled') from tasks t where t.id = c.entity_id)"
    " when 'plan' then (select p.status in ('done', 'abandoned') from plans p where p.id = c.entity_id)"
    " else (select s.status in ('done', 'skipped') from plan_steps s where s.id = c.entity_id) end";

/// True when the claim heartbeated after it was taken.
constexpr std::string_view k_heartbeated =
    "(c.last_heartbeat_at > c.claimed_at"
    " or exists (select 1 from agent_actions a where a.claim_id = c.id and a.action_kind = 'heartbeat'))";

/// The evaluation instant in the stored timestamp format, for text comparison.
constexpr std::string_view k_now = "strftime('%Y-%m-%dT%H:%M:%fZ', ?1)";

/// A finding for a claim row of columns `(id, entity_kind, entity_id, time)`.
auto claim_finding(im::diagnostic_severity severity, const db::statement& row) -> im::finding {
  im::finding f;
  f.severity       = severity;
  f.primary        = im::entity_ref{.kind = "claim", .id = row.column_int64(0)};
  f.evidence       = {f.primary, im::entity_ref{.kind = row.column_text(1), .id = row.column_int64(2)}};
  f.evidence_times = {row.column_text(3)};
  return f;
}

auto lease_lapsed(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto sql = std::format("select c.id, c.entity_kind, c.entity_id, c.lease_expires_at"
                         " from agent_work_claims c join tasks t on c.entity_kind = 'task' and t.id = c.entity_id"
                         " where c.status = 'active' and c.lease_expires_at < {}"
                         "   and t.status = 'doing' and {} and {}"
                         " order by c.id",
                         k_now, k_heartbeated, plan_filter_sql(ctx.scope, "t.plan_id"));
  return query_findings(ctx, sql, ctx.evaluated_at, {},
                        [](const db::statement& row) { return claim_finding(im::diagnostic_severity::warning, row); });
}

auto process_died(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto sql = std::format("select c.id, c.entity_kind, c.entity_id, c.lease_expires_at"
                         " from agent_work_claims c"
                         " where c.status = 'active' and c.lease_expires_at < {}"
                         "   and not {} and {}"
                         " order by c.id",
                         k_now, k_heartbeated, plan_filter_sql(ctx.scope, k_claim_plan));
  return query_findings(ctx, sql, ctx.evaluated_at, {},
                        [](const db::statement& row) { return claim_finding(im::diagnostic_severity::warning, row); });
}

auto superseded_active(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto sql = std::format("select c.id, c.entity_kind, c.entity_id, c.claimed_at"
                         " from agent_work_claims c"
                         " where c.status = 'active' and {}"
                         "   and (coalesce({}, 0)"
                         "        or exists (select 1 from agent_work_claims l"
                         "                   where l.entity_kind = c.entity_kind and l.entity_id = c.entity_id"
                         "                     and l.id > c.id and l.status != 'active'"
                         "                     and l.claim_scope = 'exclusive' and c.claim_scope = 'exclusive'))"
                         " order by c.id",
                         plan_filter_sql(ctx.scope, k_claim_plan), k_entity_terminal);
  return query_findings(ctx, sql, {}, {},
                        [](const db::statement& row) { return claim_finding(im::diagnostic_severity::error, row); });
}

auto doing_unclaimed(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto sql = std::format("select t.id, t.updated_at from tasks t"
                         " where t.status = 'doing' and {}"
                         "   and not exists (select 1 from agent_work_claims c"
                         "                   where c.entity_kind = 'task' and c.entity_id = t.id"
                         "                     and c.status = 'active' and c.lease_expires_at >= {})"
                         " order by t.id",
                         plan_filter_sql(ctx.scope, "t.plan_id"), k_now);
  return query_findings(ctx, sql, ctx.evaluated_at, {}, [](const db::statement& row) {
    im::finding f;
    f.severity       = im::diagnostic_severity::warning;
    f.primary        = im::entity_ref{.kind = "task", .id = row.column_int64(0)};
    f.evidence       = {f.primary};
    f.evidence_times = {row.column_text(1)};
    return f;
  });
}

auto closed_by_reconcile(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto sql = std::format("select c.id, c.entity_kind, c.entity_id, c.released_at"
                         " from agent_work_claims c"
                         " where c.status = 'stale' and c.released_at is not null"
                         "   and c.released_at >= ?1 and c.released_at <= ?2 and {}"
                         " order by c.released_at, c.id",
                         plan_filter_sql(ctx.scope, k_claim_plan));
  return query_findings(ctx, sql, ctx.window.from, ctx.window.to,
                        [](const db::statement& row) { return claim_finding(im::diagnostic_severity::info, row); });
}

} // namespace

auto claims_family() -> family {
  family f;
  f.checks.push_back(
      check_def{.id       = "claim-lease-lapsed",
                .kind     = im::check_kind::state,
                .severity = im::diagnostic_severity::warning,
                .category = "claim_lease_lapsed",
                .recovery = "planar-agent reconcile, or re-claim with --no-transition and finish with a terminal verb",
                .inputs   = {},
                .built    = true,
                .evaluate = lease_lapsed});
  f.checks.push_back(check_def{.id       = "claim-process-died",
                               .kind     = im::check_kind::state,
                               .severity = im::diagnostic_severity::warning,
                               .category = "claim_process_died",
                               .recovery = "planar-agent reconcile",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = process_died});
  f.checks.push_back(check_def{.id       = "claim-superseded-active",
                               .kind     = im::check_kind::state,
                               .severity = im::diagnostic_severity::error,
                               .category = "claim_superseded_active",
                               .recovery = "planar-agent abort --claim <token>",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = superseded_active});
  f.checks.push_back(check_def{.id       = "task-doing-unclaimed",
                               .kind     = im::check_kind::state,
                               .severity = im::diagnostic_severity::warning,
                               .category = "claim_doing_unclaimed",
                               .recovery = "planar-agent claim --entity task:<id> --no-transition",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = doing_unclaimed});
  f.checks.push_back(check_def{.id       = "claim-closed-by-reconcile",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::info,
                               .category = "claim_closed_by_reconcile",
                               .recovery = {},
                               .inputs   = {},
                               .built    = true,
                               .evaluate = closed_by_reconcile});
  return f;
}

} // namespace planar::engine::diagnose::detail
