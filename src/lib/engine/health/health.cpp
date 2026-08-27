/// @file health.cpp
/// @brief Implementation of `planar.engine.health` (plan 996, task 6090).
/// See health.cppm for scope and omissions.

module planar.engine.health;

import std;
import planar.db;
import planar.scope_ref;

namespace planar::engine::health {

namespace {

/// @brief Bind a nullable association-id filter.
///
/// Both `?` slots in each query take the SAME value, and the pattern is
/// `(? is null or (scope_kind='association' and scope_id=?))` — so an unset
/// filter short-circuits the whole predicate to true rather than needing a
/// separate statement per arm.
auto bind_scope(db::statement& stmt, int index, std::optional<std::int64_t> association_id) -> bool {
  if (association_id.has_value()) {
    return stmt.bind_int64(index, *association_id).has_value();
  }
  return stmt.bind_null(index).has_value();
}

auto bind_now(db::statement& stmt, int index, const std::optional<std::string>& now) -> bool {
  if (now.has_value()) {
    return stmt.bind_text(index, *now).has_value();
  }
  return stmt.bind_null(index).has_value();
}

auto query_stale_draft_plans(db::connection& conn, std::optional<std::int64_t> association_id)
    -> std::expected<std::vector<stale_draft_plan>, hygiene_error> {
  auto stmt = conn.prepare("select p.id, p.parent_plan_id, p.title,"
                           " sum(case when t.status='todo' then 1 else 0 end),"
                           " sum(case when t.status='doing' then 1 else 0 end),"
                           " sum(case when t.status='blocked' then 1 else 0 end),"
                           " sum(case when t.status='done' then 1 else 0 end),"
                           " sum(case when t.status='cancelled' then 1 else 0 end), count(t.id)"
                           " from plans p left join tasks t on t.plan_id=p.id"
                           " where p.status='draft' and (? is null or (p.scope_kind='association' and p.scope_id=?))"
                           " group by p.id, p.parent_plan_id, p.title"
                           " having count(t.id)=0 or sum(case when t.status not in ('done','cancelled') then 1 else 0 end)=0"
                           " order by p.id");
  if (!stmt) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!bind_scope(*stmt, 1, association_id) || !bind_scope(*stmt, 2, association_id)) {
    return std::unexpected(hygiene_error::query_failed);
  }

  std::vector<stale_draft_plan> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(hygiene_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    auto const id         = stmt->column_int64(0);
    auto const task_count = stmt->column_int64(8);
    rows.push_back(stale_draft_plan{
        .id             = id,
        .parent_plan_id = stmt->is_null(1) ? std::nullopt : std::optional<std::int64_t>{stmt->column_int64(1)},
        .title          = stmt->column_text(2),
        .reason         = task_count == 0 ? "zero_tasks" : "all_tasks_terminal",
        .counts =
            task_counts{
                .todo      = stmt->column_int64(3),
                .doing     = stmt->column_int64(4),
                .blocked   = stmt->column_int64(5),
                .done      = stmt->column_int64(6),
                .cancelled = stmt->column_int64(7),
            },
        // The suggested status follows the SAME `task_count == 0` test as
        // `reason`, so the two can never disagree: a zero-task draft is
        // `abandoned`, an all-terminal one is `done`.
        .suggestion = std::format("planar plan update {} --status {}", id, task_count == 0 ? "abandoned" : "done"),
    });
  }
}

auto query_stale_doing_tasks(db::connection& conn, std::optional<std::int64_t> association_id, std::int64_t threshold_days,
                             const std::optional<std::string>& now)
    -> std::expected<std::vector<stale_doing_task>, hygiene_error> {
  auto stmt = conn.prepare("select t.id, t.plan_id, t.title,"
                           " cast(julianday(coalesce(?, 'now'))-julianday(t.updated_at) as integer),"
                           " case t.scope_kind"
                           "   when 'global' then 'global'"
                           "   when 'association' then 'assoc:' || a.slug"
                           "   when 'repo' then 'repo:' || p.slug"
                           " end"
                           " from tasks t"
                           " left join associations a on t.scope_kind='association' and a.id=t.scope_id"
                           " left join projects p on t.scope_kind='repo' and p.id=t.scope_id"
                           " where t.status='doing' and julianday(coalesce(?, 'now'))-julianday(t.updated_at)>?"
                           " and (? is null or (t.scope_kind='association' and t.scope_id=?)) order by t.id");
  if (!stmt) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!bind_now(*stmt, 1, now) || !bind_now(*stmt, 2, now)) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!stmt->bind_int64(3, threshold_days)) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!bind_scope(*stmt, 4, association_id) || !bind_scope(*stmt, 5, association_id)) {
    return std::unexpected(hygiene_error::query_failed);
  }

  std::vector<stale_doing_task> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(hygiene_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    auto const id = stmt->column_int64(0);
    // The `case` has no `else`, so an unrecognised `scope_kind` yields
    // NULL. The column CHECK constraint makes that unreachable; reading it
    // as the empty string rather than crashing keeps a corrupt row a
    // reportable finding instead of a fault.
    auto const scope = stmt->is_null(4) ? std::string{} : stmt->column_text(4);
    rows.push_back(stale_doing_task{
        .id       = id,
        .plan_id  = stmt->column_int64(1),
        .scope    = scope,
        .title    = stmt->column_text(2),
        .age_days = stmt->column_int64(3),
        .suggestion =
            std::format("planar task update {} --scope {} --status done OR planar task update {} --scope {} --status blocked", id,
                        scope, id, scope),
    });
  }
}

auto query_stale_open_questions(db::connection& conn, std::optional<std::int64_t> association_id, std::int64_t threshold_days,
                                const std::optional<std::string>& now)
    -> std::expected<std::vector<stale_open_question>, hygiene_error> {
  auto stmt = conn.prepare("select id, title, cast(julianday(coalesce(?, 'now'))-julianday(updated_at) as integer)"
                           " from questions where status='open' and julianday(coalesce(?, 'now'))-julianday(updated_at)>?"
                           " and (? is null or (scope_kind='association' and scope_id=?)) order by id");
  if (!stmt) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!bind_now(*stmt, 1, now) || !bind_now(*stmt, 2, now)) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!stmt->bind_int64(3, threshold_days)) {
    return std::unexpected(hygiene_error::query_failed);
  }
  if (!bind_scope(*stmt, 4, association_id) || !bind_scope(*stmt, 5, association_id)) {
    return std::unexpected(hygiene_error::query_failed);
  }

  std::vector<stale_open_question> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(hygiene_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    auto const id = stmt->column_int64(0);
    rows.push_back(stale_open_question{
        .id         = id,
        .title      = stmt->column_text(1),
        .age_days   = stmt->column_int64(2),
        .suggestion = std::format("planar question answer {} --answer \"<resolution>\" OR planar question wontfix {}", id, id),
    });
  }
}

} // namespace

auto hygiene(db::connection& conn, const hygiene_options& options) -> std::expected<hygiene_report, hygiene_error> {
  if (options.stale_doing_days < 0 || options.stale_open_days < 0) {
    return std::unexpected(hygiene_error::invalid_threshold);
  }

  std::optional<std::int64_t> association_id;
  if (options.scope.has_value()) {
    auto ref = scope_ref::resolve(conn, *options.scope);
    if (!ref) {
      // NOTE the mapping: `slug_not_found` passes through, but ANY other
      // resolution failure becomes `query_failed`. That is the oracle's
      // `else => return error.QueryFailed`.
      return std::unexpected(ref.error() == scope_ref::error::slug_not_found ? hygiene_error::slug_not_found
                                                                             : hygiene_error::query_failed);
    }
    if (ref->kind != scope_ref::scope_kind::association || !ref->id.has_value()) {
      return std::unexpected(hygiene_error::unsupported_scope);
    }
    association_id = *ref->id;
  }

  auto plans = query_stale_draft_plans(conn, association_id);
  if (!plans) {
    return std::unexpected(plans.error());
  }
  auto tasks = query_stale_doing_tasks(conn, association_id, options.stale_doing_days, options.now);
  if (!tasks) {
    return std::unexpected(tasks.error());
  }
  auto questions = query_stale_open_questions(conn, association_id, options.stale_open_days, options.now);
  if (!questions) {
    return std::unexpected(questions.error());
  }

  return hygiene_report{
      .thresholds           = {.stale_doing_days = options.stale_doing_days, .stale_open_days = options.stale_open_days},
      .stale_draft_plans    = std::move(*plans),
      .stale_doing_tasks    = std::move(*tasks),
      .stale_open_questions = std::move(*questions),
  };
}

auto render_hygiene_text(const hygiene_report& report) -> std::string {
  std::string out = "=== Stale draft plans (all tasks terminal) ===\n";
  if (report.stale_draft_plans.empty()) {
    out += "  none\n";
  }
  for (auto const& row : report.stale_draft_plans) {
    if (row.parent_plan_id.has_value()) {
      out += std::format("  plan {} (parent: plan:{}): \"{}\"\n", row.id, *row.parent_plan_id, row.title);
    } else {
      out += std::format("  plan {}: \"{}\"\n", row.id, row.title);
    }
    out += std::format("    tasks: {} todo, {} doing, {} blocked, {} done, {} cancelled\n", row.counts.todo, row.counts.doing,
                       row.counts.blocked, row.counts.done, row.counts.cancelled);
    out += std::format("    suggest: {}  ({})\n", row.suggestion,
                       row.reason == "zero_tasks" ? "no tasks ever attached" : "all tasks terminal");
  }

  out += std::format("\n=== Stale doing tasks (status=doing for >{} days) ===\n", report.thresholds.stale_doing_days);
  if (report.stale_doing_tasks.empty()) {
    out += "  none\n";
  }
  for (auto const& row : report.stale_doing_tasks) {
    out += std::format("  task {} (plan {}, last touched {}d ago): \"{}\"\n", row.id, row.plan_id, row.age_days, row.title);
    out += std::format("    suggest: {}\n", row.suggestion);
  }

  out += std::format("\n=== Stale open questions (status=open for >{} days) ===\n", report.thresholds.stale_open_days);
  if (report.stale_open_questions.empty()) {
    out += "  none\n";
  }
  for (auto const& row : report.stale_open_questions) {
    out += std::format("  question {} ({}d old): \"{}\"\n", row.id, row.age_days, row.title);
    out += std::format("    suggest: {}\n", row.suggestion);
  }
  return out;
}

} // namespace planar::engine::health
