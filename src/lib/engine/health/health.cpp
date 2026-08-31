/// @file health.cpp
/// @brief Implementation of `planar.engine.health` (plan 996, task 6090).
/// See health.cppm for scope and omissions.

module planar.engine.health;

import std;
import planar.db;
import planar.db.migrations;
import planar.scope_ref;
import planar.installed_surface;

namespace planar::engine::health {

namespace {

/// @brief Run a statement that returns a single integer.
/// @return The value, or unset when the statement failed or produced no row.
auto int_query(db::connection& conn, std::string_view sql) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief Run `PRAGMA integrity_check` and report whether SQLite answered
/// the single "ok" row. Any other outcome (locked DB, corruption, a prepare
/// failure) is a fail-safe `false`.
auto check_integrity(db::connection& conn) -> bool {
  auto stmt = conn.prepare("PRAGMA integrity_check");
  if (!stmt) {
    return false;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return false;
  }
  return stmt->column_text(0) == "ok";
}

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

auto check(db::connection& conn, std::string_view db_path) -> std::expected<report, check_error> {
  auto schema_version = int_query(conn, "select coalesce(max(version), 0) from schema_migrations");
  if (!schema_version) {
    return std::unexpected(check_error::schema_table_missing);
  }
  auto migration_count = int_query(conn, "select count(*) from schema_migrations");
  if (!migration_count) {
    return std::unexpected(check_error::query_failed);
  }

  auto const         migrations    = db::migrations();
  std::int64_t const schema_target = migrations.empty() ? 0 : static_cast<std::int64_t>(migrations.back().version_);

  bool const integrity_ok = check_integrity(conn);

  // In-flight tasks (status in {doing, blocked}) bucketed into resumable
  // vs not-resumable. Resumable requires a non-empty next_action and at
  // least one context_snapshot row.
  std::int64_t const inflight_tasks =
      int_query(conn, "select count(*) from tasks where status in ('doing','blocked')").value_or(0);
  std::int64_t const resumable_tasks =
      int_query(conn, "select count(*) from tasks t"
                      " where t.status in ('doing','blocked')"
                      "   and coalesce(t.next_action,'') != ''"
                      "   and exists (select 1 from context_snapshots cs where cs.task_id = t.id)")
          .value_or(0);
  std::int64_t const not_resumable_tasks = inflight_tasks - resumable_tasks;

  std::int64_t const pending_handoffs =
      int_query(conn, "select count(*) from handoffs where status in ('pending', 'validated')").value_or(0);

  std::int64_t const stale_handoffs = int_query(conn, std::format("select count(*) from handoffs"
                                                                  " where status in ('pending','validated')"
                                                                  "   and (julianday('now') - julianday(created_at)) * 24 > {}",
                                                                  stale_handoff_threshold_hours))
                                          .value_or(0);

  bool const db_ok          = true;
  bool const schema_current = *schema_version == schema_target;
  bool const degraded       = !db_ok || !integrity_ok || not_resumable_tasks > 0 || stale_handoffs > 0;

  return report{
      .db_path             = std::string(db_path),
      .db_ok               = db_ok,
      .schema_version      = *schema_version,
      .schema_target       = schema_target,
      .schema_current      = schema_current,
      .migration_count     = *migration_count,
      .integrity_ok        = integrity_ok,
      .inflight_tasks      = inflight_tasks,
      .resumable_tasks     = resumable_tasks,
      .not_resumable_tasks = not_resumable_tasks,
      .pending_handoffs    = pending_handoffs,
      .stale_handoffs      = stale_handoffs,
      .projection_freshness =
          {
              .state              = "not_installed",
              .manifest_status    = installed_surface::manifest_state::missing,
              .managed            = 0,
              .fresh              = 0,
              .stale              = 0,
              .missing            = 0,
              .unmanaged          = 0,
              .unselected_vendors = installed_surface::supported_vendors.size(),
              .evidence           = "no managed Planar installation is recorded",
              .repair_command     = std::nullopt,
          },
      .overall = degraded ? "degraded" : "ok",
  };
}

auto with_projection_freshness(report report_in, const installed_surface::status_result& status) -> report {
  auto              report_out        = std::move(report_in);
  auto const        managed           = status.summary.fresh + status.summary.stale + status.summary.missing;
  bool const        manifest_degraded = status.manifest_status == installed_surface::manifest_state::legacy ||
                                        status.manifest_status == installed_surface::manifest_state::invalid ||
                                        status.manifest_status == installed_surface::manifest_state::unsupported;
  bool const        managed_degraded  = status.summary.stale > 0 || status.summary.missing > 0;
  bool const        degraded          = manifest_degraded || managed_degraded;
  std::string const state =
      degraded ? "degraded" : (status.manifest_status == installed_surface::manifest_state::missing ? "not_installed" : "fresh");
  std::optional<std::string> evidence = status.reason;
  if (!evidence.has_value() && managed_degraded) {
    evidence = "managed projections differ from the staged installation authority";
  }

  report_out.projection_freshness = projection_freshness{
      .state              = state,
      .manifest_status    = status.manifest_status,
      .managed            = managed,
      .fresh              = status.summary.fresh,
      .stale              = status.summary.stale,
      .missing            = status.summary.missing,
      .unmanaged          = status.summary.unmanaged,
      .unselected_vendors = status.summary.unselected_vendors,
      .evidence           = evidence,
      .repair_command     = degraded ? status.repair_command : std::nullopt,
  };
  if (degraded) {
    report_out.overall = "degraded";
  }
  return report_out;
}

auto manifest_state_name(installed_surface::manifest_state value) -> std::string_view {
  switch (value) {
  case installed_surface::manifest_state::current:
    return "current";
  case installed_surface::manifest_state::missing:
    return "missing";
  case installed_surface::manifest_state::legacy:
    return "legacy";
  case installed_surface::manifest_state::invalid:
    return "invalid";
  case installed_surface::manifest_state::unsupported:
    return "unsupported";
  }
  return "missing";
}

auto render_text(const report& report) -> std::string {
  std::string out;
  out += std::format("db:               {} ({})\n", report.db_ok ? "ok" : "ERROR", report.db_path);
  out += std::format("schema:           v{} of v{} ({})\n", report.schema_version, report.schema_target,
                     report.schema_current ? "current" : "behind");
  out += std::format("integrity:        {}\n", report.integrity_ok ? "ok" : "FAIL");
  out += std::format("in-flight tasks:  {} ({} resumable, {} NOT resumable)\n", report.inflight_tasks, report.resumable_tasks,
                     report.not_resumable_tasks);
  out += std::format("pending handoffs: {} ({} stale > {}h)\n", report.pending_handoffs, report.stale_handoffs,
                     stale_handoff_threshold_hours);
  out +=
      std::format("projection freshness: {} ({} managed: {} fresh, {} stale, {} missing; {} unmanaged; {} unselected vendors)\n",
                  report.projection_freshness.state, report.projection_freshness.managed, report.projection_freshness.fresh,
                  report.projection_freshness.stale, report.projection_freshness.missing, report.projection_freshness.unmanaged,
                  report.projection_freshness.unselected_vendors);
  out += std::format("projection manifest:  {}\n", manifest_state_name(report.projection_freshness.manifest_status));
  if (report.projection_freshness.evidence.has_value()) {
    out += std::format("projection evidence:  {}\n", *report.projection_freshness.evidence);
  }
  if (report.projection_freshness.repair_command.has_value()) {
    out += std::format("projection repair:    {}\n", *report.projection_freshness.repair_command);
  }
  out += std::format("overall:          {}\n", report.overall);
  return out;
}

} // namespace planar::engine::health
