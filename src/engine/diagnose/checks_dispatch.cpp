/// @file checks_dispatch.cpp
/// @brief The dispatch family of the diagnose catalog (see diagnose.cppm).
///
/// Checks over `agent_work_claims`, `agent_actions`, `routing_dispatch_previews` and
/// `routing_dispatch_snapshots` (plan 1132, tasks 7376 and 7377; tech spec 689 § Check catalog):
///
///  - `dispatch-no-role-action` (event, warning; decision 1384): a dispatch whose claim is no longer live and that
///    has no `coder`, `reviewer` or `test_coder` action tied to the claim, or tied to its task
///    inside the claim's lifetime.
///  - `dispatch-unconfirmed` (event, warning): a dispatch with a preview bound to its claim and no
///    snapshot confirming it, reported once its claim has ended or a role action has started.
///  - `dispatch-confirmed-late` (event, warning): a dispatch whose earliest snapshot was confirmed
///    strictly after its first role action started.
///  - `action-unended` (state, warning): an action with no `ended_at` whose claim is terminal or
///    past its lease.
///
/// A claim is a dispatch only when an orchestrator dispatch record exists for it (decisions 1344
/// and 1349): a `routing_dispatch_previews` row with the claim's task and token, or a
/// `routing_dispatch_snapshots` row for the claim's task confirmed inside the claim's lifetime. A
/// direct claim, with neither, is an agent doing its own work and never yields a dispatch finding.
/// A claim's lifetime runs from `claimed_at` to its release, or to its lease expiry while it is
/// still `active`.
///
/// A claim is live while it is `active` and its lease runs to the evaluation instant or later, so a
/// lease that expires exactly now is live. The dispatch checks are events and wait for the claim to
/// end: a live claim can still start its action. Every timestamp is compared as an instant
/// (`strftime`), because `dispatch confirm --now` stores the caller's RFC 3339 text, which has no
/// fraction. Evidence times are row timestamps, never the evaluation instant.

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

/// True when preview `p` is bound to claim `c`: same task, same claim token.
constexpr std::string_view k_preview_match = "p.task_id = c.entity_id and p.claim_token = c.claim_token";

/// True when claim `c` is no longer live at the instant `now` (an SQL expression).
auto not_live(std::string_view now) -> std::string {
  return std::format("(c.status != 'active' or c.lease_expires_at < {})", now);
}

/// True when snapshot `s` confirms claim `c`: it is for the claim's task and one of these holds. A preview bound to the claim's
/// token was spent on it. A preview with no claim token, for the same task, was spent on it and its `[created_at, expires_at]`
/// window overlaps the claim's lifetime: orchestrator runs often preview with no token, confirm, and only then claim, so the
/// confirm precedes `claimed_at`. Or it was confirmed inside the claim's lifetime. `expires_at` is stored without a fraction,
/// so it is compared as an instant.
auto snapshot_match() -> std::string {
  return std::format("(s.task_id = c.entity_id and (s.id in (select p.consumed_dispatch_id from routing_dispatch_previews p"
                     "                                       where {0})"
                     "   or s.id in (select p.consumed_dispatch_id from routing_dispatch_previews p"
                     "               where p.task_id = c.entity_id and p.claim_token is null"
                     "                 and strftime({1}, p.created_at) <= {2} and strftime({1}, p.expires_at) >= c.claimed_at)"
                     "   or (strftime({1}, s.confirmed_at) >= c.claimed_at and strftime({1}, s.confirmed_at) <= {2})))",
                     k_preview_match, k_format, k_ends);
}

/// True when claim `c` is a dispatch: it has a preview bound to its token, or a snapshot for its task inside its lifetime.
auto is_dispatch() -> std::string {
  return std::format("(c.entity_kind = 'task' and (exists (select 1 from routing_dispatch_previews p where {})"
                     " or exists (select 1 from routing_dispatch_snapshots s where {})))",
                     k_preview_match, snapshot_match());
}

/// The condition that action `a` is a role action tied to claim `c`: a `coder`, `reviewer` or `test_coder` action on the
/// claim, or on its task and started inside the claim's lifetime.
constexpr std::string_view k_role_action =
    "a.action_kind in ('coder', 'reviewer', 'test_coder')"
    " and (a.claim_id = c.id or (a.entity_kind = 'task' and a.entity_id = c.entity_id"
    "      and a.started_at >= c.claimed_at and a.started_at <= coalesce(c.released_at, c.lease_expires_at)))";

/// True when a role action is tied to claim `c`.
auto has_role_action() -> std::string {
  return std::format("exists (select 1 from agent_actions a where {})", k_role_action);
}

/// The grouping that fixes a dispatch finding's fingerprint on the claim and its task, so a record that
/// appears in the evidence later does not make a new incident.
auto dispatch_group(const im::finding& f) -> im::grouping {
  return im::grouping{.key_parts = {im::entity_ref_text(f.primary), im::entity_ref_text(f.evidence[1])}, .scope = "global"};
}

auto no_role_action(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // `?1` is the window start and `?2` its end, which is the evaluation instant. Columns: claim,
  // task, the claim's end, the lowest preview bound to it (0 for none) and the lowest snapshot (0 for none).
  auto now = std::format("strftime({}, ?2)", k_format);
  auto sql = std::format("select c.id, c.entity_id, {0},"
                         "       coalesce((select min(p.id) from routing_dispatch_previews p where {1}), 0),"
                         "       coalesce((select min(s.id) from routing_dispatch_snapshots s where {2}), 0)"
                         " from agent_work_claims c"
                         " where {3} and {4} and not {5}"
                         "   and {0} >= ?1 and {0} <= ?2 and {6}"
                         " order by c.id",
                         k_ends, k_preview_match, snapshot_match(), is_dispatch(), not_live(now), has_role_action(),
                         plan_filter_sql(ctx.scope, k_claim_plan));
  return query_findings(ctx, sql, ctx.window.from, ctx.window.to, [](const db::statement& row) {
    im::finding f;
    f.severity = im::diagnostic_severity::warning;
    f.primary  = im::entity_ref{.kind = "claim", .id = row.column_int64(0)};
    f.evidence = {f.primary, im::entity_ref{.kind = "task", .id = row.column_int64(1)}};
    if (row.column_int64(3) != 0) {
      f.evidence.push_back(im::entity_ref{.kind = "dispatch_preview", .id = row.column_int64(3)});
    }
    if (row.column_int64(4) != 0) {
      f.evidence.push_back(im::entity_ref{.kind = "dispatch_snapshot", .id = row.column_int64(4)});
    }
    f.evidence_times = {row.column_text(2)};
    f.group          = dispatch_group(f);
    return f;
  });
}

auto unconfirmed(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // `?1` is the window start and `?2` its end, the evaluation instant. A dispatch here is a claim with a
  // preview bound to it (`pid` is the newest) and no snapshot confirming it; it is reported once its claim has
  // ended or a role action has started, because a live claim that has only been previewed may still be confirmed.
  auto now = std::format("strftime({}, ?2)", k_format);
  auto sql =
      std::format("select q.id, q.task_id, q.pid, p.created_at"
                  " from (select c.id, c.entity_id as task_id,"
                  "              (select max(p.id) from routing_dispatch_previews p where {0}) as pid"
                  "       from agent_work_claims c"
                  "       where c.entity_kind = 'task'"
                  "         and exists (select 1 from routing_dispatch_previews p where {0})"
                  "         and not exists (select 1 from routing_dispatch_snapshots s where {1})"
                  "         and ({2} or {3}) and {4}) q"
                  " join routing_dispatch_previews p on p.id = q.pid"
                  " where p.created_at >= ?1 and p.created_at <= ?2"
                  " order by q.id",
                  k_preview_match, snapshot_match(), not_live(now), has_role_action(), plan_filter_sql(ctx.scope, k_claim_plan));
  return query_findings(ctx, sql, ctx.window.from, ctx.window.to, [](const db::statement& row) {
    im::finding f;
    f.severity       = im::diagnostic_severity::warning;
    f.primary        = im::entity_ref{.kind = "claim", .id = row.column_int64(0)};
    f.evidence       = {f.primary, im::entity_ref{.kind = "task", .id = row.column_int64(1)},
                        im::entity_ref{.kind = "dispatch_preview", .id = row.column_int64(2)}};
    f.evidence_times = {row.column_text(3)};
    f.group          = dispatch_group(f);
    return f;
  });
}

auto confirmed_late(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // `fa_at` is the start of the claim's first role action and `sc_at` its earliest confirmation, normalised to
  // the stored format. Late is strictly after: a confirm at the instant the action started is in time.
  auto sql = std::format("with cand as ("
                         "  select c.id as claim_id, c.entity_id as task_id,"
                         "         (select min(a.started_at) from agent_actions a where {0}) as fa_at,"
                         "         (select min(strftime({2}, s.confirmed_at)) from routing_dispatch_snapshots s"
                         "          where {1}) as sc_at"
                         "  from agent_work_claims c where {3} and {4})"
                         " select cand.claim_id, cand.task_id, cand.fa_at, cand.sc_at,"
                         "        (select min(a.id) from agent_actions a join agent_work_claims c on c.id = cand.claim_id"
                         "         where {0} and a.started_at = cand.fa_at),"
                         "        (select min(s.id) from routing_dispatch_snapshots s"
                         "         join agent_work_claims c on c.id = cand.claim_id"
                         "         where {1} and strftime({2}, s.confirmed_at) = cand.sc_at)"
                         " from cand"
                         " where cand.fa_at is not null and cand.sc_at is not null and cand.sc_at > cand.fa_at"
                         "   and cand.sc_at >= ?1 and cand.sc_at <= ?2"
                         " order by cand.claim_id",
                         k_role_action, snapshot_match(), k_format, is_dispatch(), plan_filter_sql(ctx.scope, k_claim_plan));
  return query_findings(ctx, sql, ctx.window.from, ctx.window.to, [](const db::statement& row) {
    im::finding f;
    f.severity       = im::diagnostic_severity::warning;
    f.primary        = im::entity_ref{.kind = "claim", .id = row.column_int64(0)};
    f.evidence       = {f.primary, im::entity_ref{.kind = "task", .id = row.column_int64(1)},
                        im::entity_ref{.kind = "dispatch_snapshot", .id = row.column_int64(5)},
                        im::entity_ref{.kind = "action", .id = row.column_int64(4)}};
    f.evidence_times = {row.column_text(2), row.column_text(3)};
    f.group          = dispatch_group(f);
    return f;
  });
}

auto action_unended(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  auto now = std::format("strftime({}, ?1)", k_format);
  auto sql = std::format("select a.id, c.id, c.entity_kind, c.entity_id, a.started_at"
                         " from agent_actions a join agent_work_claims c on c.id = a.claim_id"
                         " where a.ended_at is null and {} and {}"
                         " order by a.id",
                         not_live(now), plan_filter_sql(ctx.scope, k_claim_plan));
  return query_findings(ctx, sql, ctx.evaluated_at, {}, [](const db::statement& row) {
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
  f.checks.push_back(check_def{.id       = "dispatch-no-role-action",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::warning,
                               .category = "dispatch_no_role_action",
                               .recovery = "start a task-tied action before spawning",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = no_role_action});
  f.checks.push_back(check_def{.id       = "dispatch-unconfirmed",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::warning,
                               .category = "dispatch_unconfirmed",
                               .recovery = "planar-agent dispatch confirm before spawning",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = unconfirmed});
  f.checks.push_back(check_def{.id       = "dispatch-confirmed-late",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::warning,
                               .category = "dispatch_confirmed_late",
                               .recovery = "confirm before spawning",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = confirmed_late});
  f.checks.push_back(check_def{.id       = "action-unended",
                               .kind     = im::check_kind::state,
                               .severity = im::diagnostic_severity::warning,
                               .category = "claim_action_unended",
                               .recovery = "planar-agent reconcile",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = action_unended});
  return f;
}

} // namespace planar::engine::diagnose::detail
