/// @file conformance.cpp
/// @brief Implementation of `planar.engine.runtime.conformance`.

module planar.engine.runtime.conformance;

import std;
import planar.db;
import planar.json_text;
import planar.engine.runtime.completion;

namespace planar::engine::runtime::conformance {

namespace comp = completion;

namespace {

/// @brief The audit summary `task done --force` and `task update --status done --force` record.
///
/// Equal to `planar.engine.planning::forced_done_summary`; this bucket cannot import that one.
constexpr std::string_view k_forced_done_summary = "done (--force)";

/// @brief The plan tree prefix every query below shares; bind the plan id at index 1.
constexpr std::string_view k_plan_tree = "with recursive plan_tree(id) as (select id from plans where id = ? union all "
                                         "select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id) ";

auto failed() -> check_error {
  return check_error{.kind = error_kind::query_failed, .detail = {}};
}

/// @brief One claim of the plan tree, with the facts the rules read.
struct claim_row {
  std::int64_t               id = 0;
  std::string                token;
  std::string                entity_kind;
  std::int64_t               entity_id = 0;
  std::string                status;
  std::optional<std::string> role;
  std::optional<std::string> model;
  std::optional<std::string> digest;
  std::string                lease_expires_at;
  std::int64_t               plan_id           = 0;
  bool                       has_parent_action = false;
};

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

/// @brief Run a one-value query, returning its first column or unset when it yields no row or NULL.
auto scalar_text(db::connection& conn, std::string_view sql, std::optional<std::int64_t> bound = std::nullopt)
    -> std::expected<std::optional<std::string>, check_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(failed());
  }
  if (bound.has_value() && !stmt->bind_int64(1, *bound)) {
    return std::unexpected(failed());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(failed());
  }
  if (*stepped != db::step_result::row) {
    return std::optional<std::string>{};
  }
  return opt_text(*stmt, 0);
}

auto scalar_int(db::connection& conn, std::string_view sql, std::optional<std::int64_t> bound = std::nullopt)
    -> std::expected<std::int64_t, check_error> {
  auto const text = scalar_text(conn, sql, bound);
  if (!text) {
    return std::unexpected(text.error());
  }
  if (!text->has_value()) {
    return std::int64_t{0};
  }
  std::int64_t value = 0;
  auto const   view  = std::string_view{**text};
  auto const   ec    = std::from_chars(view.data(), view.data() + view.size(), value).ec;
  if (ec != std::errc{}) {
    return std::unexpected(failed());
  }
  return value;
}

auto next_claim_id(db::connection& conn) -> std::expected<std::int64_t, check_error> {
  auto const highest = scalar_int(conn, "select coalesce(max(id), 0) from agent_work_claims");
  if (!highest) {
    return std::unexpected(highest.error());
  }
  return *highest + 1;
}

auto load_claims(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<claim_row>, check_error> {
  auto stmt = conn.prepare(
      std::string{k_plan_tree} +
      "select id, claim_token, entity_kind, entity_id, status, role, model, policy_digest, lease_expires_at, plan_id, "
      "has_parent\n"
      "from (\n"
      "  select c.id, c.claim_token, c.entity_kind, c.entity_id, c.status, c.role, c.model, c.policy_digest,\n"
      "         c.lease_expires_at,\n"
      "         case c.entity_kind\n"
      "           when 'plan' then c.entity_id\n"
      "           when 'task' then (select t.plan_id from tasks t where t.id = c.entity_id)\n"
      "           else (select s.plan_id from plan_steps s where s.id = c.entity_id) end as plan_id,\n"
      "         exists (select 1 from agent_actions a\n"
      "                 where a.claim_id = c.id and a.action_kind <> 'review' and a.parent_action_id is not null) as has_parent\n"
      "  from agent_work_claims c\n"
      ")\n"
      "where plan_id in (select id from plan_tree)\n"
      "order by id");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(failed());
  }
  std::vector<claim_row> rows;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(failed());
    }
    if (*stepped != db::step_result::row) {
      return rows;
    }
    rows.push_back(claim_row{.id                = stmt->column_int64(0),
                             .token             = stmt->column_text(1),
                             .entity_kind       = stmt->column_text(2),
                             .entity_id         = stmt->column_int64(3),
                             .status            = stmt->column_text(4),
                             .role              = opt_text(*stmt, 5),
                             .model             = opt_text(*stmt, 6),
                             .digest            = opt_text(*stmt, 7),
                             .lease_expires_at  = stmt->column_text(8),
                             .plan_id           = stmt->column_int64(9),
                             .has_parent_action = stmt->column_int64(10) != 0});
  }
}

/// @brief One done task of the plan tree.
struct done_task {
  std::int64_t                id = 0;
  std::string                 updated_at;
  bool                        completed_claim = false;
  std::string                 claim_statuses; ///< Distinct statuses of the task's claims, comma separated; empty for none.
  std::optional<std::int64_t> forced_audit_id;
  std::optional<std::string>  forced_at;
};

auto load_done_tasks(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<done_task>, check_error> {
  auto stmt = conn.prepare(
      std::string{k_plan_tree} +
      "select t.id, t.updated_at,\n"
      "  exists (select 1 from agent_work_claims c\n"
      "          where c.entity_kind = 'task' and c.entity_id = t.id and c.status = 'completed'),\n"
      "  coalesce((select group_concat(s, ',') from (select distinct c.status as s from agent_work_claims c\n"
      "                                               where c.entity_kind = 'task' and c.entity_id = t.id order by 1)), ''),\n"
      "  (select a.id from audit_log a where a.entity_kind = 'task' and a.entity_id = t.id and a.verb = 'status_change'\n"
      "     and a.summary = ?2 order by a.id desc limit 1),\n"
      "  (select a.recorded_at from audit_log a where a.entity_kind = 'task' and a.entity_id = t.id and a.verb = "
      "'status_change'\n"
      "     and a.summary = ?2 order by a.id desc limit 1)\n"
      "from tasks t where t.status = 'done' and t.plan_id in (select id from plan_tree)\n"
      "order by t.id");
  if (!stmt || !stmt->bind_int64(1, plan_id) || !stmt->bind_text(2, k_forced_done_summary)) {
    return std::unexpected(failed());
  }
  std::vector<done_task> rows;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(failed());
    }
    if (*stepped != db::step_result::row) {
      return rows;
    }
    done_task row{.id              = stmt->column_int64(0),
                  .updated_at      = stmt->column_text(1),
                  .completed_claim = stmt->column_int64(2) != 0,
                  .claim_statuses  = stmt->column_text(3)};
    if (!stmt->is_null(4)) {
      row.forced_audit_id = stmt->column_int64(4);
      row.forced_at       = opt_text(*stmt, 5);
    }
    rows.push_back(std::move(row));
  }
}

auto is_worker_role(const std::optional<std::string>& role) -> bool {
  return role.has_value() && !role->empty() && *role != "orchestrator";
}

auto describe_entity(const claim_row& claim) -> std::string {
  return std::format("{}:{}", claim.entity_kind, claim.entity_id);
}

auto claim_violation(rule broken, const claim_row& claim, std::string reason) -> violation {
  return violation{.broken         = broken,
                   .entity_kind    = "claim",
                   .entity_id      = claim.id,
                   .claim_token    = claim.token,
                   .claimed_entity = describe_entity(claim),
                   .reason         = std::move(reason)};
}

auto short_token(const std::string& token) -> std::string {
  return token.substr(0, 8);
}

} // namespace

auto rule_number(rule value) -> int {
  return static_cast<int>(value);
}

auto rule_name(rule value) -> std::string_view {
  switch (value) {
  case rule::completed_claim:
    return "completed-claim";
  case rule::terminal_verb:
    return "terminal-verb";
  case rule::coder_model:
    return "coder-model";
  case rule::parent_action:
    return "parent-action";
  case rule::queued_gate:
    return "queued-gate";
  case rule::independent_review:
    return "independent-review";
  }
  return "unknown";
}

auto floor_origin_name(floor_origin value) -> std::string_view {
  switch (value) {
  case floor_origin::migration:
    return "migration";
  case floor_origin::digest:
    return "digest";
  case floor_origin::explicit_:
    return "explicit";
  }
  return "unknown";
}

auto resolve_floor(db::connection& conn, std::optional<std::int64_t> explicit_floor) -> std::expected<floor_info, check_error> {
  if (explicit_floor.has_value()) {
    auto const below = scalar_int(conn, "select count(*) from agent_work_claims where id < ?", *explicit_floor);
    if (!below) {
      return std::unexpected(below.error());
    }
    floor_info info{.claim_id = *explicit_floor, .origin = floor_origin::explicit_, .cut_at = std::nullopt};
    if (*below == 0) {
      // Nothing lies below the floor: every task is graded too.
      info.cut_at = std::string{};
      return info;
    }
    auto const at =
        scalar_text(conn, "select claimed_at from agent_work_claims where id >= ? order by id limit 1", *explicit_floor);
    if (!at) {
      return std::unexpected(at.error());
    }
    info.cut_at = *at;
    return info;
  }
  auto const applied = scalar_text(conn, "select applied_at from schema_migrations where version = 42");
  if (!applied) {
    return std::unexpected(applied.error());
  }
  if (applied->has_value()) {
    auto stmt = conn.prepare("select min(id) from agent_work_claims where claimed_at >= ?");
    if (!stmt || !stmt->bind_text(1, **applied)) {
      return std::unexpected(failed());
    }
    auto stepped = stmt->step();
    if (!stepped || *stepped != db::step_result::row) {
      return std::unexpected(failed());
    }
    std::int64_t claim_id = 0;
    if (stmt->is_null(0)) {
      auto const next = next_claim_id(conn);
      if (!next) {
        return std::unexpected(next.error());
      }
      claim_id = *next;
    } else {
      claim_id = stmt->column_int64(0);
    }
    return floor_info{.claim_id = claim_id, .origin = floor_origin::migration, .cut_at = **applied};
  }
  auto stmt = conn.prepare("select id, claimed_at from agent_work_claims where policy_digest is not null order by id limit 1");
  if (!stmt) {
    return std::unexpected(failed());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(failed());
  }
  if (*stepped == db::step_result::row) {
    return floor_info{.claim_id = stmt->column_int64(0), .origin = floor_origin::digest, .cut_at = stmt->column_text(1)};
  }
  auto const next = next_claim_id(conn);
  if (!next) {
    return std::unexpected(next.error());
  }
  return floor_info{.claim_id = *next, .origin = floor_origin::digest, .cut_at = std::nullopt};
}

auto check(db::connection& conn, const options& opts, const policy_resolver& resolve, const completion::gate_run_lookup& lookup)
    -> std::expected<report, check_error> {
  auto const exists = scalar_int(conn, "select count(*) from plans where id = ?", opts.plan_id);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (*exists == 0) {
    return std::unexpected(check_error{.kind = error_kind::plan_not_found, .detail = {}});
  }
  auto const plans = scalar_int(conn, std::string{k_plan_tree} + "select count(*) from plan_tree", opts.plan_id);
  if (!plans) {
    return std::unexpected(plans.error());
  }
  auto const floor = resolve_floor(conn, opts.floor_claim_id);
  if (!floor) {
    return std::unexpected(floor.error());
  }
  auto const claims = load_claims(conn, opts.plan_id);
  if (!claims) {
    return std::unexpected(claims.error());
  }
  auto const tasks = load_done_tasks(conn, opts.plan_id);
  if (!tasks) {
    return std::unexpected(tasks.error());
  }
  std::string now_text;
  if (opts.now.has_value()) {
    now_text = *opts.now;
  } else {
    auto const clock = scalar_text(conn, "select strftime('%Y-%m-%dT%H:%M:%fZ', 'now')");
    if (!clock || !clock->has_value()) {
      return std::unexpected(failed());
    }
    now_text = **clock;
  }

  report out;
  out.plan_id = opts.plan_id;
  out.plans   = *plans;
  out.floor   = *floor;

  // Rule 1, and the done tasks before the cut-over.
  for (auto const& task : *tasks) {
    bool const graded = floor->cut_at.has_value() && task.updated_at >= *floor->cut_at;
    if (!graded) {
      ++out.before.done_tasks;
      continue;
    }
    ++out.done_tasks_graded;
    if (task.completed_claim) {
      continue;
    }
    if (task.forced_audit_id.has_value()) {
      out.overrides.push_back(override_entry{
          .task_id = task.id,
          .reason  = std::format("operator moved the task to done with --force (audit row {}{})", *task.forced_audit_id,
                                 task.forced_at.has_value() ? std::format(" at {}", *task.forced_at) : std::string{})});
      continue;
    }
    out.violations.push_back(violation{
        .broken         = rule::completed_claim,
        .entity_kind    = "task",
        .entity_id      = task.id,
        .claim_token    = std::nullopt,
        .claimed_entity = std::nullopt,
        .reason = task.claim_statuses.empty()
                      ? std::string{"task is done and no claim was ever taken on it"}
                      : std::format("task is done and no claim on it ended completed (claims ended: {})", task.claim_statuses)});
  }

  std::map<std::int64_t, policy_view> policies;
  auto const                          policy_for = [&](std::int64_t plan_id) -> std::expected<const policy_view*, check_error> {
    if (auto found = policies.find(plan_id); found != policies.end()) {
      return &found->second;
    }
    auto resolved = resolve(conn, plan_id);
    if (!resolved) {
      return std::unexpected(check_error{.kind = error_kind::policy_unavailable, .detail = resolved.error()});
    }
    return &policies.emplace(plan_id, std::move(*resolved)).first->second;
  };

  for (auto const& claim : *claims) {
    if (claim.id < floor->claim_id) {
      ++out.before.claims;
      continue;
    }
    ++out.claims_graded;

    // Rule 2.
    if (claim.status == "stale") {
      out.violations.push_back(
          claim_violation(rule::terminal_verb, claim, "claim ended stale: its lease lapsed and no terminal verb ran"));
    } else if (claim.status == "active" && claim.lease_expires_at < now_text) {
      out.violations.push_back(claim_violation(
          rule::terminal_verb, claim, std::format("claim is still active past its lease expiry at {}", claim.lease_expires_at)));
    }

    // Rule 4.
    if (is_worker_role(claim.role) && !claim.has_parent_action) {
      out.violations.push_back(
          claim_violation(rule::parent_action, claim, std::format("claim for role {} carries no parent action", *claim.role)));
    }

    // Rules 3, 5 and 6 depend on the policy the claim ran under.
    auto const policy = policy_for(claim.plan_id);
    if (!policy) {
      return std::unexpected(policy.error());
    }
    if (claim.digest.has_value() && *claim.digest != (*policy)->digest) {
      out.policy_changed.push_back(policy_changed_claim{.claim_id = claim.id, .claim_token = claim.token});
      continue;
    }
    if ((*policy)->require_model && claim.role.has_value() && *claim.role == "coder" &&
        (!claim.model.has_value() || claim.model->empty())) {
      out.violations.push_back(claim_violation(rule::coder_model, claim, "coder claim records no model under require_model"));
    }
    if (claim.status != "completed" || claim.entity_kind != "task") {
      continue;
    }
    if ((*policy)->use_queue && (*policy)->task_gate.has_value()) {
      auto const unmet = comp::unmet_completion_requirements(
          conn, claim.token, comp::completion_requirements{.review_required = false, .task_gate = (*policy)->task_gate}, lookup);
      if (!unmet) {
        return std::unexpected(failed());
      }
      for (auto const& item : *unmet) {
        out.violations.push_back(claim_violation(rule::queued_gate, claim, item.text));
      }
    }
    if ((*policy)->review_required) {
      auto const unmet = comp::unmet_completion_requirements(
          conn, claim.token, comp::completion_requirements{.review_required = true, .task_gate = std::nullopt}, lookup);
      if (!unmet) {
        return std::unexpected(failed());
      }
      for (auto const& item : *unmet) {
        out.violations.push_back(claim_violation(rule::independent_review, claim, item.text));
      }
    }
  }

  std::ranges::stable_sort(out.violations, [](const violation& lhs, const violation& rhs) {
    return std::tuple{rule_number(lhs.broken), lhs.entity_kind, lhs.entity_id} <
           std::tuple{rule_number(rhs.broken), rhs.entity_kind, rhs.entity_id};
  });
  return out;
}

auto render_text(const report& value) -> std::string {
  std::string out =
      std::format("conformance: plan {} ({} plan{} graded)\n", value.plan_id, value.plans, value.plans == 1 ? "" : "s");
  out += std::format("floor: claim {} ({}{})\n", value.floor.claim_id, floor_origin_name(value.floor.origin),
                     !value.floor.cut_at.has_value() ? "; nothing is graded"
                     : value.floor.cut_at->empty()   ? "; everything is graded"
                                                     : std::format("; since {}", *value.floor.cut_at));
  out += std::format("graded: {} claims, {} done tasks\n", value.claims_graded, value.done_tasks_graded);
  if (value.violations.empty()) {
    out += "violations: none\n";
  } else {
    out += std::format("violations: {}\n", value.violations.size());
    for (auto const& item : value.violations) {
      out +=
          std::format("  rule {} {}: {} {}", rule_number(item.broken), rule_name(item.broken), item.entity_kind, item.entity_id);
      if (item.claim_token.has_value()) {
        out += std::format(" [{}] on {}", short_token(*item.claim_token), *item.claimed_entity);
      }
      out += std::format(": {}\n", item.reason);
    }
  }
  if (value.overrides.empty()) {
    out += "overrides: none\n";
  } else {
    out += std::format("overrides: {}\n", value.overrides.size());
    for (auto const& item : value.overrides) {
      out += std::format("  task {}: {}\n", item.task_id, item.reason);
    }
  }
  if (!value.policy_changed.empty()) {
    out += std::format("graded without the policy rules (policy changed since the claim): {}\n", value.policy_changed.size());
    for (auto const& item : value.policy_changed) {
      out += std::format("  claim {} [{}]\n", item.claim_id, short_token(item.claim_token));
    }
  }
  out += std::format("before the floor: {} claims, {} done tasks\n", value.before.claims, value.before.done_tasks);
  return out;
}

auto render_json(const report& value) -> std::string {
  std::string out =
      std::format("{{\"plan_id\":{},\"plans\":{},\"conforms\":{},\"floor\":{{\"claim_id\":{},\"origin\":", value.plan_id,
                  value.plans, value.conforms() ? "true" : "false", value.floor.claim_id);
  json_text::append_json_string(out, floor_origin_name(value.floor.origin));
  out += ",\"cut_at\":";
  if (value.floor.cut_at.has_value()) {
    json_text::append_json_string(out, *value.floor.cut_at);
  } else {
    out += "null";
  }
  out += std::format("}},\"graded\":{{\"claims\":{},\"done_tasks\":{}}},\"violations\":[", value.claims_graded,
                     value.done_tasks_graded);
  bool first = true;
  for (auto const& item : value.violations) {
    out += first ? "" : ",";
    first = false;
    out += std::format("{{\"rule\":{},\"name\":", rule_number(item.broken));
    json_text::append_json_string(out, rule_name(item.broken));
    out += ",\"entity_kind\":";
    json_text::append_json_string(out, item.entity_kind);
    out += std::format(",\"entity_id\":{},\"claim_token\":", item.entity_id);
    if (item.claim_token.has_value()) {
      json_text::append_json_string(out, *item.claim_token);
    } else {
      out += "null";
    }
    out += ",\"claimed_entity\":";
    if (item.claimed_entity.has_value()) {
      json_text::append_json_string(out, *item.claimed_entity);
    } else {
      out += "null";
    }
    out += ",\"reason\":";
    json_text::append_json_string(out, item.reason);
    out += "}";
  }
  out += "],\"overrides\":[";
  first = true;
  for (auto const& item : value.overrides) {
    out += first ? "" : ",";
    first = false;
    out += std::format("{{\"task_id\":{},\"reason\":", item.task_id);
    json_text::append_json_string(out, item.reason);
    out += "}";
  }
  out += "],\"policy_changed\":[";
  first = true;
  for (auto const& item : value.policy_changed) {
    out += first ? "" : ",";
    first = false;
    out += std::format("{{\"claim_id\":{},\"claim_token\":", item.claim_id);
    json_text::append_json_string(out, item.claim_token);
    out += "}";
  }
  out += std::format("],\"before_floor\":{{\"claims\":{},\"done_tasks\":{}}}}}\n", value.before.claims, value.before.done_tasks);
  return out;
}

} // namespace planar::engine::runtime::conformance
