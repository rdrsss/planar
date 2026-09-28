// @file builder.t.cpp
// @brief Context-assembly tests for `planar.engine.templates.builder`
// (plan 996, task 6190).
//
// HOME SAFETY. Every test opens a uniquely-named scratch SQLite file under
// the system temp dir and deletes it on the way out. Nothing here reads an
// environment variable or resolves a home directory, so there is no path by
// which a run could touch the operator's real `~/.planar/planar.db`.
//
// ORACLE PROVENANCE. The anchor-walk rules and the two asymmetries below
// were established by running `templates render --entity <kind>:<id>`
// against a seeded arena on both binaries and diffing the payloads.
//
// WHAT THIS FILE IS ACTUALLY GUARDING. Three of the four SQL queries here
// carry a filter, and a filter that silently does not filter returns
// plausible rows that no exit-code or stdout check catches. So every filter
// is proven to EXCLUDE by seeding a row that must NOT come back and
// asserting it did not — never by asserting only that the wanted row
// survived.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.templates.builder;
import planar.engine.templates.context;

namespace tpl = planar::engine::templates;
namespace pdb = planar::db;

namespace {

/// @brief A scratch database file that deletes itself and its sidecars.
struct scratch_db {
  std::filesystem::path path_;

  scratch_db()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_tmpl_builder_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db(const scratch_db&)            = delete;
  scratch_db& operator=(const scratch_db&) = delete;
  scratch_db(scratch_db&&)                 = delete;
  scratch_db& operator=(scratch_db&&)      = delete;
  ~scratch_db() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

/// @brief Open `scratch` and apply every migration.
/// @param scratch The scratch path.
/// @return The open connection.
auto open_migrated(const scratch_db& scratch) -> pdb::connection {
  auto conn = pdb::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(pdb::apply_all(*conn).has_value());
  return std::move(*conn);
}

/// @brief Run a statement that returns one integer.
/// @param conn The connection.
/// @param sql The statement.
/// @param binds Text values to bind, 1-indexed.
/// @return The first column of the first row.
auto insert_returning_id(pdb::connection& conn, std::string_view sql, std::span<const std::string_view> binds) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  for (std::size_t i = 0; i < binds.size(); ++i) {
    REQUIRE(stmt->bind_text(static_cast<int>(i) + 1, binds[i]).has_value());
  }
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == pdb::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Execute a statement with no results.
/// @param conn The connection.
/// @param sql The statement.
auto exec(pdb::connection& conn, std::string_view sql) -> void {
  REQUIRE(conn.execute(sql).has_value());
}

/// @brief Insert an association.
/// @param conn The connection.
/// @param slug The slug.
/// @param name The display name.
/// @return The new row id.
auto add_assoc(pdb::connection& conn, std::string_view slug, std::string_view name) -> std::int64_t {
  std::array<std::string_view, 2> const binds{slug, name};
  return insert_returning_id(conn, "insert into associations (slug, name, kind) values (?, ?, 'project') returning id", binds);
}

/// @brief Insert a plan.
/// @param conn The connection.
/// @param slug The slug (globally unique).
/// @param title The title.
/// @param summary The summary — the column `{{.Plan.Body}}` reads.
/// @param assoc_id The owning association, or 0 for global scope.
/// @param parent The parent plan id, or 0 for a root plan.
/// @return The new row id.
auto add_plan(pdb::connection& conn, std::string_view slug, std::string_view title, std::string_view summary,
              std::int64_t assoc_id, std::int64_t parent) -> std::int64_t {
  auto stmt = conn.prepare("insert into plans (scope_kind, scope_id, title, slug, summary, status, parent_plan_id) "
                           "values (?, ?, ?, ?, ?, 'draft', ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, assoc_id == 0 ? "global" : "association").has_value());
  // `check ((scope_kind = 'global' and scope_id is null) or ...)` —
  // migrations/00003_work_items.up.sql. Binding 0 for a global-scope row
  // violates it, which is how this helper failed the first time.
  if (assoc_id == 0) {
    REQUIRE(stmt->bind_null(2).has_value());
  } else {
    REQUIRE(stmt->bind_int64(2, assoc_id).has_value());
  }
  REQUIRE(stmt->bind_text(3, title).has_value());
  REQUIRE(stmt->bind_text(4, slug).has_value());
  REQUIRE(stmt->bind_text(5, summary).has_value());
  if (parent == 0) {
    REQUIRE(stmt->bind_null(6).has_value());
  } else {
    REQUIRE(stmt->bind_int64(6, parent).has_value());
  }
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == pdb::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Insert a task.
/// @param conn The connection.
/// @param title The title.
/// @param body The body.
/// @param plan_id The owning plan, or 0 for none.
/// @return The new row id.
auto add_task(pdb::connection& conn, std::string_view title, std::string_view body, std::int64_t plan_id) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, scope_id, title, body, status, priority, plan_id) "
                           "values ('association', 1, ?, ?, 'todo', 100, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_text(2, body).has_value());
  if (plan_id == 0) {
    REQUIRE(stmt->bind_null(3).has_value());
  } else {
    REQUIRE(stmt->bind_int64(3, plan_id).has_value());
  }
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == pdb::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Insert a project (a "repo" endpoint for touches edges).
/// @param conn The connection.
/// @param slug The slug.
/// @return The new row id.
auto add_project(pdb::connection& conn, std::string_view slug) -> std::int64_t {
  std::array<std::string_view, 2> const binds{slug, slug};
  return insert_returning_id(conn, "insert into projects (slug, name, root_path) values (?, ?, '/tmp/x') returning id", binds);
}

/// @brief Insert an `entity_links` edge.
/// @param conn The connection.
/// @param from_kind The source kind.
/// @param from_id The source id.
/// @param to_kind The target kind.
/// @param to_id The target id.
/// @param rel The relationship.
auto add_link(pdb::connection& conn, std::string_view from_kind, std::int64_t from_id, std::string_view to_kind,
              std::int64_t to_id, std::string_view rel) -> void {
  auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values (?, ?, ?, ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, from_kind).has_value());
  REQUIRE(stmt->bind_int64(2, from_id).has_value());
  REQUIRE(stmt->bind_text(3, to_kind).has_value());
  REQUIRE(stmt->bind_int64(4, to_id).has_value());
  REQUIRE(stmt->bind_text(5, rel).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Insert an `external_links` row.
/// @param conn The connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param external_id The external key.
/// @param role The link role.
auto add_external(pdb::connection& conn, std::string_view kind, std::int64_t id, std::string_view external_id,
                  std::string_view role) -> void {
  // `external_links.system_id` is a FOREIGN KEY into `external_systems`,
  // not a `system` text column (migrations/00006_external.up.sql). The
  // system row is created once, lazily, because several tests add more
  // than one link.
  exec(conn, "insert or ignore into external_systems (id, kind, slug, auth_method, auth_ref) "
             "values (1, 'jira', 'jira-test', 'token-env', 'JIRA_TOKEN')");
  auto stmt = conn.prepare("insert into external_links (entity_kind, entity_id, system_id, external_id, link_role) "
                           "values (?, ?, 1, ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, kind).has_value());
  REQUIRE(stmt->bind_int64(2, id).has_value());
  REQUIRE(stmt->bind_text(3, external_id).has_value());
  REQUIRE(stmt->bind_text(4, role).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Insert a test scenario.
/// @param conn The connection.
/// @param title The title.
/// @param body The body.
/// @return The new row id.
auto add_scenario(pdb::connection& conn, std::string_view title, std::string_view body) -> std::int64_t {
  std::array<std::string_view, 2> const binds{title, body};
  return insert_returning_id(
      conn, "insert into test_scenarios (scope_kind, scope_id, title, body) values ('association', 1, ?, ?) returning id", binds);
}

} // namespace

// --- task contexts ----------------------------------------------------

TEST_CASE("build_task_context reads every task column the renderer exposes", "[templates][builder][task]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor Title", "anchor summary", assoc, 0);
  auto const       task   = add_task(conn, "Task Title", "task body", anchor);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->task.id == task);
  CHECK(ctx->task.title == "Task Title");
  CHECK(ctx->task.body == "task body");
  CHECK(ctx->task.status == "todo");
  CHECK(ctx->task.priority == 100);
  CHECK(ctx->task.scope_kind == "association");
  CHECK(ctx->task.scope_id == 1);
}

TEST_CASE("build_task_context sets Plan and Feature to the SAME anchor row", "[templates][builder][task]") {
  // The oracle exposes only the anchor for a task — `{{.Plan.Title}}` on a
  // task template names the FEATURE, not the task's own milestone plan.
  // Seeded through a child plan so a builder that used `tasks.plan_id`
  // directly would produce the child and fail here.
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor Title", "anchor summary", assoc, 0);
  auto const       child  = add_plan(conn, "child", "Child Title", "child summary", assoc, anchor);
  auto const       task   = add_task(conn, "T", "b", child);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->feature.id == anchor);
  CHECK(ctx->plan.id == anchor);
  CHECK(ctx->feature.title == "Anchor Title");
}

TEST_CASE("build_task_context climbs to the anchor through a derives-from edge", "[templates][builder][task][anchor]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       child  = add_plan(conn, "child", "Child", "s", assoc, anchor);
  // No `plan_id` at all — only the edge.
  auto const task = add_task(conn, "T", "b", 0);
  add_link(conn, "task", task, "plan", child, "derives-from");

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->feature.id == anchor);
}

TEST_CASE("build_task_context falls back to tasks.plan_id when the edge is ABSENT", "[templates][builder][task][anchor]") {
  // Tasks hang off a FOREIGN KEY rather than an edge, so the missing edge
  // is the normal case rather than an error.
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", anchor);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->feature.id == anchor);
}

TEST_CASE("build_task_context REFUSES a task with neither an edge nor a plan_id", "[templates][builder][task][anchor]") {
  // An operator-reachable refusal: `task add --scope global` with no plan.
  scratch_db const s;
  auto             conn = open_migrated(s);
  auto const       task = add_task(conn, "orphan", "b", 0);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE_FALSE(ctx.has_value());
  CHECK(ctx.error() == tpl::builder_error::anchor_plan_not_found);
}

TEST_CASE("build_task_context REFUSES a task id that does not exist", "[templates][builder][task]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       result = tpl::build_task_context(conn, 9999);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == tpl::builder_error::not_found);
}

TEST_CASE("build_task_context prefers the LOWEST-id derives-from edge", "[templates][builder][task][anchor]") {
  // `order by id limit 1`. With two edges the choice is observable, and a
  // builder that took the last one would pick a different feature.
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       first  = add_plan(conn, "first", "First", "s", assoc, 0);
  auto const       second = add_plan(conn, "second", "Second", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", 0);
  add_link(conn, "task", task, "plan", first, "derives-from");
  add_link(conn, "task", task, "plan", second, "derives-from");

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->feature.id == first);
  CHECK(ctx->feature.id != second);
}

// --- the filters, each proven to EXCLUDE ------------------------------

TEST_CASE("the anchor walk ignores an edge whose RELATIONSHIP is not derives-from", "[templates][builder][filter]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       wanted = add_plan(conn, "wanted", "Wanted", "s", assoc, 0);
  auto const       decoy  = add_plan(conn, "decoy", "Decoy", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", wanted);
  // A LOWER-id edge with the wrong relationship (`cites` -- the
  // relationship enum is closed; see migrations/00033). If the filter were
  // dropped, `order by id limit 1` would pick this one and the test would
  // see `decoy`.
  add_link(conn, "task", task, "plan", decoy, "cites");

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->feature.id == wanted);
}

TEST_CASE("loadTouches returns repo slugs in EDGE-INSERTION order, not sorted", "[templates][builder][touches]") {
  // `{{range .Touches}}` emits them in this order into an issue body, so
  // sorting would silently reorder a rendered payload. Seeded
  // deliberately out of alphabetical order.
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", anchor);
  auto const       zebra  = add_project(conn, "org/zebra");
  auto const       alpha  = add_project(conn, "org/alpha");
  add_link(conn, "task", task, "repo", zebra, "touches");
  add_link(conn, "task", task, "repo", alpha, "touches");

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  REQUIRE(ctx->touches.size() == 2);
  CHECK(ctx->touches[0] == "org/zebra");
  CHECK(ctx->touches[1] == "org/alpha");
}

TEST_CASE("loadTouches EXCLUDES another entity's edges and non-touches edges", "[templates][builder][touches][filter]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       mine   = add_task(conn, "mine", "b", anchor);
  auto const       other  = add_task(conn, "other", "b", anchor);
  auto const       repo_a = add_project(conn, "org/mine");
  auto const       repo_b = add_project(conn, "org/theirs");
  auto const       repo_c = add_project(conn, "org/wrongrel");

  add_link(conn, "task", mine, "repo", repo_a, "touches");
  add_link(conn, "task", other, "repo", repo_b, "touches"); // different from_id
  add_link(conn, "task", mine, "repo", repo_c, "cites");    // different relationship
  add_link(conn, "plan", mine, "repo", repo_b, "touches");  // different from_kind

  auto const ctx = tpl::build_task_context(conn, mine);
  REQUIRE(ctx.has_value());
  REQUIRE(ctx->touches.size() == 1);
  CHECK(ctx->touches[0] == "org/mine");

  // And the OTHER task still sees its own — proving the rows exist and the
  // filter excluded them rather than the seed having failed.
  auto const other_ctx = tpl::build_task_context(conn, other);
  REQUIRE(other_ctx.has_value());
  REQUIRE(other_ctx->touches.size() == 1);
  CHECK(other_ctx->touches[0] == "org/theirs");
}

TEST_CASE("loadExternalKey takes only link_role='mirror'", "[templates][builder][external][filter]") {
  // `{{if .ExternalKey}}` is the shipped templates' "has this been
  // propagated yet?" test, so a non-mirror row leaking in would make an
  // unpropagated entity look propagated.
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", anchor);
  // A LOWER-id non-mirror row. Without the filter, `order by id limit 1`
  // returns this one.
  add_external(conn, "task", task, "REF-1", "reference");

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->external_key.empty());

  add_external(conn, "task", task, "JIRA-9", "mirror");
  auto const with_mirror = tpl::build_task_context(conn, task);
  REQUIRE(with_mirror.has_value());
  CHECK(with_mirror->external_key == "JIRA-9");
}

TEST_CASE("loadExternalKey EXCLUDES another entity's mirror", "[templates][builder][external][filter]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       mine   = add_task(conn, "mine", "b", anchor);
  auto const       other  = add_task(conn, "other", "b", anchor);
  add_external(conn, "task", other, "OTHER-1", "mirror");
  // Same id, different KIND — the from_kind filter's own probe.
  add_external(conn, "plan", mine, "PLAN-1", "mirror");

  auto const ctx = tpl::build_task_context(conn, mine);
  REQUIRE(ctx.has_value());
  CHECK(ctx->external_key.empty());

  auto const other_ctx = tpl::build_task_context(conn, other);
  REQUIRE(other_ctx.has_value());
  CHECK(other_ctx->external_key == "OTHER-1");
}

// --- plan contexts ----------------------------------------------------

TEST_CASE("build_plan_context distinguishes Plan from Feature for a CHILD plan", "[templates][builder][plan]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor Title", "anchor summary", assoc, 0);
  auto const       child  = add_plan(conn, "child", "Child Title", "child summary", assoc, anchor);

  auto const ctx = tpl::build_plan_context(conn, child);
  REQUIRE(ctx.has_value());
  CHECK(ctx->plan.id == child);
  CHECK(ctx->plan.title == "Child Title");
  CHECK(ctx->feature.id == anchor);
  CHECK(ctx->feature.title == "Anchor Title");
}

TEST_CASE("build_plan_context makes a TOP-LEVEL plan its own anchor", "[templates][builder][plan]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);

  auto const ctx = tpl::build_plan_context(conn, anchor);
  REQUIRE(ctx.has_value());
  CHECK(ctx->plan.id == anchor);
  CHECK(ctx->feature.id == anchor);
}

TEST_CASE("build_plan_context climbs MULTIPLE parent levels", "[templates][builder][plan][anchor]") {
  // Two levels, so a walk that climbed exactly once would pick the middle
  // plan and still look like it worked.
  scratch_db const s;
  auto             conn  = open_migrated(s);
  auto const       assoc = add_assoc(conn, "project:proj", "Proj");
  auto const       root  = add_plan(conn, "root", "Root", "s", assoc, 0);
  auto const       mid   = add_plan(conn, "mid", "Mid", "s", assoc, root);
  auto const       leaf  = add_plan(conn, "leaf", "Leaf", "s", assoc, mid);

  auto const ctx = tpl::build_plan_context(conn, leaf);
  REQUIRE(ctx.has_value());
  CHECK(ctx->feature.id == root);
  CHECK(ctx->feature.id != mid);
}

TEST_CASE("build_plan_context reads Body from the plans.SUMMARY column", "[templates][builder][plan]") {
  // There IS no `plans.body` column. The template-visible field name and
  // the column name genuinely disagree.
  scratch_db const s;
  auto             conn  = open_migrated(s);
  auto const       assoc = add_assoc(conn, "project:proj", "Proj");
  auto const       plan  = add_plan(conn, "p", "T", "the summary text", assoc, 0);

  auto const ctx = tpl::build_plan_context(conn, plan);
  REQUIRE(ctx.has_value());
  CHECK(ctx->plan.body == "the summary text");
}

TEST_CASE("build_plan_context REFUSES a plan id that does not exist", "[templates][builder][plan]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       result = tpl::build_plan_context(conn, 9999);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == tpl::builder_error::not_found);
}

// --- scenario contexts ------------------------------------------------

TEST_CASE("build_scenario_context reads the scenario and climbs its derives-from edge", "[templates][builder][scenario]") {
  scratch_db const s;
  auto             conn     = open_migrated(s);
  auto const       assoc    = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor   = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       scenario = add_scenario(conn, "Scenario Title", "scenario body");
  // NOTE the kind spelling: `test_scenario` in entity_links, even though
  // the CLI accepts `scenario:` for `--entity`.
  add_link(conn, "test_scenario", scenario, "plan", anchor, "derives-from");

  auto const ctx = tpl::build_scenario_context(conn, scenario);
  REQUIRE(ctx.has_value());
  CHECK(ctx->scenario.id == scenario);
  CHECK(ctx->scenario.title == "Scenario Title");
  CHECK(ctx->scenario.body == "scenario body");
  CHECK(ctx->feature.id == anchor);
}

TEST_CASE("build_scenario_context REFUSES a scenario with no derives-from edge", "[templates][builder][scenario][anchor]") {
  // Unlike a task, a scenario has NO column-based fallback. This refusal is
  // operator-reachable: `templates render … --entity scenario:<id>` on an
  // unlinked scenario exits non-zero rather than rendering against an
  // empty feature.
  scratch_db const s;
  auto             conn     = open_migrated(s);
  auto const       scenario = add_scenario(conn, "S", "b");

  auto const ctx = tpl::build_scenario_context(conn, scenario);
  REQUIRE_FALSE(ctx.has_value());
  CHECK(ctx.error() == tpl::builder_error::anchor_plan_not_found);
}

TEST_CASE("build_scenario_context leaves Touches EMPTY even when edges exist", "[templates][builder][scenario][asymmetry]") {
  // The oracle's scenario builder never queries the touches edges, unlike
  // its task and plan siblings. Reproduced, not corrected — a scenario
  // template's `{{range .Touches}}` renders nothing on BOTH trees.
  //
  // Seeded WITH edges so this asserts the asymmetry rather than the
  // absence of data.
  scratch_db const s;
  auto             conn     = open_migrated(s);
  auto const       assoc    = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor   = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       scenario = add_scenario(conn, "S", "b");
  auto const       repo     = add_project(conn, "org/repo");
  add_link(conn, "test_scenario", scenario, "plan", anchor, "derives-from");
  add_link(conn, "test_scenario", scenario, "repo", repo, "touches");

  auto const ctx = tpl::build_scenario_context(conn, scenario);
  REQUIRE(ctx.has_value());
  CHECK(ctx->touches.empty());

  // The same edge shape DOES populate touches for a task, which is what
  // makes this an asymmetry rather than a broken query.
  auto const task = add_task(conn, "T", "b", anchor);
  add_link(conn, "task", task, "repo", repo, "touches");
  auto const task_ctx = tpl::build_task_context(conn, task);
  REQUIRE(task_ctx.has_value());
  CHECK(task_ctx->touches.size() == 1);
}

TEST_CASE("build_scenario_context REFUSES a scenario id that does not exist", "[templates][builder][scenario]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       result = tpl::build_scenario_context(conn, 9999);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == tpl::builder_error::not_found);
}

// --- association resolution -------------------------------------------

TEST_CASE("the association is resolved from the ANCHOR's scope_id", "[templates][builder][assoc]") {
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj Display");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", anchor);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->assoc.slug == "project:proj");
  CHECK(ctx->assoc.name == "Proj Display");
}

TEST_CASE("a GLOBAL-scope anchor yields an EMPTY association rather than a failure", "[templates][builder][assoc]") {
  // A plan in global scope has no association, and that is not an error —
  // `{{.Assoc.Slug}}` renders the empty string.
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", 0, 0);
  auto const       task   = add_task(conn, "T", "b", anchor);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->assoc.slug.empty());
  CHECK(ctx->assoc.name.empty());
}

TEST_CASE("a DANGLING association id yields an empty association rather than a failure", "[templates][builder][assoc]") {
  scratch_db const s;
  auto             conn = open_migrated(s);
  exec(conn, "insert into plans (scope_kind, scope_id, title, slug, summary, status) "
             "values ('association', 4242, 'Anchor', 'anchor', 's', 'draft')");
  auto const task = add_task(conn, "T", "b", 1);

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->assoc.slug.empty());
}

// --- NULL handling ----------------------------------------------------

TEST_CASE("NULL text columns come back as the EMPTY STRING, not as a crash", "[templates][builder][null]") {
  // Every text column is `coalesce`d IN SQL, so NULL and '' are
  // indistinguishable by construction — which is what the renderer wants
  // (`{{if .Task.Body}}` is falsy for both).
  scratch_db const s;
  auto             conn   = open_migrated(s);
  auto const       assoc  = add_assoc(conn, "project:proj", "Proj");
  auto const       anchor = add_plan(conn, "anchor", "Anchor", "s", assoc, 0);
  auto const       task   = add_task(conn, "T", "b", anchor);
  exec(conn, "update tasks set body = null");
  exec(conn, "update plans set summary = null");

  auto const ctx = tpl::build_task_context(conn, task);
  REQUIRE(ctx.has_value());
  CHECK(ctx->task.body.empty());
  CHECK(ctx->feature.body.empty());
}
