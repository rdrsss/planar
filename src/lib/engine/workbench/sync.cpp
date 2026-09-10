/// @file sync.cpp
/// @brief Implementation of `planar.engine.workbench.sync` (plan 996, task
/// 6037). See sync.cppm for the classification table and the filter rules.

module planar.engine.workbench.sync;

import std;
import planar.db;
import planar.engine.workbench.feature;
import planar.engine.workbench.fsutil;
import planar.engine.workbench.manifest;
import planar.engine.workbench.parse;
import planar.engine.workbench.render;
import planar.engine.workbench.terminal;

namespace planar::engine::workbench::sync {

namespace {

/// @brief One enumerated entity: what to render and how to classify it.
struct entity {
  std::string  kind;
  std::int64_t id = 0;
  std::string  updated_at;
  std::string  status; ///< Empty when unreachable; the filter treats that as active.
};

/// @brief Run a statement to completion, discarding rows.
auto exec_step(db::connection& conn, std::string_view sql, auto&& bind) -> bool {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return false;
  }
  if (!bind(*stmt)) {
    return false;
  }
  return stmt->step().has_value();
}

auto table_for_kind(std::string_view kind) -> std::string_view {
  if (kind == "plan") {
    return "plans";
  }
  if (kind == "task") {
    return "tasks";
  }
  if (kind == "artifact") {
    return "artifacts";
  }
  if (kind == "decision") {
    return "decisions";
  }
  if (kind == "question") {
    return "questions";
  }
  if (kind == "scenario") {
    return "test_scenarios";
  }
  return {};
}

auto status_sql_for_kind(std::string_view kind) -> std::string_view {
  if (kind == "plan") {
    return "select coalesce(status, '') from plans where id = ?";
  }
  if (kind == "task") {
    return "select coalesce(status, '') from tasks where id = ?";
  }
  if (kind == "decision") {
    return "select coalesce(status, '') from decisions where id = ?";
  }
  if (kind == "question") {
    return "select coalesce(status, '') from questions where id = ?";
  }
  if (kind == "scenario" || kind == "test_scenario") {
    return "select coalesce(status, '') from test_scenarios where id = ?";
  }
  if (kind == "artifact") {
    return "select coalesce(status, '') from artifacts where id = ?";
  }
  return {};
}

auto fetch_status(db::connection& conn, std::string_view kind, std::int64_t id) -> std::string {
  auto const sql = status_sql_for_kind(kind);
  if (sql.empty()) {
    return {};
  }
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, id)) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return {};
  }
  return stmt->column_text(0);
}

auto fetch_updated_at(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<std::string, sync_error> {
  auto const table = table_for_kind(kind);
  if (table.empty()) {
    return std::unexpected(sync_error::not_found);
  }
  auto stmt = conn.prepare(std::format("select coalesce(updated_at, '') from {} where id = ?", table));
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  return stmt->column_text(0);
}

/// @brief The feature-directory key: the plan's `external_id` when it has an
/// external link, else `p<id>`.
///
/// gc.zig carries a comment recording that hardcoding `p<id>` here made GC
/// compute the wrong feature directory for externally-linked plans and
/// silently skip their trees. One definition, used by every path.
auto resolve_plan_key(db::connection& conn, std::int64_t plan_id) -> std::string {
  auto const fallback = std::format("p{}", plan_id);
  auto       stmt     = conn.prepare("select coalesce(external_id, '') from external_links "
                                     "where entity_kind = 'plan' and entity_id = ? order by id limit 1");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return fallback;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return fallback;
  }
  auto external = stmt->column_text(0);
  if (external.empty()) {
    return fallback;
  }
  return external;
}

auto contains_entity(std::span<const entity> items, std::string_view kind, std::int64_t id) -> bool {
  return std::ranges::any_of(items, [&](const entity& e) { return e.id == id && e.kind == kind; });
}

auto append_plan_tasks(db::connection& conn, std::int64_t plan_id, std::vector<entity>& out) -> bool {
  // Tasks carry their plan reference INLINE (`tasks.plan_id`), unlike
  // artifacts/decisions/scenarios which reach a plan through `entity_links`.
  // Without this pass the workbench would never enumerate a task at all.
  auto stmt = conn.prepare("select id, coalesce(updated_at, ''), coalesce(status, '') from tasks "
                           "where plan_id = ? order by id");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return false;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return false;
    }
    if (*stepped == db::step_result::done) {
      return true;
    }
    auto const id = stmt->column_int64(0);
    if (contains_entity(out, "task", id)) {
      continue;
    }
    out.push_back(entity{.kind = "task", .id = id, .updated_at = stmt->column_text(1), .status = stmt->column_text(2)});
  }
}

auto append_derived_entities(db::connection& conn, std::int64_t plan_id, std::vector<entity>& out) -> bool {
  auto stmt = conn.prepare("select case when from_kind = 'test_scenario' then 'scenario' else from_kind end, from_id "
                           "from entity_links where to_kind = 'plan' and to_id = ? and relationship = 'derives-from' "
                           "order by from_kind, from_id");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return false;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return false;
    }
    if (*stepped == db::step_result::done) {
      return true;
    }
    auto const kind = stmt->column_text(0);
    auto const id   = stmt->column_int64(1);
    if (contains_entity(out, kind, id)) {
      continue;
    }
    auto updated = fetch_updated_at(conn, kind, id);
    if (!updated) {
      // A dangling link. The Zig original propagates the error out of the
      // whole run; reproduced.
      return false;
    }
    out.push_back(entity{.kind = kind, .id = id, .updated_at = *updated, .status = fetch_status(conn, kind, id)});
  }
}

/// @brief The anchor plan, everything derived from it, its tasks, its child
/// plans, and each child's derived entities and tasks — in that order.
///
/// The order is observable: it is the order `entries` appears in `--json`
/// and in the verbose text listing.
auto enumerate_entities(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<std::vector<entity>, sync_error> {
  std::vector<entity> out;
  auto                anchor_updated = fetch_updated_at(conn, "plan", anchor_plan_id);
  if (!anchor_updated) {
    return std::unexpected(anchor_updated.error());
  }
  out.push_back(entity{
      .kind = "plan", .id = anchor_plan_id, .updated_at = *anchor_updated, .status = fetch_status(conn, "plan", anchor_plan_id)});

  if (!append_derived_entities(conn, anchor_plan_id, out) || !append_plan_tasks(conn, anchor_plan_id, out)) {
    return std::unexpected(sync_error::query_failed);
  }

  std::vector<std::int64_t> child_ids;
  {
    auto stmt = conn.prepare("select id, coalesce(updated_at, '') from plans where parent_plan_id = ? order by id");
    if (!stmt || !stmt->bind_int64(1, anchor_plan_id)) {
      return std::unexpected(sync_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(sync_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      auto const child_id = stmt->column_int64(0);
      child_ids.push_back(child_id);
      if (!contains_entity(out, "plan", child_id)) {
        out.push_back(entity{
            .kind = "plan", .id = child_id, .updated_at = stmt->column_text(1), .status = fetch_status(conn, "plan", child_id)});
      }
    }
  }
  for (auto const child_id : child_ids) {
    if (!append_derived_entities(conn, child_id, out) || !append_plan_tasks(conn, child_id, out)) {
      return std::unexpected(sync_error::query_failed);
    }
  }
  return out;
}

auto load_task_touches(db::connection& conn, std::int64_t task_id) -> std::vector<std::string> {
  std::vector<std::string> out;
  auto                     stmt = conn.prepare("select p.slug from entity_links el join projects p on p.id = el.to_id "
                                               "where el.from_kind = 'task' and el.from_id = ? and el.to_kind = 'repo' "
                                               "and el.relationship = 'touches' order by el.id");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return out;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped == db::step_result::done) {
      return out;
    }
    out.push_back(stmt->column_text(0));
  }
}

auto repo_slug_to_fs(std::string_view slug) -> std::string {
  std::string out{slug};
  for (char& ch : out) {
    if (ch == '/') {
      ch = '_';
    }
  }
  return out;
}

/// @brief Which `tasks/<dir>/` a task's file lives in.
///
/// A repo-scoped task goes under its own repo slug; otherwise the FIRST
/// `touches` repo wins; otherwise `tasks/cross`.
auto resolve_task_dir(db::connection& conn, std::string_view scope_kind, std::optional<std::int64_t> scope_id,
                      std::span<const std::string> touches) -> std::string {
  if (scope_kind == "repo" && scope_id.has_value()) {
    auto stmt = conn.prepare("select coalesce(slug, '') from projects where id = ?");
    if (stmt && stmt->bind_int64(1, *scope_id)) {
      auto stepped = stmt->step();
      if (stepped && *stepped == db::step_result::row) {
        return std::format("tasks/{}", repo_slug_to_fs(stmt->column_text(0)));
      }
    }
  }
  if (!touches.empty()) {
    return std::format("tasks/{}", repo_slug_to_fs(touches.front()));
  }
  return "tasks/cross";
}

// ---------------------------------------------------------------------------
// Per-kind renderers. Every body string below is byte-for-byte what a real
// `workbench push` wrote to a scratch tree; the two-space line endings
// before a newline are Markdown hard breaks and are load-bearing.

auto render_plan(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  auto stmt = conn.prepare("select coalesce(title,''), coalesce(slug,''), coalesce(summary,''), "
                           "coalesce(status,''), coalesce(created_at,''), coalesce(updated_at,'') "
                           "from plans where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const title      = stmt->column_text(0);
  auto const slug       = stmt->column_text(1);
  auto const summary    = stmt->column_text(2);
  auto const status_txt = stmt->column_text(3);
  auto const created_at = stmt->column_text(4);
  auto const updated_at = stmt->column_text(5);

  parse::front_matter fm{
      .entity_kind = "plan", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt};
  auto const body = std::format("# Plan {}: {}\n\n**Status:** {}  \n**Created:** {}  \n**Updated:** {}\n{}{}\n", id, title,
                                status_txt, created_at, updated_at, summary.empty() ? "" : "\n", summary);
  return rendered_entity{.rel_path = id == anchor_plan_id ? std::string{"README.md"} : std::format("plans/{}.md", slug),
                         .content  = render::render(fm, body)};
}

auto render_task(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  auto stmt = conn.prepare("select scope_kind, scope_id, coalesce(title,''), coalesce(body,''), "
                           "coalesce(status,''), priority, coalesce(next_action,''), coalesce(due_at,''), "
                           "coalesce(created_at,''), coalesce(updated_at,'') from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const                  scope_kind = stmt->column_text(0);
  std::optional<std::int64_t> scope_id;
  if (!stmt->is_null(1)) {
    scope_id = stmt->column_int64(1);
  }
  auto const title       = stmt->column_text(2);
  auto const body_text   = stmt->column_text(3);
  auto const status_txt  = stmt->column_text(4);
  auto const priority    = stmt->column_int64(5);
  auto const next_action = stmt->column_text(6);
  auto const due_at      = stmt->column_text(7);
  auto const created_at  = stmt->column_text(8);
  auto const updated_at  = stmt->column_text(9);

  auto const touches  = load_task_touches(conn, id);
  auto const task_dir = resolve_task_dir(conn, scope_kind, scope_id, touches);

  parse::front_matter fm{.entity_kind    = "task",
                         .entity_id      = id,
                         .anchor_plan_id = anchor_plan_id,
                         .title          = title,
                         .status         = status_txt,
                         .priority       = priority,
                         .touches        = touches};

  std::string body = std::format("# Task {}: {}\n\n**Status:** {}  \n**Priority:** {}  \n**Created:** {}  \n**Updated:** {}\n",
                                 id, title, status_txt, priority, created_at, updated_at);
  if (!due_at.empty()) {
    std::format_to(std::back_inserter(body), "**Due:** {}\n", due_at);
  }
  if (!body_text.empty()) {
    std::format_to(std::back_inserter(body), "\n{}", body_text);
  }
  if (!next_action.empty()) {
    std::format_to(std::back_inserter(body), "\n**Next action:** {}", next_action);
  }
  body += '\n';

  return rendered_entity{.rel_path = std::format("{}/{}-{}.md", task_dir, id, feature::slugify(title)),
                         .content  = render::render(fm, body)};
}

auto render_artifact(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  auto stmt = conn.prepare(
      "select coalesce(title,''), coalesce(status,''), coalesce(kind,''), coalesce(body,'') from artifacts where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const title      = stmt->column_text(0);
  auto const status_txt = stmt->column_text(1);
  auto const kind_txt   = stmt->column_text(2);
  auto const body_txt   = stmt->column_text(3);

  parse::front_matter fm{.entity_kind    = "artifact",
                         .entity_id      = id,
                         .anchor_plan_id = anchor_plan_id,
                         .title          = title,
                         .status         = status_txt,
                         .artifact_kind  = kind_txt};
  auto const body = std::format("# Artifact {}: {}\n\n**Kind:** {}  \n**Status:** {}\n\n## Content\n\n{}\n", id, title, kind_txt,
                                status_txt, body_txt);
  // At the feature-dir ROOT, not under `artifacts/`. Oracle-confirmed.
  return rendered_entity{.rel_path = feature::artifact_filename(id, title, kind_txt), .content = render::render(fm, body)};
}

auto render_decision(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  auto stmt = conn.prepare("select coalesce(title,''), coalesce(status,''), coalesce(body,''), "
                           "coalesce(rationale,''), coalesce(created_at,''), coalesce(updated_at,'') "
                           "from decisions where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const title      = stmt->column_text(0);
  auto const status_txt = stmt->column_text(1);
  auto const body_txt   = stmt->column_text(2);
  auto const rationale  = stmt->column_text(3);
  auto const created_at = stmt->column_text(4);
  auto const updated_at = stmt->column_text(5);

  parse::front_matter fm{
      .entity_kind = "decision", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt};
  auto const body = std::format(
      "# Decision {}: {}\n\n**Status:** {}  \n**Created:** {}  \n**Updated:** {}\n\n## Body\n\n{}\n\n## Rationale\n\n{}\n", id,
      title, status_txt, created_at, updated_at, body_txt, rationale);
  return rendered_entity{.rel_path = std::format("decisions/{}-{}.md", id, feature::slugify(title)),
                         .content  = render::render(fm, body)};
}

auto render_question(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  auto stmt = conn.prepare("select coalesce(title,''), coalesce(status,''), coalesce(body,''), "
                           "coalesce(answer_body,''), coalesce(answered_at,''), coalesce(created_at,''), "
                           "coalesce(updated_at,'') from questions where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const title       = stmt->column_text(0);
  auto const status_txt  = stmt->column_text(1);
  auto const body_txt    = stmt->column_text(2);
  auto const answer_body = stmt->column_text(3);
  auto const answered_at = stmt->column_text(4);
  auto const created_at  = stmt->column_text(5);
  auto const updated_at  = stmt->column_text(6);

  parse::front_matter fm{
      .entity_kind = "question", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt};
  std::string body = std::format("# Question {}: {}\n\n**Status:** {}  \n**Created:** {}  \n**Updated:** {}\n", id, title,
                                 status_txt, created_at, updated_at);
  if (!body_txt.empty()) {
    std::format_to(std::back_inserter(body), "\n{}\n", body_txt);
  }
  if (!answer_body.empty()) {
    std::format_to(std::back_inserter(body), "\n**Answer:** {}\n", answer_body);
    if (!answered_at.empty()) {
      std::format_to(std::back_inserter(body), "\n**Answered at:** {}\n", answered_at);
    }
  }
  return rendered_entity{.rel_path = std::format("questions/{}-{}.md", id, feature::slugify(title)),
                         .content  = render::render(fm, body)};
}

auto render_scenario(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  auto stmt = conn.prepare("select coalesce(title,''), coalesce(status,''), coalesce(body,''), "
                           "coalesce(last_run_at,''), coalesce(last_outcome,''), coalesce(created_at,''), "
                           "coalesce(updated_at,'') from test_scenarios where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const title        = stmt->column_text(0);
  auto const status_txt   = stmt->column_text(1);
  auto const body_txt     = stmt->column_text(2);
  auto const last_run_at  = stmt->column_text(3);
  auto const last_outcome = stmt->column_text(4);
  auto const created_at   = stmt->column_text(5);
  auto const updated_at   = stmt->column_text(6);

  parse::front_matter fm{
      .entity_kind = "scenario", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt};
  std::string body = std::format("# Scenario {}: {}\n\n**Status:** {}  \n**Created:** {}  \n**Updated:** {}\n", id, title,
                                 status_txt, created_at, updated_at);
  if (!last_run_at.empty()) {
    std::format_to(std::back_inserter(body), "\n**Last run:** {}", last_run_at);
  }
  if (!last_outcome.empty()) {
    std::format_to(std::back_inserter(body), "  \n**Last outcome:** {}", last_outcome);
  }
  if (!body_txt.empty()) {
    std::format_to(std::back_inserter(body), "\n\n{}\n", body_txt);
  }
  return rendered_entity{.rel_path = std::format("scenarios/{}-{}.md", id, feature::slugify(title)),
                         .content  = render::render(fm, body)};
}

// ---------------------------------------------------------------------------
// FS -> DB application.

auto resolve_project_id_by_slug(db::connection& conn, std::string_view slug) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare("select id from projects where slug = ?");
  if (!stmt || !stmt->bind_text(1, slug)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief Make a task's `touches` edges match the front matter exactly.
///
/// A slug naming no known project is SILENTLY SKIPPED (not an error, and not
/// a project creation); an edge present in the DB but absent from the front
/// matter is removed.
auto reconcile_touches(db::connection& conn, std::int64_t task_id, std::span<const std::string> slugs) -> bool {
  std::set<std::int64_t> want;
  for (auto const& slug : slugs) {
    if (auto const id = resolve_project_id_by_slug(conn, slug)) {
      want.insert(*id);
    }
  }
  std::set<std::int64_t> have;
  {
    auto stmt = conn.prepare("select to_id from entity_links where from_kind = 'task' and from_id = ? "
                             "and to_kind = 'repo' and relationship = 'touches'");
    if (!stmt || !stmt->bind_int64(1, task_id)) {
      return false;
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return false;
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      have.insert(stmt->column_int64(0));
    }
  }
  for (auto const repo_id : want) {
    if (have.contains(repo_id)) {
      continue;
    }
    if (!exec_step(
            conn,
            "insert or ignore into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
            "values ('task', ?, 'repo', ?, 'touches')",
            [&](db::statement& s) { return s.bind_int64(1, task_id).has_value() && s.bind_int64(2, repo_id).has_value(); })) {
      return false;
    }
  }
  for (auto const repo_id : have) {
    if (want.contains(repo_id)) {
      continue;
    }
    if (!exec_step(
            conn,
            "delete from entity_links where from_kind = 'task' and from_id = ? and to_kind = 'repo' "
            "and to_id = ? and relationship = 'touches'",
            [&](db::statement& s) { return s.bind_int64(1, task_id).has_value() && s.bind_int64(2, repo_id).has_value(); })) {
      return false;
    }
  }
  return true;
}

/// @brief Apply one file's contents onto its entity row.
///
/// Wrapped in a SAVEPOINT (not a transaction) so a partial `touches`
/// reconcile rolls back without disturbing any transaction the caller may
/// already hold — the same reason the Zig original uses one.
/// @return `true` when the entity was updated. A `false` return is NOT an
/// error: the caller counts the file as pending and moves on.
auto pull_to_db(db::connection& conn, std::string_view kind, std::int64_t id, std::string_view file_content) -> bool {
  auto const parsed = parse::parse(file_content);
  if (!parsed) {
    return false;
  }
  auto const body   = extract_body_text(parsed->body);
  auto const status = std::string_view{parsed->frontmatter.status};

  if (!conn.execute("savepoint workbench_pull_entity")) {
    return false;
  }
  auto const rollback = [&] {
    static_cast<void>(conn.execute("rollback to savepoint workbench_pull_entity"));
    static_cast<void>(conn.execute("release savepoint workbench_pull_entity"));
  };

  auto const bind_body_only = [&](std::string_view sql) {
    return exec_step(conn, sql,
                     [&](db::statement& s) { return s.bind_text(1, body).has_value() && s.bind_int64(2, id).has_value(); });
  };
  auto const bind_body_and_status = [&](std::string_view sql) {
    return exec_step(conn, sql, [&](db::statement& s) {
      return s.bind_text(1, body).has_value() && s.bind_text(2, status).has_value() && s.bind_int64(3, id).has_value();
    });
  };

  bool ok = false;
  if (kind == "task") {
    // An EMPTY status in the front matter leaves the DB status alone; that
    // is why `status:` is not simply always written.
    ok = status.empty()
             ? bind_body_only("update tasks set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?")
             : bind_body_and_status("update tasks set body = ?, status = ?, "
                                    "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
    ok = ok && reconcile_touches(conn, id, parsed->frontmatter.touches);
  } else if (kind == "plan") {
    ok = status.empty()
             ? bind_body_only("update plans set summary = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?")
             : bind_body_and_status("update plans set summary = ?, status = ?, "
                                    "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  } else if (kind == "artifact") {
    ok = bind_body_only("update artifacts set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  } else if (kind == "decision") {
    ok = bind_body_only("update decisions set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  } else if (kind == "question") {
    ok = bind_body_only("update questions set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  } else if (kind == "scenario") {
    ok = bind_body_only("update test_scenarios set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  }
  // NOTE: for every kind except `task` and `plan`, the front matter's
  // `status` is IGNORED on the way in. Only the body round-trips. That is
  // the shipped behavior, reproduced rather than generalized (D2).

  if (!ok) {
    rollback();
    return false;
  }
  if (!conn.execute("release savepoint workbench_pull_entity")) {
    rollback();
    return false;
  }
  return true;
}

/// @brief Set an entity to its soft-deleted status.
///
/// This is what a file DISAPPEARING from the workbench means: the entity is
/// cancelled/abandoned/retired/wontfix/withdrawn, never row-deleted.
auto soft_delete_entity(db::connection& conn, std::string_view kind, std::int64_t id) -> bool {
  std::string_view sql;
  if (kind == "task") {
    sql = "update tasks set status = 'cancelled', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?";
  } else if (kind == "plan") {
    sql = "update plans set status = 'abandoned', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?";
  } else if (kind == "artifact") {
    sql = "update artifacts set status = 'retired', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?";
  } else if (kind == "scenario") {
    // INTENTIONAL BYPASS of the status-transition matrix, carried over from
    // the Zig original: `retired` is a legal destination from every
    // non-retired scenario status and an identity from `retired` itself, so
    // the check would never refuse anything it reaches.
    sql = "update test_scenarios set status = 'retired', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?";
  } else if (kind == "question") {
    sql = "update questions set status = 'wontfix', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?";
  } else if (kind == "decision") {
    sql = "update decisions set status = 'withdrawn', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?";
  } else {
    return true; // Unknown kind: nothing to do, and not an error.
  }
  return exec_step(conn, sql, [&](db::statement& s) { return s.bind_int64(1, id).has_value(); });
}

auto insert_task_from_frontmatter(db::connection& conn, const parse::front_matter& fm, std::string_view body,
                                  std::optional<std::int64_t> assoc_id) -> std::optional<std::int64_t> {
  auto const title     = fm.title.empty() ? std::string_view{"Task from workbench"} : std::string_view{fm.title};
  auto const status    = fm.status.empty() ? std::string_view{"todo"} : std::string_view{fm.status};
  auto const body_text = extract_body_text(body);
  auto const priority  = fm.priority == 0 ? std::int64_t{100} : fm.priority;

  auto stmt = assoc_id.has_value() ? conn.prepare("insert into tasks (scope_kind, scope_id, title, body, status, priority) "
                                                  "values ('association', ?, ?, ?, ?, ?) returning id")
                                   : conn.prepare("insert into tasks (scope_kind, scope_id, title, body, status, priority) "
                                                  "values ('global', null, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::nullopt;
  }
  int index = 1;
  if (assoc_id.has_value() && !stmt->bind_int64(index++, *assoc_id)) {
    return std::nullopt;
  }
  if (!stmt->bind_text(index++, title) || !stmt->bind_text(index++, body_text) || !stmt->bind_text(index++, status) ||
      !stmt->bind_int64(index, priority)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return std::nullopt;
  }
  auto const new_id = stmt->column_int64(0);
  if (!reconcile_touches(conn, new_id, fm.touches)) {
    return std::nullopt;
  }
  return new_id;
}

// ---------------------------------------------------------------------------
// Conflict events.

struct conflict_context {
  std::int64_t anchor_plan_id = 0;
  std::string  entity_kind;
  std::int64_t entity_id = 0;
  std::string  file_path;
};

/// @brief Read one string field out of a flat conflict-context object.
///
/// A four-field flat object with no nesting, no escapes and no arrays is not
/// worth a JSON reader; this scans for `"key":"` and takes bytes up to the
/// next `"`. It is the exact inverse of `render_conflict_context` below and
/// they are pinned against each other in sync.t.cpp.
auto context_string(std::string_view raw, std::string_view key) -> std::optional<std::string> {
  auto const needle = std::format("\"{}\":\"", key);
  auto const at     = raw.find(needle);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  auto const start = at + needle.size();
  auto const end   = raw.find('"', start);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string{raw.substr(start, end - start)};
}

auto context_int(std::string_view raw, std::string_view key) -> std::optional<std::int64_t> {
  auto const needle = std::format("\"{}\":", key);
  auto const at     = raw.find(needle);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  auto const   start   = at + needle.size();
  auto const   end     = raw.find_first_of(",}", start);
  std::int64_t value   = 0;
  auto const   text    = raw.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

auto parse_conflict_context(std::string_view raw) -> std::optional<conflict_context> {
  auto const anchor_plan_id = context_int(raw, "anchor_plan_id");
  auto const entity_kind    = context_string(raw, "entity_kind");
  auto const entity_id      = context_int(raw, "entity_id");
  auto const file_path      = context_string(raw, "file_path");
  if (!anchor_plan_id || !entity_kind || !entity_id || !file_path) {
    return std::nullopt;
  }
  return conflict_context{
      .anchor_plan_id = *anchor_plan_id, .entity_kind = *entity_kind, .entity_id = *entity_id, .file_path = *file_path};
}

/// @brief Build the `sync_events.context_json` payload.
///
/// DELIBERATELY un-escaped, matching the Zig original's raw `{s}`
/// interpolation byte for byte. `planar.json_text`'s `append_json_string` is
/// this tree's one escaper and is used for every OPERATOR-FACING payload
/// (see render_cli.cpp) — but using it here would DIVERGE from the oracle
/// for any input containing a quote or backslash, and this string is written
/// and read back only by this module. Every field that reaches it is already
/// constrained: `entity_kind` is one of six literals, the hashes are hex,
/// the timestamps are ISO-8601, and `file_path` is built from
/// `feature::slugify` output. The one theoretical escape hatch is a plan's
/// `external_id`, which `feature::safe_path_segment` sanitizes for path
/// separators but not for quotes — an `external_id` containing `"` would
/// produce a malformed context in the Zig binary too, where it degrades to
/// "the dedupe lookup misses and a second conflict row is written". Matching
/// that exactly is the point.
auto render_conflict_context(std::int64_t anchor_plan_id, std::string_view entity_kind, std::int64_t entity_id,
                             std::string_view file_path, std::string_view fs_hash, std::string_view db_hash,
                             std::string_view fs_mtime, std::string_view db_updated_at) -> std::string {
  return std::format("{{\"anchor_plan_id\":{},\"entity_kind\":\"{}\",\"entity_id\":{},\"file_path\":\"{}\","
                     "\"fs_hash\":\"{}\",\"db_hash\":\"{}\",\"fs_mtime\":\"{}\",\"db_updated_at\":\"{}\"}}",
                     anchor_plan_id, entity_kind, entity_id, file_path, fs_hash, db_hash, fs_mtime, db_updated_at);
}

auto find_pending_conflict_event(db::connection& conn, std::int64_t anchor_plan_id, std::string_view entity_kind,
                                 std::int64_t entity_id, std::string_view file_path) -> std::int64_t {
  auto stmt = conn.prepare("select id, coalesce(context_json, '') from sync_events "
                           "where scope = 'workbench' and outcome = 'conflict' order by id desc");
  if (!stmt) {
    return 0;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped == db::step_result::done) {
      return 0;
    }
    auto const id  = stmt->column_int64(0);
    auto const raw = stmt->column_text(1);
    auto const ctx = parse_conflict_context(raw);
    if (!ctx) {
      continue;
    }
    if (ctx->anchor_plan_id == anchor_plan_id && ctx->entity_id == entity_id && ctx->entity_kind == entity_kind &&
        ctx->file_path == file_path) {
      return id;
    }
  }
}

/// @brief Reuse the open conflict row for this file, or record a new one.
///
/// Reuse is what stops a repeated `workbench status` from accumulating a
/// row per invocation.
auto ensure_conflict_event(db::connection& conn, mode run_mode, std::int64_t anchor_plan_id, std::string_view entity_kind,
                           std::int64_t entity_id, std::string_view file_path, std::string_view fs_hash, std::string_view db_hash,
                           std::string_view fs_mtime, std::string_view db_updated_at) -> std::expected<std::int64_t, sync_error> {
  if (auto const existing = find_pending_conflict_event(conn, anchor_plan_id, entity_kind, entity_id, file_path); existing > 0) {
    return existing;
  }
  std::string_view const direction = run_mode == mode::push ? "push" : "pull";
  auto const             context =
      render_conflict_context(anchor_plan_id, entity_kind, entity_id, file_path, fs_hash, db_hash, fs_mtime, db_updated_at);
  auto stmt = conn.prepare("insert into sync_events (link_id, scope, direction, outcome, context_json) "
                           "values (null, 'workbench', ?, 'conflict', ?) returning id");
  if (!stmt || !stmt->bind_text(1, direction) || !stmt->bind_text(2, context)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return std::unexpected(sync_error::query_failed);
  }
  auto const id = stmt->column_int64(0);
  if (id <= 0) {
    return std::unexpected(sync_error::query_failed);
  }
  return id;
}

// ---------------------------------------------------------------------------

auto find_state(std::span<const manifest::sync_state> rows, std::string_view kind, std::int64_t id)
    -> const manifest::sync_state* {
  for (auto const& row : rows) {
    if (row.entity_id == id && row.entity_kind == kind) {
      return &row;
    }
  }
  return nullptr;
}

auto find_state_by_path(std::span<const manifest::sync_state> rows, std::string_view file_path) -> const manifest::sync_state* {
  for (auto const& row : rows) {
    if (row.file_path == file_path) {
      return &row;
    }
  }
  return nullptr;
}

/// @brief An absolute path, reduced to its root-relative stored form.
auto to_stored_path(std::string_view absolute, std::string_view root) -> std::string {
  if (!absolute.starts_with(root)) {
    return std::string{absolute};
  }
  auto rel = absolute.substr(root.size());
  if (!rel.empty() && rel.front() == '/') {
    rel.remove_prefix(1);
  }
  return std::string{rel};
}

} // namespace

auto classification_name(classification value) -> std::string_view {
  switch (value) {
  case classification::no_op:
    return "no_op";
  case classification::fs_to_db:
    return "fs_to_db";
  case classification::db_to_fs:
    return "db_to_fs";
  case classification::conflict:
    return "conflict";
  case classification::new_on_fs:
    return "new_on_fs";
  case classification::deleted_on_fs:
    return "deleted_on_fs";
  case classification::malformed:
    return "malformed";
  }
  return "no_op";
}

auto extract_body_text(std::string_view body) -> std::string_view {
  std::size_t cursor  = 0;
  std::size_t start   = 0;
  bool        started = false;
  std::size_t pos     = 0;
  while (pos <= body.size()) {
    auto const nl      = body.find('\n', pos);
    auto const line    = body.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    auto const trimmed = [&] {
      auto       view  = line;
      auto const first = view.find_first_not_of(" \r\t");
      if (first == std::string_view::npos) {
        return std::string_view{};
      }
      auto const last = view.find_last_not_of(" \r\t");
      return view.substr(first, last - first + 1);
    }();
    if (!started) {
      bool const is_heading = trimmed.starts_with("# ");
      bool const is_label   = trimmed.starts_with("**") && trimmed.find(":**") != std::string_view::npos;
      if (is_heading || is_label || trimmed.empty()) {
        cursor += line.size() + 1;
        if (nl == std::string_view::npos) {
          break;
        }
        pos = nl + 1;
        continue;
      }
      started = true;
      start   = cursor;
    }
    cursor += line.size() + 1;
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  if (!started) {
    return {};
  }
  auto       rest  = body.substr(start);
  auto const first = rest.find_first_not_of(" \r\n\t");
  if (first == std::string_view::npos) {
    return {};
  }
  auto const last = rest.find_last_not_of(" \r\n\t");
  return rest.substr(first, last - first + 1);
}

auto fetch_anchor(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<anchor, sync_error> {
  auto stmt = conn.prepare("select p.id, coalesce(p.slug,''), coalesce(a.slug,''), p.scope_kind, p.scope_id "
                           "from plans p left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id) "
                           "where p.id = ? and p.parent_plan_id is null");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  anchor out{.id = stmt->column_int64(0), .slug = stmt->column_text(1), .assoc_slug = stmt->column_text(2)};
  if (stmt->column_text(3) == "association" && !stmt->is_null(4)) {
    out.assoc_id = stmt->column_int64(4);
  }
  out.plan_key = resolve_plan_key(conn, anchor_plan_id);
  return out;
}

auto resolve_plan_argument(db::connection& conn, std::string_view argument) -> std::expected<anchor, sync_error> {
  if (auto const id = parse::parse_int64_zig(argument)) {
    // A numeric argument is an ID, always — never falls back to a slug
    // lookup. `0` and negatives are refused as INVALID rather than
    // not-found, which is what makes `workbench push 0` exit 2 while
    // `workbench push 999` exits 1.
    if (*id < 1) {
      return std::unexpected(sync_error::invalid_input);
    }
    return fetch_anchor(conn, *id);
  }
  if (argument.empty()) {
    return std::unexpected(sync_error::invalid_input);
  }
  auto stmt = conn.prepare("select p.id from plans p where p.parent_plan_id is null and p.slug = ? "
                           "order by p.id limit 1");
  if (!stmt || !stmt->bind_text(1, argument)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  return fetch_anchor(conn, stmt->column_int64(0));
}

auto feature_dir_for(std::string_view root, const anchor& value) -> std::string {
  return feature::feature_dir(root, value.assoc_slug, value.plan_key, value.slug);
}

auto render_entity(db::connection& conn, std::int64_t anchor_plan_id, std::string_view kind, std::int64_t id)
    -> std::expected<rendered_entity, sync_error> {
  if (kind == "plan") {
    return render_plan(conn, anchor_plan_id, id);
  }
  if (kind == "task") {
    return render_task(conn, anchor_plan_id, id);
  }
  if (kind == "artifact") {
    return render_artifact(conn, anchor_plan_id, id);
  }
  if (kind == "question") {
    return render_question(conn, anchor_plan_id, id);
  }
  if (kind == "decision") {
    return render_decision(conn, anchor_plan_id, id);
  }
  if (kind == "scenario") {
    return render_scenario(conn, anchor_plan_id, id);
  }
  return std::unexpected(sync_error::not_found);
}

namespace {

/// @brief The one traversal all four verbs share. See sync.cppm's table.
auto run(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root, mode run_mode, terminal::mode filter_mode,
         bool apply_cleanup) -> std::expected<result, sync_error> {
  auto anchor_value = fetch_anchor(conn, anchor_plan_id);
  if (!anchor_value) {
    return std::unexpected(anchor_value.error());
  }
  auto const feature_dir = feature_dir_for(root, *anchor_value);
  if (run_mode != mode::status && !fsutil::make_path_all(feature_dir)) {
    return std::unexpected(sync_error::io_failed);
  }

  auto manifest_rows = manifest::load(conn, anchor_plan_id);
  if (!manifest_rows) {
    return std::unexpected(sync_error::query_failed);
  }
  auto entities = enumerate_entities(conn, anchor_plan_id);
  if (!entities) {
    return std::unexpected(entities.error());
  }

  result                             out;
  std::set<std::string, std::less<>> seen_files;

  auto const stored_for = [&](std::string_view rel) {
    return feature::stored_path(anchor_value->assoc_slug, anchor_value->plan_key, anchor_value->slug, rel);
  };

  for (auto const& e : *entities) {
    bool const is_anchor = e.kind == "plan" && e.id == anchor_plan_id;
    if (run_mode == mode::push && !is_anchor) {
      auto const drop = terminal::is_filtered_str(e.kind, e.status, filter_mode);
      if (drop.has_value() && *drop) {
        ++out.filtered;
        // The entity is excluded from the OUTPUT, but a file that already
        // exists for it is still part of this push's INPUT corpus: it is
        // parsed, counted, and optionally cleaned.
        auto rendered = render_entity(conn, anchor_plan_id, e.kind, e.id);
        if (!rendered) {
          return std::unexpected(rendered.error());
        }
        auto const stored = stored_for(rendered->rel_path);
        seen_files.insert(stored);
        auto const absolute = std::filesystem::path{feature_dir} / rendered->rel_path;
        if (fsutil::path_exists(absolute)) {
          ++out.pre_existing_terminal;
          auto const content = fsutil::read_file(absolute);
          if (content) {
            if (auto const parsed = parse::parse(*content); !parsed) {
              ++out.malformed;
              out.entries.push_back(entry{.value       = classification::malformed,
                                          .file_path   = stored,
                                          .entity_kind = e.kind,
                                          .entity_id   = e.id,
                                          .parse_error = std::string{parse::error_name(parsed.error())}});
            }
          }
          if (apply_cleanup) {
            static_cast<void>(fsutil::remove_file(absolute));
            ++out.cleaned;
            // Drop the manifest row too, or the next push would read the
            // now-missing file as drift.
            static_cast<void>(manifest::delete_by_entity(conn, anchor_plan_id, e.kind, e.id));
          }
        }
        continue;
      }
    }

    auto rendered = render_entity(conn, anchor_plan_id, e.kind, e.id);
    if (!rendered) {
      return std::unexpected(rendered.error());
    }
    auto const stored = stored_for(rendered->rel_path);
    seen_files.insert(stored);
    auto const absolute = std::filesystem::path{feature_dir} / rendered->rel_path;

    auto const fs_content = fsutil::read_file(absolute);
    auto const db_hash    = manifest::hash_content(rendered->content);

    std::optional<std::string> parse_error;
    if (fs_content) {
      if (auto const parsed = parse::parse(*fs_content); !parsed) {
        parse_error = std::string{parse::error_name(parsed.error())};
      }
    }

    auto const state = find_state(*manifest_rows, e.kind, e.id);
    auto const value = [&] -> classification {
      if (parse_error) {
        return classification::malformed;
      }
      if (state != nullptr) {
        if (!fs_content) {
          return classification::deleted_on_fs;
        }
        auto const fs_hash    = manifest::hash_content(*fs_content);
        bool const fs_changed = fs_hash != state->content_hash;
        bool const db_changed = e.updated_at != state->db_updated_at;
        if (!fs_changed && !db_changed) {
          return classification::no_op;
        }
        if (fs_changed && !db_changed) {
          return classification::fs_to_db;
        }
        if (!fs_changed) {
          return classification::db_to_fs;
        }
        // Both sides moved. If they converged on identical bytes there is
        // nothing to reconcile.
        return fs_hash == db_hash ? classification::no_op : classification::conflict;
      }
      if (!fs_content) {
        return classification::db_to_fs;
      }
      return manifest::hash_content(*fs_content) == db_hash ? classification::no_op : classification::fs_to_db;
    }();

    std::int64_t conflict_id = 0;
    switch (value) {
    case classification::no_op:
      break;
    case classification::db_to_fs:
      if (run_mode == mode::push || run_mode == mode::sync) {
        if (!fsutil::write_file_atomic(absolute, rendered->content)) {
          return std::unexpected(sync_error::io_failed);
        }
        if (!manifest::upsert(conn, manifest::sync_state{.anchor_plan_id = anchor_plan_id,
                                                         .entity_kind    = e.kind,
                                                         .entity_id      = e.id,
                                                         .file_path      = stored,
                                                         .content_hash   = db_hash,
                                                         .db_updated_at  = e.updated_at})) {
          return std::unexpected(sync_error::query_failed);
        }
        ++out.applied;
      } else {
        ++out.pending;
      }
      break;
    case classification::fs_to_db:
      if ((run_mode == mode::pull || run_mode == mode::sync) && fs_content && pull_to_db(conn, e.kind, e.id, *fs_content)) {
        auto const new_updated = fetch_updated_at(conn, e.kind, e.id);
        if (!new_updated) {
          return std::unexpected(new_updated.error());
        }
        if (!manifest::upsert(conn, manifest::sync_state{.anchor_plan_id = anchor_plan_id,
                                                         .entity_kind    = e.kind,
                                                         .entity_id      = e.id,
                                                         .file_path      = stored,
                                                         .content_hash   = manifest::hash_content(*fs_content),
                                                         .db_updated_at  = *new_updated})) {
          return std::unexpected(sync_error::query_failed);
        }
        ++out.applied;
      } else {
        ++out.pending;
      }
      break;
    case classification::conflict: {
      ++out.conflicts;
      ++out.pending;
      auto const fs_hash = fs_content ? manifest::hash_content(*fs_content) : std::string{};
      // `fs_mtime` is written EMPTY. The Zig original's `fileMtime` helper
      // ignores its path argument and returns "" unconditionally, so the
      // field has never carried a value. Reproduced (D2).
      auto const event =
          ensure_conflict_event(conn, run_mode, anchor_plan_id, e.kind, e.id, stored, fs_hash, db_hash, "", e.updated_at);
      if (!event) {
        return std::unexpected(event.error());
      }
      conflict_id = *event;
      break;
    }
    case classification::deleted_on_fs:
      if (run_mode == mode::pull || run_mode == mode::sync) {
        if (!soft_delete_entity(conn, e.kind, e.id)) {
          return std::unexpected(sync_error::query_failed);
        }
        if (!manifest::delete_by_file_path(conn, stored)) {
          return std::unexpected(sync_error::query_failed);
        }
        ++out.applied;
      } else {
        ++out.pending;
      }
      break;
    case classification::new_on_fs:
      ++out.pending;
      break;
    case classification::malformed:
      ++out.malformed;
      break;
    }

    out.entries.push_back(entry{.value       = value,
                                .file_path   = stored,
                                .entity_kind = e.kind,
                                .entity_id   = e.id,
                                .conflict_id = conflict_id,
                                .parse_error = parse_error.value_or(std::string{})});
  }

  // Manifest rows whose entity the DB no longer enumerates: if the file is
  // gone too, the entity is soft-deleted; if the file is still there, the
  // row is left alone entirely (it will be re-enumerated when the link
  // returns).
  for (auto const& state : *manifest_rows) {
    if (seen_files.contains(state.file_path)) {
      continue;
    }
    auto const absolute = std::filesystem::path{root} / state.file_path;
    if (fsutil::read_file(absolute)) {
      continue;
    }
    if (run_mode == mode::pull || run_mode == mode::sync) {
      if (!soft_delete_entity(conn, state.entity_kind, state.entity_id) ||
          !manifest::delete_by_file_path(conn, state.file_path)) {
        return std::unexpected(sync_error::query_failed);
      }
      ++out.applied;
    } else {
      ++out.pending;
    }
    out.entries.push_back(entry{.value       = classification::deleted_on_fs,
                                .file_path   = state.file_path,
                                .entity_kind = state.entity_kind,
                                .entity_id   = state.entity_id});
  }

  // Files on disk that no entity and no manifest row claims. Only a file
  // whose front matter says `entity_kind: task` is auto-created.
  auto fs_files = fsutil::collect_markdown(feature_dir);
  std::ranges::sort(fs_files);
  for (auto const& absolute : fs_files) {
    auto const stored = to_stored_path(absolute, root);
    if (seen_files.contains(stored) || find_state_by_path(*manifest_rows, stored) != nullptr) {
      continue;
    }
    auto const content = fsutil::read_file(absolute);
    if (!content) {
      continue;
    }
    auto const parsed = parse::parse(*content);
    if (!parsed) {
      ++out.malformed;
      out.entries.push_back(entry{.value       = classification::malformed,
                                  .file_path   = stored,
                                  .entity_kind = "",
                                  .entity_id   = 0,
                                  .parse_error = std::string{parse::error_name(parsed.error())}});
      continue;
    }
    if (parsed->frontmatter.entity_kind != "task") {
      ++out.pending;
      out.entries.push_back(entry{.value       = classification::new_on_fs,
                                  .file_path   = stored,
                                  .entity_kind = parsed->frontmatter.entity_kind,
                                  .entity_id   = 0});
      continue;
    }
    if (run_mode == mode::pull || run_mode == mode::sync) {
      auto const new_id = insert_task_from_frontmatter(conn, parsed->frontmatter, parsed->body, anchor_value->assoc_id);
      if (!new_id) {
        return std::unexpected(sync_error::query_failed);
      }
      if (!exec_step(conn,
                     "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                     "values ('task', ?, 'plan', ?, 'derives-from')",
                     [&](db::statement& s) {
                       return s.bind_int64(1, *new_id).has_value() && s.bind_int64(2, anchor_plan_id).has_value();
                     })) {
        return std::unexpected(sync_error::query_failed);
      }
      auto const new_updated = fetch_updated_at(conn, "task", *new_id);
      if (!new_updated) {
        return std::unexpected(new_updated.error());
      }
      if (!manifest::upsert(conn, manifest::sync_state{.anchor_plan_id = anchor_plan_id,
                                                       .entity_kind    = "task",
                                                       .entity_id      = *new_id,
                                                       .file_path      = stored,
                                                       .content_hash   = manifest::hash_content(*content),
                                                       .db_updated_at  = *new_updated})) {
        return std::unexpected(sync_error::query_failed);
      }
      ++out.applied;
      out.entries.push_back(
          entry{.value = classification::new_on_fs, .file_path = stored, .entity_kind = "task", .entity_id = *new_id});
    } else {
      ++out.pending;
      out.entries.push_back(
          entry{.value = classification::new_on_fs, .file_path = stored, .entity_kind = "task", .entity_id = 0});
    }
  }

  if (run_mode != mode::status) {
    auto rows = manifest::load(conn, anchor_plan_id);
    if (!rows) {
      return std::unexpected(sync_error::query_failed);
    }
    if (!manifest::write_sync_file(feature_dir, *rows)) {
      return std::unexpected(sync_error::io_failed);
    }
  }

  for (auto const& item : out.entries) {
    if (item.value == classification::malformed) {
      out.malformed_files.push_back(malformed_file{.path = item.file_path, .parse_error = item.parse_error});
    }
  }
  out.filter_mode = terminal::mode_to_string(filter_mode);
  return out;
}

} // namespace

auto status(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root) -> std::expected<result, sync_error> {
  return run(conn, anchor_plan_id, root, mode::status, terminal::mode::failures, false);
}

auto pull(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root) -> std::expected<result, sync_error> {
  return run(conn, anchor_plan_id, root, mode::pull, terminal::mode::failures, false);
}

auto push(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root, terminal::mode filter_mode,
          bool apply_cleanup) -> std::expected<result, sync_error> {
  return run(conn, anchor_plan_id, root, mode::push, filter_mode, apply_cleanup);
}

auto sync_both(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root) -> std::expected<result, sync_error> {
  return run(conn, anchor_plan_id, root, mode::sync, terminal::mode::failures, false);
}

auto resolve_conflict(db::connection& conn, std::string_view root, std::int64_t event_id, conflict_resolution prefer)
    -> std::expected<void, sync_error> {
  auto stmt = conn.prepare("select coalesce(context_json,''), outcome from sync_events "
                           "where id = ? and scope = 'workbench'");
  if (!stmt || !stmt->bind_int64(1, event_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  auto const context_json = stmt->column_text(0);
  auto const outcome      = stmt->column_text(1);
  // An already-settled event is INVALID INPUT, not not-found: the oracle
  // exits 2 for a second `resolve` of the same id, and 1 for an unknown one.
  if (outcome != "conflict") {
    return std::unexpected(sync_error::invalid_input);
  }
  auto const ctx = parse_conflict_context(context_json);
  if (!ctx) {
    return std::unexpected(sync_error::invalid_input);
  }

  auto const  absolute = std::filesystem::path{root} / ctx->file_path;
  std::string kept_hash;
  std::string db_updated;
  if (prefer == conflict_resolution::fs) {
    auto const fs_content = fsutil::read_file(absolute);
    if (!fs_content) {
      return std::unexpected(sync_error::not_found);
    }
    if (!pull_to_db(conn, ctx->entity_kind, ctx->entity_id, *fs_content)) {
      return std::unexpected(sync_error::invalid_input);
    }
    auto const updated = fetch_updated_at(conn, ctx->entity_kind, ctx->entity_id);
    if (!updated) {
      return std::unexpected(updated.error());
    }
    db_updated = *updated;
    kept_hash  = manifest::hash_content(*fs_content);
  } else {
    auto rendered = render_entity(conn, ctx->anchor_plan_id, ctx->entity_kind, ctx->entity_id);
    if (!rendered) {
      return std::unexpected(rendered.error());
    }
    if (!fsutil::write_file_atomic(absolute, rendered->content)) {
      return std::unexpected(sync_error::io_failed);
    }
    auto const updated = fetch_updated_at(conn, ctx->entity_kind, ctx->entity_id);
    if (!updated) {
      return std::unexpected(updated.error());
    }
    db_updated = *updated;
    kept_hash  = manifest::hash_content(rendered->content);
  }

  if (!manifest::upsert(conn, manifest::sync_state{.anchor_plan_id = ctx->anchor_plan_id,
                                                   .entity_kind    = ctx->entity_kind,
                                                   .entity_id      = ctx->entity_id,
                                                   .file_path      = ctx->file_path,
                                                   .content_hash   = kept_hash,
                                                   .db_updated_at  = db_updated})) {
    return std::unexpected(sync_error::query_failed);
  }

  std::string_view const settled = prefer == conflict_resolution::fs ? "resolved-fs" : "resolved-db";
  if (!exec_step(conn, "update sync_events set outcome = ? where id = ? and scope = 'workbench'", [&](db::statement& s) {
        return s.bind_text(1, settled).has_value() && s.bind_int64(2, event_id).has_value();
      })) {
    return std::unexpected(sync_error::query_failed);
  }
  return {};
}

auto archive(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root) -> std::expected<std::string, sync_error> {
  auto anchor_value = fetch_anchor(conn, anchor_plan_id);
  if (!anchor_value) {
    return std::unexpected(anchor_value.error());
  }
  auto const feature_dir = feature_dir_for(root, *anchor_value);
  if (fsutil::path_exists(feature_dir) && !fsutil::delete_tree(feature_dir)) {
    // Present but not a real directory (or undeletable). The Zig original
    // maps both onto QueryFailed; here they are io_failed, which reaches the
    // same exit code through the same generic bucket.
    return std::unexpected(sync_error::io_failed);
  }
  if (!manifest::delete_for_plan(conn, anchor_plan_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  return feature_dir;
}

auto restore(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root, terminal::mode filter_mode)
    -> std::expected<std::string, sync_error> {
  auto anchor_value = fetch_anchor(conn, anchor_plan_id);
  if (!anchor_value) {
    return std::unexpected(anchor_value.error());
  }
  auto const feature_dir = feature_dir_for(root, *anchor_value);
  if (!fsutil::make_path_all(feature_dir)) {
    return std::unexpected(sync_error::io_failed);
  }
  auto entities = enumerate_entities(conn, anchor_plan_id);
  if (!entities) {
    return std::unexpected(entities.error());
  }
  for (auto const& e : *entities) {
    // Same filter as push, so a restored tree mirrors what a fresh push
    // would write. The anchor plan is exempt.
    if (e.id != anchor_plan_id) {
      auto const drop = terminal::is_filtered_str(e.kind, e.status, filter_mode);
      if (drop.has_value() && *drop) {
        continue;
      }
    }
    auto rendered = render_entity(conn, anchor_plan_id, e.kind, e.id);
    if (!rendered) {
      return std::unexpected(rendered.error());
    }
    auto const absolute = std::filesystem::path{feature_dir} / rendered->rel_path;
    if (!fsutil::write_file_atomic(absolute, rendered->content)) {
      return std::unexpected(sync_error::io_failed);
    }
    auto const stored =
        feature::stored_path(anchor_value->assoc_slug, anchor_value->plan_key, anchor_value->slug, rendered->rel_path);
    if (!manifest::upsert(conn, manifest::sync_state{.anchor_plan_id = anchor_plan_id,
                                                     .entity_kind    = e.kind,
                                                     .entity_id      = e.id,
                                                     .file_path      = stored,
                                                     .content_hash   = manifest::hash_content(rendered->content),
                                                     .db_updated_at  = e.updated_at})) {
      return std::unexpected(sync_error::query_failed);
    }
  }
  auto rows = manifest::load(conn, anchor_plan_id);
  if (!rows) {
    return std::unexpected(sync_error::query_failed);
  }
  if (!manifest::write_sync_file(feature_dir, *rows)) {
    return std::unexpected(sync_error::io_failed);
  }
  return feature_dir;
}

auto list_active(db::connection& conn, std::string_view root) -> std::expected<std::vector<active_feature>, sync_error> {
  auto stmt = conn.prepare("select p.id, coalesce(p.slug,''), coalesce(p.status,''), coalesce(a.slug,'global') "
                           "from plans p left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id) "
                           "where p.parent_plan_id is null order by p.id");
  if (!stmt) {
    return std::unexpected(sync_error::query_failed);
  }
  std::vector<active_feature> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(sync_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    active_feature item{.plan_id = stmt->column_int64(0),
                        .slug    = stmt->column_text(1),
                        .status  = stmt->column_text(2),
                        // NOTE the literal `global` fallback here, where
                        // `fetch_anchor` coalesces to the EMPTY string
                        // instead. The two differ deliberately: this one is
                        // display text, and an empty assoc must collapse the
                        // directory level in the path. Both oracle-captured.
                        .assoc_slug = stmt->column_text(3)};
    item.plan_key    = resolve_plan_key(conn, item.plan_id);
    item.has_fs_tree = fsutil::path_exists(feature::feature_dir(root, item.assoc_slug, item.plan_key, item.slug));
    out.push_back(std::move(item));
  }
  return out;
}

} // namespace planar::engine::workbench::sync
